// SPDX-License-Identifier: GPL-2.0-or-later

// Whether a ctest entry is cut off from the desktop session that ran ctest, seen from inside an
// entry registered like every other (renderer/tests/support/isolated_test.cmake), in its bus or its
// display form. The caller's values are those isolated_run.sh recorded before replacing them.

#include "tests/test_report.h"

#include <sdbus-c++/sdbus-c++.h>

#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

std::string normal(const std::string& path) {
    std::string out = std::filesystem::path(path).lexically_normal().string();
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

// `path` is `root` or lies beneath it. False for an empty root: an unknown place contains nothing.
bool within(const std::string& path, const std::string& root) {
    if (path.empty() || root.empty()) return false;
    const std::string p = normal(path);
    const std::string r = normal(root);
    return p == r || (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/');
}

void checkPrivate(const char* variable, const std::string& callerHome, const std::string& callerRuntime,
                  const std::string& accountHome, const std::string& accountRuntime) {
    const std::string value = env(variable);
    const std::string name(variable);
    check(!value.empty(), (name + " is set, so nothing falls back to a default under the real home").c_str());
    if (value.empty()) return;
    const struct {
        const std::string& root;
        const char* what;
    } forbidden[] = {
        {callerHome, "the home directory of the session that ran ctest"},
        {callerRuntime, "the runtime directory of the session that ran ctest"},
        {accountHome, "this account's own home directory"},
        {accountRuntime, "this account's own runtime directory"},
    };
    for (const auto& place : forbidden) {
        const bool inside = within(value, place.root);
        check(!inside, (name + " does not point into " + place.what).c_str());
        if (inside) std::fprintf(stderr, "  %s=%s is inside %s\n", variable, value.c_str(), place.root.c_str());
    }
}

// A listener on an ephemeral 127.0.0.1 port accepts a connection from this process.
bool loopbackWorks() {
    const int server = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof address;
    const bool ok = server >= 0 && client >= 0 &&
                    ::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 &&
                    ::listen(server, 1) == 0 && ::getsockname(server, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
                    ::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0;
    if (!ok) std::fprintf(stderr, "  127.0.0.1: %s\n", std::strerror(errno));
    if (server >= 0) ::close(server);
    if (client >= 0) ::close(client);
    return ok;
}

// The CapEff line of /proc/self/status, or "" if it could not be read.
std::string effectiveCapabilities() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("CapEff:", 0) == 0) return line.substr(line.find_first_not_of(" \t", 7));
    }
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "isolation_test";
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode != "bus" && mode != "display") {
        wallpaperx::testing::harnessFailure("expected 'bus' or 'display' as the only argument");
        static_cast<void>(wallpaperx::testing::reportHarnessFaults("in this test's registration, not in the wiring"));
        return 1;
    }

    // GOAL: the entry was started through isolated_run.sh at all. False for an entry registered
    //       with a bare add_test() or a plain dbus-run-session, which inherits the caller's session.
    // CONTROL: run as `dbus-run-session --config-file=<the private conf> -- wallpaperx-isolation-test
    //       bus` from a caller with WAYLAND_DISPLAY, DISPLAY and QT_QPA_PLATFORM=xcb set -> "FAIL: this
    //       entry was started through isolated_run.sh", with HOME, every XDG directory, DISPLAY,
    //       WAYLAND_DISPLAY and the platform red beside it. Observed 2026-10-01.
    const bool wrapped = std::getenv("WALLPAPERX_CALLER_HOME") != nullptr;
    check(wrapped, "this entry was started through isolated_run.sh (it left no record of the caller's environment)");

    const std::string callerHome = env("WALLPAPERX_CALLER_HOME");
    const std::string callerRuntime = env("WALLPAPERX_CALLER_XDG_RUNTIME_DIR");
    const std::string callerWayland = env("WALLPAPERX_CALLER_WAYLAND_DISPLAY");
    const std::string callerBus = env("WALLPAPERX_CALLER_DBUS_SESSION_BUS_ADDRESS");
    const passwd* account = ::getpwuid(::getuid());
    const std::string accountHome = account != nullptr && account->pw_dir != nullptr ? account->pw_dir : "";
    const std::string accountRuntime = "/run/user/" + std::to_string(::getuid());

    // GOAL: the entry has a network of its own, in which 127.0.0.1 works, so a port it binds is not
    //       the 127.0.0.1:18462 the live desktop's wallpaper plugin polls.
    // WHY-THIS-SHAPE: compared with the namespace the wrapper recorded before leaving it. Loopback
    //       is shown by a connection, not by the namespace alone: one whose loopback is down is
    //       private too, and every test daemon would fail to bind in it.
    // CONTROL: observed 2026-10-01 on scratch copies of renderer/tests/support/isolated_run.sh.
    //       Both unshare steps and the wrapper's own namespace check removed -> "FAIL: this entry's
    //       network namespace is not the caller's". `ip link set lo up` replaced by `true` -> "FAIL:
    //       127.0.0.1 accepts a connection inside the entry's network". 2026-10-15, scratch copy:
    //       `export WALLPAPERX_CALLER_NET_NS=""` -> "FAIL: the wrapper recorded the caller's network
    //       namespace", alone; the unmodified wrapper green.
    const std::string callerNet = env("WALLPAPERX_CALLER_NET_NS");
    std::error_code netError;
    const std::string ownNet = std::filesystem::read_symlink("/proc/self/ns/net", netError).string();
    check(!callerNet.empty(), "the wrapper recorded the caller's network namespace");
    check(!ownNet.empty() && ownNet != callerNet, "this entry's network namespace is not the caller's");
    if (ownNet.empty() || ownNet == callerNet) {
        std::fprintf(stderr, "  network namespace: this entry's '%s', the caller's '%s'\n", ownNet.c_str(),
                     callerNet.c_str());
    }
    check(loopbackWorks(), "127.0.0.1 accepts a connection inside the entry's network");

    // GOAL: the namespace leaves the test as the caller's uid and with no capabilities. False for a
    //       root-mapped namespace, where ownership and permission checks stop meaning what they do
    //       for the user.
    // CONTROL: observed 2026-10-01, the inner `unshare --map-user` of a scratch isolated_run.sh
    //       dropped, so the test ran root-mapped -> "FAIL: the test runs as the caller's uid" and
    //       "FAIL: the test holds no capabilities".
    const std::string uid = std::to_string(::getuid());
    check(env("WALLPAPERX_CALLER_UID") == uid, "the test runs as the caller's uid");
    const std::string caps = effectiveCapabilities();
    check(caps.find_first_not_of('0') == std::string::npos && !caps.empty(), "the test holds no capabilities");
    if (env("WALLPAPERX_CALLER_UID") != uid || caps.find_first_not_of('0') != std::string::npos) {
        std::fprintf(stderr, "  uid %s (the caller's %s), CapEff %s\n", uid.c_str(), env("WALLPAPERX_CALLER_UID").c_str(),
                     caps.c_str());
    }

    // GOAL: HOME and every XDG base directory are private: none is, or lies inside, the caller's
    //       home or runtime directory or this account's. False when the wrapper forgets one, which
    //       sends whatever reads it -- a shader cache, a config file, a socket -- into the session.
    // WHY-THIS-SHAPE: "inside", not "equal": a HOME of $REALHOME/scratch passes an equality test.
    //       The account's directories are asked of the system too, so a wrong caller record is caught.
    // CONTROL: observed 2026-10-01 on scratch copies of renderer/tests/support/isolated_run.sh.
    //       `export HOME=...` deleted -> "FAIL: HOME does not point into the home directory of the
    //       session that ran ctest", the four XDG directories red with it. XDG_RUNTIME_DIR left out
    //       of its export -> "FAIL: XDG_RUNTIME_DIR does not point into the runtime directory of the
    //       session that ran ctest". Run root-mapped with HOME=/root/x and XDG_RUNTIME_DIR=/run/user/0
    //       -> "...into this account's own home directory" for all five and "...own runtime directory".
    const char* privateDirs[] = {"HOME",           "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                                 "XDG_STATE_HOME", "XDG_CACHE_HOME",  "XDG_RUNTIME_DIR"};
    for (const char* variable : privateDirs) {
        checkPrivate(variable, callerHome, callerRuntime, accountHome, accountRuntime);
    }
    for (const char* variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"}) {
        check(within(env(variable), env("HOME")),
              (std::string(variable) + " lies inside the private HOME, so it is removed with it").c_str());
    }
    // GOAL: TMPDIR is set, is not the caller's, lies in none of the four places above, and sits
    //       beside the private HOME, so the cleanup removes it; TMP and TEMP name the same directory.
    // WHY-THIS-SHAPE: "not the caller's" too: a caller TMPDIR of /tmp passes the place checks.
    // CONTROL: observed 2026-10-01 on copies of renderer/tests/support/isolated_run.sh. Caller
    //       TMPDIR=$HOME/tmp and no TMPDIR handling -> "the wrapper recorded the caller's TMPDIR",
    //       "TMPDIR does not point into the home directory ...", the beside-HOME and TMP/TEMP lines
    //       red. Without `export TMPDIR=...` -> "TMPDIR is not the caller's TMPDIR" and the same two.
    check(std::getenv("WALLPAPERX_CALLER_TMPDIR") != nullptr,
          "the wrapper recorded the caller's TMPDIR, so the next checks can compare against it");
    checkPrivate("TMPDIR", callerHome, callerRuntime, accountHome, accountRuntime);
    const std::string tmpdir = env("TMPDIR");
    const std::string callerTmpdir = env("WALLPAPERX_CALLER_TMPDIR");
    const bool ownTmpdir = !tmpdir.empty() && (callerTmpdir.empty() || normal(tmpdir) != normal(callerTmpdir));
    check(ownTmpdir, "TMPDIR is not the caller's TMPDIR");
    const std::string home = env("HOME");
    check(!home.empty() && within(tmpdir, std::filesystem::path(normal(home)).parent_path().string()) &&
              !within(tmpdir, home),
          "TMPDIR lies beside the private HOME, inside the scratch directory the wrapper removes");
    check(env("TMP") == tmpdir && env("TEMP") == tmpdir, "TMP and TEMP name the same private directory as TMPDIR");
    if (!ownTmpdir || env("TMP") != tmpdir || env("TEMP") != tmpdir) {
        std::fprintf(stderr, "  TMPDIR=%s TMP=%s TEMP=%s (the caller's TMPDIR was '%s')\n", tmpdir.c_str(),
                     env("TMP").c_str(), env("TEMP").c_str(), callerTmpdir.c_str());
    }

    const std::string runtime = env("XDG_RUNTIME_DIR");
    struct stat runtimeStat {};
    const bool runtimeOk = !runtime.empty() && ::stat(runtime.c_str(), &runtimeStat) == 0 &&
                           S_ISDIR(runtimeStat.st_mode) && (runtimeStat.st_mode & 0777) == 0700 &&
                           runtimeStat.st_uid == ::getuid();
    check(runtimeOk, "XDG_RUNTIME_DIR exists, is this account's, and is mode 0700 as the specification requires");

    // GOAL: no display of the caller's session is reachable: DISPLAY is unset, and WAYLAND_DISPLAY is
    //       unset (bus) or names a socket inside the private runtime directory (display).
    // CONTROL: observed 2026-10-01 on scratch copies of isolated_run.sh. WAYLAND_DISPLAY dropped
    //       from its `unset`, caller WAYLAND_DISPLAY=wayland-0 -> "FAIL: WAYLAND_DISPLAY is unset in
    //       an entry with no display of its own". Display mode, the nested KWin started on the
    //       caller's runtime directory and named by absolute path -> "FAIL: the Wayland socket is
    //       the nested compositor's, inside the private runtime directory". 2026-10-15, scratch
    //       copies of renderer/tests/support/isolated_run.sh: WAYLAND_SOCKET dropped from its `unset`,
    //       caller WAYLAND_SOCKET=99 -> "FAIL: WAYLAND_SOCKET is unset", alone (the unmodified wrapper
    //       green with the same caller); display mode, `export WAYLAND_DISPLAY="$socket-gone"` ->
    //       "FAIL: WAYLAND_DISPLAY names a socket that exists".
    check(env("DISPLAY").empty(), "DISPLAY is unset, so no X server of the caller's is reachable");
    check(std::getenv("WAYLAND_SOCKET") == nullptr, "WAYLAND_SOCKET is unset");
    const std::string wayland = env("WAYLAND_DISPLAY");
    const std::string platform = env("QT_QPA_PLATFORM");
    if (mode == "bus") {
        check(wayland.empty(), "WAYLAND_DISPLAY is unset in an entry with no display of its own");
        if (!wayland.empty()) {
            std::fprintf(stderr, "  WAYLAND_DISPLAY=%s (the caller's was '%s')\n", wayland.c_str(), callerWayland.c_str());
        }
        // GOAL: a Qt entry that did not choose a platform runs offscreen, not on the caller's choice.
        // CONTROL: the offscreen arm of isolated_run.sh's `case "${QT_QPA_PLATFORM-}"` emptied in a
        //       scratch copy, caller QT_QPA_PLATFORM=xcb -> "FAIL: Qt is put on the offscreen
        //       platform", printing 'xcb'. Observed 2026-10-01.
        check(platform.rfind("offscreen", 0) == 0, "Qt is put on the offscreen platform");
        if (platform.rfind("offscreen", 0) != 0) std::fprintf(stderr, "  QT_QPA_PLATFORM='%s'\n", platform.c_str());
    } else if (wayland.empty()) {
        notVerified("the nested-compositor isolation",
                    "no nested compositor was started for this entry (isolated_run.sh says why above)");
    } else {
        const std::string socket = wayland.front() == '/' ? wayland : runtime + "/" + wayland;
        struct stat socketStat {};
        const bool isSocket = ::stat(socket.c_str(), &socketStat) == 0 && S_ISSOCK(socketStat.st_mode);
        check(isSocket, "WAYLAND_DISPLAY names a socket that exists");
        check(within(socket, runtime) && !within(socket, callerRuntime) && !within(socket, accountRuntime),
              "the Wayland socket is the nested compositor's, inside the private runtime directory");
        if (!within(socket, runtime)) std::fprintf(stderr, "  the socket is %s\n", socket.c_str());
        check(platform.find("wayland") != std::string::npos, "Qt is pointed at the nested compositor's platform");
    }

    // GOAL: the entry's bus is private, and nothing can be activated on it: ListActivatableNames
    //       answers exactly org.freedesktop.DBus. False for dbus-run-session's default configuration,
    //       whose standard service directories let any client start the portal, the activity
    //       manager or the KDE password service as the user.
    // WHY-THIS-SHAPE: the bus is asked, not the wrapper's command line: the configuration in force
    //       decides, and an included session.d can add service directories. Asking activates nothing.
    // CONTROL: observed 2026-10-01. A scratch isolated_session.conf given a `<servicedir>` holding
    //       one inert .service file -> "FAIL: nothing can be activated on this entry's bus", listing
    //       org.wallpaperx.InertControlService. A scratch isolated_run.sh keeping the caller's
    //       DBUS_SESSION_BUS_ADDRESS (a private bus standing in) -> "FAIL: the entry's bus is not the caller's".
    //       2026-10-15, scratch copies: the test run under `env -u DBUS_SESSION_BUS_ADDRESS` inside
    //       private_bus.sh -> "FAIL: DBUS_SESSION_BUS_ADDRESS is set, ..." and "...private bus answers";
    //       under `env DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/...` -> "FAIL: the entry's
    //       private bus answers", alone.
    const std::string bus = env("DBUS_SESSION_BUS_ADDRESS");
    check(!bus.empty(), "DBUS_SESSION_BUS_ADDRESS is set, so no client goes looking for a bus of its own");
    check(bus.empty() || (bus != callerBus && bus.find(accountRuntime + "/") == std::string::npos &&
                          (callerRuntime.empty() || bus.find(callerRuntime + "/") == std::string::npos)),
          "the entry's bus is not the caller's");
    // GOAL: with no dbus-run-session the bus half is not verified, and every check above still grades.
    // WHY-THIS-SHAPE: keyed on the exact no-bus address isolated_run.sh sets, so a real bus that
    //       does not answer still fails "the entry's private bus answers".
    // CONTROL: observed 2026-10-01 by hand, no --bus -> "NOT VERIFIED -- the private bus ...", exit
    //       77; also without the wrapper's `export HOME=...` -> "FAIL: HOME does not point ...", exit 1.
    if (bus == "unix:path=" + runtime + "/no-bus") {
        notVerified("the private bus", "dbus-run-session is not installed, so the entry has no bus to ask");
        if (wallpaperx::testing::reportCheckFailures()) return 1;
        return wallpaperx::testing::reportNotVerifiedOrPassed("goal(s) could not be exercised (listed above)",
                                                              " (" + mode + " entry)");
    }
    std::vector<std::string> activatable;
    bool asked = false;
    try {
        auto connection = sdbus::createSessionBusConnection();
        auto proxy = sdbus::createProxy(*connection, sdbus::ServiceName{"org.freedesktop.DBus"},
                                        sdbus::ObjectPath{"/org/freedesktop/DBus"});
        proxy->callMethod("ListActivatableNames").onInterface("org.freedesktop.DBus").storeResultsTo(activatable);
        asked = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  the bus at '%s' could not be asked: %s\n", bus.c_str(), e.what());
    }
    check(asked, "the entry's private bus answers");
    const bool inert = asked && activatable.size() == 1 && activatable.front() == "org.freedesktop.DBus";
    check(!asked || inert, "nothing can be activated on this entry's bus (only org.freedesktop.DBus is listed)");
    if (asked && !inert) {
        for (const std::string& name : activatable) std::fprintf(stderr, "  activatable: %s\n", name.c_str());
    }

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed("goal(s) could not be exercised (listed above)",
                                                          " (" + mode + " entry)");
}
