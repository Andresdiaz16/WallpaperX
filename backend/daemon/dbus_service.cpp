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
// The fourth: there is a frame, but handing it over would leave the daemon nowhere to draw the
// next one. Retryable like NoFrame, and the daemon says when -- a FrameReady follows every
// release and every commit, so a consumer that waits for one is not polling.
constexpr const char* kErrorNoFreeImage = "io.wallpaperx.Daemon.Error.NoFreeImage";
constexpr const char* kErrorShuttingDown = "io.wallpaperx.Daemon.Error.ShuttingDown";
constexpr const char* kErrorBusy = "io.wallpaperx.Daemon.Error.Busy";
// A size for a screen no monitor answers to. Not InvalidArgs: nothing in the call was wrong, and
// the same call succeeds once SetMonitorOutput has named the screen.
constexpr const char* kErrorUnknownOutput = "io.wallpaperx.Daemon.Error.UnknownOutput";
constexpr const char* kErrorInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

}  // namespace

DbusService::DbusService(sdbus::IConnection& connection, WallpaperManager& manager)
    : m_manager(manager),
      m_object(sdbus::createObject(connection, sdbus::ObjectPath{kObjectPath})),
      m_worker(manager, [this](int monitorId, uint64_t generation) { emitFrameReady(monitorId, generation); }) {
    // The worker starts idle -- it can only do anything once a handler posts to it, and
    // no handler can be dispatched until the vtable below is registered.
    registerVTable();
    watchClientExits();
}

void DbusService::watchClientExits() {
    m_busWatch = sdbus::createProxy(m_object->getConnection(), sdbus::ServiceName{"org.freedesktop.DBus"},
                                    sdbus::ObjectPath{"/org/freedesktop/DBus"});
    m_busWatch->uponSignal("NameOwnerChanged")
        .onInterface("org.freedesktop.DBus")
        .call([this](const std::string& name, const std::string& oldOwner, const std::string& newOwner) {
            // Only a departure. The same signal announces arrivals and well-known-name handovers,
            // and reacting to those would drop a live consumer's set.
            if (!oldOwner.empty() && newOwner.empty()) {
                // A monitor the departed consumer's set left with no frame can be drawn again
                // now that the constraint is gone, and this is the only thing that will retry it.
                applyImportModifierUpdate(m_manager.forgetImportModifiers(name));

                // Likewise its claim on the images it was reading. A consumer that crashed
                // holding a frame -- plasmashell restarts are routine, this daemon does not --
                // would otherwise keep those images out of the ring for good.
                std::vector<FrameAvailable> available;
                for (const int monitorId : m_manager.forgetHeldBuffers(name, &available)) m_worker.post(monitorId);
                announceAvailableFrames(available);
            }
        });
}

DbusService::~DbusService() {
    // Explicit, because neither destruction order alone is correct: handlers may still be
    // dispatching, setWallpaper() posts to m_worker and getFrameFd's task touches m_object, so
    // destroying either first leaves the other dangling. stop() joins the render thread, then
    // leaves post() a no-op for any handler still in flight.
    //
    // The bus watch goes first: its callback reaches into the manager, and it must stop firing
    // before anything else is torn down.
    m_busWatch.reset();
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
                    // Read here and carried, not read on the worker: the currently-processed
                    // message is only current inside the handler.
                    const char* sender = m_object->getCurrentlyProcessedMessage().getSender();
                    m_worker.postTask(
                        [this, call = std::move(call), monitorId, client = std::string(sender ? sender : "")](
                            wallpaperx::RenderWorker::TaskState state) mutable {
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
                            completeGetFrameFd(std::move(call), monitorId, std::move(client));
                        });
                })
                .withInputParamNames("monitor_id")
                .withOutputParamNames("fd", "width", "height", "memory_size", "handle_type", "sync_fds",
                                      "frame_generation", "drm_format", "drm_modifier", "planes", "device_uuid",
                                      "buffer_id"),
            sdbus::registerMethod("ReleaseBuffer")
                .implementedAs([this](int32_t monitorId, uint64_t bufferId) {
                    const char* sender = m_object->getCurrentlyProcessedMessage().getSender();
                    releaseBuffer(sender ? sender : "", monitorId, bufferId);
                })
                .withInputParamNames("monitor_id", "buffer_id"),
            sdbus::registerMethod("SetImportModifiers")
                .implementedAs([this](uint32_t drmFormat, const std::vector<uint64_t>& modifiers) {
                    // The sender's unique name, not anything it could put in the arguments: the
                    // bus assigns it, so a consumer cannot report on another's behalf, and it is
                    // what NameOwnerChanged later names when the reporter exits.
                    const char* sender = m_object->getCurrentlyProcessedMessage().getSender();
                    setImportModifiers(sender ? sender : "", drmFormat, modifiers);
                })
                .withInputParamNames("drm_format", "modifiers"),
            sdbus::registerMethod("SetMonitorOutput")
                .implementedAs([this](int32_t monitorId, const std::string& outputName) {
                    setMonitorOutput(monitorId, outputName);
                })
                .withInputParamNames("monitor_id", "output_name"),
            sdbus::registerMethod("SetOutputSize")
                .implementedAs([this](const std::string& outputName, uint32_t width, uint32_t height) {
                    setOutputSize(outputName, width, height);
                })
                .withInputParamNames("output_name", "width", "height"),
            sdbus::registerMethod("SetOutputVisible")
                .implementedAs([this](const std::string& outputName, bool visible) {
                    setOutputVisible(outputName, visible);
                })
                .withInputParamNames("output_name", "visible"),
            sdbus::registerMethod("GetStatus").implementedAs([this]() { return getStatus(); }).withOutputParamNames("status"),
            // frame_generation rides along with the monitor id. A bare FrameReady(monitor)
            // was unambiguous only while rendering was synchronous; now that several
            // requests for one monitor can be in flight, a consumer receiving the signal
            // otherwise cannot tell which frame it is being told about, nor whether the
            // one it already holds is the one being announced.
            sdbus::registerSignal("FrameReady").withParameters<int32_t, uint64_t>("monitor_id", "frame_generation"),
            // No parameters: the only thing a consumer can do with it is ask again, and telling
            // it what the set now is would be telling it what the OTHER consumers reported.
            //
            // Exists because a report refused for sharing no layout with another consumer is not
            // refused for anything the caller can fix -- and a consumer that retried on its own
            // schedule would have to poll. This says "the answer may have changed", once.
            sdbus::registerSignal("ImportModifiersChanged"))
        .forInterface(std::string(kInterface));
}

void DbusService::restoreSaved() { m_restored = m_manager.restoreSaved(); }

void DbusService::renderRestored() {
    for (const int monitorId : m_restored) m_worker.post(monitorId);
    m_restored.clear();
}

void DbusService::applyImportModifierUpdate(const ImportModifierUpdate& update) {
    for (const int monitorId : update.toRender) m_worker.post(monitorId);
    if (!update.changed) return;

    try {
        m_object->emitSignal("ImportModifiersChanged").onInterface(kInterface);
    } catch (const std::exception& e) {
        // Losing it costs a consumer whose report was refused the chance to retry until the
        // next change, which is worth a line in the journal but is not worth failing the call
        // that succeeded.
        std::fprintf(stderr, "wallpaperx-daemon: failed to emit ImportModifiersChanged: %s\n", e.what());
    }
}

void DbusService::emitFrameReady(int monitorId, uint64_t generation) {
    // Called on the render worker's thread when a frame commits, and on the event-loop thread
    // when a release frees an image. sdbus-c++ documents signal emission on IObject as
    // thread-safe by design, so neither needs a hop back to the event loop.
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

    // Posted for every accepted type, drawable or not: a type this build cannot render still
    // needs its monitor's target released, and that is GPU work, so it belongs on the
    // worker rather than in this handler.
    m_worker.post(monitorId);
}

void DbusService::setImportModifiers(const std::string& client, uint32_t drmFormat,
                                     const std::vector<uint64_t>& modifiers) {
    std::printf("wallpaperx-daemon: SetImportModifiers(client=%s, drm_format=%u, count=%zu)\n",
                sanitizeForLog(client).c_str(), drmFormat, modifiers.size());
    std::fflush(stdout);

    ImportModifierUpdate update;
    try {
        update = m_manager.setImportModifiers(client, drmFormat, modifiers);
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetImportModifiers rejected: %s\n", e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    }

    // Every monitor holding a frame built against the old set. Each re-render announces its own
    // FrameReady, which is what tells the consumer to fetch the newly compatible frame.
    applyImportModifierUpdate(update);
}

void DbusService::setMonitorOutput(int32_t monitorId, const std::string& outputName) {
    std::printf("wallpaperx-daemon: SetMonitorOutput(monitor=%d, output=%s)\n", monitorId,
                sanitizeForLog(outputName).c_str());
    std::fflush(stdout);

    std::vector<int> redraw;
    try {
        m_manager.setMonitorOutput(monitorId, outputName, redraw);
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetMonitorOutput rejected (monitor=%d): %s\n",
                     monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    } catch (const std::exception& e) {
        // The binding stands although the write failed, so the redraws it owes are still owed.
        for (const int id : redraw) m_worker.post(id);
        // As SetWallpaper does: the save behind this can throw on a machine out of threads
        // or memory, and letting that escape the callback tells the client nothing.
        std::fprintf(stderr, "wallpaperx-daemon: SetMonitorOutput failed (monitor=%d): %s\n",
                     monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }

    // A name carries its screen's size, so moving one can change what a monitor is drawn at.
    for (const int id : redraw) m_worker.post(id);
}

void DbusService::setOutputSize(const std::string& outputName, uint32_t width, uint32_t height) {
    std::printf("wallpaperx-daemon: SetOutputSize(output=%s, width=%u, height=%u)\n",
                sanitizeForLog(outputName).c_str(), width, height);
    std::fflush(stdout);

    std::vector<int> redraw;
    try {
        m_manager.setOutputSize(outputName, width, height, redraw);
    } catch (const UnknownOutput& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetOutputSize rejected: %s\n", e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorUnknownOutput}, e.what());
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetOutputSize rejected: %s\n", e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    } catch (const std::exception& e) {
        // The size stands although its save could not be queued, so the redraw it owes is still owed.
        for (const int id : redraw) m_worker.post(id);
        std::fprintf(stderr, "wallpaperx-daemon: SetOutputSize failed: %s\n", e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }

    for (const int id : redraw) m_worker.post(id);
}

void DbusService::setOutputVisible(const std::string& outputName, bool visible) {
    std::printf("wallpaperx-daemon: SetOutputVisible(output=%s, visible=%s)\n", sanitizeForLog(outputName).c_str(),
                visible ? "true" : "false");
    std::fflush(stdout);

    try {
        m_manager.setOutputVisible(outputName, visible);
    } catch (const UnknownOutput& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetOutputVisible rejected: %s\n", e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorUnknownOutput}, e.what());
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetOutputVisible rejected: %s\n", e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    }

    // What resumes a video stopped because nothing could see it. No render is posted: the monitor
    // still holds the picture it stopped on, and the clock round this wake runs is what decides
    // whether anything has to be drawn or reopened.
    m_worker.wake();
}

void DbusService::releaseBuffer(const std::string& client, int32_t monitorId, uint64_t bufferId) {
    std::vector<int> toRender;
    std::vector<FrameAvailable> toAnnounce;
    try {
        toRender = m_manager.releaseBuffer(client, monitorId, bufferId, &toAnnounce);
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: ReleaseBuffer rejected (monitor=%d): %s\n", monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    }
    // A render that had nowhere to draw, now that this image is free.
    for (const int id : toRender) m_worker.post(id);
    announceAvailableFrames(toAnnounce);
}

// Re-announces a frame nothing is going to redraw. A consumer refused for want of a free image
// has no other trigger to ask again, and a consumer that already has this generation ignores it,
// so this is the whole recovery mechanism and it needs no record of who was refused.
void DbusService::announceAvailableFrames(const std::vector<FrameAvailable>& available) {
    for (const FrameAvailable& frame : available) emitFrameReady(frame.monitorId, frame.generation);
}

void DbusService::completeGetFrameFd(FrameFdCall call, int32_t monitorId, std::string client) {
    // Runs on the render worker's thread, so nothing here may throw past this frame --
    // an escaping exception would be std::terminate on a bare thread. Every path below
    // either returns results or returns an error on `call`.
    FrameExport exported;
    try {
        exported = m_manager.exportCurrentFrame(monitorId, client);
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
        if (exported.reason == NoFrameReason::NoFreeImage) {
            // Logged where the served hand-overs are logged: a wallpaper that stops changing for
            // one consumer while the daemon keeps rendering is otherwise invisible. Named,
            // because which of two consumers was refused is the whole diagnosis. Once per episode:
            // a consumer refused while a video plays is refused again for every picture.
            if (exported.firstRefusal) {
                std::printf("wallpaperx-daemon: GetFrameFd(monitor=%d) refused for %s -- no image may be handed over "
                            "until one is released\n",
                            monitorId, sanitizeForLog(client).c_str());
                std::fflush(stdout);
            }
            call.returnError(sdbus::Error(sdbus::Error::Name{kErrorNoFreeImage},
                                          "monitor " + std::to_string(monitorId) +
                                              " has no image to hand over -- one must stay free for the next "
                                              "render, and nothing is taken back from a consumer that may still "
                                              "be reading; ask again when the daemon announces a frame"));
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

    // Adopted here for the same reason and just as early: it comes from the same call and leaks in
    // exactly the same way. Adopted before it is pushed, so a throw out of the vector still runs
    // ~UnixFd rather than losing the descriptor.
    std::vector<sdbus::UnixFd> syncFds;
    if (exported.handle->acquireFd >= 0) {
        sdbus::UnixFd acquire(exported.handle->acquireFd, sdbus::adopt_fd);
        syncFds.push_back(std::move(acquire));
    }

    std::vector<sdbus::Struct<uint64_t, uint64_t>> planes;
    planes.reserve(frame.planes.size());
    for (const ExportedFramePlane& plane : frame.planes) {
        planes.emplace_back(plane.offset, plane.rowPitch);
    }

    // The one line that shows a consumer actually took a frame. Without it the log records
    // wallpapers being set and nothing collecting them, so a plugin that never fetched and one
    // that fetched and drew read identically -- which cost a whole session of misreading a
    // silent log as a failure.
    // Whether an acquire went with it, for the same reason: a frame handed over unsynchronised is
    // read without the queue-family acquire, and nothing downstream of here says so.
    if (!exported.handle->routine) {
        std::printf("wallpaperx-daemon: GetFrameFd(monitor=%d) served generation %llu as %s, %s\n", monitorId,
                    static_cast<unsigned long long>(exported.handle->generation),
                    frame.type == ExportedFrame::HandleType::DmaBuf ? "dma_buf" : "opaque fd",
                    syncFds.empty() ? "unsynchronised" : "with an acquire semaphore");
        std::fflush(stdout);
    }

    call.returnResults(fd, frame.width, frame.height, frame.memorySize,
                       frame.type == ExportedFrame::HandleType::DmaBuf ? 1 : 0,
                       // sync_fds: an already-signalled binary semaphore the consumer waits on
                       // before sampling. It carries the queue-family acquire and the layout
                       // transition reversing the render's release, which the importing API has
                       // no other way to express. Empty only where the GPU cannot export one; a
                       // consumer that cannot wait may close it and read anyway, which is
                       // undefined but is what every consumer did before this existed.
                       std::move(syncFds),
                       exported.handle->generation, frame.drmFormat, frame.modifier, planes,
                       // Which GPU allocated the memory. An opaque fd is importable only on that
                       // device, and without this the plugin can only report the driver's refusal.
                       std::vector<uint8_t>(frame.deviceUuid.begin(), frame.deviceUuid.end()),
                       // Which image, as against frame_generation's which render. A consumer
                       // keeping several buffers imports each id once.
                       frame.bufferId);
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
        // Whether GetFrameFd's sync_fds carries anything. A consumer seeing an empty array
        // otherwise cannot tell "this daemon produces none" from "this frame had none".
        gpuJson["frame_sync"] = gpu.frameSync ? "semaphore" : "none";
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
        // What the next render draws at, and whether a screen reported it. Always emitted: a
        // monitor drawn at the default size looks healthy in every other field.
        entry["target_width"] = monitor.targetSize.width;
        entry["target_height"] = monitor.targetSize.height;
        entry["target_size_source"] = monitor.targetSizeReported ? "reported" : "default";
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
        // The third reason a successful SetWallpaper produces no FrameReady: every image on
        // this monitor is being read and the render is waiting for one back. Always emitted,
        // and zero for a monitor that is keeping up -- a producer quietly drawing less often
        // than it was asked to is otherwise indistinguishable from one that is not.
        entry["deferred_renders"] = monitor.deferredRenders;
        // Why a consumer sees no new frame while this monitor keeps committing them: its images
        // are not coming back. Always emitted, like the count above, which cannot report this.
        entry["refused_handovers"] = monitor.refusedHandovers;
        // Who is holding which image. The count above says a hand-over was refused; only this says
        // by whom it is being kept, which is the whole diagnosis of a screen that has stopped --
        // an image is given back by its holder or by that holder leaving the bus, and by nothing
        // else. Names are bus-assigned but escaped like every other string that came from outside
        // this process.
        nlohmann::json holders = nlohmann::json::array();
        for (const BufferHold& hold : monitor.holds) {
            holders.push_back({{"client", sanitizeForLog(hold.client, 256, LogFraming::Bare)},
                               {"buffers", hold.buffers}});
        }
        entry["holders"] = std::move(holders);
        // Why a video has stopped moving, in the two facts it is decided from: what the screen
        // last said about being drawn, and whether anything has taken a picture off this monitor
        // recently. A stopped wallpaper reads identically in every other field.
        entry["screen_visible"] = monitor.screenVisible;
        entry["watched"] = monitor.watched;
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
