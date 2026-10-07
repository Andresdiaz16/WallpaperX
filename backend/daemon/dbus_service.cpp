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
// The fifth, and the only one that is the caller's own doing: it has been sent as many descriptors
// as it may hold without giving any back. Nothing announces the cure, since only the caller can
// apply it -- a release, or leaving the bus.
constexpr const char* kErrorTooManyDescriptors = "io.wallpaperx.Daemon.Error.TooManyDescriptors";
// The sixth, also the caller's own doing: it already has its share of the worker's queue waiting.
// Distinct from Busy, which is everyone's queue being full.
constexpr const char* kErrorTooManyRequests = "io.wallpaperx.Daemon.Error.TooManyRequests";
// The seventh, also the caller's own doing: it has been sent every copy of its acquire timeline one
// connection is given. Not retryable on that connection.
constexpr const char* kErrorTooManyTimelineCopies = "io.wallpaperx.Daemon.Error.TooManyTimelineCopies";
constexpr const char* kErrorShuttingDown = "io.wallpaperx.Daemon.Error.ShuttingDown";
constexpr const char* kErrorBusy = "io.wallpaperx.Daemon.Error.Busy";

// Below the whole queue, or one caller's share would still be all of it.
static_assert(WallpaperManager::kMaxQueuedRequestsPerClient < static_cast<int>(RenderWorker::kMaxQueuedTasks));
// A size for a screen no monitor answers to. Not InvalidArgs: nothing in the call was wrong, and
// the same call succeeds once SetMonitorOutput has named the screen.
constexpr const char* kErrorUnknownOutput = "io.wallpaperx.Daemon.Error.UnknownOutput";
constexpr const char* kErrorInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

// What a caller refused as TooManyDescriptors is told, and the line logged once per run of refusals.
std::string descriptorRefusal(const std::string& client) {
    return "caller " + sanitizeForLog(client) + " has been sent " +
           std::to_string(WallpaperManager::kMaxDescriptorsPerClient) +
           " file descriptors it has not given back (the images it holds, each frame fetched again while "
           "held, and one for its acquire timeline, which stays counted until it disconnects); a caller "
           "holding more could make the bus disconnect this daemon. Give images back with ReleaseBuffer, "
           "naming the newest hand-over read of each, or disconnect, then ask again";
}

void logDescriptorRefusal(const std::string& call, const std::string& client) {
    std::printf("wallpaperx-daemon: %s refused for %s until it gives descriptors back -- it has been sent %u "
                "it has not returned\n",
                call.c_str(), sanitizeForLog(client).c_str(), WallpaperManager::kMaxDescriptorsPerClient);
    std::fflush(stdout);
}

std::string requestRefusal(const std::string& client) {
    return "caller " + sanitizeForLog(client) + " already has " +
           std::to_string(WallpaperManager::kMaxQueuedRequestsPerClient) +
           " requests waiting to be answered; wait for their replies, then ask again";
}

// Each value by its D-Bus type -- d, b, or s holding "#rrggbb" -- and nothing converted: an i for
// a float property is a client that does not know what it is sending. Which keys and values are
// acceptable is the manager's rule.
WallpaperProperties fromWire(const std::map<std::string, sdbus::Variant>& wire) {
    // Before the copy, which is what the bound is for.
    if (wire.size() > kMaxProperties) {
        throw InvalidRequest("at most " + std::to_string(kMaxProperties) + " properties, got " +
                             std::to_string(wire.size()));
    }
    WallpaperProperties properties;
    for (const auto& [key, value] : wire) {
        if (value.containsValueOfType<double>()) {
            properties.emplace(key, value.get<double>());
        } else if (value.containsValueOfType<bool>()) {
            properties.emplace(key, value.get<bool>());
        } else if (value.containsValueOfType<std::string>()) {
            const std::optional<Colour> colour = parseColour(value.get<std::string>());
            if (!colour) {
                throw InvalidRequest("property " + sanitizeForLog(key, 128) + " is a string but not a colour \"#rrggbb\"");
            }
            properties.emplace(key, *colour);
        } else {
            const char* signature = value.peekValueType();
            throw InvalidRequest("property " + sanitizeForLog(key, 128) + " has D-Bus type " +
                                 sanitizeForLog(signature != nullptr ? signature : "", 64) +
                                 "; expected d, b or s");
        }
    }
    return properties;
}

// For a method whose only effect is its reply. A caller that set the no-reply-expected flag is sent
// nothing -- sd-bus drops the reply without an error -- so a hand-over or descriptor charged to it
// is one it can never name to give back. The flag is read here because sdbus-c++'s Result hides it.
sdbus::MethodVTableItem answeredOnly(sdbus::MethodVTableItem item) {
    item.callbackHandler = [handler = std::move(item.callbackHandler)](sdbus::MethodCall call) {
        if (call.doesntExpectReply()) return;
        handler(std::move(call));
    };
    return item;
}

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
                m_manager.noteDeparted(name);
                forgetClient(name);
            }
        });
}

void DbusService::forgetClient(const std::string& client) {
    // A monitor the departed consumer's set left with no frame can be drawn again now that the
    // constraint is gone, and this is the only thing that will retry it.
    applyImportModifierUpdate(m_manager.forgetImportModifiers(client));

    // Its acquire timeline goes too: nothing will ever be handed to that name again.
    m_manager.forgetAcquireTimeline(client);

    // And what it said about its screens' windows: with nobody left to say a window is focused
    // there, a pause that rested on it ends.
    if (m_manager.forgetOutputFocus(client)) m_worker.wake();

    // Likewise its claim on the images it was reading. A consumer that crashed holding a frame --
    // plasmashell restarts are routine, this daemon does not -- would otherwise keep those images
    // out of the ring for good.
    std::vector<FrameAvailable> available;
    for (const int monitorId : m_manager.forgetHeldBuffers(client, &available)) m_worker.post(monitorId);
    announceAvailableFrames(available);
}

template <class Call, class Complete>
void DbusService::postCall(Call call, Complete complete) {
    const char* sender = m_object->getCurrentlyProcessedMessage().getSender();
    const std::string client = sender ? sender : "";
    // Answered here, before anything is recorded or queued, so a burst costs the worker nothing.
    if (const auto admission = m_manager.admitRequest(client); admission != WallpaperManager::Admission::Admitted) {
        if (admission == WallpaperManager::Admission::FirstRefusal) {
            std::printf("wallpaperx-daemon: refusing requests from %s -- it already has %d waiting to be answered\n",
                        sanitizeForLog(client).c_str(), WallpaperManager::kMaxQueuedRequestsPerClient);
            std::fflush(stdout);
        }
        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorTooManyRequests}, requestRefusal(client)));
        return;
    }
    // Counted down on every path, a throw included: a count left above zero means this client's
    // departure is never applied again, and an image it held stays out of the ring for good.
    try {
        m_worker.postTask([this, call = std::move(call), complete = std::move(complete),
                           client](wallpaperx::RenderWorker::TaskState state) mutable {
            std::exception_ptr failed;
            try {
                // Answered rather than dropped: destroying the Result sends nothing, leaving the client
                // blocked for the full reply timeout on a request that will never run.
                switch (state) {
                    case RenderWorker::TaskState::ShuttingDown:
                        call.returnError(
                            sdbus::Error(sdbus::Error::Name{kErrorShuttingDown}, "the daemon is shutting down"));
                        break;
                    case RenderWorker::TaskState::Busy:
                        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorBusy},
                                                      "too many requests are already waiting; retry"));
                        break;
                    case RenderWorker::TaskState::Run:
                        complete(std::move(call), client);
                        break;
                }
            } catch (...) {
                failed = std::current_exception();
            }
            // Not applied at shutdown: the manager goes with the process, and forgetting would only
            // post work and emit signals during teardown.
            if (m_manager.noteRequestDone(client) && state != RenderWorker::TaskState::ShuttingDown) {
                forgetClient(client);
            }
            if (failed) std::rethrow_exception(failed);
        });
    } catch (...) {
        // postTask throws only before the task is queued, so nothing else will count this one down.
        if (m_manager.noteRequestDone(client)) forgetClient(client);
        throw;
    }
}

DbusService::~DbusService() {
    // Explicit, because neither destruction order alone is correct: handlers may still be
    // dispatching, setWallpaper() posts to m_worker and GetFrame's task touches m_object, so
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
                    setWallpaper("SetWallpaper", type, path, monitorId, {});
                })
                .withInputParamNames("type", "path", "monitor_id"),
            // A method of its own rather than a fourth argument on SetWallpaper: a changed signature
            // is refused by the bus for every client still sending three.
            sdbus::registerMethod("SetWallpaperWithProperties")
                .implementedAs([this](const std::string& type, const std::string& path, int32_t monitorId,
                                      const WireProperties& properties) {
                    setWallpaper("SetWallpaperWithProperties", type, path, monitorId, properties);
                })
                .withInputParamNames("type", "path", "monitor_id", "properties"),
            sdbus::registerMethod("SetWallpaperProperties")
                .implementedAs([this](int32_t monitorId, const WireProperties& properties) {
                    setWallpaperProperties(monitorId, properties);
                })
                .withInputParamNames("monitor_id", "properties"),
            answeredOnly(sdbus::registerMethod("GetFrame")
                // Async: the handler hands the export to the GPU thread and returns, so
                // the event loop keeps serving while the frame is produced.
                .implementedAs([this](FrameCall&& call, int32_t monitorId, std::vector<uint64_t> knownBuffers,
                                      bool onTimeline) {
                    // Refused here rather than queued, so an oversized list never reaches the worker.
                    try {
                        WallpaperManager::requireKnownBuffers(knownBuffers);
                    } catch (const InvalidRequest& e) {
                        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what()));
                        return;
                    }
                    postCall(std::move(call), [this, monitorId, knownBuffers = std::move(knownBuffers),
                                               onTimeline](FrameCall&& queued, const std::string& client) {
                        completeFrame(std::move(queued), monitorId, client, onTimeline, knownBuffers);
                    });
                })
                .withInputParamNames("monitor_id", "known_buffer_ids", "on_timeline")
                .withOutputParamNames("memory_fd", "width", "height", "memory_size", "handle_type", "sync_fds",
                                      "frame_generation", "drm_format", "drm_modifier", "planes", "device_uuid",
                                      "buffer_id", "acquire_value", "handover")),
            answeredOnly(sdbus::registerMethod("GetAcquireTimeline")
                .implementedAs([this](AcquireTimelineCall&& call) {
                    postCall(std::move(call), [this](AcquireTimelineCall&& queued, const std::string& client) {
                        completeGetAcquireTimeline(std::move(queued), client);
                    });
                })
                .withOutputParamNames("timeline")),
            sdbus::registerMethod("ReleaseBuffer")
                .implementedAs([this](int32_t monitorId, uint64_t bufferId, uint64_t handover) {
                    const char* sender = m_object->getCurrentlyProcessedMessage().getSender();
                    releaseBuffer(sender ? sender : "", monitorId, bufferId, handover);
                })
                .withInputParamNames("monitor_id", "buffer_id", "handover"),
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
            sdbus::registerMethod("SetMonitorPause")
                .implementedAs([this](int32_t monitorId, const std::string& mode) { setMonitorPause(monitorId, mode); })
                .withInputParamNames("monitor_id", "mode"),
            sdbus::registerMethod("SetOutputFocus")
                .implementedAs([this](const std::string& outputName, bool windowFocused, bool windowMaximized) {
                    // The sender, for SetImportModifiers' reason: it is what a departure names.
                    const char* sender = m_object->getCurrentlyProcessedMessage().getSender();
                    setOutputFocus(sender ? sender : "", outputName, windowFocused, windowMaximized);
                })
                .withInputParamNames("output_name", "window_focused", "window_maximized"),
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
        // std::terminate. The frame itself is fine and GetFrame will still serve it;
        // only the notification was lost, which a polling consumer recovers from.
        std::fprintf(stderr, "wallpaperx-daemon: failed to emit FrameReady(monitor=%d, generation=%llu): %s\n",
                     monitorId, static_cast<unsigned long long>(generation), e.what());
    }
}

void DbusService::setWallpaper(const char* method, const std::string& type, const std::string& path,
                               int32_t monitorId, const WireProperties& properties) {
    // On entry, not on success: most clients drop the error reply, so logging afterwards
    // left failed requests indistinguishable from ones that never arrived. Both strings
    // come off the wire, hence sanitizeForLog -- see log_escape.h.
    // The count only when there are some, so a line with none reads as it always has. One printf:
    // the render thread writes stdout too, and could land inside a line built from several.
    const std::string counted = properties.empty() ? "" : ", properties=" + std::to_string(properties.size());
    std::printf("wallpaperx-daemon: %s(type=%s, path=%s, monitor=%d%s)\n", method, sanitizeForLog(type).c_str(),
                sanitizeForLog(path).c_str(), monitorId, counted.c_str());
    std::fflush(stdout);

    try {
        m_manager.requestWallpaper(type, path, monitorId, fromWire(properties));
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: %s rejected (monitor=%d): %s\n", method, monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: %s failed (monitor=%d): %s\n", method, monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }

    // Posted for every accepted type, drawable or not: a type this build cannot render still
    // needs its monitor's target released, and that is GPU work, so it belongs on the
    // worker rather than in this handler.
    m_worker.post(monitorId);
}

void DbusService::setWallpaperProperties(int32_t monitorId, const WireProperties& properties) {
    // Refusals only: a slider sends one of these per step, and a line for each accepted one would
    // bury everything else in the journal.
    try {
        m_manager.setWallpaperProperties(monitorId, fromWire(properties));
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetWallpaperProperties rejected (monitor=%d): %s\n", monitorId,
                     e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetWallpaperProperties failed (monitor=%d): %s\n", monitorId,
                     e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }

    // No render: the picture on screen is still right. The round this wakes times the picture
    // waiting after it at the new speed, which the worker's current sleep was computed without.
    m_worker.wake();
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
        // A write that failed leaves the binding standing, so the redraws it owes are still owed.
        // A save that could not be queued undid the binding and owes none.
        for (const int id : redraw) m_worker.post(id);
        m_worker.wake();
        // As SetWallpaper does: the save behind this can throw on a machine out of threads
        // or memory, and letting that escape the callback tells the client nothing.
        std::fprintf(stderr, "wallpaperx-daemon: SetMonitorOutput failed (monitor=%d): %s\n",
                     monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }

    // A name carries its screen's size, so moving one can change what a monitor is drawn at.
    for (const int id : redraw) m_worker.post(id);
    // And its window report, whose loss can end a pause.
    m_worker.wake();
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
        // No redraw: a size whose save could not be queued was undone.
        std::fprintf(stderr, "wallpaperx-daemon: SetOutputSize failed: %s\n", e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }

    for (const int id : redraw) m_worker.post(id);
}

void DbusService::setOutputVisible(const std::string& outputName, bool visible) {
    // Changes and refusals only: every wallpaper item reports when it starts and several can share
    // one screen, so most reports change nothing, and logging each would bury the ones that do.
    try {
        if (m_manager.setOutputVisible(outputName, visible)) {
            std::printf("wallpaperx-daemon: SetOutputVisible(output=%s, visible=%s)\n",
                        sanitizeForLog(outputName).c_str(), visible ? "true" : "false");
            std::fflush(stdout);
        }
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

void DbusService::setMonitorPause(int32_t monitorId, const std::string& mode) {
    std::printf("wallpaperx-daemon: SetMonitorPause(monitor=%d, mode=%s)\n", monitorId, sanitizeForLog(mode).c_str());
    std::fflush(stdout);

    try {
        m_manager.setMonitorPause(monitorId, mode);
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetMonitorPause rejected (monitor=%d): %s\n", monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetMonitorPause failed (monitor=%d): %s\n", monitorId, e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }

    // No render: a paused video keeps the picture it is showing, and one resumed goes on from it.
    m_worker.wake();
}

void DbusService::setOutputFocus(const std::string& client, const std::string& outputName, bool windowFocused,
                                 bool windowMaximized) {
    // Refusals only: this follows every change of the focused window.
    try {
        m_manager.setOutputFocus(client, outputName, windowFocused, windowMaximized);
    } catch (const UnknownOutput& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetOutputFocus rejected: %s\n", e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorUnknownOutput}, e.what());
    } catch (const InvalidRequest& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetOutputFocus rejected: %s\n", e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wallpaperx-daemon: SetOutputFocus failed: %s\n", e.what());
        throw sdbus::Error(sdbus::Error::Name{kErrorRenderFailed}, e.what());
    }

    m_worker.wake();
}

void DbusService::releaseBuffer(const std::string& client, int32_t monitorId, uint64_t bufferId,
                                uint64_t handover) {
    std::vector<int> toRender;
    std::vector<FrameAvailable> toAnnounce;
    try {
        toRender = m_manager.releaseBuffer(client, monitorId, bufferId, handover, &toAnnounce);
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

void DbusService::completeFrame(FrameCall call, int32_t monitorId, const std::string& client, bool onTimeline,
                                const std::vector<uint64_t>& knownBuffers) {
    // Runs on the render worker's thread. Every path below answers `call`; a frame that fails to
    // send is answered with an error where the bus still allows it, then rethrown for the worker
    // to log.
    constexpr const char* method = "GetFrame";
    FrameExport exported;
    // Filled only by an export that failed after taking an image out of the ring. Never throws, so
    // the call below is still answered.
    std::vector<int> toRender;
    std::vector<FrameAvailable> toAnnounce;
    const auto reportFreed = [&]() noexcept {
        try {
            for (const int id : toRender) m_worker.post(id);
            announceAvailableFrames(toAnnounce);
        } catch (...) {
        }
    };
    try {
        exported = m_manager.exportCurrentFrame(monitorId, client, onTimeline, knownBuffers, toRender, &toAnnounce);
    } catch (const InvalidRequest& e) {
        reportFreed();
        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorInvalidArgs}, e.what()));
        return;
    } catch (const std::exception& e) {
        reportFreed();
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
                std::printf("wallpaperx-daemon: %s(monitor=%d) refused for %s -- no image may be handed over "
                            "until one is released\n",
                            method, monitorId, sanitizeForLog(client).c_str());
                std::fflush(stdout);
            }
            call.returnError(sdbus::Error(sdbus::Error::Name{kErrorNoFreeImage},
                                          "monitor " + std::to_string(monitorId) +
                                              " has no image to hand over -- one must stay free for the next "
                                              "render, and nothing is taken back from a consumer that may still "
                                              "be reading; ask again when the daemon announces a frame"));
            return;
        }
        if (exported.reason == NoFrameReason::TooManyDescriptors) {
            if (exported.firstRefusal) {
                logDescriptorRefusal(std::string(method) + "(monitor=" + std::to_string(monitorId) + ")", client);
            }
            call.returnError(sdbus::Error(sdbus::Error::Name{kErrorTooManyDescriptors}, descriptorRefusal(client)));
            return;
        }
        if (exported.reason == NoFrameReason::Unsupported) {
            // No lastError: this is not a failure and nothing wrote one. The type can only be one
            // of requestWallpaper()'s literals, and is escaped all the same.
            call.returnError(sdbus::Error(sdbus::Error::Name{kErrorUnsupported},
                                          "monitor " + std::to_string(monitorId) + " holds a " +
                                              sanitizeForLog(exported.type, 64) +
                                              " wallpaper, a type this build cannot render, so no "
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

    const FrameHandle& handle = *exported.handle;
    try {
        sendFrame(call, handle);
    } catch (...) {
        // Whatever went wrong, nothing reached the bus: the caller cannot name this image to release
        // it, so the claim and the descriptors charged for it go now rather than at its departure.
        // Each step on its own, and the original failure is the one rethrown for the worker to log.
        try {
            std::vector<FrameAvailable> toAnnounce;
            for (const int id : m_manager.withdrawHandover(monitorId, client, handle, &toAnnounce)) {
                m_worker.post(id);
            }
            announceAvailableFrames(toAnnounce);
        } catch (...) {
        }
        try {
            call.returnError(sdbus::Error(sdbus::Error::Name{kErrorExportFailed},
                                          "the frame for monitor " + std::to_string(monitorId) + " could not be sent"));
        } catch (...) {
        }
        throw;
    }

    // The one line that shows a consumer actually took a frame. Without it the log records
    // wallpapers being set and nothing collecting them, so a plugin that never fetched and one
    // that fetched and drew read identically -- which cost a whole session of misreading a
    // silent log as a failure.
    // Whether an acquire went with it, for the same reason: a frame handed over unsynchronised is
    // read without the queue-family acquire, and nothing downstream of here says so.
    // Whether the memory went too, since a consumer drawing an import it no longer has is
    // otherwise indistinguishable here from one drawing the new picture.
    if (!m_manager.handoverDelivered(monitorId, client)) {
        std::printf("wallpaperx-daemon: %s(monitor=%d) served generation %llu as %s, %s, %s\n", method, monitorId,
                    static_cast<unsigned long long>(handle.generation),
                    handle.frame.type == ExportedFrame::HandleType::DmaBuf ? "dma_buf" : "opaque fd",
                    handle.frame.fd >= 0 ? "memory sent" : "memory already held",
                    handle.acquireValue != 0 ? "with a value on its acquire timeline"
                    : handle.acquireFd >= 0  ? "with an acquire semaphore"
                                             : "unsynchronised");
        std::fflush(stdout);
    }
}

void DbusService::sendFrame(const FrameCall& call, const FrameHandle& handle) {
    const ExportedFrame& frame = handle.frame;

    // adopt_fd, not the duplicating constructor: exportFrame() handed over ownership of a fresh
    // fd and nothing else closes it. Adopted on the first lines after the export, each before it
    // is pushed, so a throw below -- out of a vector included -- runs ~UnixFd: a polling consumer
    // calls this repeatedly, so a leak becomes EMFILE.
    std::vector<sdbus::UnixFd> memoryFd;
    std::vector<sdbus::UnixFd> syncFds;
    {
        sdbus::UnixFd memory(frame.fd, sdbus::adopt_fd);
        sdbus::UnixFd acquire(handle.acquireFd, sdbus::adopt_fd);
        if (memory.isValid()) memoryFd.push_back(std::move(memory));
        if (acquire.isValid()) syncFds.push_back(std::move(acquire));
    }

    std::vector<sdbus::Struct<uint64_t, uint64_t>> planes;
    planes.reserve(frame.planes.size());
    for (const ExportedFramePlane& plane : frame.planes) {
        planes.emplace_back(plane.offset, plane.rowPitch);
    }

    // sync_fds: an already-signalled binary semaphore the consumer waits on before sampling. It
    // carries the queue-family acquire and the layout transition reversing the render's release,
    // which the importing API has no other way to express. Empty only where the GPU cannot export
    // one, or where the picture carries a value instead; a consumer that cannot wait may close it
    // and read anyway, which is undefined.
    const int32_t handleType = frame.type == ExportedFrame::HandleType::DmaBuf ? 1 : 0;
    // Which GPU allocated the memory. An opaque fd is importable only on that device, and without
    // this the plugin can only report the driver's refusal.
    std::vector<uint8_t> deviceUuid(frame.deviceUuid.begin(), frame.deviceUuid.end());
    // buffer_id: which image, as against frame_generation's which render. A consumer keeping
    // several buffers imports each id once. acquire_value: the value to wait for, already reached,
    // standing in for sync_fds; 0 when sync_fds carries the acquire. handover: what ReleaseBuffer
    // names to give this reply back, 0 when no claim was recorded.
    call.returnResults(memoryFd, frame.width, frame.height, frame.memorySize, handleType, syncFds,
                       handle.generation, frame.drmFormat, frame.modifier, planes, deviceUuid, frame.bufferId,
                       handle.acquireValue, handle.handover ? handle.handover->number : 0);
}

void DbusService::completeGetAcquireTimeline(AcquireTimelineCall call, const std::string& client) {
    // The worker's thread, like completeFrame(), and under the same rule: nothing escapes.
    int fd = -1;
    try {
        fd = m_manager.acquireTimelineFd(client);
    } catch (const TimelineUnavailable& e) {
        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorUnsupported}, e.what()));
        return;
    } catch (const TimelineLimitReached& e) {
        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorBusy}, e.what()));
        return;
    } catch (const TimelineCopyLimitReached& e) {
        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorTooManyTimelineCopies}, e.what()));
        return;
    } catch (const DescriptorLimitReached& e) {
        if (e.firstRefusal) logDescriptorRefusal("GetAcquireTimeline", client);
        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorTooManyDescriptors}, descriptorRefusal(client)));
        return;
    } catch (const std::exception& e) {
        call.returnError(sdbus::Error(sdbus::Error::Name{kErrorExportFailed}, e.what()));
        return;
    }
    // Adopted at once, so a throw below still closes it.
    sdbus::UnixFd timeline(fd, sdbus::adopt_fd);
    call.returnResults(timeline);
    m_manager.timelineCopyDelivered(client);
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
        // Whether GetFrame's sync_fds carries anything. A consumer seeing an empty array
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
        // As set, not as applied: a speed below the playback range plays at its bottom. Keys
        // need no escaping, since propertiesError() admits only ASCII letters, digits and "_-.".
        entry["properties"] = toJson(monitor.properties);
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
        // Shares isRenderableType() with GetFrame so the two cannot disagree.
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
        // `descriptors` is what this monitor's hand-overs to that holder count against
        // kMaxDescriptorsPerClient: an image fetched again while held is one claim but another reply.
        nlohmann::json holders = nlohmann::json::array();
        for (const BufferHold& hold : monitor.holds) {
            std::vector<uint64_t> ids;
            for (const HeldBuffer& buffer : hold.buffers) ids.push_back(buffer.id);
            holders.push_back({{"client", sanitizeForLog(hold.client, 256, LogFraming::Bare)},
                               {"buffers", std::move(ids)},
                               {"descriptors", WallpaperManager::descriptorsOf(hold)}});
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

    // Its own object for the reason `outputs` is: a screen can be set to pause before anything is
    // chosen for it. `mode` is null for a setting a newer build wrote, which this one keeps and
    // does not apply; `paused` is what that setting and the screen's last report come to now.
    nlohmann::json pause = nlohmann::json::object();
    for (const auto& [monitorId, monitor] : snap.pause) {
        pause[std::to_string(monitorId)] = {
            {"mode", monitor.mode ? nlohmann::json(toString(*monitor.mode)) : nlohmann::json(nullptr)},
            {"paused", monitor.paused},
            {"window_focused", monitor.windowFocused},
            {"window_maximized", monitor.windowMaximized}};
    }
    status["pause"] = std::move(pause);

    return status.dump();
}

}
