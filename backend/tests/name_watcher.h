// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The daemon's bus name, watched from the test side, and the guard that keeps a test binary run
// by hand off the desktop's own session bus. Shared by every binary that spawns the real daemon.

#include "daemon/dbus_service.h"
#include "tests/spawned_daemon.h"
#include "tests/test_report.h"

#include <sdbus-c++/sdbus-c++.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

#include <sys/types.h>
#include <unistd.h>

namespace wallpaperx::testing {

// Watches org.freedesktop.DBus for the daemon's name appearing and disappearing.
//
// Subscribed before the daemon is spawned, so there is no window in which the name
// could be acquired unobserved -- which is what makes this a signal wait rather than a
// poll loop with a sleep in it.
class NameWatcher {
public:
    NameWatcher()
        : m_proxy(sdbus::createProxy(sdbus::ServiceName{"org.freedesktop.DBus"},
                                     sdbus::ObjectPath{"/org/freedesktop/DBus"})) {
        m_proxy->uponSignal("NameOwnerChanged")
            .onInterface("org.freedesktop.DBus")
            .call([this](const std::string& name, const std::string& oldOwner, const std::string& newOwner) {
                if (name != DbusService::kBusName) return;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_events;
                }
                (void)oldOwner;
                (void)newOwner;
                m_changed.notify_all();
            });
    }

    // True if the daemon currently holds the name, asked of the bus itself.
    bool ownedNow() {
        bool has = false;
        m_proxy->callMethod("NameHasOwner")
            .onInterface("org.freedesktop.DBus")
            .withArguments(std::string(DbusService::kBusName))
            .storeResultsTo(has);
        return has;
    }

    // How many NameOwnerChanged events for this name have arrived. The only evidence OUTSIDE
    // the daemon's own log that the name was ever claimed, which is what a phase grading that
    // log needs -- it cannot use the log as its own witness. A counter rather than a cached
    // ownership bool, for the reason waitFor's comment below gives.
    uint64_t events() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_events;
    }

    // Waits, to a deadline, for that counter to move off `from`. False means it did not, which
    // for a daemon that has already exited means the name was never taken and never released.
    bool waitForEventsPast(uint64_t from, std::chrono::milliseconds patience) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, patience, [&] { return m_events != from; });
    }

    // Waits until the name is (or is not) owned. Returns false on timeout.
    //
    // Deliberately not "cache a bool in the signal handler and wait on it", which fails both
    // ways: set only by the handler, the flag still holds the previous daemon's value after an
    // early return, so a later wait is satisfied before the next daemon exists; with the bus's
    // answer written back into it, a NameOwnerChanged delivered between the reply and the lock
    // is overwritten, losing the very transition being waited for.
    //
    // The signal carries no state here at all -- it only bumps a counter, which cannot be
    // lost or clobbered. Ownership itself is always read from the bus, which is the one
    // authority that cannot go stale. Snapshotting the counter *before* asking means any
    // transition racing the query bumps it and wakes the wait, so nothing is missed.
    bool waitFor(bool owned) {
        const auto deadline = std::chrono::steady_clock::now() + kProcessTimeout;
        while (true) {
            uint64_t seen = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                seen = m_events;
            }
            if (ownedNow() == owned) return true;

            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_changed.wait_until(lock, deadline, [&] { return m_events != seen; })) return false;
        }
    }

private:
    // GOAL: X.4 -- no test aborts or corrupts memory instead of reporting.
    // WHY-THIS-SHAPE: m_proxy is declared *last*, which reads backwards next to the
    //       constructor that initialises it first, and it has to be. Members are
    //       destroyed in reverse declaration order, and the signal callback registered
    //       on the proxy touches m_mutex, m_events and m_changed. With the proxy
    //       declared first it was destroyed last, so a NameOwnerChanged arriving during
    //       teardown ran notify_all() on an already-destroyed condition_variable.
    //       ThreadSanitizer caught it on a real run: "data race ...
    //       std::condition_variable::~condition_variable()" with the previous access
    //       inside NameWatcher's own lambda. Declaring the proxy last makes it the
    //       first thing destroyed, and sdbus unregisters the handler there.
    // CONTROL: none that reproduces, and saying so is the point. Putting the
    //       declaration back above m_mutex and running daemon_system_lavapipe six times
    //       under the TSan tier produced zero ThreadSanitizer reports -- the racy layout
    //       is silent here on demand. The race needs a NameOwnerChanged still in flight
    //       at the instant the watcher is destroyed, and this test's shutdown almost
    //       never produces that window; the one time TSan did catch it was on an
    //       unrelated run whose timing happened to hit it.
    //       So this ordering is justified by the language rule rather than by a
    //       control: members are destroyed in reverse declaration order, and the
    //       callback registered on m_proxy touches all three members below it. That is
    //       checkable by reading it and does not depend on a scheduler. Do not read the
    //       silence above as evidence the ordering is optional -- an unreproducible race
    //       is still a race, and a control that cannot make this go red means this
    //       comment is a claim about C++ semantics, not an observation.
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    uint64_t m_events = 0;
    std::unique_ptr<sdbus::IProxy> m_proxy;
};

// The parent of process `pid`, from /proc/<pid>/stat; -1 when it cannot be read. The command name
// in that file is parenthesised and may itself hold ") ", so the fields are read after the last one.
inline pid_t parentOf(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    const std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t close = stat.rfind(')');
    if (close == std::string::npos) return -1;
    std::istringstream fields(stat.substr(close + 1));
    std::string state;
    pid_t parent = -1;
    if (!(fields >> state >> parent)) return -1;
    return parent;
}

// The process id of the bus daemon `connection` is on, as the bus itself reports it.
inline pid_t busDaemonPid(sdbus::IConnection& connection) {
    auto proxy = sdbus::createProxy(connection, sdbus::ServiceName{"org.freedesktop.DBus"},
                                    sdbus::ObjectPath{"/org/freedesktop/DBus"});
    uint32_t pid = 0;
    proxy->callMethod("GetConnectionUnixProcessID")
        .onInterface("org.freedesktop.DBus")
        .withArguments(std::string("org.freedesktop.DBus"))
        .storeResultsTo(pid);
    return static_cast<pid_t>(pid);
}

// Call first in main(), before anything opens a bus connection: an exit status when this process
// must not run, nothing when it may. A daemon-driving test run by hand, outside `dbus-run-session`,
// finds the desktop's own daemon owning the name, takes it for its own and drives it -- which has
// overwritten a real config.
//
// Exit 77 (not verified) when the bus is, or may be, the user's own: nothing is wrong, the binary
// was started the wrong way. Exit 1 when the bus is private but unusable -- unreachable, or with the
// name already owned by a process no test started -- since that is the environment ctest built
// being broken, and a skip would hide it.
//
// GOAL: S14.9 -- no binary including this drives a daemon on a bus it did not get from
//       dbus-run-session, and a private bus that is broken or already occupied grades red.
// WHY-THIS-SHAPE: a positive proof, not a search for the user's bus: this process's parent is
//       dbus-run-session, and the bus it is on was started by that same parent. Recognising the
//       user's bus instead (by its id at $XDG_RUNTIME_DIR/bus) let through any user bus living
//       elsewhere -- dbus-launch, a bus under /tmp -- and had to talk to the user's bus to decide.
//       This asks no bus anything until the parent is known to be dbus-run-session. It refuses a
//       wrapper between the two (`dbus-run-session -- sh -c ...`) with 77, which fails safe. The
//       proof comes before the owner check, so the desktop bus with its daemon running is refused
//       with 77, never graded red.
// CONTROL: by hand, each inside dbus-run-session, nothing reaching a real bus; all 2026-09-24.
//       (a) A dbus-launch-style bus: a nested dbus-daemon and the test both started by one shell,
//       no bus at $XDG_RUNTIME_DIR/bus -> exit 77, "not started by dbus-run-session". With the
//       parent check made `if (false)` the whole of device_loss_restart_test ran on it (exit 0,
//       79 daemon log lines), which is what the $XDG_RUNTIME_DIR/bus comparison allowed. (b) The
//       test started by dbus-run-session with its address pointed by `env` at that nested bus ->
//       exit 77, "not started by the dbus-run-session that started this process"; with the
//       bus-parent check made `if (false)`, again exit 0 and 79 daemon lines. (c) A scratch daemon
//       owning the name -> exit 1, "FAIL (harness): ... already owned"; (d) a socket that does
//       not exist -> exit 1, "could not be reached"; (e) the address unset -> exit 77. (c)-(e)
//       for all three binaries.
inline std::optional<int> refuseUnlessPrivateBus() {
    const auto refuse = [](const std::string& reason) {
        notVerified("every goal in this file",
                    "refusing to run: " + reason + ". Run it through ctest, which gives it a private bus");
        return reportNotVerifiedOrPassed();
    };
    const auto broken = [](const std::string& fault) {
        harnessFailure(fault);
        static_cast<void>(reportHarnessFaults("in the session bus this run was given, not in the daemon"));
        return 1;
    };

    const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address == nullptr || *address == '\0') {
        return refuse("DBUS_SESSION_BUS_ADDRESS is unset, so sd-bus would fall back to the user's own session bus");
    }
    const pid_t parent = ::getppid();
    std::error_code ec;
    const std::filesystem::path parentExe =
        std::filesystem::read_symlink("/proc/" + std::to_string(parent) + "/exe", ec);
    if (ec || parentExe.filename() != "dbus-run-session") {
        return refuse("this process was not started by dbus-run-session (its parent is " +
                      (ec ? "unreadable: " + ec.message() : parentExe.string()) + ")");
    }
    // The bus a spawned daemon joins: it connects the same way.
    pid_t busPid = -1;
    try {
        auto connection = sdbus::createSessionBusConnection();
        try {
            busPid = busDaemonPid(*connection);
        } catch (const std::exception& e) {
            return refuse(std::string("the bus at ") + address + " would not say which process runs it: " + e.what());
        }
    } catch (const std::exception& e) {
        return broken(std::string("the session bus at ") + address + " could not be reached: " + e.what());
    }
    if (parentOf(busPid) != parent) {
        return refuse(std::string("the bus at ") + address + " (run by process " + std::to_string(busPid) +
                      ") was not started by the dbus-run-session that started this process");
    }
    try {
        NameWatcher probe;
        if (probe.ownedNow()) {
            return broken(std::string(DbusService::kBusName) + " is already owned on this private bus by a process "
                                                                "this test did not start; refusing to drive it");
        }
    } catch (const std::exception& e) {
        return broken(std::string("the session bus at ") + address + " could not be asked who owns " +
                      DbusService::kBusName + ": " + e.what());
    }
    return std::nullopt;
}

}  // namespace wallpaperx::testing
