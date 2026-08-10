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

#include "daemon/dbus_service.h"
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
};

inline Frame getFrameFd(sdbus::IProxy& proxy, int32_t monitorId) {
    Frame frame;
    proxy.callMethod("GetFrameFd")
        .onInterface(DbusService::kInterface)
        .withArguments(monitorId)
        .storeResultsTo(frame.fd, frame.width, frame.height, frame.memorySize, frame.handleType, frame.syncFds,
                        frame.generation, frame.drmFormat, frame.drmModifier, frame.planes);
    return frame;
}

inline void setWallpaper(sdbus::IProxy& proxy, const std::string& type, const std::string& path, int32_t monitorId) {
    proxy.callMethod("SetWallpaper").onInterface(DbusService::kInterface).withArguments(type, path, monitorId);
}

// The answer to GetStatus, or the reason there is none.
//
// A dropped, saturated or unresponsive bus makes the call THROW rather than hand back an
// unparseable reply, so `if (json.is_discarded())` is not the guard it looks like: the
// discarded case never arrives. An escaping sdbus::Error instead unwinds past every phase
// after the call site into the test's one catch-all -- one opaque line, no failure
// attributed to anything, and no NOT VERIFIED row for the phases that never ran.
//
// So the fault is a value and there is deliberately no throwing form of getStatus() left
// to reach for. That is the point of the shape: a non-throwing helper *beside* a throwing
// one leaves the unsafe call the shorter thing to type, which is how the operator[] guard
// documented on wallpaperStatus() below came to be missing from four of its five original
// sites and then from three more that were written after it.
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

    // Why there is no usable reply, empty when ok(). Names which of the three causes it
    // was -- no answer at all, a daemon-side error reply, or an answer that would not
    // parse -- because those are different components and a caller that prints this must
    // not have to guess which one it is looking at.
    const std::string& error() const { return m_error; }

    // An empty object unless ok().
    const nlohmann::json& json() const { return m_json; }

private:
    nlohmann::json m_json = nlohmann::json::object();
    std::string m_error;
};

// True for the DBus error names that mean the call never reached a live daemon, as opposed
// to a daemon that received it and answered with an error. Enumerated rather than matched
// on the org.freedesktop.DBus.Error. prefix, which a daemon-side reply can also carry.
inline bool busLevelErrorName(const std::string& name) {
    static const char* const kTransport[] = {
        "org.freedesktop.DBus.Error.ServiceUnknown", "org.freedesktop.DBus.Error.NameHasNoOwner",
        "org.freedesktop.DBus.Error.NoReply",        "org.freedesktop.DBus.Error.Timeout",
        "org.freedesktop.DBus.Error.TimedOut",       "org.freedesktop.DBus.Error.Disconnected",
        "org.freedesktop.DBus.Error.NoServer",       "org.freedesktop.DBus.Error.NotSupported",
    };
    for (const char* candidate : kTransport) {
        if (name == candidate) return true;
    }
    return false;
}

inline StatusReply getStatus(sdbus::IProxy& proxy) {
    std::string status;
    try {
        proxy.callMethod("GetStatus").onInterface(DbusService::kInterface).storeResultsTo(status);
    } catch (const sdbus::Error& e) {
        // error() promises to name the component, so the two cases must not share a
        // sentence: "the bus did not answer" sends a reader to the transport, and a daemon
        // that replied with an error name is the daemon's own fault.
        const std::string name = e.getName();
        if (busLevelErrorName(name)) {
            return StatusReply::failure("the bus did not answer GetStatus (" + name + "): " + e.getMessage());
        }
        return StatusReply::failure("the daemon answered GetStatus with an error (" + name + "): " + e.getMessage());
    } catch (const std::exception& e) {
        return StatusReply::failure(std::string("the GetStatus call failed before any reply: ") + e.what());
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
// produce. One accessor is what stops the next site from forgetting the guard; of the
// five sites that existed, four had.
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
//       A reply that never arrived is false, not true. Every other branch here returns
//       true on an absent field, which is right for a *present* reply -- the invariant
//       forbids a conjunction, so a missing half cannot violate it -- and is exactly
//       wrong for no reply at all: `{}` would report the invariant verified on nothing.
//       Deciding it at the call sites instead was rejected because it already failed
//       that way; one of the three forgot, and it was the site that graded S6.18.
// CONTROL: two, reverted. (a) WallpaperManager::snapshot() patched to clear
//       out.gpu.initialized after copying the monitor map, which is what a second lock
//       acquisition makes reachable -> "FAIL: GetStatus never reports an uninitialised
//       GPU beside a monitor that already has a frame" in both tiers. (b) DbusService's
//       GetStatus made to throw -> the same FAIL line, where before the reply-gating
//       branch this check passed on the empty reply.
inline bool statusSelfConsistent(const StatusReply& status) {
    if (!status.ok()) return false;
    const nlohmann::json& json = status.json();
    if (!json.is_object()) return true;
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
