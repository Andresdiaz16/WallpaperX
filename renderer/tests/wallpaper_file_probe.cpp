// SPDX-License-Identifier: GPL-2.0-or-later

// S13.169 and S13.171's first half -- openWallpaperFile() is the one door every wallpaper file is
// opened through, and it refuses anything that is not a regular file without ever parking the
// thread that called it.
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

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using wallpaperx::WallpaperContentError;
using wallpaperx::openWallpaperFile;
using wallpaperx::testing::check;
using wallpaperx::testing::fail;
using wallpaperx::testing::reportFailures;
using wallpaperx::testing::ScratchDir;

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
// COVERAGE-ASSERT: kIrregularKinds is a literal, compared against the list's own length, so
//       deleting a case reddens the count instead of quietly shrinking what was graded.
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
    constexpr int kIrregularKinds = 4;
    check(static_cast<int>(sizeof(kinds) / sizeof(kinds[0])) == kIrregularKinds,
          "the list of irregular kinds no longer has the four entries this check is written for");

    const std::string devNull = "/dev/null";
    int graded = 0;
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
        ++graded;
    }
    const int fdsAfter = openDescriptorCount();

    ::close(writer);

    check(graded == kIrregularKinds,
          "every irregular kind ran (" + std::to_string(graded) + " of " + std::to_string(kIrregularKinds) + ")");

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

    if (reportFailures("wallpaper_file_probe") != 0) return 1;
    std::printf("wallpaper_file_probe: all checks passed\n");
    return 0;
}
