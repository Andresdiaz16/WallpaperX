// SPDX-License-Identifier: GPL-2.0-or-later

// S13.169 and S13.171's first half -- openWallpaperFile() is the one door every wallpaper file is
// opened through, and it refuses anything that is not a regular file without ever parking the
// thread that called it. And S13.193-S13.194: the filesystems it refuses, and the daemon's own
// shortages it reports as such rather than as the file's fault.
//
// WHY THIS IS ITS OWN BINARY, with no Vulkan in it at all. The property is about the kernel and
// nothing else: which kinds of path park an open, what a descriptor says about what was opened,
// and whether a refusal leaks the descriptor it checked. A probe that built a Device to ask it
// would answer the same question and additionally skip on every host without a driver -- and this
// is exactly the check a host without a GPU should still run, because the defect it grades (one
// screen's wallpaper file stopping every screen) is a kernel wait, not a GPU one.
//
// The Device also makes the strongest case here impossible to write. `Device` pins itself to its
// creating thread and aborts on a GPU call from any other, and a StaticRenderer or VideoDecoder
// that throws unwinds through cleanup code that touches the device -- so neither can be
// constructed on a worker thread, and a worker thread with a deadline is the only way to ask "did
// this open come back?" without the failing case hanging the whole run. The renderers' own use of
// this function is graded where they live: image_bounds_probe (still) and video_decoder_probe
// (video), on paths that cannot park.

#include "support/check.h"
#include "support/video_probe_support.h"  // ScratchDir

#include "vulkan/gpu_fault.h"
#include "wallpaper_file.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

using wallpaperx::ResourceExhausted;
using wallpaperx::WallpaperContentError;
using wallpaperx::WallpaperOpen;
using wallpaperx::openWallpaper;
using wallpaperx::openWallpaperFile;
using wallpaperx::refusedFilesystemOf;
using wallpaperx::testing::check;
using wallpaperx::testing::fail;
using wallpaperx::testing::reportFailures;
using wallpaperx::testing::ScratchDir;

// What the filesystem checks below are fed instead of the machine's own answers. renderer/
// wallpaper_file.cpp reads /proc/self/mountinfo and calls fstatfs(2) with no seam of its own, and
// mounting a network or FUSE filesystem is not something a test may do, so this executable defines
// open, lstat and fstatfs itself: calls from the renderer library it links resolve here, and each
// forwards to the real call unless a check has armed it. Unarmed, one relaxed load per call.
namespace {

struct Injection {
    std::string mountTable;      // non-empty: /proc/self/mountinfo is read from this file instead
    int mountTableError = 0;     // non-zero: opening the mount table fails with this errno
    std::string failPath;        // opening exactly this path fails with failPathError
    int failPathError = 0;
    long statfsType = 0;         // non-zero: fstatfs reports this filesystem type
    std::vector<std::string> lstats;  // every path lstat was asked about while armed
};

std::atomic<bool> g_injecting{false};
std::mutex g_injectionMutex;
Injection g_injection;

constexpr const char* kMountTablePath = "/proc/self/mountinfo";

}  // namespace

extern "C" int open(const char* path, int flags, ...) {
    using Fn = int (*)(const char*, int, mode_t);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "open"));
    if (real == nullptr) return -1;
    mode_t mode = 0;
    if ((flags & (O_CREAT | O_TMPFILE)) != 0) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, unsigned int));
        va_end(args);
    }
    if (g_injecting.load(std::memory_order_relaxed) && path != nullptr) {
        std::string substitute;
        {
            std::lock_guard<std::mutex> lock(g_injectionMutex);
            if (std::strcmp(path, kMountTablePath) == 0) {
                if (g_injection.mountTableError != 0) {
                    errno = g_injection.mountTableError;
                    return -1;
                }
                substitute = g_injection.mountTable;
            } else if (!g_injection.failPath.empty() && g_injection.failPath == path) {
                errno = g_injection.failPathError;
                return -1;
            }
        }
        if (!substitute.empty()) return real(substitute.c_str(), flags, mode);
    }
    return real(path, flags, mode);
}

extern "C" int lstat(const char* path, struct stat* info) {
    using Fn = int (*)(const char*, struct stat*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "lstat"));
    if (real == nullptr) return -1;
    if (g_injecting.load(std::memory_order_relaxed) && path != nullptr) {
        std::lock_guard<std::mutex> lock(g_injectionMutex);
        g_injection.lstats.emplace_back(path);
    }
    return real(path, info);
}

extern "C" int fstatfs(int fd, struct statfs* info) {
    using Fn = int (*)(int, struct statfs*);
    static const Fn real = reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, "fstatfs"));
    if (real == nullptr) return -1;
    const int result = real(fd, info);
    if (result == 0 && g_injecting.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_injectionMutex);
        if (g_injection.statfsType != 0) info->f_type = g_injection.statfsType;
    }
    return result;
}

namespace {

constexpr const char* kComponent = "WallpaperFileProbe";

// What one call to openWallpaperFile() did, whichever of its three answers it gave.
struct Opened {
    int fd = -1;
    int err = 0;              // errno, when fd < 0 and nothing was thrown
    bool refusedAsContent = false;
    std::string message;      // the content error's text
    bool threwSomethingElse = false;
};

Opened callOpen(const std::string& path) {
    Opened out;
    errno = 0;
    try {
        out.fd = openWallpaperFile(path, kComponent);
        out.err = errno;
    } catch (const WallpaperContentError& e) {
        out.refusedAsContent = true;
        out.message = e.what();
    } catch (const std::exception& e) {
        out.threwSomethingElse = true;
        out.message = e.what();
    }
    return out;
}

int openDescriptorCount() {
    std::error_code ec;
    int n = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++n;
    }
    return ec ? -1 : n;
}

// ----------------------------------------------------------------------------------------- //

// GOAL: S13.169 -- a wallpaper path that is not a regular file is refused as the FILE's fault
//       (WallpaperContentError, which the daemon records against that monitor and goes on) rather
//       than accepted, and the refusal names the path so an operator can see which wallpaper it is
//       about. False for the code before this fix, which handed every one of these to ffmpeg or to
//       stb.
// WHY-THIS-SHAPE: four kinds, not one, and they fail for three different reasons -- a FIFO parks
//       the open, a directory and a character device open instantly and then misbehave on read, a
//       dangling symlink never opens at all. A single FIFO case would pass for an implementation
//       that special-cased pipes, which is the shape a smaller fix would have taken.
//       The FIFO here has a WRITER attached for the whole case, so this check cannot park whatever
//       production does; the no-writer FIFO -- the one that can park -- is graded on its own thread
//       in checkAnUnwritableFifoDoesNotParkTheCaller(), which is the only place a hang is possible
//       and the only place that can recover from one.
// CONTROL: in renderer/wallpaper_file.cpp, `if (!S_ISREG(info.st_mode))` -> `if (false)` -> 13
//       FAILs, four per refusable kind ("a directory was opened as a wallpaper file", "... was not
//       refused as a content error", and the two naming checks) plus the no-writer pipe below,
//       with the run printing "a directory -> OPENED". Reverted, green.
void checkIrregularPathsAreRefusedAsContent(const ScratchDir& scratch) {
    const std::string fifo = scratch.file("with-writer.fifo");
    const std::string directory = scratch.file("a-directory");
    const std::string dangling = scratch.file("dangling.link");

    std::error_code ec;
    std::filesystem::create_directory(directory, ec);
    if (ec) {
        fail("wallpaper_file_probe: could not make a directory to offer, so that kind was not exercised");
        return;
    }
    if (::mkfifo(fifo.c_str(), 0600) != 0) {
        fail(std::string("wallpaper_file_probe: could not make a FIFO (") + std::strerror(errno) +
             "), so no pipe was offered");
        return;
    }
    std::filesystem::create_symlink(scratch.file("nothing-is-here.mp4"), dangling, ec);
    if (ec) {
        fail("wallpaper_file_probe: could not make a dangling symlink, so that kind was not exercised");
        return;
    }

    // Held open for the whole phase: with a writer present, open(O_RDONLY) on a FIFO returns at
    // once even with no O_NONBLOCK, so this case grades the refusal and can never grade the wait.
    // O_RDWR, not O_WRONLY: a write-only open of a FIFO is itself the wait this probe is avoiding
    // (it blocks for a reader, or gives ENXIO with O_NONBLOCK, since nothing is reading yet).
    // Linux's O_RDWR on a FIFO opens both ends at once and cannot block.
    const int writer = ::open(fifo.c_str(), O_RDWR | O_CLOEXEC);
    if (writer < 0) {
        fail("wallpaper_file_probe: could not attach a writer to the FIFO, so the pipe case would "
             "have risked parking this thread and was not run");
        return;
    }

    struct Kind {
        const char* what;
        const std::string* path;
        bool expectContentError;  // false: refused by the OS, reported through errno
        int expectErrno;
    };
    const Kind kinds[] = {
        {"a named pipe with a writer", &fifo, true, 0},
        {"a directory", &directory, true, 0},
        {"a character device (/dev/null)", nullptr, true, 0},
        {"a dangling symlink", &dangling, false, ENOENT},
    };

    const std::string devNull = "/dev/null";
    const int fdsBefore = openDescriptorCount();
    for (const Kind& kind : kinds) {
        const std::string& path = kind.path == nullptr ? devNull : *kind.path;
        const Opened opened = callOpen(path);
        std::printf("wallpaper_file_probe: %s -> %s\n", kind.what,
                    opened.refusedAsContent ? ("refused as content: " + opened.message).c_str()
                    : opened.threwSomethingElse ? ("threw: " + opened.message).c_str()
                    : opened.fd >= 0          ? "OPENED"
                                              : std::strerror(opened.err));
        if (opened.fd >= 0) ::close(opened.fd);

        check(opened.fd < 0, std::string(kind.what) + " was opened as a wallpaper file");
        check(!opened.threwSomethingElse,
              std::string(kind.what) + " was blamed on something other than the file: " + opened.message);
        if (kind.expectContentError) {
            check(opened.refusedAsContent,
                  std::string(kind.what) + " was not refused as a content error");
            check(opened.message.find(path) != std::string::npos,
                  std::string(kind.what) + ": the refusal does not name the path it is about");
            check(opened.message.find(kComponent) != std::string::npos,
                  std::string(kind.what) + ": the refusal does not name the component that made it");
        } else {
            check(!opened.refusedAsContent,
                  std::string(kind.what) + " was turned into a content error rather than left to the caller");
            check(opened.err == kind.expectErrno,
                  std::string(kind.what) + ": errno is not the OS's own reason (expected " +
                      std::strerror(kind.expectErrno) + ", saw " + std::strerror(opened.err) + ")");
        }
    }
    const int fdsAfter = openDescriptorCount();

    ::close(writer);

    // GOAL: a refusal gives back the descriptor it checked. The function opens first and asks
    //       fstat afterwards -- which is the whole point, there being no window between the two --
    //       so every refusal holds an open descriptor at the moment it decides, and a daemon that
    //       re-renders a bad wallpaper every few seconds would run out of them.
    // CONTROL: renderer/wallpaper_file.cpp, the `::close(fd)` above the S_ISREG throw deleted ->
    //       "FAIL: a refused wallpaper file leaks no descriptor (5 open before the refusals, 8
    //       after)" -- one per refused kind, and nothing else red. Reverted.
    check(fdsBefore >= 0 && fdsAfter >= 0,
          "this host publishes /proc/self/fd, so descriptor leaks are observable here");
    if (fdsBefore >= 0 && fdsAfter >= 0) {
        check(fdsAfter <= fdsBefore,
              "a refused wallpaper file leaks no descriptor (" + std::to_string(fdsBefore) +
                  " open before the refusals, " + std::to_string(fdsAfter) + " after)");
    }
}

// GOAL: S13.169's other half -- a named pipe with nobody writing to it does not park the thread
//       that opens it. This is the defect the fix exists for: that thread is the daemon's one GPU
//       thread, and an open that never returns stops every monitor on the machine, not one.
// WHY-THIS-SHAPE: a worker thread with a deadline, because the failing case cannot be detected any
//       other way -- a call that never returns cannot report. The success path is NOT reachable by
//       the deadline expiring: expiry sets `parked`, which fails the check, and the arithmetic
//       about what was returned is only read when the future arrived. And the wait is rescued
//       rather than abandoned: a writer is attached to the FIFO, which releases a blocked open, so
//       a red control ends in a joined thread and a printed summary instead of a hung suite.
// CONTROL: in renderer/wallpaper_file.cpp, `O_NONBLOCK` removed from the open flags -> "FAIL:
//       opening a wallpaper file that is a named pipe with no writer returns instead of parking
//       the calling thread", after the ten-second deadline. The rescue then worked as designed:
//       the writer released the open, the thread joined, the refusal still printed, and the
//       process exited 1 rather than hanging. Reverted, green and instant.
void checkAnUnwritableFifoDoesNotParkTheCaller(const ScratchDir& scratch) {
    const std::string fifo = scratch.file("no-writer.fifo");
    if (::mkfifo(fifo.c_str(), 0600) != 0) {
        fail(std::string("wallpaper_file_probe: could not make a FIFO (") + std::strerror(errno) +
             "), so the parking case was not exercised");
        return;
    }

    std::promise<Opened> done;
    std::future<Opened> answer = done.get_future();
    std::thread worker([&done, &fifo] { done.set_value(callOpen(fifo)); });

    const bool parked = answer.wait_for(std::chrono::seconds(10)) != std::future_status::ready;
    check(!parked,
          "opening a wallpaper file that is a named pipe with no writer returns instead of parking "
          "the calling thread -- which in the daemon is the one thread that draws every monitor");

    if (parked) {
        // Attaching a writer is what lets a blocked O_RDONLY open return. O_NONBLOCK on this side
        // so that a thread which is NOT in the FIFO open (some other wait) gives ENXIO here rather
        // than parking the rescue as well.
        const int writer = ::open(fifo.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
        if (writer >= 0) ::close(writer);
        if (answer.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            // Reported, then exited: this thread cannot be joined and the process cannot leave
            // main. Printing the count first is what keeps this a failing run rather than a hang
            // with no output -- the one thing worse than a red check here is a suite that stops.
            std::fprintf(stderr,
                         "wallpaper_file_probe: the parked open did not return even with a writer "
                         "attached; the thread cannot be joined, so this run ends here\n");
            reportFailures("wallpaper_file_probe");
            std::fflush(nullptr);
            worker.detach();
            std::_Exit(1);
        }
    }

    const Opened opened = answer.get();
    worker.join();
    if (opened.fd >= 0) ::close(opened.fd);
    std::printf("wallpaper_file_probe: a named pipe with no writer -> %s\n",
                opened.refusedAsContent ? ("refused as content: " + opened.message).c_str()
                : opened.fd >= 0        ? "OPENED"
                                        : std::strerror(opened.err));
    check(opened.refusedAsContent && opened.fd < 0,
          "a named pipe with no writer is refused as a content error rather than opened");
}

// GOAL: a regular file still opens, through a descriptor that reads that file's bytes and no
//       other's, and the flags the function leaves on it are the ones its callers are written
//       against: close-on-exec set, and non-blocking CLEARED -- the latter because O_NONBLOCK was
//       for the open alone, and a reader that saw EAGAIN from a descriptor it was told is a plain
//       file would treat a short read as the end of the wallpaper.
// WHY-THIS-SHAPE: without this the refusals above are satisfied by a function that refuses
//       everything, which is the cheapest way to make every other check here pass.
// CONTROL: the two flag halves, separately, both in renderer/wallpaper_file.cpp. O_CLOEXEC dropped
//       from the open flags -> "FAIL: the descriptor is close-on-exec, so a wallpaper file cannot
//       leak into a child process". The F_SETFL line deleted -> "FAIL: the descriptor handed back
//       is a blocking one, so a reader written for a plain file is not given EAGAIN in place of
//       data". Both reverted; nothing else went red under either.
void checkARegularFileOpensAndReadsBack(const ScratchDir& scratch) {
    const std::string real = scratch.file("a-real-file.bin");
    const std::string contents = "wallpaper bytes, and nothing else\n";
    {
        std::ofstream out(real, std::ios::binary);
        out << contents;
    }

    const Opened opened = callOpen(real);
    check(opened.fd >= 0 && !opened.refusedAsContent,
          "a plain regular file is opened rather than refused" +
              (opened.message.empty() ? std::string() : " (said: " + opened.message + ")"));
    if (opened.fd < 0) return;

    std::vector<char> buffer(contents.size() + 8, '\0');
    const ssize_t got = ::read(opened.fd, buffer.data(), buffer.size());
    check(got == static_cast<ssize_t>(contents.size()) &&
              std::string(buffer.data(), contents.size()) == contents,
          "the descriptor reads back the bytes of the file that was asked for");

    const int flags = ::fcntl(opened.fd, F_GETFL);
    const int fdFlags = ::fcntl(opened.fd, F_GETFD);
    check(flags >= 0 && (flags & O_NONBLOCK) == 0,
          "the descriptor handed back is a blocking one, so a reader written for a plain file is "
          "not given EAGAIN in place of data");
    check(fdFlags >= 0 && (fdFlags & FD_CLOEXEC) != 0,
          "the descriptor is close-on-exec, so a wallpaper file cannot leak into a child process");
    ::close(opened.fd);
}

// ----------------------------------------------------------------------------------------- //
// Network and FUSE filesystems (E5 decision 5), and the daemon running short (E5 decision 4)
// ----------------------------------------------------------------------------------------- //

// Arms the hooks above for one case and disarms them when it goes out of scope, so a check that
// returns early cannot leave the next one reading a table it never wrote.
class Injected {
public:
    explicit Injected(Injection injection) {
        std::lock_guard<std::mutex> lock(g_injectionMutex);
        g_injection = std::move(injection);
        g_injecting.store(true);
    }
    ~Injected() {
        g_injecting.store(false);
        std::lock_guard<std::mutex> lock(g_injectionMutex);
        g_injection = Injection{};
    }
    Injected(const Injected&) = delete;
    Injected& operator=(const Injected&) = delete;

    std::vector<std::string> lstats() const {
        std::lock_guard<std::mutex> lock(g_injectionMutex);
        return g_injection.lstats;
    }
};

// One line of /proc/self/mountinfo, with the mount point escaped the way the kernel escapes it.
struct FakeMount {
    int id;
    int parent;
    std::string point;
    std::string type;
    unsigned major = 0;
    unsigned minor = 999;
};

std::string escapeMountPoint(const std::string& point) {
    std::string out;
    for (const char c : point) {
        if (c == ' ') out += "\\040";
        else if (c == '\t') out += "\\011";
        else if (c == '\n') out += "\\012";
        else if (c == '\\') out += "\\134";
        else out += c;
    }
    return out;
}

// The table always starts with the root, mount 1, whose parent is listed nowhere -- which is how the
// function finds this process's root. `mounts` follow in the order given: later lines are newer.
std::string writeMountTable(const ScratchDir& scratch, const char* name, const std::vector<FakeMount>& mounts) {
    const std::string file = scratch.file(name);
    std::ofstream out(file, std::ios::trunc);
    out << "1 0 0:1 / / rw,relatime shared:1 - ext4 /dev/root rw\n";
    for (const FakeMount& m : mounts) {
        out << m.id << ' ' << m.parent << ' ' << m.major << ':' << m.minor << " / " << escapeMountPoint(m.point)
            << " rw,relatime shared:" << m.id << " - " << m.type << " source rw\n";
    }
    return out ? file : std::string();
}

// What openWallpaper() answered, flattened so a check can print it.
struct OpenAnswer {
    enum class Kind { Opened, Refused, Failed, Shortage, OtherThrow } kind = Kind::OtherThrow;
    std::string text;  // the refusal's reason or the exception's message
    int error = 0;
};

OpenAnswer askOpen(const std::string& path) {
    OpenAnswer out;
    try {
        const WallpaperOpen opened = openWallpaper(path, kComponent);
        if (const auto* file = std::get_if<WallpaperOpen::Opened>(&opened.result)) {
            ::close(file->fd);
            out.kind = OpenAnswer::Kind::Opened;
        } else if (const auto* refused = std::get_if<WallpaperOpen::Refused>(&opened.result)) {
            out.kind = OpenAnswer::Kind::Refused;
            out.text = refused->reason;
        } else {
            out.kind = OpenAnswer::Kind::Failed;
            out.error = std::get<WallpaperOpen::Failed>(opened.result).error;
        }
    } catch (const ResourceExhausted& e) {
        out.kind = OpenAnswer::Kind::Shortage;
        out.text = e.what();
    } catch (const std::exception& e) {
        out.text = e.what();
    }
    return out;
}

std::string describe(const OpenAnswer& a) {
    switch (a.kind) {
        case OpenAnswer::Kind::Opened: return "opened";
        case OpenAnswer::Kind::Refused: return "refused: " + a.text;
        case OpenAnswer::Kind::Failed: return std::string("failed: ") + std::strerror(a.error);
        case OpenAnswer::Kind::Shortage: return "shortage: " + a.text;
        case OpenAnswer::Kind::OtherThrow: return "threw: " + a.text;
    }
    return "?";
}

// refusedFilesystemOf(), with a throw turned into text so one type's failure does not end the list.
std::string refusedTypeFor(const std::string& path) {
    try {
        return refusedFilesystemOf(path, kComponent);
    } catch (const std::exception& e) {
        return std::string("threw: ") + e.what();
    }
}

// The directory the fake mounts go under, with every symlink in the temp path resolved: the
// function resolves the path it is given, so a mount point written with an unresolved prefix would
// never be crossed and every refusal below would read as "not refused".
std::string canonicalDir(const ScratchDir& scratch, const char* name) {
    std::error_code ec;
    const std::filesystem::path dir = scratch.file(name);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path resolved = std::filesystem::canonical(dir, ec);
    return ec ? std::string() : resolved.string();
}

// GOAL: S13.194 -- a wallpaper on a kernel NETWORK filesystem is refused, before the path is
//       looked up at all, with a reason that says it is a network filesystem and names the type;
//       and a file on an ordinary kernel local filesystem is never refused, whatever it is.
// WHY-THIS-SHAPE: the mount table is substituted, not mounted, and the path under each network
//       mount does not exist -- so a refusal cannot come from the open failing, only from the table.
//       lstat is recorded to show nothing under the mount was asked about: a server that has gone
//       answers an lstat by waiting, which is the freeze the refusal exists to avoid. The local
//       half uses the same shape with files that DO exist, so "never refused" is read off opens
//       that went ahead rather than off a function that refuses nothing.
// CONTROL: in renderer/wallpaper_file.cpp, (a) refusedType()'s network lookup made unreachable
//       (`found != std::end(kNetworkTypes)` -> `false`) -> 42 FAILs, "a file on a network
//       filesystem (nfs) is refused by the mount table ... (saw '', failed: No such file or
//       directory)" and "nothing on the network mount (nfs) was looked up" for each of the 17
//       types, plus the covered-mount, space-named and symlink readings below that need a network
//       mount refused, and nothing in the local half; (b)
//       "btrfs" added to kNetworkTypes -> exactly "FAIL: a file on a kernel local filesystem
//       (btrfs) is opened, never refused", printing the network refusal; (c) in
//       refusedFilesystemOf(), the lstat moved ahead of the refusedTypeAt() check -> the 17
//       "nothing ... was looked up" FAILs and the symlink one. All observed 2026-09-21; reverted.
// COVERAGE-ASSERT: lstat is required to have been called on the local path, so the "nothing
//       under the mount was looked up" half is not read off a hook that sees nothing.
void checkNetworkAndLocalKernelFilesystems(const ScratchDir& scratch) {
    const std::string base = canonicalDir(scratch, "kernel-mounts");
    if (base.empty()) {
        fail("wallpaper_file_probe: the directory for the fake mounts could not be made or resolved");
        return;
    }
    const std::string mountPoint = base + "/share";
    const std::string file = mountPoint + "/nobody-can-answer.mp4";

    const char* const kNetwork[] = {"nfs",     "nfs4",  "cifs",   "smb3",      "smbfs", "ncpfs",
                                    "afs",     "ceph",  "9p",     "coda",      "lustre", "gpfs",
                                    "beegfs",  "orangefs", "pvfs2", "glusterfs", "davfs"};
    for (const char* type : kNetwork) {
        const std::string table =
            writeMountTable(scratch, "mountinfo-network", {{2, 1, mountPoint, type}});
        if (table.empty()) {
            fail("wallpaper_file_probe: a fake mount table could not be written");
            return;
        }
        const Injected injected(Injection{table});
        const std::string refused = refusedTypeFor(file);
        const OpenAnswer answer = askOpen(file);
        const std::vector<std::string> looked = injected.lstats();
        bool lookedInside = false;
        for (const std::string& p : looked) lookedInside |= p.rfind(mountPoint, 0) == 0;
        check(refused == type && answer.kind == OpenAnswer::Kind::Refused &&
                  answer.text.find("network filesystem (" + std::string(type) + ")") != std::string::npos,
              std::string("a file on a network filesystem (") + type +
                  ") is refused by the mount table, with a reason naming the type (saw '" + refused + "', " +
                  describe(answer) + ")");
        check(!lookedInside, std::string("nothing on the network mount (") + type + ") was looked up before the refusal");
    }

    const char* const kLocal[] = {"ext4", "btrfs", "xfs", "f2fs", "vfat", "exfat", "ntfs3",
                                  "tmpfs", "overlay", "zfs", "bcachefs", "iso9660", "ext3"};
    const std::string localFile = mountPoint + "/local.png";
    {
        std::error_code ec;
        std::filesystem::create_directories(mountPoint, ec);
        std::ofstream(localFile) << "bytes";
    }
    bool sawLookup = false;
    for (const char* type : kLocal) {
        const std::string table = writeMountTable(scratch, "mountinfo-local", {{2, 1, mountPoint, type}});
        const Injected injected(Injection{table});
        const OpenAnswer answer = askOpen(localFile);
        check(refusedTypeFor(localFile).empty() && answer.kind == OpenAnswer::Kind::Opened,
              std::string("a file on a kernel local filesystem (") + type + ") is opened, never refused (" +
                  describe(answer) + ")");
        for (const std::string& p : injected.lstats()) sawLookup |= p == localFile;
    }
    check(sawLookup, "the lstat hook saw the local file looked up, so its silence under a network mount means "
                     "something");
}

// GOAL: S13.194's FUSE rule -- a FUSE filesystem is refused unless its type is on the
//       allow-list of local ones (fuseblk, NTFS, exFAT, btrfs drivers, gocryptfs, encfs, cryfs,
//       mergerfs, bindfs); every other FUSE type, known network tools, xdg-document-portal's
//       fuse.portal and names never seen before alike, is refused with a reason saying it is FUSE;
//       and a subtype that is not plainly a name is shown as "fuse" alone.
// WHY-THIS-SHAPE: the allow-list is the contract, so it is written out here rather than read from
//       the code; an entry dropped from production refuses a user's encrypted home and must go red.
//       The refused list includes a made-up subtype on purpose: the first version allowed unknown
//       subtypes, and a list of only familiar network tools would pass that version.
// CONTROL: in renderer/wallpaper_file.cpp, (a) refusedType()'s allow-list lookup deleted -> "FAIL:
//       a file on an allow-listed FUSE filesystem (fuseblk) is opened" and the other twelve; (b) the
//       final `return plain ? ...` replaced by `return std::nullopt;` (unknown FUSE allowed) ->
//       "FAIL: a file on a FUSE filesystem that is not on the allow-list (fuse.sshfs) is refused ...
//       (opened)", the other six, and the non-plain subtype (saw ''); (c) `plain ? std::string(type) : ...` -> `std::string(type)` -> "FAIL: a
//       FUSE subtype that is not plainly a name is shown as 'fuse' alone (saw 'fuse.a$b;c')", alone.
//       All observed 2026-09-21; reverted.
void checkFuseAllowList(const ScratchDir& scratch) {
    const std::string base = canonicalDir(scratch, "fuse-mounts");
    if (base.empty()) {
        fail("wallpaper_file_probe: the directory for the fake FUSE mounts could not be made or resolved");
        return;
    }
    const std::string mountPoint = base + "/drive";
    const std::string file = mountPoint + "/wallpaper.png";
    {
        std::error_code ec;
        std::filesystem::create_directories(mountPoint, ec);
        std::ofstream(file) << "bytes";
    }

    const char* const kAllowed[] = {"fuseblk",         "fuse.ntfs-3g",   "fuse.lowntfs-3g", "fuse.ntfs",
                                    "fuse.exfat",      "fuse.exfat-fuse", "fuse.btrfs-fuse", "fuse.btrfs",
                                    "fuse.gocryptfs",  "fuse.encfs",     "fuse.cryfs",      "fuse.mergerfs",
                                    "fuse.bindfs"};
    for (const char* type : kAllowed) {
        const std::string table = writeMountTable(scratch, "mountinfo-fuse", {{2, 1, mountPoint, type}});
        const Injected injected(Injection{table});
        const OpenAnswer answer = askOpen(file);
        check(refusedTypeFor(file).empty() && answer.kind == OpenAnswer::Kind::Opened,
              std::string("a file on an allow-listed FUSE filesystem (") + type + ") is opened (" + describe(answer) +
                  ")");
    }

    const char* const kRefused[] = {"fuse.sshfs", "fuse.rclone", "fuse.gvfsd-fuse", "fuse.kio-fuse",
                                    "fuse.portal", "fuse", "fuse.never-heard-of-it"};
    for (const char* type : kRefused) {
        const std::string table = writeMountTable(scratch, "mountinfo-fuse", {{2, 1, mountPoint, type}});
        const Injected injected(Injection{table});
        const OpenAnswer answer = askOpen(file);
        check(refusedTypeFor(file) == type && answer.kind == OpenAnswer::Kind::Refused &&
                  answer.text.find("FUSE filesystem (" + std::string(type) + ")") != std::string::npos,
              std::string("a file on a FUSE filesystem that is not on the allow-list (") + type +
                  ") is refused, with a reason saying it is FUSE (" + describe(answer) + ")");
    }

    {
        const std::string table = writeMountTable(scratch, "mountinfo-fuse", {{2, 1, mountPoint, "fuse.a$b;c"}});
        const Injected injected(Injection{table});
        const std::string shown = refusedTypeFor(file);
        check(shown == "fuse", "a FUSE subtype that is not plainly a name is shown as 'fuse' alone (saw '" + shown + "')");
    }
}

// GOAL: S13.194, found as the kernel finds it -- the mount a path is on is the one a lookup
//       would reach, so a mount another has since covered is never matched: not one stacked
//       underneath on the same directory, and not one on a directory beneath a mount made later
//       over its parent. And a mount stacked ON TOP of a local one is matched, whatever order the
//       lines come in. False in the safe direction for a matcher that takes the longest prefix
//       (refuses a local file sitting over a dead NFS mount), and in the unsafe one for a matcher
//       that takes the first line (misses NFS mounted over a local directory).
// WHY-THIS-SHAPE: three tables, each pairing a network mount with a local one so the only thing
//       that decides the answer is which of the two a lookup reaches. Each is run with the covering
//       mount removed first, so the network mount is shown to be refused on its own.
// CONTROL: in renderer/wallpaper_file.cpp, refusedTypeAt() (a) made a longest-prefix matcher (the
//       last mount whose point is the longest prefix of the path) -> "FAIL: a network mount on a
//       directory beneath a later mount of its parent is not matched (saw 'cifs')" and "FAIL: a
//       network mount stacked on a local one is matched, though its line comes first (saw '')";
//       (b) its climb given a `return;` after the first move, one step per directory -> "FAIL: a
//       network mount covered by a local one stacked on the same directory is not matched (saw
//       'nfs4')" and the stacked-on-local one. Observed 2026-09-21; reverted. Deleting the climb's
//       parent test instead is not a usable control: two mounts on one point then swap for ever
//       and the probe hangs.
void checkCoveredMountsAreNotMatched(const ScratchDir& scratch) {
    const std::string base = canonicalDir(scratch, "stacked-mounts");
    if (base.empty()) {
        fail("wallpaper_file_probe: the directory for the stacked mounts could not be made or resolved");
        return;
    }
    const std::string point = base + "/pictures";
    const std::string deeper = base + "/pictures/nas";
    const std::string fileAtPoint = point + "/a.png";
    const std::string fileDeeper = deeper + "/b.png";
    {
        std::error_code ec;
        std::filesystem::create_directories(deeper, ec);
        std::ofstream(fileAtPoint) << "bytes";
        std::ofstream(fileDeeper) << "bytes";
    }
    struct Case {
        const char* what;
        std::vector<FakeMount> alone;     // the network mount by itself: must be refused
        std::vector<FakeMount> together;  // with the other mount beside it
        std::string path;
        bool refusedTogether;
    };
    const Case cases[] = {
        {"a network mount covered by a local one stacked on the same directory is not matched",
         {{2, 1, point, "nfs4"}}, {{2, 1, point, "nfs4"}, {3, 2, point, "ext4"}}, fileAtPoint, false},
        {"a network mount on a directory beneath a later mount of its parent is not matched",
         {{2, 1, deeper, "cifs"}}, {{2, 1, deeper, "cifs"}, {3, 1, point, "ext4"}}, fileDeeper, false},
        {"a network mount stacked on a local one is matched, though its line comes first",
         {{2, 1, point, "nfs4"}}, {{2, 3, point, "nfs4"}, {3, 1, point, "ext4"}}, fileAtPoint, true},
    };
    for (const Case& c : cases) {
        {
            const Injected injected(Injection{writeMountTable(scratch, "mountinfo-stack", c.alone)});
            check(!refusedTypeFor(c.path).empty(), std::string(c.what) + ": on its own the network mount is refused");
        }
        const Injected injected(Injection{writeMountTable(scratch, "mountinfo-stack", c.together)});
        const std::string refused = refusedTypeFor(c.path);
        check(refused.empty() != c.refusedTogether, std::string(c.what) + " (saw '" + refused + "')");
    }
}

// GOAL: S13.194's check after the open -- a file whose DEVICE belongs to a refused
//       filesystem is refused even when no mount on its path says so (an automount the lookup
//       itself triggered, or a bind mount of a network share), and the descriptor is given back.
// WHY-THIS-SHAPE: the fake table places the refused type on the real file's own device number at a
//       mount point nowhere near its path, so the path walk passes it and only the by-device check
//       can refuse it. The walk's own answer is asserted to be empty, which is what makes the
//       refusal attributable to the second check.
// CONTROL: in renderer/wallpaper_file.cpp, refusedFilesystemOfOpen()'s device loop made to return
//       {} -> "FAIL: a file on a device the mount table lists as a network filesystem is refused
//       after it is opened", printing "opened". Observed 2026-09-21; reverted.
void checkRefusedByDeviceAfterOpen(const ScratchDir& scratch) {
    const std::string file = scratch.file("on-a-bound-share.png");
    std::ofstream(file) << "bytes";
    struct stat info {};
    if (::stat(file.c_str(), &info) != 0) {
        fail("wallpaper_file_probe: the file for the by-device check could not be stat'ed");
        return;
    }
    const std::string table = writeMountTable(
        scratch, "mountinfo-device", {{2, 1, "/nowhere/near/it", "nfs4", major(info.st_dev), minor(info.st_dev)}});
    const Injected injected(Injection{table});
    const int fdsBefore = openDescriptorCount();
    const std::string walked = refusedTypeFor(file);
    const OpenAnswer answer = askOpen(file);
    check(walked.empty(), "the path walk finds nothing on the file's path, so the by-device check is what is graded");
    check(answer.kind == OpenAnswer::Kind::Refused && answer.text.find("nfs4") != std::string::npos,
          "a file on a device the mount table lists as a network filesystem is refused after it is opened (" +
              describe(answer) + ")");
    check(openDescriptorCount() <= fdsBefore, "and the refusal gives its descriptor back");
}

// GOAL: S13.194's fallback, and S13.193 at the table -- with the mount table unreadable for a reason that is not a
//       shortage, the opened file's filesystem type is asked instead, and EVERY FUSE mount is then
//       refused (the type carries no subtype to allow), a kernel network one too, and a local one
//       opened. With the table unreadable for want of descriptors or memory, the answer is
//       ResourceExhausted -- the daemon's shortage -- and not a refusal of the file or an open
//       that went ahead unchecked.
// WHY-THIS-SHAPE: the unreadable table is EACCES, which is what a sandbox hiding /proc gives; the
//       shortage cases are the three errnos decision 4 names. FUSE is graded through fuseblk's
//       magic too, since it is allow-listed by name and must still be refused when no name is
//       available -- the case that distinguishes "refuse every FUSE" from "refuse unknown FUSE".
// CONTROL: in renderer/wallpaper_file.cpp, (a) refusedMagic()'s `case FUSE_SUPER_MAGIC:` removed ->
//       exactly "FAIL: with the mount table unreadable, a file on any fuse mount is refused by its
//       filesystem type (opened)"; (b) the `if (isShortage(err))` in readMountTable() made
//       `if (false)` -> "FAIL: with the mount table unreadable (EMFILE), the answer is the daemon's
//       shortage, not the file's (opened)", and the same for ENFILE and ENOMEM. Observed
//       2026-09-21; reverted.
void checkUnreadableMountTable(const ScratchDir& scratch) {
    const std::string file = scratch.file("fallback.png");
    std::ofstream(file) << "bytes";

    {
        Injection injection;
        injection.mountTableError = EACCES;
        const Injected injected(std::move(injection));
        check(refusedTypeFor(file).empty(), "with the mount table unreadable, the path walk answers nothing");
        const OpenAnswer local = askOpen(file);
        check(local.kind == OpenAnswer::Kind::Opened,
              "with the mount table unreadable, a file on a local filesystem still opens (" + describe(local) + ")");
    }
    struct Magic {
        long type;
        const char* shown;
    };
    const Magic magics[] = {{FUSE_SUPER_MAGIC, "fuse"}, {NFS_SUPER_MAGIC, "nfs"}, {CIFS_SUPER_MAGIC, "cifs"}};
    for (const Magic& m : magics) {
        Injection injection;
        injection.mountTableError = EACCES;
        injection.statfsType = m.type;
        const Injected injected(std::move(injection));
        const OpenAnswer answer = askOpen(file);
        check(answer.kind == OpenAnswer::Kind::Refused && answer.text.find(std::string("(") + m.shown + ")") != std::string::npos,
              std::string("with the mount table unreadable, a file on any ") + m.shown +
                  " mount is refused by its filesystem type (" + describe(answer) + ")");
    }
    const int shortages[] = {EMFILE, ENFILE, ENOMEM};
    for (const int err : shortages) {
        Injection injection;
        injection.mountTableError = err;
        const Injected injected(std::move(injection));
        const OpenAnswer answer = askOpen(file);
        bool walkThrew = false;
        try {
            (void)refusedFilesystemOf(file, kComponent);
        } catch (const ResourceExhausted&) {
            walkThrew = true;
        } catch (const std::exception&) {
        }
        check(answer.kind == OpenAnswer::Kind::Shortage && walkThrew,
              std::string("with the mount table unreadable (") + strerrorname_np(err) +
                  "), the answer is the daemon's shortage, not the file's (" + describe(answer) + ")");
    }
}

// GOAL: S13.193 -- a wallpaper whose own open fails because the process or the system is
//       out of descriptors or kernel memory (EMFILE, ENFILE, ENOMEM) is answered with
//       ResourceExhausted -- the daemon's shortage, which the manager retries and never counts
//       against the file -- from openWallpaper() and from the renderers' openWallpaperFile() alike;
//       and an ordinary refusal of access is still the file's own answer.
// WHY-THIS-SHAPE: the open is failed by path, AFTER the mount table has been read successfully, so
//       what is graded is the open's own classification and not the table's (that is the check
//       above). EACCES is the contrast: without it, a function that called every failure a
//       shortage would pass.
// CONTROL: in renderer/wallpaper_file.cpp, the `if (isShortage(failed)) throwShortage(...)` after
//       the open in openWallpaper() deleted -> six FAILs, "a wallpaper open that fails (EMFILE) is
//       the daemon's shortage, naming the file (failed: Too many open files)" and "openWallpaperFile()
//       throws the shortage too (EMFILE)", and the same pair for ENFILE and ENOMEM. Observed
//       2026-09-21; reverted.
void checkShortageOnTheFilesOwnOpen(const ScratchDir& scratch) {
    const std::string file = scratch.file("short.png");
    std::ofstream(file) << "bytes";
    const int shortages[] = {EMFILE, ENFILE, ENOMEM};
    for (const int err : shortages) {
        Injection injection;
        injection.failPath = file;
        injection.failPathError = err;
        const Injected injected(std::move(injection));
        const OpenAnswer answer = askOpen(file);
        check(answer.kind == OpenAnswer::Kind::Shortage && answer.text.find(file) != std::string::npos,
              std::string("a wallpaper open that fails (") + strerrorname_np(err) +
                  ") is the daemon's shortage, naming the file (" + describe(answer) + ")");
        bool rendererSawShortage = false;
        try {
            const int fd = openWallpaperFile(file, kComponent);
            if (fd >= 0) ::close(fd);
        } catch (const ResourceExhausted&) {
            rendererSawShortage = true;
        } catch (const std::exception&) {
        }
        check(rendererSawShortage, std::string("openWallpaperFile() throws the shortage too (") + strerrorname_np(err) +
                                       "), rather than returning -1 for the renderer to call the file's fault");
    }
    Injection injection;
    injection.failPath = file;
    injection.failPathError = EACCES;
    const Injected injected(std::move(injection));
    const OpenAnswer denied = askOpen(file);
    check(denied.kind == OpenAnswer::Kind::Failed && denied.error == EACCES,
          "a refusal of access is still returned as the open's own answer, not as a shortage (" + describe(denied) + ")");
}

// GOAL: S13.194, the path as written -- a mount point whose name holds a space (which the
//       mount table escapes as \040) is still matched, and a symlink on the way is resolved
//       against the table before anything behind it is looked up, so a local link pointing into a
//       network share is refused without the share being asked.
// WHY-THIS-SHAPE: both are ways a naive prefix test on the path string goes wrong -- the escaped
//       name never equals the path, and the link's own path is local. The lstat record shows the
//       link itself was read and its target never touched.
// CONTROL: in renderer/wallpaper_file.cpp, (a) unescapeMountField() made to return its input
//       unchanged -> exactly "FAIL: a network mount whose name holds a space is matched (saw '')";
//       (b) in refusedFilesystemOf(), the lstat moved ahead of the refusedTypeAt() check -> "FAIL:
//       nothing behind a symlink into a network share is looked up before the refusal", beside the
//       17 network-type ones above. Observed 2026-09-21; reverted.
void checkEscapedNamesAndSymlinks(const ScratchDir& scratch) {
    const std::string base = canonicalDir(scratch, "odd-paths");
    if (base.empty()) {
        fail("wallpaper_file_probe: the directory for the escaped and linked paths could not be made or resolved");
        return;
    }
    const std::string spaced = base + "/my share";
    {
        const Injected injected(Injection{writeMountTable(scratch, "mountinfo-odd", {{2, 1, spaced, "smb3"}})});
        const std::string refused = refusedTypeFor(spaced + "/w.png");
        check(refused == "smb3", "a network mount whose name holds a space is matched (saw '" + refused + "')");
    }
    const std::string share = base + "/share";
    const std::string link = base + "/local-link.png";
    std::error_code ec;
    std::filesystem::create_symlink(share + "/behind.png", link, ec);
    if (ec) {
        fail("wallpaper_file_probe: the symlink into the fake share could not be made");
        return;
    }
    const Injected injected(Injection{writeMountTable(scratch, "mountinfo-odd", {{2, 1, share, "nfs"}})});
    const std::string refused = refusedTypeFor(link);
    bool readLink = false;
    bool touchedShare = false;
    for (const std::string& p : injected.lstats()) {
        readLink |= p == link;
        touchedShare |= p.rfind(share, 0) == 0;
    }
    check(refused == "nfs", "a local symlink pointing into a network share is refused (saw '" + refused + "')");
    check(readLink, "the symlink itself was looked up, so the silence about its target means something");
    check(!touchedShare, "nothing behind a symlink into a network share is looked up before the refusal");
}

}  // namespace

int main() {
    ScratchDir scratch("wallpaperx-file");
    if (!scratch.ok()) {
        // 77, not a failure: a host that cannot make a temporary directory has not told us
        // anything about openWallpaperFile, and an inconclusive run must not read as green.
        std::fprintf(stderr,
                     "wallpaper_file_probe: SKIPPED -- no scratch directory could be made, so no "
                     "FIFO, directory or symlink could be offered to openWallpaperFile\n");
        return 77;
    }

    checkARegularFileOpensAndReadsBack(scratch);
    checkIrregularPathsAreRefusedAsContent(scratch);
    checkAnUnwritableFifoDoesNotParkTheCaller(scratch);
    checkNetworkAndLocalKernelFilesystems(scratch);
    checkFuseAllowList(scratch);
    checkCoveredMountsAreNotMatched(scratch);
    checkRefusedByDeviceAfterOpen(scratch);
    checkUnreadableMountTable(scratch);
    checkShortageOnTheFilesOwnOpen(scratch);
    checkEscapedNamesAndSymlinks(scratch);

    if (reportFailures("wallpaper_file_probe") != 0) return 1;
    std::printf("wallpaper_file_probe: all checks passed\n");
    return 0;
}
