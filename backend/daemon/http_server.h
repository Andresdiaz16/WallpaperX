#pragma once

#include <atomic>
#include <memory>
#include <string>
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
    // Does NOT throw on a bind failure -- see listening()/lastError(). DBus, wpxctl and
    // rendering work without this, so refusing to start would take every monitor down for
    // a convenience channel.
    HttpServer(WallpaperManager& manager, int port = kDefaultPort);

    // Stops and joins on every path; a joinable thread destroyed mid-unwind is
    // std::terminate.
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool listening() const { return m_listening.load(std::memory_order_acquire); }

    // Empty when listening() is true. Set before the thread starts, so it needs no lock.
    const std::string& lastError() const { return m_bindError; }

    int port() const { return m_port; }

private:
    void registerRoutes();

    WallpaperManager& m_manager;
    int m_port;

    // By pointer so httplib.h -- 700 KB header-only -- is compiled once, here.
    std::unique_ptr<httplib::Server> m_server;

    std::thread m_thread;
    std::atomic<bool> m_listening{false};

    // Tells the destructor's stop() apart from the accept loop dying on its own.
    std::atomic<bool> m_stopping{false};
    std::string m_bindError;
};

}  // namespace wallpaperx
