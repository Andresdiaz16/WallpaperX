// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "render_worker.h"
#include "wallpaper_manager.h"

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace wallpaperx {

class WallpaperManager;

// Serves the io.wallpaperx.Daemon DBus interface on a caller-provided connection. The
// connection owns the bus-name request and the event loop (blocking in main(), async in
// tests); this class only registers and implements the methods and signals.
//
// Stateless by design: every handler forwards to the WallpaperManager, which owns both the
// wallpaper state and the lock guarding it. State kept here would be a second truth under a
// second lock, which is what lets GetStatus and GetFrame disagree.
class DbusService {
public:
    static constexpr const char* kBusName = "io.wallpaperx.Daemon";
    static constexpr const char* kObjectPath = "/io/wallpaperx/Daemon";
    static constexpr const char* kInterface = "io.wallpaperx.Daemon";

    // `manager` must outlive this object.
    DbusService(sdbus::IConnection& connection, WallpaperManager& manager);
    ~DbusService();

    // Applies the saved wallpapers without drawing them. Call once, before the event loop.
    void restoreSaved();

    // WallpaperManager::startRestoredScans() for what restoreSaved() applied. Call once, after
    // WallpaperManager::recordStartingVideos() and before renderRestored().
    void startRestoredScans();

    // Posts the renders restoreSaved() held back. Call once, after
    // WallpaperManager::recordStartingVideos(): a video that crashes the render thread before the
    // playing record is open names nothing, and the restart loop that record exists to stop goes on.
    void renderRestored();

private:
    void registerVTable();

    // Emits ConfigChanged: what the config server answers, or a setting GetStatus reports, may have
    // changed, so consumers read it again. Not at startup: the config server answers before the name is claimed, so the name's
    // arrival is that notice.
    void announceConfigChanged();

    // Emits PersistenceChanged. On the config writer's thread; emitting is thread-safe in sdbus-c++.
    void announcePersistenceChanged() noexcept;

    // Emits ProcessorDecodingChanged. On the scan's thread or the render thread.
    void announceProcessorDecodingChanged() noexcept;

    // DBus method handlers. PLAN.md's DBus Interface section is the wire contract.
    //
    // Returns once the request is recorded and queued: a successful reply means "accepted",
    // not "the frame is ready" -- FrameReady announces that. Only client-side mistakes fail
    // synchronously, since they cost nothing to detect.
    //
    // SetWallpaper and SetWallpaperWithProperties both land here, `method` naming which in the log;
    // SetWallpaper is the second with no properties.
    // Every entry as sent, not a map: deserializing into one keeps the first of a repeated key and
    // drops the rest unread.
    using WireProperties = std::vector<sdbus::DictEntry<std::string, sdbus::Variant>>;
    void setWallpaper(const char* method, const std::string& type, const std::string& path, int32_t monitorId,
                      const WireProperties& properties);

    // Replaces the properties of the wallpaper a monitor holds without setting it again, so a
    // playing video carries on. Wakes the worker, which re-times the picture waiting to be shown.
    void setWallpaperProperties(int32_t monitorId, const WireProperties& properties);

    // Complete on return, unlike SetWallpaper: an accepted reply means the mapping reached the
    // config file. An error means it did not -- the binding still stands for this run but will
    // not survive a restart, and the client may re-assert it later.
    void setMonitorOutput(int32_t monitorId, const std::string& outputName);

    // Records the pixel size the screen named `outputName` is drawn at, and posts its monitor
    // when that changes. Accepted means recorded, not saved: see WallpaperManager::setOutputSize.
    void setOutputSize(const std::string& outputName, uint32_t width, uint32_t height);

    // Records whether the screen named `outputName` is being drawn at all -- not behind a lock
    // screen, a fullscreen window or another session. The daemon cannot see any of that, and a
    // video whose screen is not drawn stops until this says otherwise, so the worker is woken
    // either way: it is what recomputes what plays.
    void setOutputVisible(const std::string& outputName, bool visible);

    // A monitor's pause setting, by name. Saved and waited on like SetWallpaper; wakes the worker,
    // which is what stops or resumes the video.
    void setMonitorPause(int32_t monitorId, const std::string& mode);

    // Sound on or off for every screen, and for one monitor. Saved and waited on like
    // SetMonitorPause; a change announces ConfigChanged.
    void setSound(bool on);
    void setMonitorSound(int32_t monitorId, bool on);

    // The frame-rate settings, as the sound ones are: global, one monitor's (0 drops it), and whether a
    // rate above a screen's refresh rate is kept. A change announces ConfigChanged and wakes the worker,
    // which plays at the new rate from its next round.
    void setFrameRate(int32_t rate);
    void setMonitorFrameRate(int32_t monitorId, int32_t rate);
    void setKeepHigherFrameRate(bool keep);

    // How many times a second the screen named `outputName` refreshes, 0 for not known. Not saved;
    // wakes the worker when it changed.
    void setOutputRefreshRate(const std::string& outputName, double rate);

    // What the screen named `outputName` says about its windows, recorded against `client` -- the
    // sender -- so it ends when that process leaves the bus. Wakes the worker for the same reason.
    void setOutputFocus(const std::string& client, const std::string& outputName, ScreenFocus focus,
                        bool windowMaximized);

    void emitFrameReady(int monitorId, uint64_t generation);

    // Announces frames that already exist, after an image came free. Same signal as a new
    // frame's: "monitor M has generation N" is true either way, and a consumer that holds N
    // already ignores it.
    void announceAvailableFrames(const std::vector<FrameAvailable>& available);

    // GetFrame. Returns (memory_fd, width, height, memory_size, handle_type, sync_fds,
    // frame_generation, drm_format, drm_modifier, planes, device_uuid, buffer_id, acquire_value,
    // handover). What each field means is PLAN.md's DBus Interface section.
    //
    // Answered from the render worker's thread, not in the handler. Exporting needs the GPU and
    // the GPU lock is held for a whole render, so answering here would park the single
    // event-loop thread behind whatever is rendering and take GetStatus with it. The rule that
    // keeps it fixed -- no DBus handler touches the GPU -- is enforced by assertGpuThread(). It is
    // also what sends one caller's replies in the order their hand-overs were numbered.
    using FrameCall =
        sdbus::Result<std::vector<sdbus::UnixFd>, uint32_t, uint32_t, uint64_t, int32_t, std::vector<sdbus::UnixFd>,
                      uint64_t, uint32_t, uint64_t, std::vector<sdbus::Struct<uint64_t, uint64_t>>,
                      std::vector<uint8_t>, uint64_t, uint64_t, uint64_t>;

    // `client` is the caller's DBus unique name: it is what the hand-over is recorded against, and
    // what a departure later withdraws. `onTimeline` and `knownBuffers` as for
    // WallpaperManager::exportCurrentFrame().
    void completeFrame(FrameCall call, int32_t monitorId, const std::string& client, bool onTimeline,
                       const std::vector<uint64_t>& knownBuffers);

    // Answers `call` with `handle`, taking ownership of its descriptors. A throw means no reply left.
    void sendFrame(const FrameCall& call, const FrameHandle& handle);

    // Returns (timeline): the caller's acquire timeline, which it imports once and then waits on for
    // each picture it asks GetFrame for on it. Answered from the render worker in the frame
    // method's queue: creating it is GPU work, and a frame asked for after this cannot be answered
    // before it. Unsupported means this daemon will not give this caller one while it runs; Busy,
    // that every slot is taken for now.
    using AcquireTimelineCall = sdbus::Result<sdbus::UnixFd>;
    void completeGetAcquireTimeline(AcquireTimelineCall call, const std::string& client);

    // Posts `complete(call, client)` to the render worker for an asynchronous method, reading the
    // caller's name here -- the message is current only inside the handler -- and answering the
    // call itself when the caller is over its share of the queue or the worker refuses it. Call
    // from inside the handler.
    template <class Call, class Complete>
    void postCall(Call call, Complete complete);

    // Everything the daemon keeps for `client`, dropped because it left the bus. On the bus thread
    // with the departure, and on the worker again when a request `client` queued before leaving
    // has run and may have recorded something for it after the first pass.
    void forgetClient(const std::string& client);

    // The consumer says it has stopped reading one image as of hand-over `handover`, so the producer
    // may draw into it again unless a later hand-over is still on its way. Synchronous: it touches
    // bookkeeping only, and the renders it unblocks go to the worker like every other one.
    void releaseBuffer(const std::string& client, int32_t monitorId, uint64_t bufferId, uint64_t handover);

    // Records what the consumer can import and re-posts the monitors whose images were built
    // against the previous set. Synchronous: it touches bookkeeping only, and the GPU work
    // happens on the worker like every other render.
    void setImportModifiers(const std::string& client, uint32_t drmFormat, const std::vector<uint64_t>& modifiers);

    // Posts the re-renders a report or withdrawal asked for, and announces the change to every
    // consumer. Both halves belong together: a consumer whose own report was refused for
    // sharing no layout with another one learns from this that asking again is worth it.
    void applyImportModifierUpdate(const ImportModifierUpdate& update);

    // Subscribes to the bus's own NameOwnerChanged so a consumer that reported import
    // modifiers and then exited stops constraining the exporter. Without it the daemon would
    // keep narrowing every image for a process that is gone, and the narrowest set any
    // consumer ever reported would outlive them all.
    void watchClientExits();

    std::string getStatus();

    // ~DbusService stops the worker before any member is destroyed: the ordering cannot be
    // left to declaration order alone, and that destructor says why.
    WallpaperManager& m_manager;
    std::unique_ptr<sdbus::IObject> m_object;
    std::unique_ptr<sdbus::IProxy> m_busWatch;
    RenderWorker m_worker;

    // What restoreSaved() applied, until renderRestored() posts it. Main thread, before the loop.
    std::vector<int> m_restored;
};

}
