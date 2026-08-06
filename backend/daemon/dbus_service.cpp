#include "dbus_service.h"

#include "log_escape.h"
#include "wallpaper_manager.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <optional>
#include <utility>

namespace wallpaperx {

namespace {

// Error names the client can branch on. NoFrame is an ordinary state (nothing has
// been set on that monitor yet), not a malfunction -- keeping it distinct from
// ExportFailed is what lets a polling consumer tell "not ready" from "broken".
constexpr const char* kErrorNoFrame = "io.wallpaperx.Daemon.Error.NoFrame";
constexpr const char* kErrorExportFailed = "io.wallpaperx.Daemon.Error.ExportFailed";
constexpr const char* kErrorRenderFailed = "io.wallpaperx.Daemon.Error.RenderFailed";
// The third answer to "why is there no frame", distinct from the two above because it
// calls for a third response: NoFrame means retry, RenderFailed means stop and show the
// reason, Unsupported means stop and stop asking -- nothing about this build will change
// while it runs. It was previously collapsed into NoFrame, so a client polling a video
// wallpaper was told to retry forever.
constexpr const char* kErrorUnsupported = "io.wallpaperx.Daemon.Error.Unsupported";
constexpr const char* kErrorInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

}  // namespace

DbusService::DbusService(sdbus::IConnection& connection, WallpaperManager& manager)
    : m_manager(manager),
      m_object(sdbus::createObject(connection, sdbus::ObjectPath{kObjectPath})),
      m_worker(manager, [this](int monitorId, uint64_t generation) { emitFrameReady(monitorId, generation); }) {
    // The worker starts idle -- it can only do anything once a handler posts to it, and
    // no handler can be dispatched until the vtable below is registered.
    registerVTable();
}

DbusService::~DbusService() {
    // Explicit, because neither destruction order alone can be correct. Handlers run on
    // the event-loop thread, which may still be dispatching while this destructor runs
    // (any daemon shut down from a signal handler, and the tests' enterEventLoopAsync):
    // setWallpaper() posts to m_worker, and getFrameFd's task touches m_object, so
    // destroying either one first leaves the other reachable through a dangling
    // reference. stop() breaks the cycle: it joins the render thread, after which
    // nothing can reach m_object, and it leaves the worker object alive and its post()
    // methods harmless no-ops for any handler still in flight.
    m_worker.stop();
}

void DbusService::registerVTable() {
    m_object
        ->addVTable(
            sdbus::registerMethod("SetWallpaper")
                .implementedAs([this](const std::string& type, const std::string& path, int32_t monitorId) {
                    setWallpaper(type, path, monitorId);
                })
                .withInputParamNames("type", "path", "monitor_id"),
            sdbus::registerMethod("GetFrameFd")
                // Async: the handler hands the export to the GPU thread and returns, so
                // the event loop keeps serving while the frame is produced.
                .implementedAs([this](FrameFdCall&& call, int32_t monitorId) {
                    m_worker.postTask(
                        [this, call = std::move(call), monitorId]() mutable {
                            completeGetFrameFd(std::move(call), monitorId);
                        });
                })
                .withInputParamNames("monitor_id")
                .withOutputParamNames("fd", "width", "height", "memory_size", "handle_type", "sync_fds",
                                      "frame_generation", "drm_format", "drm_modifier", "planes"),
            sdbus::registerMethod("GetStatus").implementedAs([this]() { return getStatus(); }).withOutputParamNames("status"),
            // frame_generation rides along with the monitor id. A bare FrameReady(monitor)
            // was unambiguous only while rendering was synchronous; now that several
            // requests for one monitor can be in flight, a consumer receiving the signal
            // otherwise cannot tell which frame it is being told about, nor whether the
            // one it already holds is the one being announced.
            sdbus::registerSignal("FrameReady").withParameters<int32_t, uint64_t>("monitor_id", "frame_generation"))
        .forInterface(std::string(kInterface));
}

void DbusService::emitFrameReady(int monitorId, uint64_t generation) {
    // Called on the render worker's thread. sdbus-c++ documents signal emission on
    // IObject as thread-safe by design, so this needs no hop back to the event loop.
    try {
        m_object->emitSignal("FrameReady")
            .onInterface(kInterface)
            .withArguments(static_cast<int32_t>(monitorId), generation);
    } catch (const std::exception& e) {
        // A failed emit must not escape into the worker thread, where it would be
        // std::terminate. The frame itself is fine and GetFrameFd will still serve it;
        // only the notification was lost, which a polling consumer recovers from.
        std::fprintf(stderr, "wallpaperx-daemon: failed to emit FrameReady(monitor=%d, generation=%llu): %s\n",
                     monitorId, static_cast<unsigned long long>(generation), e.what());
    }
}

void DbusService::setWallpaper(const std::string& type, const std::string& path, int32_t monitorId) {
    // Logged on entry, not on success. Most DBus clients drop an error reply on the
    // floor, so the daemon's log is the only place a failed request is visible -- and
    // logging after the call meant the requests that failed were exactly the ones that
    // left no trace, making them indistinguishable from requests that never arrived.
    //
    // Both strings come straight off the wire, so both go through sanitizeForLog: a path
    // containing newlines used to be printed verbatim, which let any client that could
    // reach the bus write whole lines of its own choosing into this log in the daemon's
    // own format. See log_escape.h.
    std::printf("wallpaperx-daemon: SetWallpaper(type=%s, path=%s, monitor=%d)\n",
                sanitizeForLog(type).c_str(), sanitizeForLog(path).c_str(), monitorId);
    std::fflush(stdout);

    try {
        m_manager.requestWallpaper(type, path, monitorId);
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetWallpaper rejected (monitor=%d): %s\n", monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetWallpaper failed (monitor=%d): %s\n", monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }

    // Posted for every accepted type, not just "static": video and shader requests still
    // need their monitor's target released, and that is GPU work, so it belongs on the
    // worker rather than in this handler.
    m_worker.post(monitorId);
}

void DbusService::completeGetFrameFd(FrameFdCall call, int32_t monitorId) {
    // Runs on the render worker's thread, so nothing here may throw past this frame --
    // an escaping exception would be std::terminate on a bare thread. Every path below
    // either returns results or returns an error on `call`.
    FrameExport exported;
    try {
        exported = m_manager.exportCurrentFrame(monitorId);
    } catch (const InvalidRequest& e) {
        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what()));
        return;
    } catch (const std::exception& e) {
        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorExportFailed}, e.what()));
        return;
    }

    if (!exported.handle) {
        // RenderFailed and NoFrame are answered separately because a polling consumer
        // must respond to them differently: NoFrame means ask again, RenderFailed means
        // stop asking and show the reason. Both used to come back as NoFrame carrying
        // the text "call SetWallpaper first" -- advice that is not merely unhelpful for
        // a wallpaper that already failed, but tells the client to retry a request that
        // cannot succeed until a *different* one replaces it. The reply also carries the
        // render's own error text, which was otherwise reachable only through GetStatus.
        if (exported.reason == NoFrameReason::RenderFailed) {
            call.returnError(sdbus::Error(sdbus::Error::Name{kErrorRenderFailed},
                                          "the last render for monitor " + std::to_string(monitorId) +
                                              " failed: " + exported.lastError));
            return;
        }
        if (exported.reason == NoFrameReason::Unsupported) {
            // No lastError appended: this is not a failure, nothing wrote one, and the
            // name plus this sentence already state the whole fact. The type is not
            // echoed either -- it is in the client's own request and in GetStatus, and
            // repeating client-supplied text into a reply is the habit that produced the
            // log-injection this commit closed.
            call.returnError(sdbus::Error(sdbus::Error::Name{kErrorUnsupported},
                                          "monitor " + std::to_string(monitorId) +
                                              " holds a wallpaper type this build cannot render, so no "
                                              "frame will ever be produced for it"));
            return;
        }
        call.returnError(
            sdbus::Error(sdbus::Error::Name{kErrorNoFrame},
                         "no frame available for monitor " + std::to_string(monitorId) +
                             (exported.reason == NoFrameReason::NeverRequested
                                  ? " (call SetWallpaper first)"
                                  : " yet -- a request is recorded but no frame has been produced for it")));
        return;
    }

    const ExportedFrame& frame = exported.handle->frame;

    // adopt_fd, not the duplicating constructor: exportFrame() handed us ownership of a
    // freshly created fd and nothing else will close it. Taken on the first line after
    // the export so that any throw below runs ~UnixFd instead of leaking a descriptor --
    // GetFrameFd is called repeatedly by a polling consumer, so a leak here is an
    // eventual EMFILE, not a one-off.
    sdbus::UnixFd fd(frame.fd, sdbus::adopt_fd);

    std::vector<sdbus::Struct<uint64_t, uint64_t>> planes;
    planes.reserve(frame.planes.size());
    for (const ExportedFramePlane& plane : frame.planes) {
        planes.emplace_back(plane.offset, plane.rowPitch);
    }

    call.returnResults(fd, frame.width, frame.height, frame.memorySize,
                       frame.type == ExportedFrame::HandleType::DmaBuf ? 1 : 0,
                       std::vector<sdbus::UnixFd>{},  // sync_fds: reserved, always empty today
                       exported.handle->generation, frame.drmFormat, frame.modifier, planes);
}

std::string DbusService::getStatus() {
    nlohmann::json status;
    status["running"] = true;

    // The GPU block is absent until something has actually needed the GPU -- the Device
    // is created on the first render, so reporting a tier before then would be a guess.
    // "initialized" says which of those two states this is; a client that finds it false
    // has not hit a problem, it has just caught the daemon before its first wallpaper.
    //
    // One snapshot for both halves: reading the GPU block and the monitor map under
    // separate locks would let a render commit in between and produce a reply claiming
    // no GPU beside a monitor that already has a frame.
    const WallpaperManager::Snapshot snap = m_manager.snapshot();
    const GpuStatus& gpu = snap.gpu;
    nlohmann::json gpuJson{{"initialized", gpu.initialized}};
    if (gpu.initialized) {
        gpuJson["name"] = gpu.name;
        // Which descriptor tier a machine resolved to is the first thing worth knowing
        // when a bug reproduces on one GPU and not another.
        gpuJson["descriptor_tier"] = gpu.descriptorTier;
    }
    status["gpu"] = std::move(gpuJson);

    nlohmann::json wallpapers = nlohmann::json::object();
    for (const auto& [monitorId, monitor] : snap.monitors) {
        // `path` is sanitized on the way out, not stored sanitized: the recorded path has
        // to stay byte-exact because it is what actually gets opened. JSON escaping is not
        // a substitute -- nlohmann escapes bytes below 0x20, but not U+202E, not the
        // isolates, not DEL, which are exactly the code points that let a client decide how
        // the value *renders*. Without this, `busctl call ... GetStatus` could show an
        // administrator a different path than the one on disk, while `last_error` fifteen
        // lines below -- carrying the same client string -- was escaped correctly. Two
        // views of one value, disagreeing.
        //
        // This makes `path` display-safe rather than round-trip-safe: a frontend must not
        // feed it back into SetWallpaper. Frontends set the wallpaper, so they already hold
        // the unescaped path they sent; nothing in this tree reads it back.
        //
        // `type` needs no such treatment and gets none. requestWallpaper rejects anything
        // outside {static, video, shader} before storing (wallpaper_manager.cpp:28), so it
        // is one of three literals rather than client text. Sanitizing it anyway would
        // imply a doubt the validation already settles.
        nlohmann::json entry{{"type", monitor.type},
                             {"path", sanitizeForLog(monitor.path, 4096, LogFraming::Bare)},
                             {"generation", monitor.generation},
                             {"has_frame", monitor.hasFrame}};
        if (monitor.hasFrame) {
            entry["width"] = monitor.width;
            entry["height"] = monitor.height;
        }
        // Present only when the last request failed. Since SetWallpaper now replies
        // before the render is attempted, this is the only place a render failure is
        // reported at all -- a client that gets a successful reply and then no
        // FrameReady looks here to find out why.
        if (!monitor.lastError.empty()) {
            entry["last_error"] = monitor.lastError;
        }
        // The other reason a successful SetWallpaper never produces a FrameReady, and it
        // is not a failure, so it does not belong in last_error: this build has no
        // renderer for the type. Its own key, so a frontend can say "not supported yet"
        // rather than painting a broken-image state, and so the two cases stay
        // distinguishable without string-matching an error message. Emitted always rather
        // than only when true -- a key that appears only in the bad case is one a client
        // learns about by hitting it. Mirrors GetFrameFd's Unsupported through the same
        // isRenderableType(), so the two answers cannot disagree.
        entry["unsupported"] = !isRenderableType(monitor.type);
        wallpapers[std::to_string(monitorId)] = std::move(entry);
    }
    status["wallpapers"] = wallpapers;

    return status.dump();
}

}
