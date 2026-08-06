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

// The read-only HTTP face of the daemon, for the KDE plugin's QML.
//
// It exists because plasmashell blocks `file://` XHR, so the plugin cannot read a config
// file directly; HTTP on loopback is the channel that works. It is deliberately NOT a
// second control surface -- every route is a GET, nothing here mutates state, and
// SetWallpaper stays on DBus where it is authenticated by the bus. Anything on this
// machine can reach a loopback port, so a mutating route here would be an unauthenticated
// way to change what every monitor displays.
//
// Bound to 127.0.0.1 explicitly rather than to INADDR_ANY. The difference is whether the
// daemon's wallpaper configuration is readable from the local network.
class HttpServer {
public:
    static constexpr int kDefaultPort = 18462;
    static constexpr const char* kLoopbackHost = "127.0.0.1";

    // Binds synchronously, then serves on a background thread. Binding before the thread
    // starts is what makes a port conflict reportable: bind_to_port() returns a value the
    // constructor can act on, where listen() would fail somewhere inside a thread nobody
    // is waiting on and the daemon would come up looking healthy.
    //
    // Does NOT throw on a bind failure -- see listening()/lastError(). The daemon's job is
    // wallpapers, and DBus, wpxctl and rendering all work without this. Refusing to start
    // because an unrelated process holds 18462 would take every monitor down to protect a
    // convenience channel. The failure is loud on stderr instead, and readable through
    // GetStatus, which is the channel still working when this one is not.
    HttpServer(WallpaperManager& manager, int port = kDefaultPort);

    // Stops the server and joins the thread on every path. A joinable std::thread
    // destroyed while an exception unwinds is std::terminate -- the exact crash that took
    // this daemon down in f1aa32c, on the DBus side.
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool listening() const { return m_listening.load(std::memory_order_acquire); }

    // Empty when listening() is true. Set once, before the thread starts, so reading it
    // needs no lock.
    const std::string& lastError() const { return m_bindError; }

    int port() const { return m_port; }

private:
    void registerRoutes();

    WallpaperManager& m_manager;
    int m_port;

    // Held by pointer so httplib.h -- 700 KB of header-only implementation -- is compiled
    // once, in http_server.cpp, instead of in every translation unit that needs to know a
    // daemon has one of these.
    std::unique_ptr<httplib::Server> m_server;

    std::thread m_thread;
    std::atomic<bool> m_listening{false};
    std::string m_bindError;
};

}  // namespace wallpaperx
