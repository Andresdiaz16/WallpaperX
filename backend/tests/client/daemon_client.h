// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Client-side scaffolding for talking to io.wallpaperx.Daemon over DBus, shared by the
// two test tiers that both do it:
//
//   dbus_service_test    -- service and client in one process, two connections
//   daemon_system_test   -- the real daemon binary in its own process
//
// Extracted rather than copied because the two tiers must agree on the *wire*: if the
// out-of-process test carried its own copy of `Frame` and the reply's argument order
// changed, one tier would be updated and the other would still compile, still pass, and
// be reading different fields than the daemon writes. One definition is what makes
// "both tiers agree" an actual property rather than a coincidence of maintenance.
//
// Deliberately client-only. Nothing here touches WallpaperManager or the GPU -- the
// system test has no access to either, and a helper that quietly needed them could not
// be shared.

#include "daemon/config_store.h"
#include "daemon/dbus_service.h"
#include "daemon/log_escape.h"
#include "support/validation_grade.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace wallpaperx::testing {

// Generous: it bounds a GPU render on a software rasteriser under load, and is only
// ever reached when something is actually broken.
inline constexpr auto kSignalTimeout = std::chrono::seconds(30);

// How a test records a pass/fail. Passed by reference so every helper reports through
// the caller's failure counter rather than aborting the run at the first problem --
// one FAIL line per broken property is worth far more than a stack trace from the first.
using Check = std::function<void(bool, const char*)>;

// validationWaived() -- "run without the Vulkan validation layer" -- is declared in
// support/validation_grade.h, included above, and is deliberately not redeclared here.
//
// Shared, not copied, because the two tiers must agree on what the switch *means*: a
// ThreadSanitizer run waives both with one variable, and a tier that disagreed would
// either drown the report in layer noise or claim a GPU-side oracle it did not have.
// It moved to the renderer's test-support header because the renderer probes cannot
// reach backend/tests, while the backend tests already have renderer/tests on their
// include path.
//
// Compared against "1" exactly, matching WALLPAPERX_ALLOW_NO_HEAP_TIER: a plain getenv()
// truthiness check would make "=0" mean "waived", the opposite of what anyone typing it
// intends. Every caller must announce a waiver loudly -- this disables the suite's only
// GPU-side oracle, and a silent version of the switch is exactly the vacuous-green trap
// the validation checks exist to close.
// validationWaived() is defined in support/validation_grade.h and pulled in above, so
// both test trees share one definition of what this switch means rather than two copies
// that can drift.

// A file that exists and is a regular file -- so it passes SetWallpaper's synchronous
// validation -- but cannot be decoded, so the render fails asynchronously. That
// asymmetry is the point: it is the only way to reach the RenderFailed path.
//
// The PNG *writers* are deliberately not shared alongside this. They differ per tier (a
// single solid fill against a five-colour burst palette with its own static_assert), and
// sharing them would drag stb_image_write's implementation macro into a header included
// by two translation units that each define it -- stb emits its implementation block
// outside its own include guard, so that double-defines rather than deduplicating.
inline std::string writeUndecodableFile(const std::string& dir, const std::string& name) {
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/" + name;
    std::ofstream out(path, std::ios::binary);
    out << "this is not a PNG";
    if (!out) throw std::runtime_error("could not write " + path);
    return path;
}

// Number of descriptors this process currently holds open. Used before/after a block of
// work to catch a leak: GetFrameFd is called by a polling consumer, so a descriptor
// leaked per call is an eventual EMFILE rather than a one-off.
inline size_t openFdCount() {
    size_t count = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        ++count;
    }
    return count;
}

// Records every FrameReady the daemon emits, and lets a test block until a specific
// (monitor, generation) arrives.
class FrameReadyLog {
public:
    void record(int32_t monitorId, uint64_t generation) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_events.emplace_back(monitorId, generation);
        }
        m_arrived.notify_all();
    }

    // Waits for FrameReady(monitorId, generation). Returns false on timeout.
    bool wait(int32_t monitorId, uint64_t generation) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_arrived.wait_for(lock, kSignalTimeout, [&] {
            for (const auto& [id, gen] : m_events) {
                if (id == monitorId && gen == generation) return true;
            }
            return false;
        });
    }

    // Waits for any FrameReady for this monitor whose generation is >= `generation`.
    // The exact-match wait() above cannot be used when several clients drive the same
    // monitor concurrently: which generation a given request lands on is then a race,
    // and asserting on a specific number would be asserting on the scheduler.
    //
    // PRECONDITION, and it is easy to get wrong: this scans the whole signal history, so
    // it answers "a frame at or past N exists", not "a frame arrived after I asked". The
    // caller must therefore derive `generation` from a state it knows to be QUIESCED. A
    // threshold read from GetStatus mid-flight does not qualify -- WallpaperManager bumps
    // the counter at commit time, so a render already queued when the threshold was read
    // satisfies this wait without the caller's own request ever having rendered, and the
    // check built on it passes for a daemon that has stopped rendering entirely. Use the
    // single FIFO render thread as the barrier: post a request, wait() for its exact
    // signal, and only then read the threshold.
    bool waitAtLeast(int32_t monitorId, uint64_t generation) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_arrived.wait_for(lock, kSignalTimeout, [&] {
            for (const auto& [id, gen] : m_events) {
                if (id == monitorId && gen >= generation) return true;
            }
            return false;
        });
    }

    size_t countFor(int32_t monitorId) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t n = 0;
        for (const auto& [id, gen] : m_events) {
            if (id == monitorId) ++n;
        }
        return n;
    }

    size_t total() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_events.size();
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_arrived;
    std::vector<std::pair<int32_t, uint64_t>> m_events;
};

// Everything GetFrameFd hands back, in one place, so the assertions read as claims
// about the frame rather than as tuple indices.
struct Frame {
    sdbus::UnixFd fd;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t memorySize = 0;
    int32_t handleType = 0;
    std::vector<sdbus::UnixFd> syncFds;
    uint64_t generation = 0;
    uint32_t drmFormat = 0;
    uint64_t drmModifier = 0;
    std::vector<sdbus::Struct<uint64_t, uint64_t>> planes;
    std::vector<uint8_t> deviceUuid;
    uint64_t bufferId = 0;
};

inline Frame getFrameFd(sdbus::IProxy& proxy, int32_t monitorId) {
    Frame frame;
    proxy.callMethod("GetFrameFd")
        .onInterface(DbusService::kInterface)
        .withArguments(monitorId)
        .storeResultsTo(frame.fd, frame.width, frame.height, frame.memorySize, frame.handleType, frame.syncFds,
                        frame.generation, frame.drmFormat, frame.drmModifier, frame.planes, frame.deviceUuid,
                        frame.bufferId);
    return frame;
}

// `timeout` is the method-call timeout. Zero, the default, means the bus's own (25 s) and is
// what every caller wants but one: a phase asking the question OF a daemon that may be wedged
// inside the handler passes a short one, so that daemon produces a failed check in seconds
// rather than a run that merely looks hung.
inline void setWallpaper(sdbus::IProxy& proxy, const std::string& type, const std::string& path, int32_t monitorId,
                         std::chrono::microseconds timeout = {}) {
    proxy.callMethod("SetWallpaper")
        .onInterface(DbusService::kInterface)
        .withTimeout(timeout)
        .withArguments(type, path, monitorId);
}

// The answer to GetStatus, or the reason there is none.
//
// A dropped, saturated or unresponsive bus makes the call THROW rather than hand back an
// unparseable reply, so `if (json.is_discarded())` is not the guard it looks like: the
// discarded case never arrives. An escaping sdbus::Error instead unwinds past every phase
// after the call site into the test's one catch-all -- one opaque line, no failure
// attributed to anything, and no NOT VERIFIED row for the phases that never ran.
//
// So the fault is a value and there is deliberately no throwing form of getStatus() beside
// it: a safe helper next to an unsafe one leaves the unsafe call the shorter thing to type,
// and the shorter thing is what gets written.
//
// A site that ignores ok() must still not go quiet, which is what the empty object buys:
// json() is `{}`, so wallpaperStatus() returns nullptr and every field comparison below it
// is false. statusSelfConsistent() is the one predicate that cannot be left to that rule --
// an absent field satisfies an invariant stated as a forbidden conjunction -- so it tests
// ok() itself rather than relying on its caller to.
class StatusReply {
public:
    StatusReply() = default;
    explicit StatusReply(nlohmann::json json) : m_json(std::move(json)) {}
    static StatusReply failure(std::string error) {
        StatusReply reply;
        reply.m_error = std::move(error);
        return reply;
    }

    // The call returned and its payload parsed into an object.
    bool ok() const { return m_error.empty(); }

    // Why there is no usable reply, empty when ok(). It names a component only where the
    // component is knowable -- an error name in the daemon's own namespace, or a reply that
    // arrived and would not parse. Every other failure says which possibilities remain
    // instead of picking one, because a reader sent to the transport for a daemon wedged in
    // its own handler looks in the wrong process.
    const std::string& error() const { return m_error; }

    // An empty object unless ok().
    const nlohmann::json& json() const { return m_json; }

private:
    nlohmann::json m_json = nlohmann::json::object();
    std::string m_error;
};

// True for the error names only this daemon can have produced: the ones its own interface
// defines.
//
// The daemon's namespace is the closed set here, and the transport's is not. Listing
// transport names instead -- ServiceUnknown, NoReply, Disconnected and so on -- is
// unclosable: org.freedesktop.DBus.Error has members no list here will track, sdbus-c++
// reports local failures under names of its own, and everything the list missed was
// attributed to the daemon by default. This direction fails the other way, which is the
// safe one: an unrecognised name is reported as unattributed rather than as a verdict.
// Built from kInterface so renaming the interface breaks the build instead of quietly
// leaving a prefix that matches nothing.
//
// CONTROL (both branches, reverted): DbusService::getStatus made to throw
//       sdbus::Error{"io.wallpaperx.Daemon.Error.Unsupported"} -> "the daemon answered
//       GetStatus with an error (io.wallpaperx.Daemon.Error.Unsupported)"; made to throw
//       std::runtime_error, which sdbus-c++ delivers as org.sdbuscpp.Error -> "GetStatus
//       failed with org.sdbuscpp.Error, which is not one of the daemon's own error names".
inline bool daemonErrorName(const std::string& name) {
    return name.rfind(std::string(DbusService::kInterface) + ".Error.", 0) == 0;
}

// Records the connector name a monitor answers to. `timeout` as on setWallpaper() above.
//
// Shared rather than written out in each tier for the reason this header exists: the two
// tiers have to agree on the wire. A copy in one file would keep compiling and keep
// passing if the argument order ever changed under it.
inline void setMonitorOutput(sdbus::IProxy& proxy, int32_t monitorId, const std::string& outputName,
                             std::chrono::microseconds timeout = {}) {
    proxy.callMethod("SetMonitorOutput")
        .onInterface(DbusService::kInterface)
        .withTimeout(timeout)
        .withArguments(monitorId, outputName);
}

// Reports what the frame consumer can import, as the plugin's render thread does.
//
// Here rather than in one test file for the same reason as setMonitorOutput above: the
// argument order (format first, then the list) is a wire fact, and a private copy would go
// on compiling and passing if it ever changed.
inline void setImportModifiers(sdbus::IProxy& proxy, uint32_t drmFormat, const std::vector<uint64_t>& modifiers,
                               std::chrono::microseconds timeout = {}) {
    proxy.callMethod("SetImportModifiers")
        .onInterface(DbusService::kInterface)
        .withTimeout(timeout)
        .withArguments(drmFormat, modifiers);
}

// Tells the daemon the caller has stopped reading one image, so the producer may draw into it
// again. `bufferId` is the value the same client was handed as GetFrameFd's `buffer_id`.
//
// Here rather than in one test file for the reason this header exists: the argument order
// (monitor first, then the buffer) is a wire fact, and a private copy would go on compiling
// and passing if it ever changed. The daemon attributes the release to the SENDER, so which
// proxy this is called on is part of what a caller is saying -- a proxy on a shared connection
// is the same consumer as far as the daemon can tell.
//
// `timeout` as on setWallpaper() above: zero means the bus's default.
inline void releaseBuffer(sdbus::IProxy& proxy, int32_t monitorId, uint64_t bufferId,
                          std::chrono::microseconds timeout = {}) {
    proxy.callMethod("ReleaseBuffer")
        .onInterface(DbusService::kInterface)
        .withTimeout(timeout)
        .withArguments(monitorId, bufferId);
}

// `timeout` as on setWallpaper() above: zero means the bus's default.
inline StatusReply getStatus(sdbus::IProxy& proxy, std::chrono::microseconds timeout = {}) {
    std::string status;
    try {
        proxy.callMethod("GetStatus").onInterface(DbusService::kInterface).withTimeout(timeout).storeResultsTo(status);
    } catch (const sdbus::Error& e) {
        // Only one direction is knowable from an error name. A name in the daemon's own
        // namespace came from the daemon; everything else is genuinely ambiguous, and the
        // message says so rather than picking. NoReply and Timeout are the case that makes
        // this necessary: they are exactly what a daemon deadlocked inside this very
        // handler produces, so reporting them as "the bus did not answer" sends the reader
        // to the wrong process. ServiceUnknown is the mirror image -- the bus answering,
        // that the daemon is gone. And an exception escaping the handler arrives as
        // org.sdbuscpp.Error, a daemon reply under no daemon name at all (measured).
        const std::string name = e.getName();
        if (daemonErrorName(name)) {
            return StatusReply::failure("the daemon answered GetStatus with an error (" + name +
                                        "): " + e.getMessage());
        }
        return StatusReply::failure("GetStatus failed with " + name +
                                    ", which is not one of the daemon's own error names -- so it is the bus, the "
                                    "connection, or the daemon failing outside its own taxonomy: " + e.getMessage());
    } catch (const std::exception& e) {
        // Not "before any reply", which this cannot know: it is only known that what came
        // out was not a DBus error.
        return StatusReply::failure(std::string("the GetStatus call threw something other than a DBus error: ") +
                                    e.what());
    }
    nlohmann::json json = nlohmann::json::parse(status, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) return StatusReply::failure("GetStatus replied with something that is not JSON");
    if (!json.is_object()) return StatusReply::failure("GetStatus replied with JSON that is not an object");
    return StatusReply(std::move(json));
}

// getStatus(), plus the one line of diagnosis a site needs when it is going to let the
// checks below it go red rather than skip.
//
// Without it those checks report the field they could not read ("GetStatus reports
// last_error for a failed render") when the truth is that no reply arrived at all, which
// sends the reader to the wrong component. Sites that must SKIP instead -- where the reply
// is a precondition for a later measurement rather than the thing being graded -- call
// getStatus() and route the reason through their own NOT VERIFIED channel.
inline StatusReply getStatusOrExplain(sdbus::IProxy& proxy) {
    StatusReply reply = getStatus(proxy);
    if (!reply.ok()) std::fprintf(stderr, "  %s\n", reply.error().c_str());
    return reply;
}

// GetStatus's entry for one monitor, or nullptr when the reply does not contain one.
//
// Every caller goes through this rather than writing json["wallpapers"][id] itself. The
// parsed reply is const, so operator[] on it is nlohmann's const overload -- JSON_ASSERT
// followed by a dereference. Under NDEBUG the assert compiles away and a missing key
// dereferences end(), so a daemon regression that dropped "wallpapers" or a monitor from
// the reply would crash the test instead of printing the FAIL line the check exists to
// produce. One accessor is what stops the next site from forgetting the guard.
//
// Callers read individual fields with value(), for the same reason: a missing "type" or
// "has_frame" must make the comparison false, not abort.
//
// Takes the StatusReply rather than a bare json so that a reply which never arrived is
// one thing here and not two: json() is an empty object then, so this returns nullptr and
// the caller's own check reports the monitor as missing rather than reading a field off a
// reply that does not exist.
inline const nlohmann::json* wallpaperStatus(const StatusReply& status, const std::string& monitorId) {
    const nlohmann::json& json = status.json();
    if (!json.is_object() || !json.contains("wallpapers")) return nullptr;
    const nlohmann::json& wallpapers = json["wallpapers"];
    if (!wallpapers.is_object() || !wallpapers.contains(monitorId)) return nullptr;
    const nlohmann::json& entry = wallpapers[monitorId];
    return entry.is_object() ? &entry : nullptr;
}

// GetStatus's connector-name map -- sanitized name -> monitor id -- or nullptr when the
// reply carries none.
//
// Guarded the same way wallpaperStatus() is, and for the same reason: the parsed reply is
// const, so under NDEBUG a const operator[] on a missing key dereferences end() and kills
// the test instead of printing the FAIL line the caller wanted.
inline const nlohmann::json* outputBindings(const StatusReply& status) {
    const nlohmann::json& json = status.json();
    if (!json.is_object() || !json.contains("outputs")) return nullptr;
    const nlohmann::json& outputs = json["outputs"];
    return outputs.is_object() ? &outputs : nullptr;
}

// The monitor GetStatus says `name` resolves to, or kNoOutputBinding when it resolves to
// nothing -- an absent "outputs" object, an absent key, or a value that is not an integer.
//
// `name` is the connector name as GIVEN to SetMonitorOutput, not the form GetStatus
// publishes. The two differ: GetStatus keys its map by sanitizeForLog(name), so a caller
// handing this the raw string it set would be told a perfectly bound name resolves to
// nothing -- a FAIL against a daemon that did everything right. Escaping once here is what
// keeps the daemon's input and display domains meeting in a single place. A caller that
// wants to assert on the published key instead reads outputBindings() directly, which is
// what the escaping check in dbus_service_test does.
//
// A sentinel rather than std::optional so a caller can compare against an expected id in
// one expression. -1 is safe as "no binding" because WallpaperManager rejects every
// negative monitor id, so no real binding can ever carry it.
//
// CONTROL: the sanitizeForLog() call in DbusService::getStatus (backend/daemon/dbus_service.cpp)
//       replaced by the bare name, so the daemon publishes raw keys while this still escapes
//       -> "FAIL: a hostile connector name is published by GetStatus in its escaped form,
//       bound to its own monitor" in dbus_service_test. Reverted.
inline constexpr int kNoOutputBinding = -1;

inline int boundMonitor(const StatusReply& status, const std::string& name) {
    const nlohmann::json* outputs = outputBindings(status);
    // Not idempotent: sanitizeForLog escapes a backslash as two, so this must be applied to
    // the raw name exactly once and never to an already-escaped one.
    const std::string key =
        wallpaperx::sanitizeForLog(name, wallpaperx::kMaxOutputNameBytes, wallpaperx::LogFraming::Bare);
    if (outputs == nullptr || !outputs->contains(key)) return kNoOutputBinding;
    const nlohmann::json& id = (*outputs)[key];
    return id.is_number_integer() ? id.get<int>() : kNoOutputBinding;
}

// GOAL: S6.18 -- GetStatus is one consistent snapshot: it can never report
//       gpu.initialized=false beside a monitor with has_frame=true, because a frame
//       only exists once the Device does.
// WHY-THIS-SHAPE: the obvious alternative is to test the *timing* -- race a
//       SetWallpaper against a GetStatus and hope to catch the window. That cannot be
//       made to fail reliably: the window a two-lock implementation opens is a few
//       microseconds wide, so a test built on it passes on a correct daemon and on a
//       broken one alike. Asserting the forbidden *conjunction* on every reply the
//       suite already collects is falsifiable the moment the state is ever reachable,
//       and costs nothing.
//       PRECONDITION: the reply arrived. Every branch below returns true on an absent
//       field, which is right for a reply that exists -- the invariant forbids a
//       conjunction, so a missing half cannot violate it -- and would be vacuous green
//       for `{}`. Both call sites therefore gate on ok() and print instead of grading.
//       The !ok() line is the fail-safe for a site that forgets: red is recoverable,
//       a silent pass on an empty reply is what this check was already doing once.
// CONTROL: three, all reverted. (a) WallpaperManager::snapshot() patched to clear
//       out.gpu.initialized after copying the monitor map, which is what a second lock
//       acquisition makes reachable -> "FAIL: GetStatus never reports an uninitialised
//       GPU beside a monitor that already has a frame" in both tiers. (b) the fail-safe:
//       DbusService::GetStatus made to throw AND dbus_service_test's ok() gate removed ->
//       the same FAIL line; with the gate restored, the not-graded line instead and no
//       S6.18 verdict. (c) `if (!status.ok()) return false;` deleted with the same gate
//       removed -> that FAIL line disappeared and the run reported S6.18 as held.
inline bool statusSelfConsistent(const StatusReply& status) {
    if (!status.ok()) return false;
    // No is_object() guard: getStatus() rejects a non-object payload as a failure, so an
    // ok() reply is an object by construction and the guard could never fire.
    const nlohmann::json& json = status.json();
    if (!json.contains("gpu") || !json["gpu"].is_object()) return true;
    if (json["gpu"].value("initialized", true)) return true;
    if (!json.contains("wallpapers") || !json["wallpapers"].is_object()) return true;
    for (const auto& [id, entry] : json["wallpapers"].items()) {
        (void)id;
        if (entry.is_object() && entry.value("has_frame", false)) return false;
    }
    return true;
}

// Returns the DBus error name a call failed with, or "" if it unexpectedly succeeded.
inline std::string errorNameFrom(const std::function<void()>& call) {
    try {
        call();
    } catch (const sdbus::Error& e) {
        return e.getName();
    }
    return "";
}

}  // namespace wallpaperx::testing
