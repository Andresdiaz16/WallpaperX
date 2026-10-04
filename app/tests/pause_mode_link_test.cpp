// SPDX-License-Identifier: GPL-2.0-or-later

// Goal S15.254: the app's pause-mode table (app/daemon/daemon_client.h) and the daemon's
// (backend/daemon/pause_mode.h) are both wallpaperx::kPauseModes. In one program, as the app's tests
// link them, they must stay two arrays: a variable's linker name carries no type, so two `inline`
// definitions merge into one silently. Built twice, its objects linked in each order (CMakeLists.txt),
// because which definition the linker keeps depends on that order.

#include "daemon_client.h"
#include "support/forked_child.h"
#include "tests/test_report.h"

#include <QString>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

std::vector<std::string> daemonArrayNames(); // pause_mode_link_daemon.cpp

namespace {

const char* const kModes[] = {"play", "always_paused", "paused_while_focused", "paused_while_maximized"};

// What the child reads: one line for the daemon's array, one for each name the app takes.
std::string readBoth() {
    std::string out = "daemon:";
    for (const std::string& name : daemonArrayNames()) {
        out += name + ',';
    }
    out += "\napp:";
    for (const char* name : kModes) {
        out += wallpaperx::isPauseMode(QString::fromLatin1(name)) ? '1' : '0';
    }
    out += wallpaperx::isPauseMode(QStringLiteral("bogus")) ? '1' : '0';
    return out + '\n';
}

} // namespace

int main() {
    using wallpaperx::testing::check;
    wallpaperx::testing::g_testName = "pause_mode_link";

    // GOAL: S15.254 -- linked beside the daemon's pause_mode.h, in either order, the daemon's array
    //       still reads play, always_paused, paused_while_focused, paused_while_maximized, and the
    //       app's isPauseMode() takes exactly those four.
    // WHY-THIS-SHAPE: read in a forked child: when the two merge, the side that reads the other's
    //       array as its own may dereference enum values as string pointers and die, which must be
    //       a FAIL line here, not this harness's crash.
    // CONTROL: app/daemon/daemon_client.h, `inline` put back on the app's kPauseModes
    //       (control_batch, 2026-10-04) -> red in both entries: app_pause_mode_link_app_first
    //       "S15.254: the reader exited 0 and read [daemon:play,play,play,play, ...]" (the daemon's
    //       side read the app's pointers as modes); app_pause_mode_link_daemon_first "S15.254:
    //       reading the two tables killed the reader with signal 11: ...".
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        wallpaperx::testing::harnessFailure("S15.254: no pipe for the reading child");
        return 1;
    }
    const pid_t parent = getpid();
    const pid_t child = fork();
    if (child < 0) {
        wallpaperx::testing::harnessFailure("S15.254: fork failed");
        return 1;
    }
    if (child == 0) {
        close(fds[0]);
        if (!wallpaperx::testing::armAgainstParentDeath(parent)) {
            _exit(wallpaperx::testing::kChildArmFailedExit);
        }
        wallpaperx::testing::suppressCoreDump();
        const std::string report = readBoth();
        const bool written = write(fds[1], report.data(), report.size()) == static_cast<ssize_t>(report.size());
        _exit(written ? 0 : 1);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    std::string report;
    int status = 0;
    bool ended = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ended && std::chrono::steady_clock::now() < deadline) {
        char buffer[512];
        for (ssize_t n; (n = read(fds[0], buffer, sizeof buffer)) > 0;) {
            report.append(buffer, static_cast<size_t>(n));
        }
        ended = waitpid(child, &status, WNOHANG) == child;
        if (!ended) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    char rest[512];
    for (ssize_t n; (n = read(fds[0], rest, sizeof rest)) > 0;) {
        report.append(rest, static_cast<size_t>(n));
    }
    close(fds[0]);
    if (!ended) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        wallpaperx::testing::harnessFailure("S15.254: the reading child did not finish within 10 s");
        return 1;
    }
    if (WIFEXITED(status) && wallpaperx::testing::forkPreambleFault(WEXITSTATUS(status)) != nullptr) {
        wallpaperx::testing::harnessFailure(std::string("S15.254: ") + wallpaperx::testing::forkPreambleFault(WEXITSTATUS(status)));
        return 1;
    }
    if (WIFSIGNALED(status)) {
        const std::string what = "S15.254: reading the two tables killed the reader with signal " + std::to_string(WTERMSIG(status)) +
                                 ": the app's and the daemon's kPauseModes are one array in this program";
        check(false, what.c_str());
    } else {
        const std::string wantDaemon = "daemon:play,always_paused,paused_while_focused,paused_while_maximized,\n";
        const std::string wantApp = "app:11110\n";
        const std::string what = "S15.254: the reader exited " + std::to_string(WEXITSTATUS(status)) + " and read [" + report +
                                 "], not [" + wantDaemon + wantApp + "]";
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && report == wantDaemon + wantApp, what.c_str());
    }
    if (wallpaperx::testing::reportCheckFailures()) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
