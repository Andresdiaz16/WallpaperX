// SPDX-License-Identifier: GPL-2.0-or-later

// The log writer (backend/daemon/log_pump.cpp) against a destination that fails a write: one that
// refuses it, as a full disk does, and one whose reader has gone. Each case runs in a forked child,
// because install() takes descriptors 1 and 2 over and starts a thread that lives as long as the
// process. The parent grades what reached the destination and what the child measured.
//
// The refusal is staged by log_pump_write_fault.cpp, which takes libc's place for write(2) in this
// binary. No GPU, no bus, no daemon.

#include "daemon/log_pump.h"

#include "support/forked_child.h"
#include "tests/client/scratch_dir.h"
#include "tests/test_report.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

// In log_pump_write_fault.cpp.
bool wallpaperxTestWatchWrites(int fd);
void wallpaperxTestRefuseWrites(bool refuse);
unsigned long long wallpaperxTestWriteAttempts();

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::ScratchDir;
using Clock = std::chrono::steady_clock;

// The child could not route its output or start the log writer. Distinct from forked_child.h's codes.
constexpr int kSetupFailedExit = 118;
// Past this a child is killed. Every flush below is bounded, so only a write that blocks gets here.
constexpr auto kChildTimeout = std::chrono::seconds(20);
// A flush returns as soon as the writer is idle, which a file or an abandoned destination reaches
// at once; this is only the ceiling.
constexpr auto kFlush = std::chrono::seconds(5);
// The refusing window of CpuWhileRefused: a line every 50 ms for 1 s.
constexpr int kRefusedLines = 20;
constexpr auto kLineGap = std::chrono::milliseconds(50);

enum class Case {
    RefusedOnce,      // one line refused, then the destination accepts again
    RefusedThrice,    // three, each flushed on its own, so each dropped-lines notice is refused too
    CpuWhileRefused,  // a line every 50 ms for 1 s, every one refused
    ReaderGone,       // a pipe whose reader closed after the writer opened it
};

const char* caseName(Case which) {
    switch (which) {
        case Case::RefusedOnce: return "one write refused";
        case Case::RefusedThrice: return "three writes refused";
        case Case::CpuWhileRefused: return "1 s of refused writes";
        case Case::ReaderGone: return "a reader that has gone";
    }
    return "an unnamed case";
}

// What the child measured, sent back through a pipe once it is done.
struct Measured {
    unsigned long long attempts = 0;  // writes tried on the destination while it refused (or, ReaderGone, at all)
    long long cpuNs = 0;              // CpuWhileRefused: the process's CPU time over the refusing window
};

void writeLine(const char* format, int n) {
    std::printf(format, n);
    std::fflush(stdout);
}

long long cpuNow() {
    timespec t{};
    ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return static_cast<long long>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

[[noreturn]] void runChildBody(Case which, const std::filesystem::path& file, int resultFd) {
    namespace logpump = wallpaperx::logpump;
    Measured measured;
    if (which == Case::ReaderGone) {
        // As the daemon's main() does, before the log writer starts.
        std::signal(SIGPIPE, SIG_IGN);
        int ends[2];
        if (::pipe2(ends, O_CLOEXEC) != 0 || ::dup2(ends[1], STDOUT_FILENO) < 0 || ::dup2(ends[1], STDERR_FILENO) < 0 ||
            !wallpaperxTestWatchWrites(ends[1])) {
            _exit(kSetupFailedExit);
        }
        ::close(ends[1]);
        if (!logpump::install()) _exit(kSetupFailedExit);
        // Only now: with a reader present the writer reopens the pipe non-blocking, the layout a
        // pipe that later breaks has.
        ::close(ends[0]);
        for (int i = 0; i < 5; ++i) {
            writeLine("line %d\n", i);
            logpump::flush(kFlush);
        }
        measured.attempts = wallpaperxTestWriteAttempts();
    } else {
        const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0 || !wallpaperxTestWatchWrites(fd)) {
            _exit(kSetupFailedExit);
        }
        ::close(fd);
        if (!logpump::install()) _exit(kSetupFailedExit);
        writeLine("before\n", 0);
        logpump::flush(kFlush);
        const unsigned long long before = wallpaperxTestWriteAttempts();
        wallpaperxTestRefuseWrites(true);
        if (which == Case::CpuWhileRefused) {
            const long long start = cpuNow();
            for (int i = 0; i < kRefusedLines; ++i) {
                writeLine("refused %d\n", i);
                std::this_thread::sleep_for(kLineGap);
            }
            logpump::flush(kFlush);
            measured.cpuNs = cpuNow() - start;
        } else {
            for (int i = 0; i < (which == Case::RefusedOnce ? 1 : 3); ++i) {
                writeLine("refused %d\n", i);
                logpump::flush(kFlush);
            }
        }
        measured.attempts = wallpaperxTestWriteAttempts() - before;
        wallpaperxTestRefuseWrites(false);
        writeLine("after\n", 0);
        logpump::flush(kFlush);
    }
    const ssize_t sent = ::write(resultFd, &measured, sizeof(measured));
    _exit(sent == static_cast<ssize_t>(sizeof(measured)) ? 0 : kSetupFailedExit);
}

struct Outcome {
    bool ok = false;  // false: already reported, nothing to grade
    Measured measured;
    std::vector<std::string> lines;  // what reached the destination file
};

bool reap(pid_t pid, int& status) {
    for (const auto deadline = Clock::now() + kChildTimeout;;) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) return true;
        if (done < 0 && errno != EINTR) return false;
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

Outcome runCase(Case which) {
    Outcome out;
    const std::string name = caseName(which);
    ScratchDir dir("logpump");
    if (!dir.ok()) {
        notVerified("S14.38-S14.40", name + ": no scratch directory could be created");
        return out;
    }
    const std::filesystem::path file = dir.path() / "destination";
    int result[2];
    if (::pipe2(result, O_CLOEXEC) != 0) {
        harnessFailure(name + ": could not create the result pipe");
        return out;
    }
    std::fflush(nullptr);
    const pid_t parentBeforeFork = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(result[0]);
        ::close(result[1]);
        harnessFailure(name + ": fork failed");
        return out;
    }
    if (pid == 0) {
        ::close(result[0]);
        if (!wallpaperx::testing::armAgainstParentDeath(parentBeforeFork)) _exit(wallpaperx::testing::kChildArmFailedExit);
        runChildBody(which, file, result[1]);
    }
    ::close(result[1]);
    int status = 0;
    if (!reap(pid, status)) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        ::close(result[0]);
        check(false, (name + ": the child finishes within 20 s (every flush is bounded, so only a write that "
                             "blocks holds it)")
                         .c_str());
        return out;
    }
    const ssize_t got = ::read(result[0], &out.measured, sizeof(out.measured));
    ::close(result[0]);
    if (WIFEXITED(status)) {
        if (const char* fault = wallpaperx::testing::forkPreambleFault(WEXITSTATUS(status)); fault != nullptr) {
            harnessFailure(name + ": " + fault);
            return out;
        }
        if (WEXITSTATUS(status) == kSetupFailedExit) {
            harnessFailure(name + ": the child could not route its output, start the log writer or report back");
            return out;
        }
    }
    const bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0 && got == static_cast<ssize_t>(sizeof(out.measured));
    check(clean, (name + ": the child exits 0 having reported what it measured").c_str());
    if (!clean) {
        std::fprintf(stderr, "  %s: %s %d\n", name.c_str(), WIFEXITED(status) ? "exit" : "signal",
                     WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        return out;
    }
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) out.lines.push_back(line);
    out.ok = true;
    return out;
}

std::string notice(int count) {
    char text[128];
    std::snprintf(text, sizeof(text), "wallpaperx-daemon: %d log line(s) dropped while output was blocked", count);
    return text;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) out += (out.empty() ? "" : " | ") + line;
    return out;
}

bool expectLines(const Outcome& outcome, const std::vector<std::string>& expected, const std::string& what) {
    const bool held = outcome.lines == expected;
    check(held, what.c_str());
    if (!held) {
        std::fprintf(stderr, "  expected: %s\n  seen:     %s\n", joined(expected).c_str(), joined(outcome.lines).c_str());
    }
    return held;
}

// GOAL: S14.38 -- a destination that refuses a write (ENOSPC: a full disk) is kept: once it accepts
//       again, a later line arrives, preceded by one notice counting every refused line, the
//       refused notices' own lines included. False if one refused write silences the destination
//       for good, or a notice that was itself refused shrinks the count.
// WHY-THIS-SHAPE: a regular file, the one destination that polls writable while its disk is full,
//       so a refusal cannot be mistaken for "would block". Three refusals each flushed alone, not
//       one: a notice first becomes part of a refused chunk on the second, and only a third shows
//       its count being carried (with notices counted as 1 line, three read as 1, two as 2).
// CONTROL: in backend/daemon/log_pump.cpp via control_run.sh 2026-09-27, reverted. (a) put()'s
//       default errno case returning kGone -> the notice-then-"after" line of all three refusing
//       cases, and the two "written to the refusing destination" lines (an abandoned destination
//       is not tried again). (b) `dropped += linesIn(start, chunk);` deleted -> the three
//       notice-then-"after" lines alone. (c) linesIn counting every line as 1 -> the "three writes
//       refused" and "1 s of refused writes" notice lines alone; one refusal stays green.
// COVERAGE-ASSERT: the refused lines must have been tried against the refusing destination (at
//       least one attempt per line), or the case graded a writer that never met a refusal.
void checkRefusedWrites() {
    for (const Case which : {Case::RefusedOnce, Case::RefusedThrice}) {
        const int refused = which == Case::RefusedOnce ? 1 : 3;
        const Outcome outcome = runCase(which);
        if (!outcome.ok) continue;
        const std::string name = caseName(which);
        check(outcome.measured.attempts >= static_cast<unsigned long long>(refused),
              (name + ": the refused lines were written to the refusing destination").c_str());
        expectLines(outcome, {"before", notice(refused), "after"},
                    name + ": once the destination accepts again, a notice counting " + std::to_string(refused) +
                        " dropped line(s) arrives, then the next line");
    }
}

// GOAL: S14.39 -- a destination that keeps refusing does not make the log writer spin: over 1 s in
//       which every line is refused, the process uses under a quarter of a CPU-second, and the
//       lines are then reported as one count.
// WHY-THIS-SHAPE: process CPU time, not a count of write attempts: a retry held pending would show
//       as CPU burnt, whatever the call pattern. A quarter of the window separates a spinning
//       thread (about the whole window) from a correct one (a few milliseconds) with room for a
//       loaded runner.
// CONTROL: in backend/daemon/log_pump.cpp via control_run.sh 2026-09-27, reverted: Stream::write
//       made to `return` on kRefused without advancing `sent` -> "uses under a quarter of a
//       CPU-second" red, with the three notice lines (the held line is delivered late in place of
//       a count).
// COVERAGE-ASSERT: at least one refused attempt per line, so the window had a refusing destination
//       to spin on.
void checkNoSpinWhileRefused() {
    const Outcome outcome = runCase(Case::CpuWhileRefused);
    if (!outcome.ok) return;
    const std::string name = caseName(Case::CpuWhileRefused);
    check(outcome.measured.attempts >= kRefusedLines,
          (name + ": every line was written to the refusing destination").c_str());
    const double cpu = static_cast<double>(outcome.measured.cpuNs) / 1e9;
    check(cpu < 0.25, (name + ": the process uses under a quarter of a CPU-second while the destination refuses").c_str());
    expectLines(outcome, {"before", notice(kRefusedLines), "after"},
                name + ": once it accepts again, one notice counts all " + std::to_string(kRefusedLines) + " lines");
    std::printf("log_pump_test: %s: %.3f CPU-seconds, %llu refused write(s)\n", name.c_str(), cpu,
                outcome.measured.attempts);
}

// GOAL: S14.40 -- a destination whose reader has gone (EPIPE) is abandoned: after the first failed
//       write, no later line costs a write to it. False if a broken pipe is treated as a refusal to
//       retry on every line.
// WHY-THIS-SHAPE: counted at write(2), because nothing reaches a pipe with no reader either way; the
//       reader closes after install(), so the writer holds its own non-blocking reopen, as for any
//       pipe that breaks later.
// CONTROL: in backend/daemon/log_pump.cpp via control_run.sh 2026-09-27, reverted: `case EPIPE:`
//       removed from put(), so EPIPE falls to kRefused -> "exactly one write" red, alone.
void checkReaderGoneAbandons() {
    const Outcome outcome = runCase(Case::ReaderGone);
    if (!outcome.ok) return;
    const std::string name = caseName(Case::ReaderGone);
    check(outcome.measured.attempts == 1,
          (name + ": exactly one write reaches the broken pipe; the destination is then abandoned").c_str());
    if (outcome.measured.attempts != 1) {
        std::fprintf(stderr, "  %s: %llu write(s) attempted for 5 lines\n", name.c_str(), outcome.measured.attempts);
    }
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "log_pump_test";
    try {
        checkRefusedWrites();
        checkNoSpinWhileRefused();
        checkReaderGoneAbandons();
    } catch (const std::exception& e) {
        harnessFailure(std::string("the run ended in an unhandled exception: ") + e.what());
    }
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test file, not in the log writer")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
