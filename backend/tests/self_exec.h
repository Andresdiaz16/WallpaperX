// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Runs this test binary again, in a mode its main() recognises, and reaps it -- for a check whose
// subject has to die: a process that owns a manager, a render thread or a Vulkan device is not
// forked (the child would inherit threads it cannot use), it is started afresh.
//
// The child's stdout and stderr are inherited, so what it prints lands in the ctest log beside the
// parent's verdict. It may report readiness by writing one byte to descriptor kSelfExecReadyFd.

#include "support/forked_child.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace wallpaperx::testing {

// Where a child finds the write end of the readiness pipe.
constexpr int kSelfExecReadyFd = 3;

struct SelfExecResult {
    bool started = false;  // forked and exec'd, as far as this side can tell
    bool reaped = false;   // exited or died inside the limit
    bool ready = false;    // wrote its readiness byte before it ended
    int status = 0;
    std::string fault;     // why the run is not a verdict, when it is not

    bool diedOf(int sig) const { return reaped && WIFSIGNALED(status) && WTERMSIG(status) == sig; }
    bool exitedWith(int code) const { return reaped && WIFEXITED(status) && WEXITSTATUS(status) == code; }
    std::string how() const {
        if (!reaped) return "never ended";
        if (WIFSIGNALED(status)) return "died of signal " + std::to_string(WTERMSIG(status));
        return "exited " + std::to_string(WEXITSTATUS(status));
    }
};

// `killOnReady` sends SIGKILL once the child reports ready, which is how "died with no handler
// running" is staged without a sleep. A child still running at `limit` is killed and reported as
// not reaped; a preamble failure is reported in `fault`, never as the child's own outcome.
inline SelfExecResult runSelf(const std::vector<std::string>& args, std::chrono::seconds limit,
                              bool killOnReady = false) {
    SelfExecResult out;
    int ready[2] = {-1, -1};
    if (::pipe2(ready, O_CLOEXEC) != 0) {
        out.fault = "the readiness pipe could not be created";
        return out;
    }
    std::vector<std::string> owned;
    owned.push_back("/proc/self/exe");
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : owned) argv.push_back(a.data());
    argv.push_back(nullptr);

    const pid_t parentBeforeFork = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(ready[0]);
        ::close(ready[1]);
        out.fault = "fork failed";
        return out;
    }
    if (pid == 0) {
        // dup2 clears close-on-exec on the copy, which is what carries it into the new image.
        if (::dup2(ready[1], kSelfExecReadyFd) < 0) _exit(kChildDup2FailedExit);
        execChildOrDie(parentBeforeFork, "/proc/self/exe", argv.data(), nullptr);
    }
    ::close(ready[1]);
    out.started = true;

    const auto deadline = std::chrono::steady_clock::now() + limit;
    bool killed = false;
    for (;;) {
        if (!out.ready) {
            struct pollfd waitOn {
                ready[0], POLLIN, 0
            };
            if (::poll(&waitOn, 1, 0) > 0 && (waitOn.revents & POLLIN) != 0) {
                char byte = 0;
                if (::read(ready[0], &byte, 1) == 1) {
                    out.ready = true;
                    if (killOnReady && !killed) {
                        ::kill(pid, SIGKILL);
                        killed = true;
                    }
                }
            }
        }
        const pid_t done = ::waitpid(pid, &out.status, WNOHANG);
        if (done == pid) {
            out.reaped = true;
            break;
        }
        if (done < 0 && errno != EINTR) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            int discarded = 0;
            ::waitpid(pid, &discarded, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::close(ready[0]);
    if (out.reaped && WIFEXITED(out.status)) {
        if (const char* fault = forkPreambleFault(WEXITSTATUS(out.status)); fault != nullptr) out.fault = fault;
    }
    return out;
}

// For the child: says it is ready, once.
inline void reportSelfExecReady() {
    const char byte = 'r';
    const ssize_t ignored = ::write(kSelfExecReadyFd, &byte, 1);
    (void)ignored;
}

}  // namespace wallpaperx::testing
