// SPDX-License-Identifier: GPL-2.0-or-later

// The device-loss test library (tests/device_lost_shim.cpp) itself: what its one line does to the
// stream it is written to. Every device-loss test reads the daemon's log through that stream, so a
// shim that overwrites the log or blocks on it would blame the daemon for the shim's own effect.
//
// This binary runs itself again with the shim preloaded and the trigger already present, calls
// vkQueueSubmit once, and looks at what reached the stream the child's stderr was pointed at. The
// fake submit never reaches a driver, so no GPU is needed.

#include "support/forked_child.h"
#include "tests/client/scratch_dir.h"
#include "tests/test_report.h"

#include <vulkan/vulkan.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::ScratchDir;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kShimLine = "device_lost_shim: failing every queue submission from now on\n";
constexpr const char* kEarlier = "earlier line one\nearlier line two, which is longer than the shim's own line so far\n";
constexpr const char* kLater = "a line written after the loss\n";
// The child's own verdict on the fake submit.
constexpr int kChildLost = 0;
constexpr int kChildNotLost = 3;
// Far above the microseconds a non-blocking write takes; a blocked child never gets there.
constexpr auto kPatience = std::chrono::seconds(5);

// The child: stderr is whatever the parent set up, the shim is preloaded and the trigger exists.
// `writeLater` only where the stream cannot stall: a blocking write of the child's own would
// otherwise be graded as the shim blocking.
int runChild(bool writeLater) {
    const VkResult result = vkQueueSubmit(VK_NULL_HANDLE, 0, nullptr, VK_NULL_HANDLE);
    if (writeLater) {
        // Straight to the descriptor, as the daemon's own later lines would reach it.
        const ssize_t ignored = ::write(STDERR_FILENO, kLater, std::strlen(kLater));
        (void)ignored;
    }
    return result == VK_ERROR_DEVICE_LOST ? kChildLost : kChildNotLost;
}

struct ChildRun {
    bool started = false;
    bool ended = false;  // false: still running at kPatience, and killed
    int status = 0;
    bool fired = false;  // the shim created <trigger>.fired, so it took the loss path
};

// Runs this binary as the child with `stderrFd` on its descriptor 2.
ChildRun runShimChild(const fs::path& trigger, int stderrFd, bool writeLater) {
    ChildRun run;
    std::vector<std::string> env;
    std::string preload = WALLPAPERX_DEVICE_LOST_SHIM;
    for (char** e = environ; *e != nullptr; ++e) {
        if (std::strncmp(*e, "LD_PRELOAD=", 11) == 0) {
            // Kept ahead of the shim: a sanitizer runtime the build preloads must come first.
            // CONTROL: by hand 2026-09-25 in a scratch -DWALLPAPERX_SANITIZE=address tree, reverted:
            //       the inherited value dropped -> five "FAIL (harness): ... never took its loss path",
            //       the runtime refusing to start behind the shim; the entry run without the
            //       ENVIRONMENT backend/CMakeLists.txt gives it -> the same five. Green with both.
            if ((*e)[11] != '\0') preload = std::string(*e + 11) + ":" + preload;
        } else if (std::strncmp(*e, "WALLPAPERX_TEST_LOSE_DEVICE=", 28) != 0) {
            env.emplace_back(*e);
        }
    }
    env.push_back("LD_PRELOAD=" + preload);
    env.push_back("WALLPAPERX_TEST_LOSE_DEVICE=" + trigger.string());
    std::vector<char*> envp;
    for (std::string& entry : env) envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::string self = "/proc/self/exe";
    std::string mode = "--shim-child";
    std::string later = writeLater ? "later" : "";
    char* argv[] = {self.data(), mode.data(), later.data(), nullptr};

    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) return run;
    if (pid == 0) {
        if (::dup2(stderrFd, STDERR_FILENO) < 0) _exit(wallpaperx::testing::kChildDup2FailedExit);
        wallpaperx::testing::execChildOrDie(parent, self.c_str(), argv, envp.data());
    }
    run.started = true;
    const auto deadline = Clock::now() + kPatience;
    while (Clock::now() < deadline) {
        const pid_t r = ::waitpid(pid, &run.status, WNOHANG);
        if (r == pid) {
            run.ended = true;
            break;
        }
        if (r < 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!run.ended) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &run.status, 0);
    }
    std::error_code ec;
    run.fired = fs::exists(trigger.string() + ".fired", ec);
    fs::remove(trigger.string() + ".fired", ec);
    return run;
}

// A preamble failure or a shim that never loaded is this harness's, not the shim's line handling.
// CONTROL: by hand 2026-09-24, reverted: the preload path suffixed with "x" -> five "FAIL (harness):
//       ... never took its loss path" lines and "5 fault(s) in this test's harness", no FAIL.
bool childSetUp(const ChildRun& run, const std::string& layout) {
    if (!run.started) {
        harnessFailure(layout + ": fork failed");
        return false;
    }
    if (run.ended && WIFEXITED(run.status)) {
        if (const char* fault = wallpaperx::testing::forkPreambleFault(WEXITSTATUS(run.status))) {
            harnessFailure(layout + ": " + fault);
            return false;
        }
    }
    if (!run.fired) {
        harnessFailure(layout + ": the preloaded shim never took its loss path (no <trigger>.fired), so it did not load "
                                "or the trigger was not seen -- " WALLPAPERX_DEVICE_LOST_SHIM);
        return false;
    }
    return true;
}

bool returnedLoss(const ChildRun& run) { return run.ended && WIFEXITED(run.status) && WEXITSTATUS(run.status) == kChildLost; }

std::string describe(const ChildRun& run) {
    if (!run.ended) return "still blocked after 5 s, killed";
    if (WIFSIGNALED(run.status)) return "died of signal " + std::to_string(WTERMSIG(run.status));
    return "exited " + std::to_string(WEXITSTATUS(run.status));
}

// Fills the stream behind `fd` until the next write would block: MSG_DONTWAIT on a socket, and a
// non-blocking description of the pipe's own otherwise, so the child's descriptor stays blocking.
bool fill(int fd, bool socket) {
    int writer = fd;
    if (!socket) {
        const std::string path = "/proc/self/fd/" + std::to_string(fd);
        writer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (writer < 0) return false;
    }
    bool full = false;
    for (const size_t size : {size_t{4096}, size_t{1}}) {
        const std::string chunk(size, '.');
        for (size_t written = 0; written < (size_t{8} << 20);) {
            const ssize_t n = socket ? ::send(writer, chunk.data(), size, MSG_DONTWAIT | MSG_NOSIGNAL)
                                     : ::write(writer, chunk.data(), size);
            if (n > 0) {
                written += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            full = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
        if (!full) break;
    }
    if (writer != fd) ::close(writer);
    return full;
}

std::string drain(int fd) {
    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == ENOTSOCK) {
            const int flags = ::fcntl(fd, F_GETFL);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            for (ssize_t m; (m = ::read(fd, buf, sizeof(buf))) > 0;) out.append(buf, static_cast<size_t>(m));
        }
        return out;
    }
}

// GOAL: S14.20 -- the shim's line, on a daemon whose stderr is a regular file opened without
//       O_APPEND (a shell's `2>log`), lands after what the process already wrote there and before what it
//       writes next: it destroys no line of the log it is observed through.
// WHY-THIS-SHAPE: the whole file is compared, not searched for the shim line: a line written at
//       offset 0 of a fresh description is still "in the file", over the start of the earlier
//       ones. The earlier text is longer than the shim line, so an overwrite leaves a torn tail
//       rather than an exact match. The later line, written by the child through its own fd 2
//       after the loss, catches the other half: a daemon line landing on top of the shim's.
// CONTROL: the original shim (reopening /proc/self/fd/2 with no O_APPEND), via control_run.sh on
//       backend/tests/device_lost_shim.cpp 2026-09-24, reverted -> this line red, the file
//       starting "device_lost_shim: failing..." then "him's own line so far", the torn tail of the
//       second earlier line; with the two stalled-socket lines below. The fixed shim's line
//       dropped -> this line red with both "reaches a drained" lines.
void checkRegularFile(const fs::path& dir) {
    const std::string layout = "stderr a regular file";
    const fs::path log = dir / "stderr.log";
    const fs::path trigger = dir / "lose-file";
    if (!std::ofstream(trigger)) {
        harnessFailure(layout + ": the trigger file could not be created");
        return;
    }
    const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || ::write(fd, kEarlier, std::strlen(kEarlier)) != static_cast<ssize_t>(std::strlen(kEarlier))) {
        harnessFailure(layout + ": the log file could not be created and written");
        if (fd >= 0) ::close(fd);
        return;
    }
    const ChildRun run = runShimChild(trigger, fd, true);
    ::close(fd);
    if (!childSetUp(run, layout)) return;
    check(returnedLoss(run), (layout + ": the shim returns VK_ERROR_DEVICE_LOST (child " + describe(run) + ")").c_str());
    std::ifstream in(log, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    const std::string expected = std::string(kEarlier) + kShimLine + kLater;
    check(text.str() == expected,
          (layout + ": the shim's line follows the earlier lines and precedes the later one, overwriting none").c_str());
    if (text.str() != expected) std::fprintf(stderr, "  the file holds:\n%s\n  --\n", text.str().c_str());
}

// GOAL: S14.20 -- on a stderr stream nobody is draining -- a stream socket (journald's layout) or
//       a pipe -- the shim returns VK_ERROR_DEVICE_LOST at once rather than blocking in its announcement, so
//       the loss still reaches the daemon; on the same stream with a reader, the line arrives.
// WHY-THIS-SHAPE: the stream is filled until a write is refused before the child starts, so the
//       stall is real, and the child's end stays blocking (a socket filled with MSG_DONTWAIT, a
//       pipe through a /proc description of its own). The child has 5 s; one blocked is killed
//       and graded red, never waited out as a pass. <trigger>.fired proves the child reached the
//       announcement at all. The drained half keeps a shim that drops every line from passing.
// CONTROL: via control_run.sh on backend/tests/device_lost_shim.cpp 2026-09-24, reverted. (a) The
//       original shim (a socket fails the /proc reopen, so it fell back to a blocking write(2)) ->
//       both "stalled stream socket" lines red, child "still blocked after 5 s", with the
//       regular-file line. (b) The fixed shim's pipe arm made a blocking write(2) -> both "stalled
//       pipe" lines red, alone. (c) The fixed shim's line dropped on every layout -> both "reaches
//       a drained ..." lines red, with the regular-file line.
void checkStream(const fs::path& dir, bool socket, bool stalled) {
    const std::string layout = std::string(stalled ? "stalled " : "drained ") + (socket ? "stream socket" : "pipe");
    const fs::path trigger = dir / ("lose-" + std::to_string(socket) + std::to_string(stalled));
    if (!std::ofstream(trigger)) {
        harnessFailure(layout + ": the trigger file could not be created");
        return;
    }
    int ends[2] = {-1, -1};  // [0] read by this process, [1] the child's stderr
    const int made = socket ? ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) : ::pipe2(ends, O_CLOEXEC);
    if (made != 0) {
        harnessFailure(layout + ": the stream could not be created");
        return;
    }
    if (stalled && !fill(ends[1], socket)) {
        harnessFailure(layout + ": the stream could not be filled until a write was refused");
        ::close(ends[0]);
        ::close(ends[1]);
        return;
    }
    const ChildRun run = runShimChild(trigger, ends[1], false);
    ::close(ends[1]);
    const std::string received = stalled ? std::string() : drain(ends[0]);
    ::close(ends[0]);
    if (!childSetUp(run, layout)) return;
    if (stalled) {
        check(run.ended, (layout + ": the shim returns the loss without blocking on the stream").c_str());
        check(returnedLoss(run), (layout + ": the shim returns VK_ERROR_DEVICE_LOST (child " + describe(run) + ")").c_str());
    } else {
        check(returnedLoss(run), (layout + ": the shim returns VK_ERROR_DEVICE_LOST (child " + describe(run) + ")").c_str());
        check(received.find(kShimLine) != std::string::npos,
              (layout + ": the shim's line reaches a drained " + (socket ? "socket" : "pipe")).c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--shim-child") == 0) return runChild(std::strcmp(argv[2], "later") == 0);
    wallpaperx::testing::g_testName = "device_lost_shim_test";
    // A write to a pipe whose reader this process closed must fail, not kill the harness.
    ::signal(SIGPIPE, SIG_IGN);

    ScratchDir dir("devlost_shim");
    if (!dir.ok()) {
        notVerified("every goal in this file", "a scratch directory could not be created");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    checkRegularFile(dir.path());
    for (const bool socket : {true, false}) {
        for (const bool stalled : {true, false}) checkStream(dir.path(), socket, stalled);
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test's harness, not in the shim")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
