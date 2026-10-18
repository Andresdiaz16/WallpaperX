// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Holding or failing the daemon's render thread from inside a call it makes.
//
// A test defines the hooked function in its own executable, so the daemon's call resolves there;
// the gate decides whether that call waits, fails, or proceeds. This is how a check reads state
// during a span that is otherwise invisible -- inside an image rebuild, inside a decode, inside a
// decoder's teardown -- rather than before or after the pass, where the span does not exist.
//
// One definition rather than one per binary: dbus_service_test's resize-barrier phase and
// video_wallpaper_test's ordering and crash-record phases both need it, and two copies of a
// synchronisation primitive is two things that can drift. Client-only, like its neighbours here:
// it includes no daemon header.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>

namespace wallpaperx::testing {

class CallGate {
public:
    enum class Mode { Hold, Fail };

    // `path`, when given, limits the gate to calls naming that path. A Fail gate fails `times`
    // calls in a row; a Hold gate holds one.
    void arm(Mode mode, std::string path = {}, int times = 1) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mode = mode;
        m_path = std::move(path);
        m_remaining = mode == Mode::Fail ? times : 1;
        m_fired = false;
        m_released = false;
        m_armed.store(true);
    }

    // In the hooked call; true when that call must fail. A held call waits at most a minute, so a
    // phase that never releases costs a minute rather than the run.
    bool enter(const char* path = nullptr) {
        if (!m_armed.load()) return false;
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_armed.load() || (!m_path.empty() && (path == nullptr || m_path != path))) return false;
        if (--m_remaining <= 0) m_armed.store(false);
        m_fired = true;
        m_changed.notify_all();
        if (m_mode == Mode::Fail) return true;
        m_changed.wait_for(lock, std::chrono::seconds(60), [this] { return m_released; });
        return false;
    }

    // Fail calls the gate has yet to make.
    int remaining() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_armed.load() ? m_remaining : 0;
    }

    bool waitFired(std::chrono::seconds limit) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, limit, [this] { return m_fired; });
    }

    // Lets a held call go, and disarms a gate whose call never came.
    void release() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_armed.store(false);
        m_released = true;
        m_changed.notify_all();
    }

private:
    std::atomic<bool> m_armed{false};
    std::mutex m_mutex;
    std::condition_variable m_changed;
    Mode m_mode = Mode::Hold;
    std::string m_path;
    int m_remaining = 0;
    bool m_fired = false;
    bool m_released = false;
};

}  // namespace wallpaperx::testing
