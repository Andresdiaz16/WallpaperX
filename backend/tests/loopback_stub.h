// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The loopback HTTP half of a stub daemon: main.qml asks 127.0.0.1:18462 for /config. Binding that
// port, serving on a thread, and stopping again, for the tests that stand in for the daemon there.

#include <httplib.h>

#include <sys/socket.h>

#include <thread>

namespace wallpaperx::testing {

// main.qml's daemonOrigin. Not a test's choice: a stub anywhere else is never asked.
inline constexpr int kDaemonPort = 18462;

// Binds `server` to 127.0.0.1:kDaemonPort with SO_REUSEADDR, as the daemon's own HttpServer sets.
// httplib's default, SO_REUSEPORT, does not rebind over a socket in TIME_WAIT, so a daemon from the
// entry before that had just released the port made the stub fail to bind for a port nobody held.
inline bool bindDaemonPort(httplib::Server& server) {
    server.set_socket_options([](socket_t sock) {
        int on = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const void*>(&on), static_cast<socklen_t>(sizeof(on)));
    });
    return server.bind_to_port("127.0.0.1", kDaemonPort);
}

// Serves the already bound `server` on `thread`, returning once it accepts: a request made before
// then is refused and read as "no daemon". No deadline, deliberately: httplib's stop() does nothing
// to a server that is not running yet, so a listener given up on at a deadline could start later
// and never be stopped, hanging the join. A listener that never starts is a ctest timeout instead.
// False when the listen gave up; `thread` is then finished, and stopServing() still joins it.
inline bool serveOnThread(httplib::Server& server, std::thread& thread) {
    thread = std::thread([&server] { server.listen_after_bind(); });
    server.wait_until_ready();
    return server.is_running();
}

// Stops what serveOnThread() started and joins it. Safe whatever serveOnThread() returned, and when
// it was never called.
inline void stopServing(httplib::Server& server, std::thread& thread) {
    server.stop();
    if (thread.joinable()) thread.join();
}

}  // namespace wallpaperx::testing
