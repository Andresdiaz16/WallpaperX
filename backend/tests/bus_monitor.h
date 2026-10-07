// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A third party on the private test bus that sees what the bus routed: every frame request and
// release on the daemon's interface, every GetNameOwner, and a barrier call -- with the DESTINATION
// each call was addressed to. Neither end of a call can read that: the plugin's QtDBus leaves a
// received message's destination unset, and a stub that owns both the well-known and the unique name
// answers both. Whether the plugin addressed a request to the daemon's unique name or to its
// well-known one is only visible here.
//
// sd-bus rather than QtDBus: a monitor may not send anything once it is one, and a QtDBus connection
// answers calls it receives on its own. Runs its own thread; every read of what it saw goes through
// the mutex.

#include "tests/private_test_bus.h"

#include <systemd/sd-bus.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace wallpaperx::testing {

class BusMonitor {
public:
    struct Seen {
        std::string member;
        std::string destination;  // as the message was addressed
        std::chrono::steady_clock::time_point at;
    };

    // The member and first argument the barrier call carries: a NameHasOwner on the bus driver for a
    // name nobody owns, so the call is answered whatever the bus, and nothing else sends it.
    static constexpr const char* kBarrierMember = "NameHasOwner";
    static constexpr const char* kBarrierName = "io.wallpaperx.test.monitor.barrier";

    BusMonitor() {
        if (!onPrivateTestBus(m_why)) return;
        const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        sd_bus* bus = nullptr;
        int r = sd_bus_new(&bus);
        if (r < 0) {
            m_why = "sd_bus_new failed";
            return;
        }
        m_bus = bus;
        if ((r = sd_bus_set_monitor(bus, 1)) < 0 || (r = sd_bus_negotiate_fds(bus, 1)) < 0 ||
            (r = sd_bus_set_address(bus, address)) < 0 || (r = sd_bus_set_bus_client(bus, 1)) < 0 ||
            (r = sd_bus_start(bus)) < 0) {
            m_why = std::string("the monitor connection could not be started: ") + std::strerror(-r);
            return;
        }
        sd_bus_message* call = nullptr;
        r = sd_bus_message_new_method_call(bus, &call, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                           "org.freedesktop.DBus.Monitoring", "BecomeMonitor");
        if (r >= 0) r = sd_bus_message_open_container(call, 'a', "s");
        const std::string barrierRule =
            std::string("type='method_call',member='") + kBarrierMember + "',arg0='" + kBarrierName + "'";
        const std::string rules[] = {
            "type='method_call',interface='io.wallpaperx.Daemon',member='GetFrame'",
            "type='method_call',interface='io.wallpaperx.Daemon',member='ReleaseBuffer'",
            "type='method_call',member='GetNameOwner'",
            barrierRule,
        };
        for (const std::string& rule : rules) {
            if (r >= 0) r = sd_bus_message_append(call, "s", rule.c_str());
        }
        if (r >= 0) r = sd_bus_message_close_container(call);
        if (r >= 0) r = sd_bus_message_append(call, "u", 0u);
        sd_bus_error error = SD_BUS_ERROR_NULL;
        if (r >= 0) r = sd_bus_call(bus, call, 0, &error, nullptr);
        sd_bus_message_unref(call);
        if (r < 0) {
            m_why = std::string("BecomeMonitor was refused: ") + (error.message ? error.message : std::strerror(-r));
            sd_bus_error_free(&error);
            return;
        }
        sd_bus_error_free(&error);
        m_fd = sd_bus_get_fd(bus);
        // Caught here, not left to escape: the constructor holds the bus, and an exception out of it
        // would skip the destructor that closes it, ending the run outside any harness catch. Not ok()
        // with a reason instead, which every caller reports as not verified.
        // CONTROL: test-side: `throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));`
        //       put in place of the thread's start -> item_refusal_retry_lookup reports "NOT VERIFIED ... the
        //       monitor thread could not be started: Resource temporarily unavailable", exit 77; with this catch
        //       removed the same run ends in std::terminate.
        try {
            m_thread = std::thread([this] { run(); });
        } catch (const std::system_error& e) {
            m_why = std::string("the monitor thread could not be started: ") + e.what();
            return;
        }
        m_ok = true;
    }

    ~BusMonitor() {
        m_stop.store(true);
        // Wakes the thread's wait with a hang-up; the sd_bus object itself is touched by that thread
        // alone until it has been joined.
        if (m_fd >= 0) ::shutdown(m_fd, SHUT_RDWR);
        if (m_thread.joinable()) m_thread.join();
        if (m_bus != nullptr) sd_bus_flush_close_unref(m_bus);
    }

    BusMonitor(const BusMonitor&) = delete;
    BusMonitor& operator=(const BusMonitor&) = delete;

    // Started, and still reading: a monitor whose connection failed after it started has stopped
    // seeing traffic, and a zero read off it would be a silence it never observed.
    bool ok() const { return m_ok && !m_lost.load(); }
    std::string why() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_why;
    }

    // Calls of `member` addressed to `destination`.
    int callsTo(const std::string& member, const std::string& destination) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        int n = 0;
        for (const Seen& s : m_seen) {
            if (s.member == member && s.destination == destination) ++n;
        }
        return n;
    }

    int calls(const std::string& member) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        int n = 0;
        for (const Seen& s : m_seen) {
            if (s.member == member) ++n;
        }
        return n;
    }

    // When each call of `member` was routed, in order.
    std::vector<std::chrono::steady_clock::time_point> timesOf(const std::string& member) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::chrono::steady_clock::time_point> out;
        for (const Seen& s : m_seen) {
            if (s.member == member) out.push_back(s.at);
        }
        return out;
    }

private:
    void run() {
        while (!m_stop.load()) {
            sd_bus_message* message = nullptr;
            const int r = sd_bus_process(m_bus, &message);
            if (r < 0) {
                lost(std::string("reading the bus failed: ") + std::strerror(-r));
                return;
            }
            if (message != nullptr) {
                const bool disconnected = sd_bus_message_is_signal(message, "org.freedesktop.DBus.Local", "Disconnected") > 0;
                if (!disconnected) record(message);
                sd_bus_message_unref(message);
                if (disconnected) {
                    lost("the bus disconnected the monitor");
                    return;
                }
                continue;
            }
            if (r > 0) continue;
            const int waited = sd_bus_wait(m_bus, UINT64_MAX);
            if (waited < 0) {
                lost(std::string("waiting on the bus failed: ") + std::strerror(-waited));
                return;
            }
        }
    }

    // A failure seen while the destructor is stopping the thread is that stop, not a loss.
    void lost(const std::string& why) {
        if (m_stop.load()) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_why = why;
        }
        m_lost.store(true);
    }

    void record(sd_bus_message* message) {
        uint8_t type = 0;
        if (sd_bus_message_get_type(message, &type) < 0 || type != SD_BUS_MESSAGE_METHOD_CALL) return;
        const auto text = [](const char* value) { return value != nullptr ? std::string(value) : std::string(); };
        Seen seen{text(sd_bus_message_get_member(message)), text(sd_bus_message_get_destination(message)),
                  std::chrono::steady_clock::now()};
        std::lock_guard<std::mutex> lock(m_mutex);
        m_seen.push_back(std::move(seen));
    }

    sd_bus* m_bus = nullptr;
    int m_fd = -1;
    bool m_ok = false;
    std::atomic<bool> m_lost{false};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::string m_why;  // guarded by m_mutex once the thread runs
    std::vector<Seen> m_seen;
};

}  // namespace wallpaperx::testing
