#include "http_server.h"

#include "log_escape.h"
#include "wallpaper_manager.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <string>

#ifndef WALLPAPERX_VERSION
// Supplied by CMake from project(... VERSION). Compiled in rather than written here, so
// /health cannot drift from the version the build actually is -- the failure mode being a
// user reporting a bug against a version string that was true two releases ago.
#error "WALLPAPERX_VERSION must be defined by the build"
#endif

namespace wallpaperx {

namespace {

constexpr const char* kContentTypeJson = "application/json";

// Requests are polls from a local QML plugin, not uploads. A body cannot mean anything to
// a GET-only server, so the cap exists to stop a local process parking memory in the
// daemon by opening a request with a large declared length.
constexpr size_t kMaxPayloadBytes = 8 * 1024;

// A poller that stops reading mid-response must not hold a worker forever. Loopback has
// no latency to speak of, so this is generous by a wide margin and still bounded.
constexpr time_t kIoTimeoutSeconds = 5;

void sendJson(httplib::Response& res, const nlohmann::json& body, int status = 200) {
    res.status = status;
    // dump() throws on invalid UTF-8, which is why every client-derived string below goes
    // through sanitizeForLog first -- the same reason GetStatus does it. A throw here
    // would reach httplib's exception handler and become a 500 with no diagnosis.
    res.set_content(body.dump(), kContentTypeJson);
}

// Parses the optional `monitor` query parameter.
//
// PLAN.md documents `/config?t=<ts>` with a `monitor_id` in the *response* and no way to
// ask for one in the request. That cannot serve its own consumer: the daemon supports
// kMaxMonitors displays and Step 10's plugin runs one QML instance per desktop, each
// needing its own. `monitor` is therefore accepted and defaults to 0, which leaves the
// documented single-argument form working exactly as written.
//
// Rejected rather than clamped. A plugin asking for monitor 5 on a 2-monitor machine has
// a bug, and answering with monitor 0's wallpaper would show the user the wrong image on
// a screen they are looking at, with nothing anywhere saying why.
bool parseMonitorParam(const httplib::Request& req, int& outMonitorId, std::string& outError) {
    outMonitorId = 0;
    if (!req.has_param("monitor")) return true;

    const std::string raw = req.get_param_value("monitor");
    std::size_t consumed = 0;
    long parsed = 0;
    try {
        parsed = std::stol(raw, &consumed);
    } catch (const std::exception&) {
        outError = "monitor must be an integer, got " + sanitizeForLog(raw, 64);
        return false;
    }
    // stol stops at the first non-digit and reports success, so "0abc" and "1 " would both
    // parse as valid ids without this. Trailing junk means the client built the URL wrong.
    if (consumed != raw.size()) {
        outError = "monitor must be an integer, got " + sanitizeForLog(raw, 64);
        return false;
    }
    if (parsed < 0 || parsed >= WallpaperManager::kMaxMonitors) {
        outError = "monitor " + std::to_string(parsed) + " is out of range [0, " +
                   std::to_string(WallpaperManager::kMaxMonitors) + ")";
        return false;
    }
    outMonitorId = static_cast<int>(parsed);
    return true;
}

}  // namespace

HttpServer::HttpServer(WallpaperManager& manager, int port)
    : m_manager(manager), m_port(port), m_server(std::make_unique<httplib::Server>()) {
    m_server->set_payload_max_length(kMaxPayloadBytes);
    m_server->set_read_timeout(kIoTimeoutSeconds);
    m_server->set_write_timeout(kIoTimeoutSeconds);

    // Without this, an exception escaping a handler unwinds through httplib's worker
    // thread and reaches std::terminate -- a handler bug would take the whole daemon down,
    // including the DBus interface and every rendered wallpaper, over a status poll.
    m_server->set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            std::string what = "unknown error";
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
            }
            std::fprintf(stderr, "wallpaperx-daemon: http handler failed: %s\n",
                         sanitizeForLog(what, 1024, LogFraming::Bare).c_str());
            res.status = 500;
            res.set_content(nlohmann::json{{"error", "internal error"}}.dump(), kContentTypeJson);
        });

    registerRoutes();

    // Bind first, on this thread, so the result is a value rather than a race.
    if (!m_server->bind_to_port(kLoopbackHost, m_port)) {
        m_bindError = std::string("could not bind ") + kLoopbackHost + ":" + std::to_string(m_port) +
                      " (is another process using it?)";
        std::fprintf(stderr,
                     "wallpaperx-daemon: %s -- the KDE plugin will not be able to read config; "
                     "DBus and rendering are unaffected\n",
                     m_bindError.c_str());
        return;
    }

    m_listening.store(true, std::memory_order_release);
    m_thread = std::thread([this] { m_server->listen_after_bind(); });
    std::printf("wallpaperx-daemon: config server on http://%s:%d\n", kLoopbackHost, m_port);
    std::fflush(stdout);
}

HttpServer::~HttpServer() {
    // stop() is safe to call whether or not listen_after_bind() has been reached; it is
    // also safe when the bind failed and there is no thread, which is why the join is
    // guarded on joinable() rather than on m_listening.
    m_server->stop();
    if (m_thread.joinable()) m_thread.join();
}

void HttpServer::registerRoutes() {
    // GET /config?t=<cache-buster>&monitor=<id>
    //
    // `t` is ignored on purpose: it exists so QML's XHR cache does not serve a stale body,
    // and its value carries no meaning to the daemon. Reading it would invite a client to
    // think it selects something.
    m_server->Get("/config", [this](const httplib::Request& req, httplib::Response& res) {
        int monitorId = 0;
        std::string error;
        if (!parseMonitorParam(req, monitorId, error)) {
            sendJson(res, nlohmann::json{{"error", error}}, 400);
            return;
        }

        const WallpaperManager::Snapshot snap = m_manager.snapshot();

        // The documented shape, kept exactly: type, path, monitor_id. A monitor nothing
        // has been set on answers with empty strings rather than 404 -- the plugin polls
        // this every second from startup, and a monitor with no wallpaper yet is the
        // ordinary state on a fresh session, not an error to log once a second.
        nlohmann::json out{{"monitor_id", monitorId}, {"type", ""}, {"path", ""}};

        const auto it = snap.monitors.find(monitorId);
        if (it != snap.monitors.end()) {
            out["type"] = it->second.type;
            // Sanitized for the same reason GetStatus's path is, and it must stay the same
            // decision in both places: this is the same client-supplied string reaching a
            // second consumer, and a value that is escaped on one channel and raw on
            // another is how the two views of one path came to disagree in aded2e5.
            // Display-safe, not round-trip-safe -- a caller must not feed it back into
            // SetWallpaper.
            out["path"] = sanitizeForLog(it->second.path, 4096, LogFraming::Bare);
        }

        sendJson(res, out);
    });

    // GET /health
    m_server->Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        const WallpaperManager::Snapshot snap = m_manager.snapshot();

        nlohmann::json out{{"status", "ok"}, {"version", WALLPAPERX_VERSION}};

        // Always present, null until a device exists, never absent.
        //
        // The device is created lazily on the first SetWallpaper, so on a freshly started
        // daemon there is no tier to report and inventing one would be a guess about the
        // machine's GPU. A key that appears only once something has happened is a key a
        // client learns about by hitting its absence -- the same argument that put
        // `unsupported` unconditionally into GetStatus.
        //
        // NOTE, and it is a real conflict: PLAN.md's Definition of Done #2 expects
        // `/health` to return `{"descriptor_tier":"heap"}`, which is only true after a
        // wallpaper has been set. Verifying that check against a daemon that has just
        // started will see null. Either the check sets a wallpaper first, or the daemon
        // stops being lazy -- and being lazy is deliberate, so that a machine with a
        // broken GPU stack still comes up and can be asked what is wrong.
        out["descriptor_tier"] = snap.gpu.initialized ? nlohmann::json(snap.gpu.descriptorTier)
                                                      : nlohmann::json(nullptr);
        out["gpu_initialized"] = snap.gpu.initialized;

        sendJson(res, out);
    });
}

}  // namespace wallpaperx
