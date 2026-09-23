// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <thread>

namespace httplib {
class Server;
}

namespace wallpaperx {

class WallpaperManager;

// The read-only HTTP face of the daemon, for the KDE plugin's QML: plasmashell blocks
// `file://` XHR, so loopback HTTP is the channel that works.
//
// GET-only by design. Any local process can reach a loopback port with no authentication,
// so a mutating route here would be an unauthenticated way to change every monitor.
// SetWallpaper stays on DBus. Bound to 127.0.0.1, never INADDR_ANY.
class HttpServer {
public:
    static constexpr int kDefaultPort = 18462;
    static constexpr const char* kLoopbackHost = "127.0.0.1";

    // Binds synchronously, then serves on a background thread, so a port conflict is a value
    // the constructor reports rather than a failure inside a thread nobody awaits. A bind
    // failure does NOT throw: it is published as the manager's config-server health, and DBus,
    // wpxctl and rendering all work without this, so refusing to start would take every monitor
    // down for a convenience channel.
    //
    // A second instance for one manager DOES throw std::logic_error -- it would stamp its own
    // bind result over the first's, including a failure over a server that is serving. That is
    // a programming error rather than an environment one, hence the difference.
    //
    // `port` 0 binds whatever port the kernel chooses; port() then returns it, and the health
    // detail and the startup line name it.
    HttpServer(WallpaperManager& manager, int port = kDefaultPort);

    // Stops and joins on every path; a joinable thread destroyed mid-unwind is std::terminate.
    // Records the server as down on the way out, so a manager outliving it cannot go on
    // reporting a closed port as ok.
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    int port() const { return m_port; }

private:
    // Declared first so it is released last, after the destructor has joined the serving
    // thread. It also retracts the success record if the constructor throws before the thread
    // starts; past that point the constructor cannot throw at all, since destroying a joinable
    // std::thread is std::terminate.
    class Registration {
    public:
        Registration(WallpaperManager& manager, int port);
        ~Registration();

        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;

        // The port actually bound, which differs from the one asked for when that was 0.
        void bound(int port) { m_port = port; }

    private:
        WallpaperManager& m_manager;
        int m_port;
    };

    void registerRoutes();

    Registration m_registration;
    WallpaperManager& m_manager;
    int m_port;

    // By pointer so httplib.h -- 700 KB header-only -- is compiled once, here.
    std::unique_ptr<httplib::Server> m_server;

    std::thread m_thread;

    // Tells the destructor's stop() apart from the accept loop dying on its own.
    std::atomic<bool> m_stopping{false};
};

}  // namespace wallpaperx
