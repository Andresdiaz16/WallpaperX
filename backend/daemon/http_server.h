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

    // Binds synchronously, then serves on a background thread, so a port conflict is a
    // value the constructor reports rather than a failure inside a thread nobody awaits.
    //
    // Does NOT throw on a bind failure -- the result is published as the manager's
    // config-server health, which GetStatus carries. DBus, wpxctl and rendering work
    // without this, so refusing to start would take every monitor down for a convenience
    // channel.
    //
    // At most one instance per manager, enforced: constructing one publishes its bind
    // result as that manager's config-server health, and a second would overwrite the
    // first's with its own -- including a failed bind stamping fail over a server that is
    // serving. A second instance throws std::logic_error. That is a programming error and
    // not an environment one, which is why it throws where a bind failure does not.
    HttpServer(WallpaperManager& manager, int port = kDefaultPort);

    // Stops and joins on every path; a joinable thread destroyed mid-unwind is
    // std::terminate. Records the config server as down on the way out, so a manager that
    // outlives its server cannot go on reporting a port that is closed as ok.
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    int port() const { return m_port; }

private:
    // Declared first so it is released last, after the destructor has joined the serving
    // thread. It also unwinds a constructor that throws before the thread starts, which
    // retracts the success record written just above the thread; past that point the
    // constructor cannot throw and unwind at all, since destroying a joinable std::thread
    // is std::terminate.
    class Registration {
    public:
        Registration(WallpaperManager& manager, int port);
        ~Registration();

        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;

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
