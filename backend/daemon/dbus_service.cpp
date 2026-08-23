// SPDX-License-Identifier: GPL-2.0-or-later

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
// The third answer to "why is there no frame", separate because it calls for a third
// response: NoFrame means retry, RenderFailed means stop and show the reason, Unsupported
// means stop asking -- nothing about this build changes while it runs.
constexpr const char* kErrorUnsupported = "io.wallpaperx.Daemon.Error.Unsupported";
constexpr const char* kErrorShuttingDown = "io.wallpaperx.Daemon.Error.ShuttingDown";
constexpr const char* kErrorBusy = "io.wallpaperx.Daemon.Error.Busy";
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
    // Explicit, because neither destruction order alone is correct: handlers may still be
    // dispatching, setWallpaper() posts to m_worker and getFrameFd's task touches m_object, so
    // destroying either first leaves the other dangling. stop() joins the render thread, then
    // leaves post() a no-op for any handler still in flight.
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
                        [this, call = std::move(call),
                         monitorId](wallpaperx::RenderWorker::TaskState state) mutable {
                            // Answered rather than dropped: destroying the Result sends
                            // nothing, leaving a GUI client blocked for the full reply
                            // timeout on a request that will never run.
                            switch (state) {
                                case RenderWorker::TaskState::ShuttingDown:
                                    call.returnError(
                                        sdbus::Error(sdbus::Error::Name{kErrorShuttingDown},
                                                     "the daemon is shutting down"));
                                    return;
                                case RenderWorker::TaskState::Busy:
                                    call.returnError(
                                        sdbus::Error(sdbus::Error::Name{kErrorBusy},
                                                     "too many frame exports are already "
                                                     "waiting; retry"));
                                    return;
                                case RenderWorker::TaskState::Run:
                                    break;
                            }
                            completeGetFrameFd(std::move(call), monitorId);
                        });
                })
                .withInputParamNames("monitor_id")
                .withOutputParamNames("fd", "width", "height", "memory_size", "handle_type", "sync_fds",
                                      "frame_generation", "drm_format", "drm_modifier", "planes"),
            sdbus::registerMethod("SetMonitorOutput")
                .implementedAs([this](int32_t monitorId, const std::string& outputName) {
                    setMonitorOutput(monitorId, outputName);
                })
                .withInputParamNames("monitor_id", "output_name"),
            sdbus::registerMethod("GetStatus").implementedAs([this]() { return getStatus(); }).withOutputParamNames("status"),
            // frame_generation rides along with the monitor id. A bare FrameReady(monitor)
            // was unambiguous only while rendering was synchronous; now that several
            // requests for one monitor can be in flight, a consumer receiving the signal
            // otherwise cannot tell which frame it is being told about, nor whether the
            // one it already holds is the one being announced.
            sdbus::registerSignal("FrameReady").withParameters<int32_t, uint64_t>("monitor_id", "frame_generation"))
        .forInterface(std::string(kInterface));
}

void DbusService::restoreSaved() {
    for (const int monitorId : m_manager.restoreSaved()) {
        m_worker.post(monitorId);
    }
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
    // On entry, not on success: most clients drop the error reply, so logging afterwards
    // left failed requests indistinguishable from ones that never arrived. Both strings
    // come off the wire, hence sanitizeForLog -- see log_escape.h.
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

void DbusService::setMonitorOutput(int32_t monitorId, const std::string& outputName) {
    std::printf("wallpaperx-daemon: SetMonitorOutput(monitor=%d, output=%s)\n", monitorId,
                sanitizeForLog(outputName).c_str());
    std::fflush(stdout);

    try {
        m_manager.setMonitorOutput(monitorId, outputName);
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetMonitorOutput rejected (monitor=%d): %s\n",
                     monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    } catch (const std::exception& e) {
        // As SetWallpaper does: the save behind this can throw on a machine out of threads
        // or memory, and letting that escape the callback tells the client nothing.
        std::fprintf(stderr, "wallpaperx-daemon: SetMonitorOutput failed (monitor=%d): %s\n",
                     monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }

    // No m_worker.post(): naming a screen changes nothing about what it draws, so there is
    // no render to schedule and no target to release.
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
        // Answered separately because a polling consumer must respond differently: NoFrame
        // means ask again, RenderFailed means stop asking and show the reason. The reply
        // carries the render's own error text, otherwise reachable only through GetStatus.
        if (exported.reason == NoFrameReason::RenderFailed) {
            call.returnError(sdbus::Error(sdbus::Error::Name{kErrorRenderFailed},
                                          "the last render for monitor " + std::to_string(monitorId) +
                                              " failed: " + exported.lastError));
            return;
        }
        if (exported.reason == NoFrameReason::Unsupported) {
            // No lastError: this is not a failure and nothing wrote one. The type is not
            // echoed either -- it is in the client's own request, and repeating client text
            // into a reply is the habit that produces log injection.
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

    // adopt_fd, not the duplicating constructor: exportFrame() handed over ownership of a fresh
    // fd and nothing else closes it. Taken on the first line after the export so a throw below
    // runs ~UnixFd -- a polling consumer calls this repeatedly, so a leak becomes EMFILE.
    sdbus::UnixFd fd(frame.fd, sdbus::adopt_fd);

    std::vector<sdbus::Struct<uint64_t, uint64_t>> planes;
    planes.reserve(frame.planes.size());
    for (const ExportedFramePlane& plane : frame.planes) {
        planes.emplace_back(plane.offset, plane.rowPitch);
    }

    // The one line that shows a consumer actually took a frame. Without it the log records
    // wallpapers being set and nothing collecting them, so a plugin that never fetched and one
    // that fetched and drew read identically -- which cost a whole session of misreading a
    // silent log as a failure.
    std::printf("wallpaperx-daemon: GetFrameFd(monitor=%d) served generation %llu as %s\n", monitorId,
                static_cast<unsigned long long>(exported.handle->generation),
                frame.type == ExportedFrame::HandleType::DmaBuf ? "dma_buf" : "opaque fd");
    std::fflush(stdout);

    call.returnResults(fd, frame.width, frame.height, frame.memorySize,
                       frame.type == ExportedFrame::HandleType::DmaBuf ? 1 : 0,
                       // sync_fds: empty because the export waits on a fence before returning,
                       // so the frame is complete by the time this reply is built. Reserved for
                       // an async export; a client that cannot wait must refuse a non-empty one
                       // rather than sample a half-drawn frame.
                       std::vector<sdbus::UnixFd>{},
                       exported.handle->generation, frame.drmFormat, frame.modifier, planes);
}

std::string DbusService::getStatus() {
    nlohmann::json status;
    status["running"] = true;

    // One snapshot for both halves: reading the GPU block and the monitor map under
    // separate locks would let a render commit between them and produce a reply claiming
    // no GPU beside a monitor that already has a frame.
    const WallpaperManager::Snapshot snap = m_manager.snapshot();
    const GpuStatus& gpu = snap.gpu;
    nlohmann::json gpuJson{{"initialized", gpu.initialized}};
    if (gpu.initialized) {
        // Escaped like every other string that came from outside this process. The driver
        // is a likelier source of an odd byte than of an attack, but a client should not
        // have to know which published strings are safe and which are not.
        gpuJson["name"] = sanitizeForLog(gpu.name, 1024, LogFraming::Bare);
        // Which descriptor tier a machine resolved to is the first thing worth knowing
        // when a bug reproduces on one GPU and not another.
        gpuJson["descriptor_tier"] = gpu.descriptorTier;
    }
    status["gpu"] = std::move(gpuJson);

    // The one dependency /health cannot report on: if the bind failed, something else is
    // answering that port and will say whatever it likes.
    status["config_server"] = {{"status", toString(snap.health.configServer.status)},
                               {"detail", snap.health.configServer.detail}};

    status["persistence"] = {{"status", toString(snap.health.persistence.status)},
                             {"detail", snap.health.persistence.detail}};

    nlohmann::json wallpapers = nlohmann::json::object();
    for (const auto& [monitorId, monitor] : snap.monitors) {
        // Sanitized on the way out, not in storage: the recorded path is what gets opened and
        // must stay byte-exact. JSON escaping is no substitute -- nlohmann escapes below 0x20
        // but not U+202E, the isolates or DEL, so `busctl` could show an admin a path other
        // than the one on disk. `type` needs none; requestWallpaper allows three literals.
        //
        // path_literal is the field to act on: the bytes as stored, handed straight back to
        // SetWallpaper. Null rather than absent when they are not valid UTF-8, since an absent
        // key is indistinguishable from an older daemon.
        const std::optional<std::string> literal = literalPath(monitor.path);
        nlohmann::json entry{{"type", monitor.type},
                             {"path", sanitizeForLog(monitor.path, 4096, LogFraming::Bare)},
                             {"path_literal", literal ? nlohmann::json(*literal) : nlohmann::json(nullptr)},
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
        // The other reason a successful SetWallpaper never yields a FrameReady, and not a
        // failure, so it gets its own key rather than last_error. Always emitted: a key
        // that appears only in the bad case is one a client learns about by hitting it.
        // Shares isRenderableType() with GetFrameFd so the two cannot disagree.
        entry["unsupported"] = !isRenderableType(monitor.type);
        wallpapers[std::to_string(monitorId)] = std::move(entry);
    }
    status["wallpapers"] = wallpapers;

    // Its own object, not a field on each wallpaper: a screen can be named before anything
    // is chosen for it, leaving no wallpaper entry to carry the name. The cap is the bound
    // the name was accepted under, so nothing truncates -- truncation is the only way
    // escaping could map two names onto one key and drop a screen from here.
    nlohmann::json outputs = nlohmann::json::object();
    nlohmann::json outputsLiteral = nlohmann::json::object();
    for (const auto& [name, monitorId] : snap.outputs) {
        outputs[sanitizeForLog(name, kMaxOutputNameBytes, LogFraming::Bare)] = monitorId;
        // Keyed by the bytes the client sent, because the escaped key above is a display
        // form and matches nothing the caller holds. Safe as a key: outputNameError()
        // refused invalid UTF-8 before the name was ever stored, so dump() cannot throw.
        outputsLiteral[name] = monitorId;
    }
    status["outputs"] = std::move(outputs);
    status["outputs_literal"] = std::move(outputsLiteral);

    return status.dump();
}

}
