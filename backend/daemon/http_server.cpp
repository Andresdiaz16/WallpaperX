// SPDX-License-Identifier: GPL-2.0-or-later

#include "http_server.h"

#include "log_escape.h"
#include "wallpaper_manager.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <utility>
#include <exception>
#include <stdexcept>
#include <thread>
#include <string>
#include <system_error>

#include <sys/socket.h>

#ifndef WALLPAPERX_VERSION
// From CMake's project(... VERSION), so /health cannot report a version the build isn't.
#error "WALLPAPERX_VERSION must be defined by the build"
#endif

namespace wallpaperx {

namespace {

constexpr const char* kContentTypeJson = "application/json";

// A GET-only server has no use for a body; the cap stops a local process parking memory
// by declaring a large one.
constexpr size_t kMaxPayloadBytes = 8 * 1024;

// Bounds a poller that stops reading mid-response. Per-read, not per-request: every byte
// received restarts it, so it bounds a silent client and not a slow one.
//
// Also bounds how long shutdown takes. Server::stop() closes the listening socket and
// nothing else, so an accepted connection that goes quiet holds the serving thread this
// long and the destructor joins it -- letting a status client decide when the daemon may
// finish closing. Sub-second because the traffic is loopback, GET-only and 8 kB-capped;
// no legitimate client comes near it.
constexpr time_t kIoTimeoutSeconds = 0;
constexpr time_t kIoTimeoutMicroseconds = 500000;

void sendJson(httplib::Response& res, const nlohmann::json& body, int status = 200) {
    res.status = status;
    // dump() throws on invalid UTF-8, which is why client-derived strings are sanitized.
    res.set_content(body.dump(), kContentTypeJson);
}

// Parses the optional `monitor` query parameter, defaulting to 0 so PLAN.md's documented
// `/config?t=<ts>` form still works. Out-of-range is rejected rather than clamped:
// answering with monitor 0's wallpaper would put the wrong image on a screen the user is
// looking at, silently.
bool parseMonitorParam(const httplib::Request& req, int& outMonitorId, std::string& outError) {
    outMonitorId = 0;
    if (!req.has_param("monitor")) return true;

    const std::string raw = req.get_param_value("monitor");
    int parsed = 0;
    // from_chars rather than stol: stol skips leading whitespace and a '+' sign and counts
    // them as consumed, so " 1" and "+1" passed a did-it-consume-everything check. This one
    // accepts digits and a '-' only, and reports overflow through ec.
    const auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (ec != std::errc{} || end != raw.data() + raw.size()) {
        outError = "monitor must be an integer, got " + sanitizeForLog(raw, 64);
        return false;
    }
    if (parsed < 0 || parsed >= WallpaperManager::kMaxMonitors) {
        outError = "monitor " + std::to_string(parsed) + " is out of range [0, " +
                   std::to_string(WallpaperManager::kMaxMonitors) + ")";
        return false;
    }
    outMonitorId = parsed;
    return true;
}

// Which identity the request asked by. Both given is a 400, not a precedence rule: picking
// either silently is a guess about which screen the caller meant.
enum class IdentityKind { Default, Monitor, Output };

bool parseIdentity(const httplib::Request& req, IdentityKind& outKind, std::string& outOutput,
                   std::string& outError) {
    const bool hasMonitor = req.has_param("monitor");
    const bool hasOutput = req.has_param("output");
    if (hasMonitor && hasOutput) {
        outError = "monitor and output cannot both be given: they name the screen two ways";
        return false;
    }

    // Same rule for one parameter repeated: get_param_value() returns the first of them, so
    // without this "output=DP-3&output=DP-4" would be answered for DP-3. Repeats of an equal
    // value are refused too rather than compared -- the caller still asked twice.
    if (req.params.count("monitor") > 1 || req.params.count("output") > 1) {
        outError = "monitor and output may each be given once: repeating one names the screen "
                   "more than one way";
        return false;
    }

    outKind = hasOutput ? IdentityKind::Output : (hasMonitor ? IdentityKind::Monitor : IdentityKind::Default);
    if (!hasOutput) return true;

    outOutput = req.get_param_value("output");
    // A name the daemon could never have been given is refused, not answered "never heard".
    if (const std::string why = outputNameError(outOutput); !why.empty()) {
        outError = why;
        return false;
    }
    return true;
}

}  // namespace

HttpServer::Registration::Registration(WallpaperManager& manager, int port)
    : m_manager(manager), m_port(port) {
    if (!m_manager.tryAttachConfigServer()) {
        throw std::logic_error("HttpServer: this WallpaperManager already has a config server");
    }
}

HttpServer::Registration::~Registration() {
    // The manager can outlive the server. Without this it would go on publishing the last
    // thing the server said -- ok, for a port that is now closed -- and there is nothing
    // else left to contradict it.
    m_manager.recordConfigServerShutdown(std::string("the config server on ") + kLoopbackHost + ":" +
                                         std::to_string(m_port) + " was shut down");
    m_manager.detachConfigServer();
}

HttpServer::HttpServer(WallpaperManager& manager, int port)
    : m_registration(manager, port),
      m_manager(manager),
      m_port(port),
      m_server(std::make_unique<httplib::Server>()) {
    m_server->set_payload_max_length(kMaxPayloadBytes);
    m_server->set_read_timeout(kIoTimeoutSeconds, kIoTimeoutMicroseconds);
    m_server->set_write_timeout(kIoTimeoutSeconds, kIoTimeoutMicroseconds);

    // httplib's default sets SO_REUSEPORT, which lets another process of the same user
    // bind alongside the daemon and take a share of the plugin's polls. SO_REUSEADDR is
    // exclusive and still rebinds over TIME_WAIT.
    // Reported rather than ignored: this callback replaces httplib's default, so a silent
    // failure leaves the socket with neither option, and the symptom is a bind refused over
    // TIME_WAIT after a restart -- which reads as a stale socket, not as this.
    m_server->set_socket_options([](socket_t sock) {
        int on = 1;
        if (::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const void*>(&on),
                         static_cast<socklen_t>(sizeof(on))) != 0) {
            std::fprintf(stderr, "wallpaperx-daemon: could not set SO_REUSEADDR (%s)\n",
                         std::strerror(errno));
        }
    });

    // httplib catches a throwing handler either way, but without this it answers 500 with an
    // empty body. This adds the daemon's own JSON error and the stderr line -- so a check
    // asserting only the status would pass with this deleted.
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

    if (!m_server->bind_to_port(kLoopbackHost, m_port)) {
        const std::string bindError = std::string("could not bind ") + kLoopbackHost + ":" +
                                      std::to_string(m_port) + " (is another process using it?)";
        std::fprintf(stderr,
                     "wallpaperx-daemon: %s -- the KDE plugin will not be able to read config; "
                     "DBus and rendering are unaffected\n",
                     bindError.c_str());
        m_manager.recordConfigServer(false, bindError);
        return;
    }

    // Recorded here, not by the caller: the serving thread below is the only other writer,
    // and starting it after this write is what orders the two.
    m_manager.recordConfigServer(true, {});

    m_thread = std::thread([this] {
        // false only when httplib hit an accept error outside EMFILE/EINTR/EAGAIN and
        // closed its own listening socket; a deliberate stop() leaves it true. Without
        // this the port can close while GetStatus still reports config_server ok, which
        // is the one thing a client cannot find out any other way.
        const bool clean = m_server->listen_after_bind();
        if (!clean && !m_stopping.load(std::memory_order_acquire)) {
            m_manager.recordConfigServer(
                false, std::string("stopped serving ") + kLoopbackHost + ":" +
                           std::to_string(m_port) + " (accept failed; the port is closed)");
        }
    });

    // httplib gates stop() on is_running_, which the serving thread sets only once it
    // reaches its accept loop -- destroy before that and stop() no-ops, then join() waits
    // forever.
    m_server->wait_until_ready();
    std::printf("wallpaperx-daemon: config server on http://%s:%d\n", kLoopbackHost, m_port);
    std::fflush(stdout);
}

HttpServer::~HttpServer() {
    m_stopping.store(true, std::memory_order_release);
    m_server->stop();
    if (m_thread.joinable()) m_thread.join();
}

void HttpServer::registerRoutes() {
    // `t` is ignored: it is QML's cache-buster and means nothing to the daemon.
    m_server->Get("/config", [this](const httplib::Request& req, httplib::Response& res) {
        IdentityKind kind = IdentityKind::Default;
        std::string outputName;
        std::string error;
        if (!parseIdentity(req, kind, outputName, error)) {
            sendJson(res, nlohmann::json{{"error", error}}, 400);
            return;
        }

        int monitorId = 0;
        if (kind != IdentityKind::Output && !parseMonitorParam(req, monitorId, error)) {
            sendJson(res, nlohmann::json{{"error", error}}, 400);
            return;
        }

        const WallpaperManager::Snapshot snap = m_manager.snapshot();

        // Emitted only for output= queries: a range-checked id always resolves, so adding
        // the field elsewhere would change a shape that has consumers for a constant.
        bool known = true;
        if (kind == IdentityKind::Output) {
            const auto bound = snap.outputs.find(outputName);
            known = bound != snap.outputs.end();
            if (known) monitorId = bound->second;
        }

        // An unset monitor answers with empty strings rather than 404: the plugin polls
        // every second from startup, and "no wallpaper yet" is ordinary, not an error.
        nlohmann::json out{
            {"monitor_id", monitorId}, {"type", ""}, {"path", ""}, {"path_literal", ""}};

        if (kind == IdentityKind::Output) {
            out["known"] = known;
            out["output"] = sanitizeForLog(outputName, kMaxOutputNameBytes, LogFraming::Bare);
            if (!known) {
                // Null, never 0: an id the caller could hand back to ?monitor= would put
                // monitor 0's wallpaper on a screen that is not monitor 0.
                out["monitor_id"] = nullptr;
                sendJson(res, out);
                return;
            }
        }

        const auto it = snap.monitors.find(monitorId);
        if (it != snap.monitors.end()) {
            out["type"] = it->second.type;
            // Must stay the same decision as GetStatus: the same client string escaped on
            // one channel and raw on another is how the two views diverged. `path` is
            // display-safe; `path_literal` is the one to act on, null when not UTF-8.
            out["path"] = sanitizeForLog(it->second.path, 4096, LogFraming::Bare);
            const std::optional<std::string> literal = literalPath(it->second.path);
            out["path_literal"] = literal ? nlohmann::json(*literal) : nlohmann::json(nullptr);
        }

        sendJson(res, out);
    });

    // GET /health -- what is currently broken, not merely whether this thread answers.
    m_server->Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        const WallpaperManager::Snapshot snap = m_manager.snapshot();

        // Split the same four ways GetFrameFd's NoFrameReason splits, so the two channels
        // cannot disagree about whether a monitor has a picture on it.
        std::size_t failed = 0, unsupported = 0, showing = 0, daemonFaults = 0, outstanding = 0;
        std::string firstFailure, firstUnsupported, firstDaemonFault;

        // Per monitor as well as counted: the aggregate says a wallpaper is missing, this
        // says which one and why, so a frontend need not call GetStatus to find out.
        nlohmann::json perMonitor = nlohmann::json::object();
        for (const auto& [id, monitor] : snap.monitors) {
            const std::string key = std::to_string(id);
            if (!monitor.lastError.empty()) {
                if (failed++ == 0) firstFailure = monitor.lastError;
                if (monitor.lastErrorIsDaemonFault && daemonFaults++ == 0) {
                    firstDaemonFault = monitor.lastError;
                }
                // Sanitized where it was recorded, so it is safe to quote here.
                perMonitor[key] = {{"state", "failed"}, {"detail", monitor.lastError}};
            } else if (!isRenderableType(monitor.type)) {
                // Type is whitelisted at requestWallpaper, so it is safe to quote into JSON.
                const std::string why = "monitor " + std::to_string(id) + " is set to a '" + monitor.type +
                                        "' wallpaper, which this build cannot render";
                if (unsupported++ == 0) firstUnsupported = why;
                perMonitor[key] = {{"state", "unsupported"}, {"detail", why}};
            } else if (monitor.hasFrame) {
                ++showing;
                perMonitor[key] = {{"state", "showing"}, {"detail", ""}};
            } else {
                // Every monitor reaching here has an outstanding render: no frame is only
                // ever set alongside a bumped request counter, and every path that marks a
                // request serviced leaves an error, a frame, or an unsupported type. Kept as
                // a count rather than a bool because a fourth render outcome -- one that
                // finishes with neither frame nor error -- would make the two differ again.
                if (monitor.renderOutstanding) ++outstanding;
                perMonitor[key] = {{"state", "rendering"}, {"detail", ""}};
            }
        }

        // Carried into the in-flight branch so a wedged render does not silence a failure
        // the same daemon is also reporting.
        const std::string failureNote =
            (failed > 0 || unsupported > 0)
                ? "; " + std::to_string(failed + unsupported) + " failed or cannot be rendered: " +
                      (failed > 0 ? firstFailure : firstUnsupported)
                : std::string();

        HealthCheck wallpapers;
        if (snap.monitors.empty()) {
            // Ok: nothing was tried because nothing was asked for. Pending would claim the
            // daemon has not finished something it started.
            wallpapers.status = HealthStatus::Ok;
            wallpapers.detail = "no wallpaper set";
        } else if (daemonFaults > 0) {
            // Fail: something inside the daemon went wrong and no file the user can replace
            // will fix it. Reported here rather than against gpu or shaders, because the
            // fault may belong to neither -- attributing it to a named dependency would
            // accuse a component that is working.
            wallpapers.status = HealthStatus::Fail;
            wallpapers.detail = std::to_string(daemonFaults) + " of " +
                                std::to_string(snap.monitors.size()) +
                                " monitors failed inside the daemon: " + firstDaemonFault;
        } else if (outstanding > 0) {
            // Ahead of the failure branch, not after it: ordering this second meant one
            // monitor with a bad image hid a render that never finished on every other
            // monitor. Keyed on an accepted request no pass has run for, not on "has no
            // frame" -- the second is also true of a monitor nobody is waiting on, and
            // promoting that would make an idle daemon report itself unfinished.
            wallpapers.status = HealthStatus::Pending;
            wallpapers.detail = std::to_string(outstanding) + " of " +
                                std::to_string(snap.monitors.size()) +
                                " monitors have not finished rendering" + failureNote;
        } else if (failed > 0 || unsupported > 0) {
            // Ok, not Fail: a deleted file or a type this build cannot draw is that
            // wallpaper's problem, not the daemon's, and the daemon cannot fix either. The
            // detail carries the cause so an operator need not go read GetStatus.
            wallpapers.status = HealthStatus::Ok;
            wallpapers.detail = std::to_string(failed + unsupported) + " of " +
                                std::to_string(snap.monitors.size()) +
                                " monitors failed or cannot be rendered: " +
                                (failed > 0 ? firstFailure : firstUnsupported);
        } else {
            wallpapers.status = HealthStatus::Ok;
            wallpapers.detail = std::to_string(showing) + " monitor(s) showing a frame";
        }

        const std::array<std::pair<const char*, const HealthCheck*>, 4> checks{{
            {"shaders", &snap.health.shaders},
            {"gpu", &snap.health.gpu},
            {"wallpapers", &wallpapers},
            {"persistence", &snap.health.persistence},
        }};

        // Failures only. Pending means a dependency nothing has needed yet, and a component
        // being idle is not a fault of the daemon -- the renderer is created on the first
        // render by design, so propagating it made an untouched daemon report itself
        // unhealthy. Nothing is hidden by this: the component's own entry still says pending,
        // and a reader wanting "has everything been exercised" reads the entries.
        HealthStatus overall = HealthStatus::Ok;
        nlohmann::json checksJson = nlohmann::json::object();
        for (const auto& [name, check] : checks) {
            if (check->status == HealthStatus::Fail) overall = HealthStatus::Fail;
            checksJson[name] = {{"status", toString(check->status)}, {"detail", check->detail}};
        }
        // The exception to "failures only": a render that was started and never finished is
        // the daemon owing an answer, not a dependency sitting idle. Without this a wedged
        // render worker reports ok forever.
        if (wallpapers.status == HealthStatus::Pending && overall == HealthStatus::Ok) {
            overall = HealthStatus::Pending;
        }
        checksJson["wallpapers"]["monitors"] = std::move(perMonitor);

        sendJson(res, {{"status", toString(overall)},
                       {"version", WALLPAPERX_VERSION},
                       {"checks", checksJson}});
    });
}

}  // namespace wallpaperx
