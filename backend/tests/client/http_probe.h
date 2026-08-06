#pragma once

// Client-side scaffolding for talking to the daemon's HTTP config server, shared by the
// two tiers that both do it:
//
//   http_server_test    -- an HttpServer in this process, on an ephemeral port
//   daemon_system_test  -- the real daemon binary, on the shipped 18462
//
// Extracted rather than copied for the same reason daemon_client.h was: the two tiers
// have to agree about what a reply *is*. If the system tier carried its own idea of "the
// request completed" or "the body parsed", one tier could be updated and the other would
// still compile, still pass, and be grading something else.
//
// Deliberately client-only, like its DBus sibling: nothing here touches WallpaperManager,
// HttpServer or the GPU, so the out-of-process tier -- which has access to none of them
// -- can use every line of it.

#include <httplib.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wallpaperx::testing {

// Refusal and timeout are kept apart on purpose. A host whose firewall DROPs traffic
// answers "timed out" whatever is or is not listening, so a probe that collapsed the two
// into "did not connect" would report a server bound to 0.0.0.0 as unreachable -- and the
// bind-address check built on it would pass while establishing nothing.
enum class ConnectResult { Connected, Refused, TimedOut, Failed };

inline const char* describe(ConnectResult r) {
    switch (r) {
        case ConnectResult::Connected: return "connected";
        case ConnectResult::Refused: return "refused";
        case ConnectResult::TimedOut: return "timed out";
        case ConnectResult::Failed: return "failed";
    }
    return "?";
}

// One non-blocking TCP connect against `ip`:`port`, bounded by `timeoutMs`.
inline ConnectResult tryConnect(const std::string& ip, int port, int timeoutMs) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return ConnectResult::Failed;
    struct FdGuard {
        int fd;
        ~FdGuard() { ::close(fd); }
    } guard{fd};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return ConnectResult::Failed;

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return ConnectResult::Connected;
    if (errno == ECONNREFUSED) return ConnectResult::Refused;
    if (errno != EINPROGRESS) return ConnectResult::Failed;

    pollfd pfd{fd, POLLOUT, 0};
    const int ready = ::poll(&pfd, 1, timeoutMs);
    if (ready == 0) return ConnectResult::TimedOut;
    if (ready < 0) return ConnectResult::Failed;

    int soErr = 0;
    socklen_t len = sizeof(soErr);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) != 0) return ConnectResult::Failed;
    if (soErr == 0) return ConnectResult::Connected;
    if (soErr == ECONNREFUSED) return ConnectResult::Refused;
    return ConnectResult::Failed;
}

inline httplib::Client makeClient(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(std::chrono::seconds(5));
    cli.set_read_timeout(std::chrono::seconds(10));
    cli.set_write_timeout(std::chrono::seconds(10));
    // Keep-alive, and it is not a performance tweak. Without it httplib opens a fresh
    // connection per request, so the Poller below -- which issues hundreds of them
    // against one port -- leaves hundreds of TIME_WAIT sockets on the SERVER side. On the
    // system tier that port is the shipped 18462, and a pile of TIME_WAITs there is a
    // side effect of running the suite that outlives it by a minute and can stop an
    // unrelated process binding the port. One connection per client removes it.
    cli.set_keep_alive(true);
    return cli;
}

struct HttpReply {
    bool reached = false;  // the request completed at the HTTP level at all
    int status = 0;
    std::string body;
    nlohmann::json json;  // discarded when the body did not parse

    // `reached` is separate from `status` because httplib reports a refused connection as
    // a falsy Result with no status at all. Folding that into status 0 would let a check
    // written as `status != 200` be satisfied by a daemon that was not running.
    bool ok() const { return reached && status == 200 && !json.is_discarded(); }
};

inline HttpReply get(httplib::Client& cli, const std::string& path) {
    HttpReply out;
    const httplib::Result res = cli.Get(path);
    if (!res) return out;
    out.reached = true;
    out.status = res->status;
    out.body = res->body;
    out.json = nlohmann::json::parse(out.body, nullptr, /*allow_exceptions=*/false);
    return out;
}

// Hammers one path in a loop on a thread of its own, recording when each response
// completed and whether it was well-formed. Used by the two phases that need requests in
// flight while something else happens -- a render, or a shutdown.
class Poller {
public:
    struct Sample {
        std::chrono::steady_clock::time_point completed;
        bool reached = false;
        int status = 0;
        bool bodyParsed = false;
    };

    Poller(int port, std::string path) : m_port(port), m_path(std::move(path)) {
        m_thread = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    // Joins the thread. Idempotent, and the destructor calls it too, so a throw between
    // construction and an explicit stop() still joins (X.4).
    void stop() {
        if (m_thread.joinable()) {
            m_thread.request_stop();
            m_thread.join();
        }
    }

    ~Poller() { stop(); }

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    // How many responses have been recorded so far. Safe to call while the thread runs;
    // samples() is not, which is why it is a counter and not a size().
    size_t recorded() const { return m_recorded.load(std::memory_order_acquire); }

    // Valid only after stop(): the thread owns the vector until then.
    const std::vector<Sample>& samples() const { return m_samples; }
    const std::string& error() const { return m_error; }

private:
    void run(std::stop_token stop) {
        // Every throwing call is inside. An exception crossing this thread boundary is
        // std::terminate, which would destroy exactly the diagnostics this poller exists
        // to produce.
        try {
            httplib::Client cli = makeClient(m_port);
            while (!stop.stop_requested()) {
                Sample sample;
                const httplib::Result res = cli.Get(m_path);
                sample.completed = std::chrono::steady_clock::now();
                if (res) {
                    sample.reached = true;
                    sample.status = res->status;
                    sample.bodyParsed = !nlohmann::json::parse(res->body, nullptr, false).is_discarded();
                }
                m_samples.push_back(sample);
                m_recorded.store(m_samples.size(), std::memory_order_release);
            }
        } catch (const std::exception& e) {
            m_error = e.what();
        } catch (...) {
            m_error = "unknown exception";
        }
    }

    int m_port;
    std::string m_path;
    std::vector<Sample> m_samples;
    std::atomic<size_t> m_recorded{0};
    std::string m_error;
    // Declared last so it is destroyed first: the thread's body touches every member
    // above it, and a jthread destroyed after them would be joining while they are gone.
    std::jthread m_thread;
};

}  // namespace wallpaperx::testing
