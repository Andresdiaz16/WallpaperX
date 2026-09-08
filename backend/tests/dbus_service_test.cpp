// SPDX-License-Identifier: GPL-2.0-or-later

// System-level test for Step 6: stands up the real DbusService on the session bus
// and drives it through a real sdbus-c++ proxy client (same process, separate
// connections -- messages still round-trip through the bus). A static wallpaper is
// rendered on the GPU and its frame comes back over DBus as a real Unix fd plus the
// metadata an importer needs.
//
// Run under a private bus, e.g. `dbus-run-session -- ./wallpaperx-dbus-service-test`.
//
// Two structural rules this file follows, both learned the hard way:
//
//  - Everything that needs no GPU runs first. This entry used to be
//    hardware-independent and no longer is, and the parts that still are -- the
//    wire-signature lock above all -- are too valuable to lose on a host whose Vulkan
//    stack cannot come up.
//  - Nothing waits on a sleep. Since task 3, SetWallpaper returns before the render
//    happens, so "the frame is ready" is knowable only from the FrameReady signal.
//    Negative cases ("no signal for this one") are settled by posting a later request
//    that *does* signal: the daemon has a single FIFO render thread, so that later
//    signal proves the earlier request was already processed.
#include "daemon/dbus_service.h"
#include "daemon/render_worker.h"
#include "daemon/wallpaper_manager.h"

#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"

// The renderer's shared test scaffolding, on the include path as `support/...`. The
// backend's own lives in tests/client/, deliberately NOT tests/support/: a quoted
// include searches the including file's own directory before any -I path, and this file
// sits in backend/tests/ -- so a backend/tests/support/exported_frame_map.h would
// silently shadow the renderer's for the backend tests only, leaving the renderer's own
// probes on the original. Different directory names make that unrepresentable instead
// of merely unlikely.
#include "support/exported_frame_map.h"
#include "support/validation_grade.h"
#include "tests/client/daemon_client.h"
#include "tests/test_report.h"

#include <sdbus-c++/sdbus-c++.h>

#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using wallpaperx::DbusService;

namespace {

// The client-side wire helpers now live in tests/client/daemon_client.h, shared with
// daemon_system_test so both tiers read the same reply layout. Pulled in one name at a
// time rather than with a blanket using-directive, so it stays visible which of them
// this file depends on.
using wallpaperx::testing::Check;
using wallpaperx::testing::errorNameFrom;
using wallpaperx::testing::Frame;
using wallpaperx::testing::FrameReadyLog;
using wallpaperx::testing::getFrameFd;
using wallpaperx::testing::getStatusOrExplain;
using wallpaperx::testing::openFdCount;
using wallpaperx::testing::setImportModifiers;
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::StatusReply;
using wallpaperx::testing::validationWaived;
using wallpaperx::testing::wallpaperStatus;

// The pass / fail contract lives in tests/test_report.h, shared with the other five test
// binaries so the wording and the exit codes cannot drift apart.
//
// All three channels are used. A host with no validation-layer package is still a hard
// failure, not a skip -- that host loses the only GPU-side oracle the suite has, and reporting
// it as a skip would retire that oracle quietly. But two goals here genuinely cannot be asked
// on every host, and both used to print a reason and exit 0, which ctest cannot tell apart from
// a run that established them:
//
//   S6.12's unreadable-file clause  needs a process that cannot read a mode-000 file. Root can.
//   S6.24's content half            needs a CPU-addressable export: DMA_BUF on a LINEAR
//                                   modifier, seekable, mappable. NVIDIA ships OPAQUE_FD.
//
// Those now call notVerified(), so ctest reports the run as a skip. The wording names the
// clause rather than the whole goal in the first case: S6.12's is_regular_file half runs and
// holds on every host, and marking all of S6.12 unverified would understate what ran.

// Deliberately not square. With equal dimensions a width/height swap anywhere between
// the target and the wire would satisfy every assertion below.
constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 48;

// Monitor ids are kept disjoint per scenario on purpose: the "nothing rendered yet"
// assertions have to run against an id that no other check has ever touched, or they
// silently start passing/failing for the wrong reason as the test grows.
constexpr int32_t kMonitorStatic = 0;
constexpr int32_t kMonitorVideo = 1;
constexpr int32_t kMonitorCorrupt = 2;
constexpr int32_t kMonitorBarrier = 3;
constexpr int32_t kMonitorBurstFailLast = 4;
constexpr int32_t kMonitorBurstRenderLast = 5;
constexpr int32_t kMonitorBurstContent = 6;
constexpr int32_t kMonitorCoalesceBarrier = 7;
constexpr int32_t kMonitorErrorRecovery = 8;
constexpr int32_t kMonitorNeverSet = 9;
constexpr int32_t kMonitorWorkerCoalesce = 10;
// Its own id, kept off kMonitorNeverSet: the rejected-path checks below assert that the
// *reply* refuses them, and if one ever stopped refusing, the request would be recorded
// -- turning the separate "never set" assertions into a second failure for the same
// fault, pointing at the wrong property.
constexpr int32_t kMonitorRejectedPaths = 11;

// Used only by runUnservicedRequestChecks, which brings a WallpaperManager and a
// DbusService of its own. Numbered on from the ids above even though they live in a
// different manager: an id that means two things in one file is how a check quietly
// starts observing the wrong monitor.
constexpr int32_t kMonitorUnservicedStatic = 12;
constexpr int32_t kMonitorUnservicedVideo = 13;
constexpr int32_t kMonitorUnservicedNeverSet = 14;

// Step 10's connector-name ids, on the shared manager the host checks drive. Naming a
// monitor records nothing in the wallpaper map, so these ids never appear in GetStatus's
// `wallpapers` object and cannot disturb the never-set assertions above.
constexpr int32_t kMonitorOutputA = 15;
constexpr int32_t kMonitorOutputB = 16;
// Holds one accepted name ("DP-KEEP"), then receives every refused name. The accepted name
// is load-bearing, not setup: it is what lets the refusal check grade destroys-nothing
// rather than only adds-nothing.
constexpr int32_t kMonitorOutputRejected = 17;
constexpr int32_t kMonitorOutputAtCap = 18;
constexpr int32_t kMonitorOutputHostile = 19;

// runImportModifierChecks brings a manager of its own, for the reason that function's
// header gives. Two monitors, because the property under test is what happens to the one
// whose re-render has NOT run yet -- a single monitor cannot express it.
constexpr int32_t kMonitorModifiersRerendered = 20;
constexpr int32_t kMonitorModifiersPending = 21;

// On the SHARED manager, unlike the two above: runImportModifierWireChecks drives the real
// daemon over the bus. Its own id so the exact generation it waits for is a fact about
// this monitor alone and cannot be reached by another phase's render.
constexpr int32_t kMonitorModifiersWire = 22;

// The barrier for the repeated-report check, and deliberately a monitor of its own rather
// than the one above: a barrier posted on the same monitor can be coalesced with the
// re-render the check is looking for, which makes the defect and the fix produce identical
// evidence. Set for the first time inside that check, so it is absent from the manager's
// map while the repeat is processed and cannot be re-posted by it.
constexpr int32_t kMonitorModifiersBarrier = 23;

// runImportModifierClientChecks brings a manager of its own and never renders. Its monitor
// holds a recorded request and no frame, which is what makes the list setImportModifiers
// returns a usable "did the effective set change" probe: such a monitor is re-posted on every
// real change and on nothing else.
constexpr int32_t kMonitorModifiersIdle = 24;

// On the SHARED manager: runModifierWithdrawalRenderChecks needs a monitor it can hold at "no
// frame at all" across a consumer's departure, and a barrier of its own to prove the render
// worker has drained. Two ids rather than one because the barrier's own render must not be
// the render the check is waiting for.
constexpr int32_t kMonitorWithdrawRender = 25;
constexpr int32_t kMonitorWithdrawBarrier = 26;

// On runImportModifierChecks' own manager. The first is driven to a wallpaper type this build
// cannot draw while it still owns the image it rendered as a static one -- the state S6.53 is
// about. The other two are a pair whose frames must survive a report that is undone before the
// render thread sees it (S6.54); two, because the monitor that proves it must be one that is
// never re-rendered, and something else has to carry the set across to the GPU side.
constexpr int32_t kMonitorModifiersUnrenderable = 27;
constexpr int32_t kMonitorModifiersUndoneA = 28;
constexpr int32_t kMonitorModifiersUndoneB = 29;

// On runImportModifierAnnouncementChecks' own manager, which renders nothing. The pair exists to
// separate "the set changed" from "a monitor has to be redrawn": the first is a type this build
// cannot draw, so it is never handed back to re-render, and the second is a renderable monitor
// with no frame yet, which is handed back on every change.
constexpr int32_t kMonitorAnnounceUnrenderable = 30;
constexpr int32_t kMonitorAnnounceCondemned = 31;

// The ids below are LOW, and repeat numbers the shared manager uses, because the daemon caps
// monitor ids at WallpaperManager::kMaxMonitors and the names above have used every one. Each
// of these lives on a manager of its own, where the id space starts empty again, so the repeat
// is only in the number and never in the state.
//
// On runBufferHoldChecks' own manager. The first is driven until only the producer's reserve
// is left free, which needs TWO consumers at their per-consumer cap -- see that phase's header.
// The second carries the opposite case and must not share a monitor with it: one consumer that
// never releases anything, which has to run out of hand-overs on the cap alone while the ring
// still has images free.
constexpr int32_t kMonitorHoldRing = 0;
constexpr int32_t kMonitorHoldNeverReleases = 1;
// The third case, and it needs a monitor of its own for the same reason: frames exported to a
// caller with no bus name at all, whose claims -- if the daemon recorded any -- nothing could
// ever withdraw.
constexpr int32_t kMonitorHoldUnnamed = 2;
// The fourth, and it needs its own monitor because it is the only one driven past the holder
// limit: five consumers, where every other case here has one to three. On a shared monitor the
// holder count would be decided by an earlier case's traffic rather than by this one.
constexpr int32_t kMonitorHoldLimit = 3;

// On runBufferReleaseWireChecks' own manager, which is driven entirely over the bus.
constexpr int32_t kMonitorReleaseWire = 0;

// How many images a monitor is expected to hold, written out rather than taken from
// WallpaperManager::kTargetDepth. Comparing the daemon against its own constant would make
// "the ring is five deep" unfalsifiable: a change to kTargetDepth would move both sides of
// the comparison at once and the checks built on it would stay green for any depth at all.
constexpr int kExpectedRingDepth = 5;

// The same, for how many images one consumer may hold before a further hand-over is refused,
// and deliberately not WallpaperManager::kMaxHeldPerClient for the same reason.
constexpr int kExpectedHeldPerClient = 2;

// The same again, for how many consumers may hold images on one monitor before a further one is
// refused, and deliberately not WallpaperManager::kMaxHoldersPerMonitor: reading the daemon's
// own constant would let a change to it move both sides of every comparison below and keep the
// holder-limit checks green for any limit at all.
constexpr int kExpectedHoldersPerMonitor = 4;

// The reserve check drives two consumers to their cap and needs one image still free afterwards,
// which is the image the producer keeps. A depth the caps could not fill would leave that check
// grading a monitor with slack in it rather than one down to its reserve.
static_assert(kExpectedRingDepth == 2 * kExpectedHeldPerClient + 1,
              "the ring phase fills the ring with exactly two consumers at their cap and expects one image left; a "
              "different depth needs a different number of consumers or the reserve is never reached");

// The holder-limit checks fill the limit with consumers that all claim the SAME image, so the
// reserve stays out of it and a refusal there is the holder bookkeeping. That only works while
// one image plus the producer's reserve is less than the depth.
static_assert(kExpectedHoldersPerMonitor >= 2 && kExpectedRingDepth >= 2,
              "the holder-limit checks need at least two holders on one image and a ring with room to spare, or the "
              "refusal they grade could equally be the producer's reserve");

// Well-formed DRM format modifiers that no driver in this suite can allocate: 0x0f is an
// unassigned vendor byte in drm_fourcc.h, so these cannot collide with a real layout however
// the host's driver changes. A consumer reporting one is reporting a layout it can read and
// this GPU cannot produce, which is the state the invalidation half of the contract needs.
// Deliberately not kDrmModifierInvalid: that is the "no layout at all" sentinel, and a report
// naming it would be testing the sentinel rather than an unavailable layout.
constexpr uint64_t kUnavailableModifierA = 0x0f00000000000001ull;
constexpr uint64_t kUnavailableModifierB = 0x0f00000000000002ull;
constexpr uint64_t kUnavailableModifierC = 0x0f00000000000003ull;
constexpr uint64_t kUnavailableModifierD = 0x0f00000000000004ull;

// The bytes that make the hostile connector name hostile, named once so the fixture and
// the scan that looks for them in GetStatus's reply cannot drift apart. Spelled as the
// HTTP tier spells them (http_server_test.cpp), which carries the same pair.
constexpr const char* kBidiOverride = "\xE2\x80\xAE";  // U+202E RIGHT-TO-LEFT OVERRIDE
constexpr char kDel = '\x7F';

// The three error names GetFrameFd answers with when there is no frame. S6.39 is the
// claim that these are three and not two, so they are named once, here, and compared by
// constant -- a literal repeated at each site is how two of them come to be spelled the
// same by accident.
constexpr const char* kErrNoFrame = "io.wallpaperx.Daemon.Error.NoFrame";
constexpr const char* kErrRenderFailed = "io.wallpaperx.Daemon.Error.RenderFailed";
constexpr const char* kErrUnsupported = "io.wallpaperx.Daemon.Error.Unsupported";
static_assert(std::string_view(kErrNoFrame) != std::string_view(kErrRenderFailed) &&
                  std::string_view(kErrNoFrame) != std::string_view(kErrUnsupported) &&
                  std::string_view(kErrRenderFailed) != std::string_view(kErrUnsupported),
              "the three frameless error names must be distinct, or the S6.39 checks that assert them "
              "side by side would pass for a daemon that had collapsed two of them");

// How many requests each coalescing burst fires at one monitor. Five is enough that a
// worker draining one per render still leaves several to collapse.
constexpr int kBurstSize = 5;

std::string testAssetDir() { return std::string(WALLPAPERX_TEST_IMAGE_DIR); }

// The *message* a call failed with, as opposed to errorNameFrom's name. Kept local
// rather than pushed into daemon_client.h: only this tier asserts on message text, and
// a shared helper would invite the system tier to do the same, where the text is not
// the thing under test.
std::string errorMessageFrom(const std::function<void()>& call) {
    try {
        call();
    } catch (const sdbus::Error& e) {
        return e.getMessage();
    }
    return "";
}

// Overwrites `path` with a solid-colour PNG. Separate from writeTestImage because one phase
// turns an undecodable file into a decodable one at a moment of its choosing, and the path has
// to stay the same across that change -- a new path would need a SetWallpaper, which is itself
// a re-post and would grade nothing about who posted the render.
void writeSolidPng(const std::string& path, unsigned char r, unsigned char g, unsigned char b) {
    const int w = 8;
    const int h = 8;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        pixels[i * 4 + 0] = r;
        pixels[i * 4 + 1] = g;
        pixels[i * 4 + 2] = b;
        pixels[i * 4 + 3] = 255;
    }
    if (!stbi_write_png(path.c_str(), w, h, 4, pixels.data(), w * 4)) {
        throw std::runtime_error("dbus_service_test: failed to write " + path);
    }
}

std::string writeTestImage() {
    std::filesystem::create_directories(testAssetDir());
    const std::string path = testAssetDir() + "/dbus_test_blue.png";
    writeSolidPng(path, 0, 0, 255);
    return path;
}

// The colours the coalescing burst cycles through, as RGB. Solid fills, so the rendered
// frame is one colour end to end and identifying which request produced it needs no
// knowledge of how StaticRenderer scales its input.
//
// Chosen so the first and last differ in every channel: a daemon that served the first
// request of a burst instead of the last must be wrong about red, green *and* blue, not
// merely dim.
constexpr int kBurstColorCount = kBurstSize;
constexpr unsigned char kBurstColors[kBurstColorCount][3] = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {32, 160, 224},
};

// Asserted here, at build time, rather than as a runtime check() inside the burst.
// Every operand is constexpr, so the compiler folds the whole expression -- checking it
// at run time meant an edit that made the palette non-discriminating was caught only
// after building, launching a daemon and running a burst, on whichever machine happened
// to run the suite, and was then reported as a test failure rather than as what it is:
// a bad edit to a constant.
static_assert(kBurstColors[0][0] != kBurstColors[kBurstColorCount - 1][0] &&
                  kBurstColors[0][1] != kBurstColors[kBurstColorCount - 1][1] &&
                  kBurstColors[0][2] != kBurstColors[kBurstColorCount - 1][2],
              "the burst palette's first and last colours must differ in every channel, or the pixel comparison in "
              "runCoalescingChecks cannot tell first-wins from last-wins");

// Writes one solid-colour PNG per burst step and returns their paths, in order.
std::vector<std::string> writeBurstImages() {
    std::filesystem::create_directories(testAssetDir());
    std::vector<std::string> paths;
    for (int i = 0; i < kBurstColorCount; ++i) {
        const std::string path = testAssetDir() + "/dbus_burst_" + std::to_string(i) + ".png";

        const int w = 8;
        const int h = 8;
        std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
        for (int p = 0; p < w * h; ++p) {
            pixels[p * 4 + 0] = kBurstColors[i][0];
            pixels[p * 4 + 1] = kBurstColors[i][1];
            pixels[p * 4 + 2] = kBurstColors[i][2];
            pixels[p * 4 + 3] = 255;
        }
        if (!stbi_write_png(path.c_str(), w, h, 4, pixels.data(), w * 4)) {
            throw std::runtime_error("dbus_service_test: failed to write burst test image");
        }
        paths.push_back(path);
    }
    return paths;
}


// Every <arg> of an introspection element, in document order, as
// (direction, "type:name"). Attribute order and whitespace inside the tag don't
// matter -- only the values, which are the wire contract. `element` is "method" or
// "signal"; a signal's args carry no direction, which shows up as an empty string.
std::vector<std::pair<std::string, std::string>> parseArgs(const std::string& xml, const std::string& element,
                                                           const std::string& name) {
    std::vector<std::pair<std::string, std::string>> args;
    // [\s\S] rather than . -- the XML is multi-line and . does not match newline.
    const std::regex elementRe("<" + element + " name=\"" + name + "\">([\\s\\S]*?)</" + element + ">");
    std::smatch elementMatch;
    if (!std::regex_search(xml, elementMatch, elementRe)) {
        return args;
    }
    const std::string body = elementMatch[1].str();

    const std::regex argRe("<arg\\b[^>]*/>");
    const std::regex attrRe("(\\w+)=\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), argRe); it != std::sregex_iterator(); ++it) {
        const std::string tag = it->str();
        std::string type, argName, direction;
        for (auto a = std::sregex_iterator(tag.begin(), tag.end(), attrRe); a != std::sregex_iterator(); ++a) {
            const std::string key = (*a)[1].str(), value = (*a)[2].str();
            if (key == "type") type = value;
            else if (key == "name") argName = value;
            else if (key == "direction") direction = value;
        }
        args.emplace_back(direction, type + ":" + argName);
    }
    return args;
}

using ArgList = std::vector<std::pair<std::string, std::string>>;

// The body of one <interface name="..."> element, or "" when the object does not
// export it. Everything the wire-signature lock below inspects is looked up inside
// this, never in the whole document -- see the annotation at its call site.
std::string interfaceBody(const std::string& xml, const std::string& name) {
    const std::regex interfaceRe("<interface name=\"" + name + "\">([\\s\\S]*?)</interface>");
    std::smatch match;
    if (!std::regex_search(xml, match, interfaceRe)) return {};
    return match[1].str();
}

// Every member of one kind ("method" or "signal") the interface body declares, sorted.
// This is the direction the per-member signature comparison cannot see: it iterates the
// expected table, so a member on the bus that the table omits is never visited.
std::vector<std::string> elementNames(const std::string& ifaceXml, const std::string& element) {
    std::vector<std::string> names;
    const std::regex re("<" + element + " name=\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(ifaceXml.begin(), ifaceXml.end(), re); it != std::sregex_iterator(); ++it) {
        names.push_back((*it)[1].str());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string joinQuoted(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) out += ", ";
        out += "'" + value + "'";
    }
    return out.empty() ? "(none)" : out;
}

// --------------------------------------------------------------------------- //
// Checks that need no working GPU. Kept first and separate so a host that cannot
// create a Vulkan device still verifies the wire contract.
// --------------------------------------------------------------------------- //
void runHostChecks(sdbus::IProxy& proxy, const std::string& imagePath, const Check& check) {
    // GOAL: S6.1 -- the bus name, object path and interface are exactly the three
    //       strings PLAN.md documents; a typo in any of them fails a test.
    // WHY-THIS-SHAPE: every other check in both tiers reaches the daemon through
    //       DbusService::kBusName / kObjectPath / kInterface, so a typo is applied to
    //       the client and the service *together* and the whole suite stays green.
    //       Comparing the constants against literals written out here is the only
    //       shape that can fail, because it is the only place the documented string
    //       appears independently of the code under test. Deliberately literals, not
    //       a shared constant -- a constant would be the same coupling one level up.
    // CONTROL: all three constants in dbus_service.h changed to "...Deamon". Before
    //       this check the whole in-process tier still passed and the system tier
    //       produced one FAIL, whose message was about validation capture. With it:
    //       "FAIL: the bus name is exactly io.wallpaperx.Daemon" plus the object-path
    //       and interface lines. Reverted.
    check(std::string(DbusService::kBusName) == "io.wallpaperx.Daemon",
          "the bus name is exactly io.wallpaperx.Daemon");
    check(std::string(DbusService::kObjectPath) == "/io/wallpaperx/Daemon",
          "the object path is exactly /io/wallpaperx/Daemon");
    check(std::string(DbusService::kInterface) == "io.wallpaperx.Daemon",
          "the interface is exactly io.wallpaperx.Daemon");

    const StatusReply status = getStatusOrExplain(proxy);
    check(status.ok(), "GetStatus returns parseable JSON");
    const nlohmann::json& json = status.json();
    check(json.value("running", false), "GetStatus reports running=true");

    // Nothing has needed the GPU yet, so there is no tier to report. Reporting one here
    // would mean the daemon had guessed.
    // is_object() before value(): value() on a "gpu" that is not an object throws
    // nlohmann's type_error, which would take the run down instead of failing this line.
    check(json.contains("gpu") && json["gpu"].is_object() && json["gpu"].value("initialized", true) == false,
          "GetStatus reports the GPU as uninitialised before the first render");

    // GOAL: S6.16 -- gpu.name and gpu.descriptor_tier are *absent* before the first
    //       render, not merely wrong. The device is created lazily, so any value here
    //       would be a guess, and a client cannot tell a guess from a fact.
    // WHY-THIS-SHAPE: checking initialized==false above is not the same claim. A
    //       daemon that reported initialized=false alongside a hopeful
    //       "descriptor_tier": "Heap" would satisfy it and still hand a client a
    //       number it must not trust. contains() is the only form that fails for that.
    // CONTROL: dbus_service.cpp patched to write gpuJson["descriptor_tier"] before
    //       the `if (gpu.initialized)` block -> "FAIL: no GPU name or descriptor tier
    //       is reported before the device exists". Reverted.
    check(json.contains("gpu") && !json["gpu"].contains("name") && !json["gpu"].contains("descriptor_tier"),
          "no GPU name or descriptor tier is reported before the device exists");

    // A monitor nothing was ever set on has no frame -- a named, distinguishable error
    // rather than a bogus fd.
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorNeverSet); }) == "io.wallpaperx.Daemon.Error.NoFrame",
          "GetFrameFd reports NoFrame for a monitor that was never set");

    check(errorNameFrom([&] { setWallpaper(proxy, "banana", imagePath, kMonitorVideo); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "an unknown wallpaper type is rejected as InvalidArgs");

    check(errorNameFrom([&] { setWallpaper(proxy, "static", imagePath, -1); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a negative monitor id is rejected as InvalidArgs");

    const int32_t tooLarge = wallpaperx::WallpaperManager::kMaxMonitors;
    check(errorNameFrom([&] { setWallpaper(proxy, "static", imagePath, tooLarge); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a monitor id at or above the cap is rejected as InvalidArgs");

    // Both entry points must reject an id the same way. GetFrameFd answering NoFrame
    // for an out-of-range id would tell a polling consumer to retry forever on what is
    // actually a permanent client-side mistake.
    check(errorNameFrom([&] { getFrameFd(proxy, -1); }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "GetFrameFd rejects a negative monitor id as InvalidArgs, not NoFrame");
    check(errorNameFrom([&] { getFrameFd(proxy, tooLarge); }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "GetFrameFd rejects an out-of-range monitor id as InvalidArgs, not NoFrame");

    // A path that is not a file is the commonest user error, and rendering is
    // asynchronous -- so it must fail on the reply rather than becoming silence the
    // client can only discover by polling.
    check(errorNameFrom([&] { setWallpaper(proxy, "static", "/nonexistent/wallpaperx.png", kMonitorNeverSet); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a static path that is not a file is rejected synchronously as InvalidArgs");
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorNeverSet); }) == "io.wallpaperx.Daemon.Error.NoFrame",
          "a rejected request leaves the monitor with no frame");

    // GOAL: S6.12 -- a `static` path that is not a *regular file*, and one that exists
    //       but cannot be *read*, are both rejected on the reply as InvalidArgs.
    //       Existence alone is not the check.
    // WHY-THIS-SHAPE: the nonexistent-path case above is the only one that was
    //       tested, and it is satisfied by a bare exists() test -- so the two halves
    //       the daemon actually implements (is_regular_file, access(R_OK)) had no
    //       coverage at all. A directory and a mode-000 file are the minimal pair that
    //       separates them: both exist, neither is openable, and each fails a
    //       different clause. Rejecting them on the *reply* is the property: rendering
    //       is asynchronous, so anything not caught here becomes silence the client
    //       can only discover by polling GetStatus.
    // CONTROL: the condition in WallpaperManager::requestWallpaper replaced with
    //       `!std::filesystem::exists(path, ec)`. Before these two lines the whole
    //       suite stayed green on both tiers and both ICDs; with them, "FAIL: a
    //       directory is rejected synchronously as InvalidArgs" and "FAIL: an
    //       unreadable file is rejected synchronously as InvalidArgs". Reverted.
    check(errorNameFrom([&] { setWallpaper(proxy, "static", testAssetDir(), kMonitorRejectedPaths); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a directory is rejected synchronously as InvalidArgs");

    // GOAL: S6.12's access(R_OK) clause is either exercised or reported as unexercised --
    //       never silently passed over.
    // WHY-THIS-SHAPE: notVerified() names the clause, not all of S6.12. The
    //       is_regular_file half two lines up runs on every host including root, so
    //       marking the whole goal unverified would understate what the run established.
    //       The obvious simpler alternative -- check(false) when the file stays readable
    //       -- reports a property of the caller's uid as a daemon defect, which is the
    //       attribution error X.3 forbids.
    // CONTROL: the `|| ::access(...) == 0` disjunct forced true. Observed: "NOT VERIFIED
    //       -- S6.12's unreadable-file clause ... could not be exercised", the summary
    //       line "1 goal(s) could not be exercised on this host", exit 77 instead of 0,
    //       and ctest reporting the entry as Skipped rather than Passed. Reverted.
    const std::string unreadablePath = testAssetDir() + "/dbus_test_unreadable.png";
    {
        std::ofstream out(unreadablePath, std::ios::binary);
        out << "not readable";
    }
    std::error_code permEc;
    std::filesystem::permissions(unreadablePath, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, permEc);
    if (permEc || ::access(unreadablePath.c_str(), R_OK) == 0) {
        wallpaperx::testing::notVerified(
            "S6.12's unreadable-file clause (its is_regular_file clause ran and held above)",
            "this process can still read a mode-000 file (running as root?), so the daemon accepting or "
            "rejecting it would say nothing about the access(R_OK) check");
    } else {
        check(errorNameFrom([&] { setWallpaper(proxy, "static", unreadablePath, kMonitorRejectedPaths); }) ==
                  "org.freedesktop.DBus.Error.InvalidArgs",
              "an unreadable file is rejected synchronously as InvalidArgs");
    }
    // Restored so a later `rm -rf` of the asset dir, and a rebuild that rewrites it,
    // are not blocked by a file nothing can open.
    std::filesystem::permissions(unreadablePath, std::filesystem::perms::owner_read |
                                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, permEc);

    // GOAL: S6.38 -- the path check applies to every known type. An empty string, a
    //       nonexistent path and a directory are InvalidArgs for `video` and `shader`
    //       exactly as they are for `static`.
    // WHY-THIS-SHAPE: driven as a table over both types and all three paths rather than
    //       as one representative case, because the defect being closed was a *branch*
    //       (`if (type == "static")`) and a single case cannot distinguish "the check
    //       moved out of the branch" from "the check is still in the branch and this one
    //       path happens to fail for another reason". The final assertion is the one that
    //       makes it about S6.38 rather than about error names: after six rejections the
    //       monitor must still read as never set. If any rejection had instead been
    //       *accepted*, the request would be recorded and GetFrameFd would answer
    //       Unsupported (video/shader are non-static) rather than NoFrame -- so that line
    //       fails loudly in the exact configuration this goal forbids, which is the
    //       silence ("accepted, then frameless forever") the goal describes.
    // CONTROL: the path check in WallpaperManager::requestWallpaper put back inside
    //       `if (type == "static")` (backend/daemon/wallpaper_manager.cpp), re-run after
    //       this file moved onto tests/test_report.h -> 8 FAIL lines and
    //       "dbus_service_test: 8 check(s) failed": one per
    //       (type, path) pair ("a video wallpaper with an empty path is rejected as
    //       InvalidArgs" and the other five), plus "a monitor whose every request was
    //       rejected is still reported as never set" (it answered
    //       io.wallpaperx.Daemon.Error.Unsupported) and the "told to call SetWallpaper
    //       first" line that follows it. Every FAIL line is now a counted check, which is
    //       the point of the per-case shape. Reverted.
    // COVERAGE-ASSERT: the number of (type, path) pairs actually driven is counted and
    //       asserted against the table's own size, so an edit that empties or short-
    //       circuits the loop fails instead of silently asserting nothing.
    {
        const std::string cases[][2] = {
            {"", "an empty path"},
            {"/nonexistent/wallpaperx-video.mp4", "a nonexistent path"},
            {testAssetDir(), "a directory"},
        };
        // Named rather than a braced init-list in the loop head, so the coverage assert
        // below can be computed from the tables instead of restating their size as a
        // literal -- a literal is exactly what goes stale when someone adds a case.
        const char* const types[] = {"video", "shader"};
        const int expectedCases =
            static_cast<int>((sizeof(types) / sizeof(types[0])) * (sizeof(cases) / sizeof(cases[0])));

        int driven = 0;
        for (const char* type : types) {
            for (const auto& [path, description] : cases) {
                ++driven;
                // Each case is its own counted check rather than a hand-printed "FAIL:"
                // line feeding one aggregate assertion. "FAIL: " is the harness's marker
                // for a counted failure, and writing it without incrementing the counter
                // made the summary line disagree with the number of FAIL lines on stderr
                // -- six regressions printed seven FAILs and reported one. Six named
                // checks are also strictly stronger than their conjunction.
                const std::string message = std::string("a ") + type + " wallpaper with " + description +
                                            " is rejected as InvalidArgs";
                check(errorNameFrom([&] { setWallpaper(proxy, type, path, kMonitorRejectedPaths); }) ==
                          "org.freedesktop.DBus.Error.InvalidArgs",
                      message.c_str());
            }
        }
        check(driven == expectedCases,
              "every (type, path) rejection case in the table was actually driven");

        // The point of the goal, as opposed to its symptom: a rejected request must not
        // be *recorded*. A recorded video request answers Unsupported, not NoFrame.
        const std::string afterRejections = errorNameFrom([&] { getFrameFd(proxy, kMonitorRejectedPaths); });
        check(afterRejections == kErrNoFrame,
              "a monitor whose every request was rejected is still reported as never set");
        if (afterRejections != kErrNoFrame) {
            std::fprintf(stderr, "  it answered '%s', so one of the rejected requests was recorded\n",
                         afterRejections.c_str());
        }
        check(errorMessageFrom([&] { getFrameFd(proxy, kMonitorRejectedPaths); }).find("call SetWallpaper first") !=
                  std::string::npos,
              "a monitor whose every request was rejected is told to call SetWallpaper first");
    }

    // Lock the whole wire contract: which methods and signals exist, and every argument
    // of each, in order. The KDE plugin (Step 12) is generated against it, so a
    // reordering, a type change, a dropped slot or an added one must fail here rather
    // than at plugin-import time -- a partial check would let width/height be swapped
    // and still report green.
    //
    // Parsed rather than substring-matched: matching sd-bus's emitted XML verbatim
    // would couple this test to libsystemd's attribute order and spacing, so a
    // formatting change there would look like a wire-format regression.
    std::string introspection;
    proxy.callMethod("Introspect").onInterface("org.freedesktop.DBus.Introspectable").storeResultsTo(introspection);

    // GOAL: S6.1/S6.2 -- the methods and signal belong to io.wallpaperx.Daemon, not
    //       merely to some interface this object happens to export.
    // WHY-THIS-SHAPE: the simpler version searched the whole introspection document
    //       for <method name="SetWallpaper">. That passes for a vtable registered
    //       under any interface name at all -- including a misspelled one -- because
    //       the element it matches is the same either way. Extracting the interface
    //       body first makes the interface name part of what the signature lock
    //       actually locks.
    // CONTROL: kInterface changed to "io.wallpaperx.Deamon" -> "FAIL: the object
    //       exports the io.wallpaperx.Daemon interface" followed by all four
    //       signature lines, since none of the elements are reachable. Reverted.
    const std::string ifaceXml = interfaceBody(introspection, "io.wallpaperx.Daemon");
    check(!ifaceXml.empty(), "the object exports the io.wallpaperx.Daemon interface");

    const std::vector<std::tuple<std::string, std::string, ArgList>> expected = {
        {"method", "SetWallpaper",
         ArgList{{"in", "s:type"}, {"in", "s:path"}, {"in", "i:monitor_id"}}},
        {"method", "GetFrameFd",
         ArgList{{"in", "i:monitor_id"},
                 {"out", "h:fd"},
                 {"out", "u:width"},
                 {"out", "u:height"},
                 {"out", "t:memory_size"},
                 {"out", "i:handle_type"},
                 {"out", "ah:sync_fds"},
                 {"out", "t:frame_generation"},
                 {"out", "u:drm_format"},
                 {"out", "t:drm_modifier"},
                 {"out", "a(tt):planes"},
                 {"out", "ay:device_uuid"},
                 {"out", "t:buffer_id"}}},
        // The id is the one GetFrameFd's `buffer_id` handed out, which is why both are `t`:
        // a consumer says which image it has finished with by naming it back.
        // CONTROL: the second name in `.withInputParamNames("monitor_id", "buffer_id")` on
        //       ReleaseBuffer in DbusService::registerVTable (backend/daemon/dbus_service.cpp)
        //       changed to "buffer" -> "FAIL: wire signature of method ReleaseBuffer is
        //       unchanged", printing expected `t:buffer_id(in)` against actual `t:buffer(in)`.
        //       Reverted.
        {"method", "ReleaseBuffer", ArgList{{"in", "i:monitor_id"}, {"in", "t:buffer_id"}}},
        // CONTROL: the second name in `.withInputParamNames("monitor_id", "output_name")` in
        //       DbusService::registerVTable (backend/daemon/dbus_service.cpp) changed to
        //       "output" -> "FAIL: wire signature of method SetMonitorOutput is unchanged",
        //       printing expected `s:output_name(in)` against actual `s:output(in)`.
        //       Reverted. Dropping the name entirely was tried first and is NOT a usable
        //       control: sd-bus refuses a vtable whose parameter-name count disagrees with
        //       the signature, so the daemon fails to register at all and the run reports an
        //       exception instead of this line.
        {"method", "SetImportModifiers", ArgList{{"in", "u:drm_format"}, {"in", "at:modifiers"}}},
        {"method", "SetMonitorOutput", ArgList{{"in", "i:monitor_id"}, {"in", "s:output_name"}}},
        {"method", "GetStatus", ArgList{{"out", "s:status"}}},
        // A signal's args carry no direction attribute, hence the empty first field.
        {"signal", "FrameReady", ArgList{{"", "i:monitor_id"}, {"", "t:frame_generation"}}},
        // Deliberately argument-free: it says the layouts in force have changed, not what they
        // are, because what they are is what the OTHER consumers reported.
        {"signal", "ImportModifiersChanged", ArgList{}},
    };

    for (const auto& [element, name, expectedArgs] : expected) {
        const ArgList actualArgs = parseArgs(ifaceXml, element, name);
        const std::string label = element + " " + name;
        check(actualArgs == expectedArgs, ("wire signature of " + label + " is unchanged").c_str());
        if (actualArgs != expectedArgs) {
            std::fprintf(stderr, "  %s expected:", label.c_str());
            for (const auto& [dir, arg] : expectedArgs) std::fprintf(stderr, " %s(%s)", arg.c_str(), dir.c_str());
            std::fprintf(stderr, "\n  %s actual:  ", label.c_str());
            for (const auto& [dir, arg] : actualArgs) std::fprintf(stderr, " %s(%s)", arg.c_str(), dir.c_str());
            std::fprintf(stderr, "\n");
        }
    }

    // GOAL: io.wallpaperx.Daemon exports exactly the members named above -- no extra
    //       method or signal reaches the public interface unreviewed. False if the
    //       daemon's vtable declares a member this table does not.
    // WHY-THIS-SHAPE: the loop above iterates the *table*, so it compares only members
    //       the table already names. That makes it a one-directional containment
    //       (table is a subset of the bus) which catches a removed or retyped member and
    //       is structurally blind to an added one -- exactly the change that puts an
    //       unreviewed slot on the interface the KDE plugin is generated against.
    //       Comparing sorted name lists is the direction the per-member loop cannot take.
    //       Scoped to `ifaceXml`, not the whole document, so the standard
    //       org.freedesktop.DBus.* interfaces are not swept in. Methods and signals only:
    //       registerVTable declares no properties, and a property added later would need
    //       its own list rather than silently joining one of these.
    // CONTROL: `sdbus::registerMethod("Debug").implementedAs([this]() { (void)this; })`
    //       added to DbusService::registerVTable (backend/daemon/dbus_service.cpp) ->
    //       "FAIL: io.wallpaperx.Daemon exports exactly the methods this table names",
    //       printing expected "'GetFrameFd', 'GetStatus', 'SetMonitorOutput',
    //       'SetWallpaper'" against actual with 'Debug' first, and it was the only red
    //       line -- every per-member signature check stayed green, which is the
    //       measurement showing this is not redundant with them. Reverted.
    {
        std::vector<std::string> expectedMethods, expectedSignals;
        for (const auto& [element, name, expectedArgs] : expected) {
            (void)expectedArgs;
            (element == "method" ? expectedMethods : expectedSignals).push_back(name);
        }
        std::sort(expectedMethods.begin(), expectedMethods.end());
        std::sort(expectedSignals.begin(), expectedSignals.end());

        const std::pair<const std::vector<std::string>*, const char*> kinds[] = {
            {&expectedMethods, "method"},
            {&expectedSignals, "signal"},
        };
        for (const auto& [expectedNames, element] : kinds) {
            const std::vector<std::string> actualNames = elementNames(ifaceXml, element);
            const std::string message =
                std::string("io.wallpaperx.Daemon exports exactly the ") + element + "s this table names";
            check(actualNames == *expectedNames, message.c_str());
            if (actualNames != *expectedNames) {
                std::fprintf(stderr, "  %ss expected: %s\n  %ss actual:   %s\n", element,
                             joinQuoted(*expectedNames).c_str(), element, joinQuoted(actualNames).c_str());
            }
        }
    }
}

// --------------------------------------------------------------------------- //
// Step 10 -- SetMonitorOutput: which screen answers to which connector name.
// Needs no GPU: naming a screen says nothing about what it draws.
// --------------------------------------------------------------------------- //

// Every connector name GetStatus currently publishes for one monitor. A vector rather
// than a lookup, because the property under test is a COUNT -- "a monitor answers to at
// most one name" is false the moment this has two entries, and a find()-shaped helper
// could not express that.
std::vector<std::string> namesFor(const StatusReply& status, int32_t monitorId) {
    std::vector<std::string> names;
    const nlohmann::json* outputs = wallpaperx::testing::outputBindings(status);
    if (outputs == nullptr) return names;
    for (const auto& [name, id] : outputs->items()) {
        if (id.is_number_integer() && id.get<int>() == monitorId) names.push_back(name);
    }
    return names;
}

// Escaped, always: these names came from a client over the bus, and a failing run is
// exactly when one of them holds the newline or right-to-left override the escaping
// checks below exist to forbid. Printing them raw would let a hostile name break the
// line that reports it -- a name containing "\nFAIL: " would forge an uncounted failure.
// A correct daemon already escaped them, so this is a second pass and not idempotent;
// that is the point, since the fault being reported is the daemon having skipped the first.
std::string joinNames(const std::vector<std::string>& names) {
    std::vector<std::string> escaped;
    escaped.reserve(names.size());
    for (const std::string& name : names) {
        escaped.push_back(
            wallpaperx::sanitizeForLog(name, wallpaperx::kMaxOutputNameBytes, wallpaperx::LogFraming::Bare));
    }
    return joinQuoted(escaped);
}

void runOutputNameChecks(sdbus::IProxy& proxy, const Check& check) {
    using wallpaperx::testing::boundMonitor;
    using wallpaperx::testing::outputBindings;
    using wallpaperx::testing::setMonitorOutput;

    constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";

    check(outputBindings(getStatusOrExplain(proxy)) != nullptr,
          "GetStatus carries an outputs object mapping connector names to monitors");

    // Every SetMonitorOutput below is made through errorNameFrom -- or, for the UTF-8 case,
    // its own try/catch -- the calls made only for their side effect included: bare, an
    // unexpected error reply becomes an exception whose nearest
    // handler is main's catch-all above every later phase, so one refusal here turns "a named
    // check failed" into "the render and coalescing phases never ran".
    // CONTROL: `if (outputName == "DP-MOVE") throw std::runtime_error(...)` added to
    //       WallpaperManager::setMonitorOutput (backend/daemon/wallpaper_manager.cpp) -> four
    //       named FAIL lines from "naming monitor A 'DP-MOVE' is accepted" onward, and the
    //       render phase still ran ("SetWallpaper replied in 0 ms, FrameReady at 164 ms").
    //       Reverting only that one call to a bare one, mutation still in place: a single
    //       "FAIL: unexpected exception: [io.wallpaperx.Daemon.Error.RenderFailed]", no render
    //       phase, and the Vulkan oracle reporting that no device was ever created. Both
    //       reverted.

    // GOAL: S10.7 -- SetMonitorOutput validates its arguments the way every other
    //       externally-reachable call does. An out-of-range monitor id, an empty name and
    //       an oversized name are each refused with a reason, none of them becomes a
    //       binding, and none of them costs the target monitor the name it already answered
    //       to. False if any is accepted, silently unbinds, or crashes the daemon.
    // WHY-THIS-SHAPE: the error name alone is not the check -- a daemon that threw AFTER
    //       recording would satisfy an errorNameFrom() assertion with the wrong screen
    //       mapped -- so each refusal is graded on the state it left, by two different
    //       routes because the two rejection families leave their trace in different
    //       places. The name-validation refusals target monitor 17, so they are graded
    //       through that monitor's name list. The two id refusals target -1 and the cap,
    //       ids no monitor-keyed lookup in this phase can reach, so their adds-nothing half
    //       is graded through the NAME they used: "DP-1" must not be published at all. That
    //       one is asserted with contains() rather than boundMonitor(), whose
    //       kNoOutputBinding sentinel is -1 and would collide with the very id under test.
    //       Monitor 17 is given "DP-KEEP" first and asserted to hold it AFTER the refusals,
    //       not merely asserted empty: a refusal has two obligations, adds-nothing and
    //       destroys-nothing, and an empty monitor can only express the first -- a daemon
    //       that released the monitor's existing name before validating would pass an
    //       emptiness check. The id refusals are therefore made TWICE, once with a name
    //       nothing holds and once with "DP-KEEP": a bad id carrying an unheld name has
    //       nothing to destroy, so that pair alone leaves the destroys-nothing half of this
    //       family ungraded -- which is precisely the shape a stale monitor id from the
    //       plugin takes after a screen is unplugged, where the name is one a live screen
    //       still answers to. The at-cap case is here for the same reason the HTTP tier has
    //       one: an oversized-only check passes for a bound that is off by one in the
    //       accepting direction, and the two surfaces share outputNameError() precisely so
    //       a name one accepts is one the other can hold.
    // CONTROL: five, all reverted, all in backend/daemon/. (a) `requireValidMonitorId(monitorId);`
    //       deleted from WallpaperManager::setMonitorOutput (wallpaper_manager.cpp) -> six
    //       red: all four InvalidArgs lines below, "FAIL: a refused connector name neither
    //       binds ..." printing "monitor 17 answers to (none)", and the cross-monitor check
    //       at the end of the phase. "the connector name refused for an out-of-range monitor
    //       id is not published at all" stays GREEN under this mutation -- the last "DP-KEEP"
    //       call binds to id 32 and its release loop drops the "DP-1" entry the earlier call
    //       had wrongly made -- so the id family's adds-nothing half is carried by the two
    //       InvalidArgs lines here, not by that one.
    //       (b) the same call moved below the bindOutputLocked block, so the id is recorded
    //       and only then refused -> all four InvalidArgs lines stay green (the call still
    //       throws) and exactly two are red: "FAIL: a refused connector name neither binds
    //       ..." printing "monitor 17 answers to (none)", and the cross-monitor check. This
    //       is the mutation the "DP-KEEP" pair was added for -- recording a bad id costs
    //       monitor 17 the name it held.
    //       (c) the `outputNameError` check deleted from setMonitorOutput -> four red: "FAIL:
    //       an empty connector name is rejected as InvalidArgs", "FAIL: an oversized connector
    //       name is rejected as InvalidArgs", "FAIL: a refused connector name neither binds to
    //       the monitor it targeted nor disturbs the name it already answered to" printing the
    //       257-byte name it had wrongly taken -- one name, not two, because the release loop
    //       drops both "DP-KEEP" and the empty name when the oversized one lands on the same
    //       monitor -- and the cross-monitor check. That control is also what measured the
    //       UTF-8 clause below staying green.
    //       (d) destroys-nothing for the NAME family: a copy of bindOutputLocked's release
    //       loop hoisted into setMonitorOutput above the `outputNameError` throw -> exactly
    //       two red, that same check printing "monitor 17 answers to (none)" and the
    //       cross-monitor check, with every InvalidArgs line still green.
    //       (e) destroys-nothing for the ID family, which is what the "DP-KEEP" pair adds:
    //       bindOutputLocked's `find`/`erase` displacement block hoisted into
    //       setMonitorOutput ABOVE `requireValidMonitorId` -> exactly two red, "FAIL: a
    //       refused connector name neither binds ..." printing "monitor 17 answers to
    //       (none)" and the cross-monitor check at the end of the phase, with all four
    //       InvalidArgs lines green. With the "DP-KEEP" pair deleted and that mutation still
    //       in place the run reported "all checks passed" -- measured -- which is the gap
    //       the pair closes. A sixth, `name.size() > kMaxOutputNameBytes` in config_store.h
    //       weakened to `>=`, belongs to the at-cap pair below and is recorded there.
    //
    // Split rather than ANDed: a single conjunction reports one FAIL that cannot say which
    // half broke, and every refusal check below is graded against this binding, so a setup
    // fault would print as four failures of the validation code.
    // CONTROL: the setup name here changed to "" -> "FAIL: the connector name the refusals
    //       below rely on is accepted" printing "SetMonitorOutput refused 'DP-KEEP' with
    //       org.freedesktop.DBus.Error.InvalidArgs", and separately "FAIL: the monitor the
    //       refusals below target answers to a connector name before they are made" printing
    //       "'DP-KEEP' resolves to monitor -1, not 17". Reverted.
    const std::string keepError =
        errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputRejected, "DP-KEEP"); });
    check(keepError.empty(), "the connector name the refusals below rely on is accepted");
    if (!keepError.empty()) {
        std::fprintf(stderr, "  SetMonitorOutput refused 'DP-KEEP' with %s\n", keepError.c_str());
    }
    const int keepBinding = boundMonitor(getStatusOrExplain(proxy), "DP-KEEP");
    check(keepBinding == kMonitorOutputRejected,
          "the monitor the refusals below target answers to a connector name before they are made");
    if (keepBinding != kMonitorOutputRejected) {
        std::fprintf(stderr, "  'DP-KEEP' resolves to monitor %d, not %d\n", keepBinding, kMonitorOutputRejected);
    }

    check(errorNameFrom([&] { setMonitorOutput(proxy, -1, "DP-1"); }) == kInvalidArgs,
          "a negative monitor id is rejected by SetMonitorOutput as InvalidArgs");
    check(errorNameFrom([&] {
              setMonitorOutput(proxy, wallpaperx::WallpaperManager::kMaxMonitors, "DP-1");
          }) == kInvalidArgs,
          "a monitor id at or above the cap is rejected by SetMonitorOutput as InvalidArgs");
    // The same two ids again, now carrying a name a live monitor holds -- the only form in
    // which a bad id has something to destroy.
    check(errorNameFrom([&] { setMonitorOutput(proxy, -1, "DP-KEEP"); }) == kInvalidArgs,
          "a negative monitor id is rejected even when the connector name is one a monitor already holds");
    check(errorNameFrom([&] {
              setMonitorOutput(proxy, wallpaperx::WallpaperManager::kMaxMonitors, "DP-KEEP");
          }) == kInvalidArgs,
          "a monitor id at or above the cap is rejected even when the connector name is one a monitor already holds");
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputRejected, ""); }) == kInvalidArgs,
          "an empty connector name is rejected as InvalidArgs");
    check(errorNameFrom([&] {
              setMonitorOutput(proxy, kMonitorOutputRejected,
                               std::string(wallpaperx::kMaxOutputNameBytes + 1, 'D'));
          }) == kInvalidArgs,
          "an oversized connector name is rejected as InvalidArgs");

    // GOAL: S10.7's UTF-8 clause. A name that is not valid UTF-8 never becomes a binding.
    // WHY-THIS-SHAPE: deliberately NOT written as "the daemon rejects it", because the
    //       daemon never sees it: sd-bus validates every 's' argument as it builds the
    //       message, so the call fails client-side with "Failed to serialize a string value
    //       (Invalid argument)" and no traffic is sent (measured on this stack, and the
    //       error is reprinted below rather than attributed). What is left that this tier CAN
    //       falsify is the outcome -- no binding appears -- and that is what is asserted.
    //       The daemon's own outputNameError() UTF-8 arm is reachable from a hand-edited
    //       config.json (config_store_test) and from GET /config?output= (http_server_test),
    //       and both of those grade it directly.
    //       The graded fact is the THROW, recorded here rather than taken from
    //       errorMessageFrom(): that helper returns "" both for a call that succeeded and
    //       for one that threw carrying no text, so grading `!message.empty()` would report
    //       a differently-worded sdbus-c++ or libsystemd as the daemon having accepted an
    //       invalid name -- a harness fault printed as a production defect.
    //       The printed line no longer ASSERTS which component refused, because the same
    //       catch fires for a daemon error reply and the two are indistinguishable by error
    //       name: sd-bus reports its client-side serialization refusal as InvalidArgs, which
    //       is also what the daemon's own UTF-8 arm answers with. Only a name in the
    //       daemon's own error namespace is attributable, so that is the one branch that
    //       names a component; otherwise the message text is printed and the reader is told
    //       the name does not settle it.
    //       A refusal under one of the transport-failure names is NOT graded as the property
    //       holding: NoReply, ServiceUnknown and Disconnected mean the call never got an
    //       answer at all, and counting them as "the name was refused" would turn a dead
    //       daemon into a green check.
    // CONTROL: not controllable by mutating the daemon *while the bus stack rejects the
    //       argument client-side*, which is what the printed line now states per run. With
    //       WallpaperManager::setMonitorOutput's validation deleted entirely (control (c)
    //       above) this check stayed GREEN -- observed -- because the message still never
    //       left the client. It is controllable from the harness side, both observed and
    //       reverted: (i) `badUtf8Name` set to a valid "DP-3" -> "FAIL: a connector name
    //       that is not valid UTF-8 does not complete as a successful call", printing
    //       "unexpectedly completed as a successful call", plus the refusal-safety check
    //       below red with "monitor 17 answers to 'DP-3'" and the cross-monitor check at the
    //       end of the phase -- the name lands for real once it is serialisable. (ii) the call redirected to a proxy for a bus name nothing owns ->
    //       the same FAIL line, printing "the call ... failed without an answer
    //       (org.freedesktop.DBus.Error.ServiceUnknown: The name io.wallpaperx.NobodyOwnsThis
    //       was not provided by any .service files), so this run establishes nothing about
    //       the name" -- where grading the throw alone would have called that transport
    //       failure a refusal and passed.
    const std::string badUtf8Name = std::string("DP\xFF-3");
    bool utf8NameRefused = false;
    std::string busRefusal;
    std::string refusalName;
    try {
        setMonitorOutput(proxy, kMonitorOutputRejected, badUtf8Name);
    } catch (const sdbus::Error& e) {
        utf8NameRefused = true;
        refusalName = std::string(e.getName());
        // Name as well as message: an error carrying no text still has to say which
        // component answered, or the diagnostic reads as though nothing happened.
        busRefusal = refusalName + (e.getMessage().empty() ? "" : ": " + std::string(e.getMessage()));
    }
    const bool noAnswer = refusalName == "org.freedesktop.DBus.Error.NoReply" ||
                          refusalName == "org.freedesktop.DBus.Error.ServiceUnknown" ||
                          refusalName == "org.freedesktop.DBus.Error.Disconnected";
    if (!utf8NameRefused) {
        std::printf("dbus_service_test: a connector name that is not valid UTF-8 unexpectedly completed as a "
                    "successful call\n");
    } else if (noAnswer) {
        std::printf("dbus_service_test: the call carrying a connector name that is not valid UTF-8 failed without an "
                    "answer (%s), so this run establishes nothing about the name\n",
                    busRefusal.c_str());
    } else if (wallpaperx::testing::daemonErrorName(refusalName)) {
        std::printf("dbus_service_test: a connector name that is not valid UTF-8 reached the daemon, which refused "
                    "it: '%s'\n",
                    busRefusal.c_str());
    } else {
        // InvalidArgs is the one name both sides can produce -- sd-bus reports a client-side
        // serialization refusal under it, and the daemon's own UTF-8 arm answers with it --
        // so the message text is printed instead of an attribution the name cannot support.
        std::printf("dbus_service_test: a connector name that is not valid UTF-8 was refused with '%s' -- the message "
                    "says which component answered, the error name alone does not\n",
                    busRefusal.c_str());
    }
    std::fflush(stdout);
    check(utf8NameRefused && !noAnswer,
          "a connector name that is not valid UTF-8 does not complete as a successful call");

    // One snapshot, read two ways: the name-validation refusals graded through the monitor
    // they targeted, the id refusals through the name they used.
    const StatusReply afterRefusals = getStatusOrExplain(proxy);
    const std::vector<std::string> refusedNames = namesFor(afterRefusals, kMonitorOutputRejected);
    const std::vector<std::string> keptOnly{"DP-KEEP"};
    check(refusedNames == keptOnly,
          "a refused connector name neither binds to the monitor it targeted nor disturbs the name it already "
          "answered to");
    if (refusedNames != keptOnly) {
        std::fprintf(stderr, "  monitor %d answers to %s\n", kMonitorOutputRejected, joinNames(refusedNames).c_str());
    }

    // The reply's arrival is part of the assertion, not a precondition assumed: a GetStatus
    // that never answered publishes no "DP-1" either, and would otherwise be graded as the
    // property holding.
    const nlohmann::json* afterRefusalBindings = outputBindings(afterRefusals);
    const bool badIdNamePublished = afterRefusalBindings != nullptr && afterRefusalBindings->contains("DP-1");
    check(afterRefusalBindings != nullptr && !badIdNamePublished,
          "the connector name refused for an out-of-range monitor id is not published at all");
    if (badIdNamePublished) {
        std::fprintf(stderr, "  'DP-1' resolves to monitor %s\n", (*afterRefusalBindings)["DP-1"].dump().c_str());
    }

    // Split for the same reason as the "DP-KEEP" precondition above: refused-outright and
    // accepted-but-not-published are different faults, and one graded conjunction can name
    // neither. The name itself is not printed -- it is 256 bytes of 'D' -- only its length.
    // CONTROL: `status["outputs"]` dropped from DbusService::getStatus
    //       (backend/daemon/dbus_service.cpp) -> the publication half alone red, "FAIL: a
    //       connector name of exactly the maximum length is published for the monitor it was
    //       given to" printing "the 256-byte name resolves to monitor -1, not 18", with the
    //       acceptance half green. `name.size() > kMaxOutputNameBytes` weakened to `>=` in
    //       config_store.h reddens both halves, the acceptance one printing "SetMonitorOutput
    //       refused a 256-byte name with org.freedesktop.DBus.Error.InvalidArgs" -- which is
    //       the fault the single conjunction could not name. Both reverted.
    const std::string atCap(wallpaperx::kMaxOutputNameBytes, 'D');
    const std::string atCapError = errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputAtCap, atCap); });
    check(atCapError.empty(), "a connector name of exactly the maximum length is accepted");
    if (!atCapError.empty()) {
        std::fprintf(stderr, "  SetMonitorOutput refused a %zu-byte name with %s\n", atCap.size(),
                     atCapError.c_str());
    }
    const int atCapBinding = boundMonitor(getStatusOrExplain(proxy), atCap);
    check(atCapBinding == kMonitorOutputAtCap,
          "a connector name of exactly the maximum length is published for the monitor it was given to");
    if (atCapBinding != kMonitorOutputAtCap) {
        std::fprintf(stderr, "  the %zu-byte name resolves to monitor %d, not %d\n", atCap.size(), atCapBinding,
                     kMonitorOutputAtCap);
    }

    // GOAL: S10.4 -- a connector name maps to exactly one monitor. Assigning a name another
    //       monitor already holds MOVES it. False if, after reassigning, the old monitor
    //       still answers for that name: two plugin instances would then claim one screen,
    //       and whichever the daemon returned first would decide which desktop is wrong.
    // WHY-THIS-SHAPE: asserted as "which monitor does this name resolve to now", not as
    //       "how many entries does the map have". GetStatus publishes name -> id, so a name
    //       held twice is unrepresentable in the reply and a size check could never see the
    //       defect; the resolution, on the other hand, is exactly what a plugin asks for and
    //       goes wrong in the observable direction. The first binding is asserted BEFORE the
    //       reassignment, so a run where the first call silently did nothing cannot be read
    //       as a successful move.
    //       The releasing half is graded on the reply having arrived, not on the old monitor's
    //       name list merely being empty: a GetStatus that never answered lists nothing for
    //       monitor 15 either, so a bare emptiness check would read a lost reply as the
    //       release having happened.
    // CONTROL: the body of the `if (existing != m_outputs.end())` branch in
    //       WallpaperManager::bindOutputLocked (backend/daemon/wallpaper_manager.cpp) --
    //       `m_outputs.erase(existing); displaced = true;` -- replaced with
    //       `return BindResult::Unchanged;`, i.e. first-wins -> "FAIL: reassigning a connector
    //       name moves it to the new monitor", printing "'DP-MOVE' still resolves to monitor
    //       15, not 16", and "FAIL: the monitor a connector name was taken from answers to it
    //       no longer", printing "monitor 15 still answers to 'DP-MOVE'". The arrival half:
    //       `status["outputs"] = std::move(outputs);` dropped from DbusService::getStatus
    //       (backend/daemon/dbus_service.cpp) -> both halves red, the move check printing
    //       "'DP-MOVE' still resolves to monitor -1, not 16" and the release check "GetStatus
    //       published no outputs object to read the old monitor from", beside every other
    //       outputs-reading check in this phase. Both reverted.
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputA, "DP-MOVE"); }).empty(),
          "naming monitor A 'DP-MOVE' is accepted");
    check(boundMonitor(getStatusOrExplain(proxy), "DP-MOVE") == kMonitorOutputA,
          "a connector name resolves to the monitor it was just given to");
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputB, "DP-MOVE"); }).empty(),
          "reassigning 'DP-MOVE' to monitor B is accepted");
    // One snapshot, read two ways: arrival on the new monitor and release from the old are
    // two halves of ONE state. Two reads would let a daemon that reported the move in the
    // first reply and a stale map in the second be graded correct, and would print two
    // states that never coexisted.
    const StatusReply afterMoveStatus = getStatusOrExplain(proxy);
    const int afterMove = boundMonitor(afterMoveStatus, "DP-MOVE");
    check(afterMove == kMonitorOutputB, "reassigning a connector name moves it to the new monitor");
    if (afterMove != kMonitorOutputB) {
        std::fprintf(stderr, "  'DP-MOVE' still resolves to monitor %d, not %d\n", afterMove, kMonitorOutputB);
    }
    const nlohmann::json* afterMoveBindings = outputBindings(afterMoveStatus);
    const std::vector<std::string> namesOnOldMonitor = namesFor(afterMoveStatus, kMonitorOutputA);
    check(afterMoveBindings != nullptr && namesOnOldMonitor.empty(),
          "the monitor a connector name was taken from answers to it no longer");
    if (afterMoveBindings == nullptr) {
        std::fprintf(stderr, "  GetStatus published no outputs object to read the old monitor from\n");
    } else if (!namesOnOldMonitor.empty()) {
        std::fprintf(stderr, "  monitor %d still answers to %s\n", kMonitorOutputA,
                     joinNames(namesOnOldMonitor).c_str());
    }

    // GOAL: the converse GOALS.md records as a deliberate decision under S10.4 -- naming a
    //       monitor releases whatever name it already answered to, so a monitor responds to
    //       at most one connector. False if a screen the app renames after a replug goes on
    //       resolving under its old name, which is a name pointing at hardware that is no
    //       longer behind it.
    // WHY-THIS-SHAPE: the name COUNT for the monitor is asserted, not just that the new name
    //       resolves. "DP-SECOND resolves to 15" is true whether or not DP-FIRST was
    //       released, so the obvious assertion is satisfied by exactly the implementation
    //       this goal forbids. Counting is possible here only because GetStatus publishes
    //       the whole map rather than answering one lookup at a time.
    // CONTROL: the `for (auto it = m_outputs.begin(); ...)` release loop deleted from
    //       WallpaperManager::bindOutputLocked (backend/daemon/wallpaper_manager.cpp) ->
    //       "FAIL: naming a monitor releases the name it answered to before", printing
    //       "monitor 15 answers to 'DP-FIRST', 'DP-SECOND'", and "FAIL: naming a monitor with
    //       the name it already has leaves exactly one binding". Reverted.
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputA, "DP-FIRST"); }).empty(),
          "naming monitor A 'DP-FIRST' is accepted");
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputA, "DP-SECOND"); }).empty(),
          "renaming monitor A to 'DP-SECOND' is accepted");
    const std::vector<std::string> namesAfterRename = namesFor(getStatusOrExplain(proxy), kMonitorOutputA);
    const bool exactlyOne = namesAfterRename == std::vector<std::string>{"DP-SECOND"};
    check(exactlyOne, "naming a monitor releases the name it answered to before");
    if (!exactlyOne) {
        std::fprintf(stderr, "  monitor %d answers to %s\n", kMonitorOutputA, joinNames(namesAfterRename).c_str());
    }

    // Idempotent, and complete on return: the same pair twice is not an error and leaves
    // one binding. Unlike SetWallpaper there is no render behind the call, so an accepted
    // reply means the mapping is in place -- which is what lets every check in this phase
    // read GetStatus immediately instead of waiting for a signal.
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputA, "DP-SECOND"); }).empty(),
          "naming a monitor with the name it already has is not an error");
    check(namesFor(getStatusOrExplain(proxy), kMonitorOutputA) == std::vector<std::string>{"DP-SECOND"},
          "naming a monitor with the name it already has leaves exactly one binding");

    // GOAL: S10.8 -- a connector name is escaped wherever it is displayed, GetStatus
    //       included. False if a name holding a newline, a right-to-left override or DEL
    //       reaches a display field raw. The name arrives over the bus from a client and is
    //       exactly as untrusted as the GPU name that produced this defect in 4d2d5d8.
    // WHY-THIS-SHAPE: two claims, and neither is enough alone. The specific escaped key must
    //       be present AND resolve to the right monitor -- escaping the display copy must
    //       not change the key the lookup uses -- and separately NO key anywhere in the
    //       reply may hold one of the three raw byte classes. The second is what stops a
    //       future name from being published raw beside a correctly escaped one. Inspected
    //       on the PARSED keys rather than on the serialised reply, because JSON's own
    //       encoder escapes bytes below 0x20 by itself: a newline searched for in the wire
    //       text is absent even from a daemon that does no escaping at all (measured on the
    //       HTTP tier, where a whole-name search stayed green under exactly that mutation).
    // CONTROL: `sanitizeForLog(name, kMaxOutputNameBytes, LogFraming::Bare)` in
    //       DbusService::getStatus (backend/daemon/dbus_service.cpp) replaced with `name` ->
    //       "FAIL: a hostile connector name is published by GetStatus in its escaped form,
    //       bound to its own monitor" ("the escaped name resolves to -1, not 19") and "FAIL:
    //       no connector name GetStatus publishes holds a raw newline, U+202E or DEL", which
    //       printed the offending key as "DP-\xe2\x80\xae\nHDMI\x7f-1" -- one readable line.
    //       With the diagnostic printing rawKey unescaped instead, the same run emitted the
    //       override byte and a real newline, splitting the line in two. Dropping
    //       `status["outputs"]` from the same function
    //       makes the scan red too, printing "the scan saw no key at all for monitor 19, so it
    //       graded nothing" rather than passing on an empty scan. Both reverted.
    // COVERAGE-ASSERT: the fixture is asserted to carry all three byte classes the scan below
    //       searches for, and to be one sanitizeForLog alters. Both halves are needed: a
    //       fixture edit that dropped DEL would leave the scan looking for a byte no key can
    //       hold -- green for a daemon that published it raw -- and the sanitizeForLog clause
    //       alone cannot see that, since it stays true for any name with a newline in it. The
    //       scan itself asserts it saw a key for monitor 19, so an outputs object that arrived
    //       empty cannot be read as "nothing raw was published". Controlled by dropping kDel
    //       from the fixture -> "FAIL: the hostile connector name holds every byte class the
    //       scan looks for...", the only red line. Reverted.
    const std::string hostileName = std::string("DP-") + kBidiOverride + "\nHDMI" + kDel + "-1";
    const std::string escapedName = wallpaperx::sanitizeForLog(hostileName, wallpaperx::kMaxOutputNameBytes,
                                                              wallpaperx::LogFraming::Bare);
    check(hostileName.find(kBidiOverride) != std::string::npos && hostileName.find(kDel) != std::string::npos &&
              hostileName.find('\n') != std::string::npos && escapedName != hostileName,
          "the hostile connector name holds every byte class the scan looks for and is one sanitizeForLog alters");
    check(errorNameFrom([&] { setMonitorOutput(proxy, kMonitorOutputHostile, hostileName); }).empty(),
          "the hostile connector name is accepted by SetMonitorOutput");
    const StatusReply hostileStatus = getStatusOrExplain(proxy);
    // The raw name, not `escapedName`: boundMonitor escapes what it is given, and
    // sanitizeForLog is not idempotent, so handing it the escaped form would look the
    // doubly-escaped name up and miss.
    const int hostileBinding = boundMonitor(hostileStatus, hostileName);
    check(hostileBinding == kMonitorOutputHostile,
          "a hostile connector name is published by GetStatus in its escaped form, bound to its own monitor");
    if (hostileBinding != kMonitorOutputHostile) {
        std::fprintf(stderr, "  the escaped name resolves to %d, not %d\n", hostileBinding, kMonitorOutputHostile);
    }
    // Arrival and the hostile monitor's own key are part of the assertion, not assumed: an
    // empty or missing outputs object scans clean, which an emptiness check alone would
    // grade as the escaping property holding.
    const nlohmann::json* hostileBindings = outputBindings(hostileStatus);
    std::string rawKey;
    bool hostileKeyScanned = false;
    if (hostileBindings != nullptr) {
        for (const auto& [name, id] : hostileBindings->items()) {
            if (id.is_number_integer() && id.get<int>() == kMonitorOutputHostile) hostileKeyScanned = true;
            if (rawKey.empty() && (name.find(kBidiOverride) != std::string::npos ||
                                   name.find(kDel) != std::string::npos || name.find('\n') != std::string::npos)) {
                rawKey = name;
            }
        }
    }
    check(hostileKeyScanned && rawKey.empty(),
          "no connector name GetStatus publishes holds a raw newline, U+202E or DEL");
    if (!hostileKeyScanned) {
        std::fprintf(stderr, "  the scan saw no key at all for monitor %d, so it graded nothing\n",
                     kMonitorOutputHostile);
    }
    // Shown escaped: rawKey is non-empty only when a hostile name reached the reply raw,
    // so printing it verbatim would feed the terminal the very override byte this line is
    // reporting -- and would split the diagnostic across lines on the newline case.
    if (!rawKey.empty()) {
        std::fprintf(stderr, "  raw key published (shown escaped): '%s'\n",
                     wallpaperx::sanitizeForLog(rawKey, wallpaperx::kMaxOutputNameBytes,
                                                wallpaperx::LogFraming::Bare)
                         .c_str());
    }

    // GOAL: a client can find its own screen in GetStatus under the bytes it sent. False if
    //       the only published key is the escaped display form -- for any name escaping
    //       alters, that matches nothing the caller holds, so a client that just assigned a
    //       name cannot locate the entry it created.
    // WHY-THIS-SHAPE: the raw name is looked up directly in `outputs_literal`, not through
    //       boundMonitor(), which escapes before looking up and so passes on a daemon that
    //       publishes only the escaped map -- which is the defect. The escaped map keeps its
    //       own checks above; this one must fail when the literal map alone is missing.
    // CONTROL: `status["outputs_literal"] = std::move(outputsLiteral);` dropped from
    //       DbusService::getStatus (backend/daemon/dbus_service.cpp) -> "FAIL: a connector
    //       name resolves in GetStatus under the exact bytes the client sent", printing
    //       "no outputs_literal object came back", while both escaped-key checks above and
    //       the whole rest of the phase stayed green. Reverted.
    const nlohmann::json& hostileJson = hostileStatus.json();
    const bool literalIsObject = hostileJson.is_object() && hostileJson.contains("outputs_literal") &&
                                 hostileJson["outputs_literal"].is_object();
    const bool literalCarries = literalIsObject && hostileJson["outputs_literal"].contains(hostileName) &&
                                hostileJson["outputs_literal"][hostileName].is_number_integer();
    check(literalCarries && hostileJson["outputs_literal"][hostileName].get<int>() == kMonitorOutputHostile,
          "a connector name resolves in GetStatus under the exact bytes the client sent");
    if (!literalCarries) {
        std::fprintf(stderr, "  %s\n",
                     literalIsObject ? "outputs_literal came back without the name this phase assigned"
                                     : "no outputs_literal object came back");
    }

    // GOAL: naming one monitor leaves every OTHER monitor's connector name intact. False if
    //       the daemon can hold only one name at a time for the whole machine -- on a
    //       two-screen desktop that means naming the second screen erases the first, whose
    //       plugin instance then cannot identify itself and never restores its wallpaper.
    // WHY-THIS-SHAPE: every other check in this phase reads back only the monitor it just
    //       named, so the phase's entire view of the binding map is one monitor wide, and a
    //       release loop that over-erased -- dropping entries belonging to OTHER monitors --
    //       would be invisible to all of them. The gap is in the read pattern, not in any
    //       one check, so it is closed once at the end over every monitor this phase named
    //       rather than by a cross-monitor read after each binding.
    // CONTROL: the `if (it->second != monitorId) { ++it; continue; }` guard deleted from the
    //       release loop in WallpaperManager::bindOutputLocked
    //       (backend/daemon/wallpaper_manager.cpp), so it erases unconditionally -> "FAIL:
    //       naming one monitor leaves every other monitor's connector name intact" printing
    //       monitors 15..18 as "(none)", and it was the ONLY red line in the phase -- which
    //       is the measurement showing it is not redundant with the checks above. Reverted.
    // COVERAGE-ASSERT: the number of monitors actually compared is counted and asserted
    //       against the table's size, so an edit that empties or short-circuits the loop
    //       fails instead of asserting nothing. The reply's arrival is a separate clause of
    //       `independent`, because a missing outputs object makes every lookup return
    //       "(none)" -- a scan that agrees with nothing, which is not the same as a scan
    //       that found no disagreement. Controlled by changing the loop's `continue` to
    //       `break`, so it compares one monitor and stops -> "FAIL: every monitor this phase
    //       named was compared by the cross-monitor check" as the only red line, with the
    //       independence check above it green. Reverted.
    {
        const StatusReply finalNames = getStatusOrExplain(proxy);
        const std::pair<int32_t, std::string> stillHeld[] = {
            {kMonitorOutputA, "DP-SECOND"},
            {kMonitorOutputB, "DP-MOVE"},
            {kMonitorOutputRejected, "DP-KEEP"},
            {kMonitorOutputAtCap, atCap},
            {kMonitorOutputHostile, escapedName},
        };
        int compared = 0;
        bool independent = outputBindings(finalNames) != nullptr;
        for (const auto& [monitorId, expectedName] : stillHeld) {
            ++compared;
            const std::vector<std::string> held = namesFor(finalNames, monitorId);
            if (held == std::vector<std::string>{expectedName}) continue;
            independent = false;
            std::fprintf(stderr, "  monitor %d answers to %s, not to the one name it was last given\n", monitorId,
                         joinNames(held).c_str());
        }
        check(independent, "naming one monitor leaves every other monitor's connector name intact");
        check(compared == static_cast<int>(sizeof(stillHeld) / sizeof(stillHeld[0])),
              "every monitor this phase named was compared by the cross-monitor check");
    }
}

// --------------------------------------------------------------------------- //
// Checks that render on the GPU and export a real frame.
// --------------------------------------------------------------------------- //
void runRenderChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& imagePath,
                     const std::string& corruptPath, const std::string& videoPath, const Check& check) {
    // Timed, because this is the one behaviour nothing else here can distinguish: every
    // other assertion in this function is satisfied just as well by rendering inside the
    // handler. This is also the most expensive request the daemon ever serves -- the
    // first one creates the Vulkan instance, loads the validation layers, builds the
    // device and the descriptor allocator, compiles the pipeline and only then renders.
    //
    // The assertion compares the reply against *this run's own* render duration rather
    // than against a fixed bound. A fixed bound was tried first and was useless: with
    // the render inside the handler the reply took 186 ms on NVIDIA and 29 ms on
    // Lavapipe, so any threshold loose enough to be safe on the slower machine also
    // passed the regression on the faster one. The two clauses cover each other -- a
    // reply that is essentially instant is proof on its own, and the ratio catches a
    // slow bus round trip on a loaded machine, where an absolute bound would flake.
    const auto requestedAt = std::chrono::steady_clock::now();
    setWallpaper(proxy, "static", imagePath, kMonitorStatic);
    const auto replyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - requestedAt).count();

    check(signals.wait(kMonitorStatic, 1), "FrameReady(monitor=0, generation=1) arrives after the first render");
    const auto frameMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - requestedAt).count();

    std::printf("dbus_service_test: SetWallpaper replied in %lld ms, FrameReady at %lld ms\n",
                static_cast<long long>(replyMs), static_cast<long long>(frameMs));
    check(replyMs <= 2 || replyMs * 4 < frameMs, "SetWallpaper replies without waiting for the render to finish");

    {
        const Frame frame = getFrameFd(proxy, kMonitorStatic);

        check(frame.fd.isValid(), "GetFrameFd returns a valid Unix fd after a static render");
        check(frame.width == kTargetWidth && frame.height == kTargetHeight,
              "GetFrameFd reports the target's dimensions");
        // The signal's generation must name the frame the export hands back, or a
        // consumer cannot tell whether the frame it holds is the announced one.
        check(frame.generation == 1, "the exported frame carries the generation FrameReady announced");
        check(frame.handleType == 0 || frame.handleType == 1, "handle_type is OPAQUE_FD (0) or DMA_BUF (1)");

        // GOAL (S6.63, S6.64): a frame is handed over with an acquire descriptor exactly when this
        //       daemon says it can produce one, and with none when it says it cannot -- and the
        //       descriptor that arrives is a live open file this client now owns.
        // WHY-THIS-SHAPE: the two fields are compared against each other rather than either being
        //       asserted outright. "sync_fds is non-empty" is red on Lavapipe, which advertises
        //       VK_KHR_external_semaphore_fd and cannot export an opaque-fd semaphore; "sync_fds
        //       is empty" was the reservation this work replaced. They are produced independently
        //       -- gpu.frame_sync from DeviceInfo when the GPU came up, sync_fds from the export's
        //       own return value on this call -- so a daemon that announces one mode and serves
        //       the other fails here on whichever driver it is wrong on.
        //       Both hand-overs of the SAME generation are graded, not just the first: the
        //       plugin re-fetches a generation on every scene change, and an export that produced
        //       an acquire only the first time would satisfy a check that looked once.
        // CONTROL: `if (!device.info().hasExternalSemaphoreFd) return -1;` in exportFrameAcquireFd
        //       (renderer/vulkan/frame_export.cpp) replaced with an unconditional `return -1;` ->
        //       "FAIL: sync_fds carries an acquire descriptor exactly when GetStatus says this
        //       daemon produces them" twice on dbus_service, dbus_service_dmabuf and
        //       dbus_service_classic, with the Lavapipe entries still green. Reverted.
        // CONTROL: in WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp)
        //       the acquire export guarded by a `static int handedOut` so only the first
        //       hand-over gets one -- a stand-in for any export that produces a payload less
        //       often than once per hand-over -> the same FAIL, once, on the SECOND hand-over
        //       only, with the first still green. Reverted.
        // CONTROL: `gpuJson["frame_sync"]` in DbusService::getStatus (backend/daemon/dbus_service.cpp)
        //       made to publish "yes"/"no" -> "FAIL: GetStatus names the frame hand-over mode as
        //       either \"semaphore\" or \"none\"" on both drivers, and the agreement below did not
        //       run against a mode nobody can read. Reverted.
        // COVERAGE-ASSERT: the mode string is asserted to be one of the two names, and the number
        //       of hand-overs actually compared is asserted. Without the first, a reply carrying
        //       no gpu object satisfies the agreement by leaving both sides empty; without the
        //       second, an edit that stops re-fetching leaves this grading one reply and green.
        const StatusReply syncModeReply = getStatusOrExplain(proxy);
        const nlohmann::json syncGpu = syncModeReply.json().contains("gpu") && syncModeReply.json()["gpu"].is_object()
                                           ? syncModeReply.json()["gpu"]
                                           : nlohmann::json::object();
        const std::string syncMode = syncGpu.value("frame_sync", std::string());
        check(syncMode == "semaphore" || syncMode == "none",
              "GetStatus names the frame hand-over mode as either \"semaphore\" or \"none\"");
        if (syncMode != "semaphore" && syncMode != "none") {
            std::fprintf(stderr, "  gpu.frame_sync was [%s]\n", syncMode.c_str());
        } else {
            const size_t expectedSyncFds = syncMode == "semaphore" ? 1 : 0;
            // The frame already in hand, plus one re-fetch of the same generation. Held together
            // so the second cannot be answered by handing back the first's descriptor.
            const Frame refetched = getFrameFd(proxy, kMonitorStatic);
            int handOversCompared = 0;
            for (const Frame* served : {&frame, &refetched}) {
                ++handOversCompared;
                check(served->syncFds.size() == expectedSyncFds,
                      "sync_fds carries an acquire descriptor exactly when GetStatus says this daemon produces them");
                if (served->syncFds.size() != expectedSyncFds) {
                    std::fprintf(stderr, "  gpu.frame_sync=[%s] but this hand-over carried %zu sync fd(s)\n",
                                 syncMode.c_str(), served->syncFds.size());
                }
                size_t open = 0;
                for (const sdbus::UnixFd& syncFd : served->syncFds) {
                    if (syncFd.get() >= 0 && fcntl(syncFd.get(), F_GETFD) != -1) ++open;
                }
                check(open == served->syncFds.size(),
                      "every descriptor in sync_fds arrives as an open file the client now holds");
            }
            check(refetched.generation == frame.generation,
                  "re-fetching hands back the same generation, so the two hand-overs are of one frame");
            check(handOversCompared == 2, "both hand-overs of that generation were compared against the mode");
            std::printf("dbus_service_test: gpu.frame_sync=%s, sync_fds per hand-over=%zu\n", syncMode.c_str(),
                        frame.syncFds.size());
        }

        // GOAL: memory_size never claims more bytes than the fd actually carries.
        //       glImportMemoryFdEXT and EGL take it as *the* size of the imported object, so a
        //       value past the end of the allocation fails every import.
        // WHY-THIS-SHAPE: an upper bound, not equality. `lseek` measures the page-rounded
        //       backing object, which S5.9 records as OVER-reporting on NVIDIA -- so equality
        //       accuses the daemon of a driver's rounding, and the check could only ever have
        //       run on drivers that happen not to round. Paired with the pixel floor below, it
        //       still pins memory_size into [what the frame needs, what the fd holds]: a daemon
        //       reporting a size it invented fails one end or the other.
        if (frame.handleType == 1) {
            const off_t seekSize = lseek(frame.fd.get(), 0, SEEK_END);
            check(seekSize > 0 && frame.memorySize <= static_cast<uint64_t>(seekSize),
                  "memory_size is within the dma_buf's actual size");
            if (seekSize > 0 && static_cast<uint64_t>(seekSize) != frame.memorySize) {
                std::fprintf(stderr, "  memory_size=%llu, dma_buf is %lld bytes (rounding is the driver's)\n",
                             static_cast<unsigned long long>(frame.memorySize),
                             static_cast<long long>(seekSize));
            }
        }
        // The other end of that bound, and the reason the one above may be an inequality.
        check(frame.memorySize >= static_cast<uint64_t>(kTargetWidth) * kTargetHeight * 4,
              "memory_size covers at least the frame's pixels");

        // The fourcc is channel order, not layout, so it is published on both paths: an
        // importer that has to guess it draws red and blue transposed.
        //
        // What this establishes is that the field REACHES the wire on both handle types -- the
        // opaque path published 0 before. It cannot catch a wrong mapping: both sides of the
        // comparison come from drmFourccForVkFormat, so changing that moves them together.
        // Measured -- mapping B8G8R8A8_UNORM to ARGB instead of XRGB leaves this green, while
        // frame_import_probe's sampled check reds. That check is where a wrong mapping is graded.
        // CONTROL: `frame.drmFormat = drmFormat;` in exportFrame replaced with 0 -> "FAIL: every
        //       exported frame reports the target format's fourcc, whichever handle type it
        //       uses", here and in frame_export_probe. Observed 2026-08-22. Reverted.
        check(frame.drmFormat == wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat),
              "every exported frame reports the target format's fourcc, whichever handle type it uses");

        // The layout fields are populated together or not at all. A half-populated
        // set is the failure that matters: an importer given a modifier but no planes,
        // or planes with no modifier, builds a wrong image rather than failing.
        const bool populated = frame.drmModifier != wallpaperx::kDrmModifierInvalid;
        if (populated) {
            check(frame.handleType == 1, "layout metadata is only published on the DMA_BUF path");
            check(!frame.planes.empty(), "a populated frame carries at least one plane");
            for (const auto& plane : frame.planes) {
                check(plane.get<1>() >= static_cast<uint64_t>(kTargetWidth) * 4,
                      "each plane's rowPitch covers a row of pixels");
            }
        } else {
            check(frame.planes.empty(), "an unpopulated frame reports no planes");
        }
        std::printf("dbus_service_test: handle_type=%d drm_format=0x%08x modifier=0x%llx planes=%zu\n",
                    frame.handleType, frame.drmFormat, static_cast<unsigned long long>(frame.drmModifier),
                    frame.planes.size());
    }

    // The GPU is up now, so GetStatus must say which one and which descriptor tier it
    // resolved to -- the first thing worth knowing when a bug reproduces on one GPU and
    // not another.
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json& json = status.json();
        // value(), not operator[], for the reason wallpaperStatus() exists: a reply with
        // no "gpu" object -- which is also what an unanswered call leaves behind -- would
        // otherwise dereference end() under NDEBUG instead of failing this check.
        const nlohmann::json gpu = json.contains("gpu") && json["gpu"].is_object()
                                       ? json["gpu"]
                                       : nlohmann::json::object();
        const bool reported = gpu.value("initialized", false) && !gpu.value("name", std::string()).empty() &&
                              !gpu.value("descriptor_tier", std::string()).empty();
        check(reported, "GetStatus reports the GPU name and resolved descriptor tier once initialised");
        if (gpu.value("initialized", false)) {
            std::printf("dbus_service_test: gpu=%s tier=%s\n", gpu.value("name", "?").c_str(),
                        gpu.value("descriptor_tier", "?").c_str());
        }

        // See the annotation on statusSelfConsistent in tests/client/daemon_client.h.
        // Asserted here because this is the first reply in the run where the forbidden
        // conjunction is even expressible: monitor 0 holds a frame from the render above.
        //
        // Gated on ok(), and the gate cannot hide a regression: the check above reads its
        // fields off the same reply and is already red when none arrived. Ungated, a reply
        // that never came printed an S6.18 snapshot-atomicity FAIL for an invariant nothing
        // had evaluated -- a second, wrong verdict on top of the one true one.
        if (!status.ok()) {
            std::printf("dbus_service_test: the snapshot-consistency check (S6.18) is not graded -- GetStatus did "
                        "not answer, and the failure that caused that is reported above.\n");
        } else {
            check(wallpaperx::testing::statusSelfConsistent(status),
                  "GetStatus never reports an uninitialised GPU beside a monitor that already has a frame");
        }

        // GOAL: S6.34 -- the Classic descriptor-tier path works end to end *through
        //       the daemon*, not only in a renderer probe. [DoD 12]
        // WHY-THIS-SHAPE: the tier a machine resolves to is a property of the GPU, so
        //       there is nothing to assert unconditionally -- NVIDIA gives Heap and
        //       Lavapipe gives Buffer here, and neither will ever exercise Classic on
        //       its own. The ctest entry dbus_service_classic sets
        //       WALLPAPERX_FORCE_TIER=classic; this reads the same variable and turns
        //       the whole render-and-export run that follows into a Classic-tier run
        //       with a verified tier. Without the assertion the entry would be a
        //       second identical run of the default tier and nobody would know.
        // CONTROL: the entry run with WALLPAPERX_FORCE_TIER=buffer instead ->
        //       "FAIL: WALLPAPERX_FORCE_TIER is honoured through the daemon (tier is
        //       Classic)". Reverted.
        if (const char* forced = std::getenv("WALLPAPERX_FORCE_TIER");
            forced != nullptr && std::strcmp(forced, "classic") == 0) {
            const std::string tier = gpu.value("descriptor_tier", std::string());
            check(tier == "Classic", "WALLPAPERX_FORCE_TIER is honoured through the daemon (tier is Classic)");
            if (tier != "Classic") {
                std::fprintf(stderr, "  WALLPAPERX_FORCE_TIER=classic but the daemon reports tier '%s'\n",
                             tier.c_str());
            }
        }
    }

    // Every GetFrameFd produces a fresh descriptor that the daemon must hand over
    // exactly once. Asserting the returned fd is open proves nothing -- sdbus-c++ dups
    // out of the received message, so the client's copy is always open and is not the
    // daemon's descriptor at all. What is observable is the process fd count: service
    // and client share this process, so a daemon that keeps its own copy (the
    // duplicating UnixFd constructor instead of adopt_fd) shows up as one leaked
    // descriptor per call. One warm-up call first, so one-time bus allocations are not
    // counted as growth.
    { const Frame warmup = getFrameFd(proxy, kMonitorStatic); }
    const size_t fdsBefore = openFdCount();
    constexpr int kPollCount = 32;
    for (int i = 0; i < kPollCount; ++i) {
        const Frame polled = getFrameFd(proxy, kMonitorStatic);
        check(polled.fd.isValid(), "each polled frame carries a valid fd");
    }
    const size_t fdsAfter = openFdCount();
    check(fdsAfter <= fdsBefore + 4, "repeated GetFrameFd calls do not leak descriptors");
    if (fdsAfter > fdsBefore + 4) {
        std::fprintf(stderr, "  fd count grew from %zu to %zu across %d GetFrameFd calls\n", fdsBefore, fdsAfter,
                     kPollCount);
    }

    // Re-rendering the same monitor exercises the queue-family acquire that pairs with
    // the release the exports above performed -- the path that only runs from the second
    // frame onward, and the one validation is silent about.
    setWallpaper(proxy, "static", imagePath, kMonitorStatic);
    check(signals.wait(kMonitorStatic, 2), "FrameReady(generation=2) arrives after a re-render");
    {
        const Frame frame = getFrameFd(proxy, kMonitorStatic);
        check(frame.generation == 2, "frame_generation increments on re-render");
        check(frame.fd.isValid(), "a re-rendered frame still exports a valid fd");
    }

    // --- Requests that must produce no frame and no signal. ---

    // The failure case gets a working wallpaper first, so there is a frame to go stale.
    // Asserting "no frame after a failed render" on a monitor that never had one passes
    // no matter what the daemon does on failure -- which is exactly how a regression
    // that served the previous wallpaper's pixels under the new path would slip through.
    setWallpaper(proxy, "static", imagePath, kMonitorCorrupt);
    check(signals.wait(kMonitorCorrupt, 1), "the monitor used for the failure case renders a frame first");
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorCorrupt); }).empty(),
          "the monitor used for the failure case starts with a valid frame");

    // These three are posted before a request that does signal. The daemon does all GPU
    // work on one FIFO thread, so once the barrier's FrameReady arrives, they have
    // provably been processed -- no sleep, no flake. What that buys the checks below is
    // `servicedSeq`: the corrupt render has provably been attempted (so its last_error is
    // written) and the video request has provably been through the worker. It is NOT what
    // makes the video monitor's `unsupported` key true -- that is derived from the
    // recorded type and holds from the instant SetWallpaper returns, which is a separate
    // property and is asserted on its own in runUnservicedRequestChecks.
    setWallpaper(proxy, "video", videoPath, kMonitorVideo);
    setWallpaper(proxy, "static", corruptPath, kMonitorCorrupt);
    setWallpaper(proxy, "static", imagePath, kMonitorBarrier);
    check(signals.wait(kMonitorBarrier, 1), "FrameReady arrives for the barrier request");

    check(signals.countFor(kMonitorVideo) == 0, "a video request emits no FrameReady");
    // Still 1: the good render above, and nothing for the corrupt one that followed.
    check(signals.countFor(kMonitorCorrupt) == 1, "a render that fails emits no FrameReady");

    // GOAL: S6.39 -- the frameless taxonomy has *three* names, not two, because a client
    //       must respond to them differently: NoFrame means retry, RenderFailed means
    //       stop and show the reason, Unsupported means stop and stop asking, since
    //       nothing about a running build will change it.
    // WHY-THIS-SHAPE: the three names are collected first and asserted together, against
    //       three monitors driven into the three states by the barrier above, rather than
    //       being checked one per scenario where they happen to arise. A daemon that
    //       collapses any *pair* -- which is the regression, and is what shipping code did
    //       for Unsupported-into-NoFrame -- then fails on the line naming the state it
    //       lost, whichever pair it was. Checking them apart cannot do that: a lone
    //       `== NoFrame` assertion passes for a daemon that answers NoFrame to
    //       everything. All three are printed so a failure shows the taxonomy that was
    //       actually observed rather than one mismatched string.
    // CONTROL: two, from opposite ends, both re-run 2026-08-05. (a) the
    //       `reason == Unsupported` branch disabled in DbusService::completeGetFrameFd:
    //       the taxonomy line read "unrenderable type:
    //       'io.wallpaperx.Daemon.Error.NoFrame'" -> "FAIL: a wallpaper type this build
    //       cannot render is reported as Unsupported" (plus the Unsupported-reply-text
    //       line here and the keying line in the unserviced phase), while the NoFrame and
    //       RenderFailed lines of this trio stayed green.
    //       (b) `exportCurrentFrame`'s `!isRenderableType(...)` branch disabled: the
    //       observed name is NoFrame, not the RenderFailed this block used to claim -- the
    //       worker no longer writes a lastError for a non-renderable type, so the
    //       fall-through now reaches NotRendered. Observed: taxonomy line "unrenderable type:
    //       'io.wallpaperx.Daemon.Error.NoFrame'", the same "reported as Unsupported"
    //       FAIL plus "the Unsupported reply says the wait is pointless" and, from the
    //       unserviced phase, "an unrenderable type is Unsupported from the moment it is
    //       recorded". Both reverted.
    {
        const std::string neverSet = errorNameFrom([&] { getFrameFd(proxy, kMonitorNeverSet); });
        const std::string failed = errorNameFrom([&] { getFrameFd(proxy, kMonitorCorrupt); });
        const std::string unsupported = errorNameFrom([&] { getFrameFd(proxy, kMonitorVideo); });
        std::printf("dbus_service_test: frameless taxonomy -- never set: '%s', failed render: '%s', "
                    "unrenderable type: '%s'\n",
                    neverSet.c_str(), failed.c_str(), unsupported.c_str());

        check(neverSet == kErrNoFrame, "a monitor that was never set is reported as NoFrame");
        check(failed == kErrRenderFailed,
              "a failed render is reported as RenderFailed, distinctly from a monitor with nothing rendered yet");
        check(unsupported == kErrUnsupported,
              "a wallpaper type this build cannot render is reported as Unsupported, distinctly from both");

        // The name is only half of it: an Unsupported reply that carried NoFrame's "call
        // SetWallpaper first" text would send a client back round the same loop the new
        // name exists to break.
        const std::string unsupportedText = errorMessageFrom([&] { getFrameFd(proxy, kMonitorVideo); });
        check(unsupportedText.find("call SetWallpaper first") == std::string::npos,
              "the Unsupported reply does not tell the client to call SetWallpaper first");
        check(unsupportedText.find("no frame will ever be produced") != std::string::npos,
              "the Unsupported reply says the wait is pointless rather than merely that there is no frame");
    }

    // GOAL: S6.14 -- the RenderFailed reply carries the render's *own* error text, so
    //       a client that never sees GetStatus can still say why the wallpaper is
    //       missing.
    // WHY-THIS-SHAPE: the name check above passes for a reply whose message is a
    //       fixed string, which is what the error used to be, and that is the half
    //       that made a polling client useless. Compared against the same text
    //       GetStatus publishes for that monitor rather than against a literal:
    //       matching a literal would pin the test to StaticRenderer's wording and
    //       break on any message edit, while agreement between the two channels is
    //       the actual contract and cannot be satisfied by a canned string.
    // CONTROL: the `+ exported.lastError` dropped from the RenderFailed reply in
    //       dbus_service.cpp -> "FAIL: the RenderFailed reply carries the same error
    //       text GetStatus reports". Reverted.
    {
        const std::string replyText = errorMessageFrom([&] { getFrameFd(proxy, kMonitorCorrupt); });
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* corrupt = wallpaperStatus(status, std::to_string(kMonitorCorrupt));
        const std::string statusText = corrupt == nullptr ? std::string() : corrupt->value("last_error", std::string());
        check(!statusText.empty() && replyText.find(statusText) != std::string::npos,
              "the RenderFailed reply carries the same error text GetStatus reports");
        if (statusText.empty() || replyText.find(statusText) == std::string::npos) {
            std::fprintf(stderr, "  reply said '%s' but GetStatus reports last_error '%s'\n", replyText.c_str(),
                         statusText.c_str());
        }
    }

    // The reply to SetWallpaper was already sent by the time the render failed, so
    // GetStatus is the only place that failure is reported.
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* corrupt = wallpaperStatus(status, std::to_string(kMonitorCorrupt));
        const bool reported = corrupt != nullptr && !corrupt->value("last_error", std::string()).empty();
        check(reported, "GetStatus reports last_error for an asynchronous render failure");
    }

    // A monitor that rendered successfully must not carry a stale error.
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* still = wallpaperStatus(status, std::to_string(kMonitorStatic));
        const nlohmann::json* video = wallpaperStatus(status, std::to_string(kMonitorVideo));
        const bool reflected =
            still != nullptr && video != nullptr && still->value("type", std::string()) == "static" &&
            still->value("path", std::string()) == imagePath && still->value("has_frame", false) &&
            still->value("generation", uint64_t{0}) == 2 && still->value("width", uint32_t{0}) == kTargetWidth &&
            still->value("height", uint32_t{0}) == kTargetHeight && !still->contains("last_error") &&
            video->value("type", std::string()) == "video" && !video->value("has_frame", true);
        check(reflected, "GetStatus reflects per-monitor type, path, size, generation and frame availability");
    }

    // GOAL: S6.39's second half -- the unrenderable state is visible on GetStatus too, as
    //       its own always-present `unsupported` boolean. `has_frame=false` with no
    //       `last_error` is exactly what a static render still in flight looks like, so
    //       without a dedicated key a client polling GetStatus cannot tell "wait a moment"
    //       from "never".
    // WHY-THIS-SHAPE: three lines, and the first two are what stop the other two going
    //       vacuous. `unsupported` is asserted as a *pair* -- true on the video monitor,
    //       false on the one that rendered -- because "the flag is set" alone is satisfied
    //       by a daemon that sets it on every monitor, which destroys the distinction in
    //       the other direction. And the key's *presence* is asserted separately on both,
    //       because dbus_service.cpp emits it unconditionally on purpose ("a key that
    //       appears only in the bad case is one a client learns about by hitting it"): were
    //       it emitted only when true, `value("unsupported", false)` on the rendered
    //       monitor would read false from a missing key and the negative direction would
    //       pass without observing anything. last_error is asserted *absent* on the video
    //       monitor rather than present -- the earlier shape of this check read the type
    //       name out of last_error, and production deliberately moved the fact out of that
    //       field because a frontend branching on last_error painted a broken-image state
    //       on a wallpaper the daemon had accepted exactly as asked.
    // CONTROL: `entry["unsupported"] = !isRenderableType(monitor.type);` in
    //       DbusService::getStatus replaced by a literal `false` -> observed exactly one
    //       new FAIL, "GetStatus marks a wallpaper type this build cannot render as
    //       unsupported...", with the presence line and the rendered-monitor line still
    //       green (printed: video=false rendered=false). Replaced by a literal `true`
    //       instead -> the rendered-monitor line goes red on its own. Both reverted.
    // COVERAGE-LOST: the human-readable wording naming *which* type cannot be rendered is
    //       no longer anywhere in GetStatus, so nothing asserts it there; the client-facing
    //       sentence survives only on the GetFrameFd Unsupported reply, checked above.
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* video = wallpaperStatus(status, std::to_string(kMonitorVideo));
        const nlohmann::json* rendered = wallpaperStatus(status, std::to_string(kMonitorBarrier));
        auto flag = [](const nlohmann::json* entry) {
            if (entry == nullptr) return std::string("<no such monitor>");
            if (!entry->contains("unsupported")) return std::string("<key absent>");
            return std::string(entry->value("unsupported", false) ? "true" : "false");
        };
        // Printed unconditionally: when one of the lines below fails, what was actually
        // published is the first thing the reader needs, and a diagnostic gated on the
        // failure has to restate the condition to know it happened.
        std::printf("dbus_service_test: GetStatus unsupported -- video monitor: %s, rendered monitor: %s\n",
                    flag(video).c_str(), flag(rendered).c_str());

        check(video != nullptr && rendered != nullptr && video->contains("unsupported") &&
                  rendered->contains("unsupported"),
              "GetStatus emits the unsupported key for every monitor, not only for the unsupported ones");
        check(video != nullptr && video->value("unsupported", false) && !video->contains("last_error"),
              "GetStatus marks a wallpaper type this build cannot render as unsupported, without also "
              "claiming the request failed");
        check(rendered != nullptr && rendered->value("has_frame", false) && !rendered->contains("last_error") &&
                  !rendered->value("unsupported", true),
              "a monitor that rendered successfully carries no last_error and is not marked unsupported (so "
              "both fields still mean something)");
    }
}

// --------------------------------------------------------------------------- //
// The states a request passes through before anything has rendered it.
//
// This phase exists because those states became unreachable through the daemon the test
// drives above, and the reason is structural rather than incidental. RenderWorker holds
// one FIFO for renders and export tasks alike (its own header says so), and
// DbusService::setWallpaper posts the render from the same event-loop thread that would
// post a later GetFrameFd's export task. So over the bus, an export can never overtake
// the render of the request that preceded it: by the time GetFrameFd is serviced, the
// monitor has either a frame or a failure. Until 2026-08-05 the `video` monitor stood in
// for this -- it stayed recorded-but-unrendered forever -- but a video request now answers
// Unsupported, which is a different state with a different name.
//
// The way back to it is to record the request WITHOUT queueing a render: calling
// WallpaperManager::requestWallpaper directly does exactly that, since it is
// DbusService::setWallpaper's `m_worker.post()` -- not the manager -- that schedules the
// work. Everything the checks then observe still crosses the real wire, through the real
// handler, on the real worker thread: only the render is absent, which is the state under
// test. Same argument as the coalescing phase below: the bus is skipped where, and only
// where, driving through it cannot state the claim.
//
// Its own manager, connection and service, on the connection's unique bus name rather
// than io.wallpaperx.Daemon -- the phases above own that name, and a second holder would
// either be refused or steal it. No render is ever posted here, so this manager never
// builds a Device: the phase costs nothing on the GPU and has no validation grade to
// make, which is why it does not take one.
void runUnservicedRequestChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                                const std::string& videoPath, const Check& check) {
    wallpaperx::WallpaperManager manager(config);

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();

    auto proxyOwner = sdbus::createProxy(sdbus::ServiceName{serviceConn->getUniqueName()},
                                         sdbus::ObjectPath{DbusService::kObjectPath});
    sdbus::IProxy& proxy = *proxyOwner;

    // Recorded, not queued. This is the whole mechanism of the phase, so it is asserted
    // rather than assumed: a request that had been rendered would answer with a frame and
    // the checks below would silently be measuring something else.
    manager.requestWallpaper("static", imagePath, kMonitorUnservicedStatic);
    manager.requestWallpaper("video", videoPath, kMonitorUnservicedVideo);

    // GOAL: S6.7 -- a monitor whose request is recorded but has produced nothing yet is
    //       NoFrame, and that is ordinary protocol state, reported *distinctly* from a
    //       monitor nothing was ever set on. The two demand different client responses:
    //       "your request is in flight, ask again" versus "you never made one".
    // WHY-THIS-SHAPE: both directions are asserted together, and on the *message* rather
    //       than the name, because both states are NoFrame by design -- which is precisely
    //       why the reason lives in the text. A single-direction check passes for a daemon
    //       that gives every monitor the same advice. This phase drives it from an
    //       unserviced request because the route the old check used is gone: it used the
    //       `video` monitor, which is now Unsupported, and no *static* request can be
    //       caught mid-flight over the bus (see the note above this function). The
    //       alternative of racing a real render and hoping to beat it would be an
    //       assertion on the scheduler.
    // CONTROL: NoFrameReason::NotRendered folded into NeverRequested in
    //       WallpaperManager::exportCurrentFrame (the else-branch returning
    //       NeverRequested) -> "FAIL: a recorded-but-unrendered monitor is not told to
    //       call SetWallpaper first", with the never-set line still green. Reverted.
    const std::string pendingName = errorNameFrom([&] { getFrameFd(proxy, kMonitorUnservicedStatic); });
    check(pendingName == kErrNoFrame,
          "a recorded-but-unrendered static monitor is reported as NoFrame, not as a failure");
    const std::string pendingText = errorMessageFrom([&] { getFrameFd(proxy, kMonitorUnservicedStatic); });
    check(pendingText.find("call SetWallpaper first") == std::string::npos,
          "a recorded-but-unrendered monitor is not told to call SetWallpaper first");
    check(errorMessageFrom([&] { getFrameFd(proxy, kMonitorUnservicedNeverSet); }).find("call SetWallpaper first") !=
              std::string::npos,
          "a monitor that was never set is told to call SetWallpaper first");
    if (pendingName != kErrNoFrame) {
        std::fprintf(stderr, "  the unserviced monitor answered '%s': '%s'\n", pendingName.c_str(),
                     pendingText.c_str());
    }

    // GOAL: S6.39's keying -- Unsupported is decided from the recorded *type*, not from
    //       whether the worker has serviced the request yet.
    // WHY-THIS-SHAPE: this is the one place the distinction is observable. In the bus
    //       phase every video request has been serviced by the time it is asked about (the
    //       barrier guarantees it), so an implementation keyed on servicedSeq would pass
    //       there. Here nothing has been serviced at all, so a servicedSeq-keyed daemon
    //       answers NoFrame/NotRendered -- "not yet" for a request whose answer is
    //       "never", which is the retry loop S6.39 exists to break. Asserted beside the
    //       unserviced *static* monitor above, which must answer NoFrame in the same
    //       instant: the pair is what shows the daemon is branching on type rather than
    //       on progress.
    // CONTROL: `if (!isRenderableType(it->second.type))` in exportCurrentFrame made
    //       conditional on `it->second.servicedSeq == it->second.requestSeq` as well
    //       (re-run 2026-08-05 against the current spelling of that branch) -> "FAIL: an
    //       unrenderable type is Unsupported from the moment it is recorded, before any
    //       worker has seen it" (diagnostic: it answered
    //       io.wallpaperx.Daemon.Error.NoFrame), and that FAIL was the ONLY one in the
    //       run: the bus tier's taxonomy line still read Unsupported throughout and the
    //       GetStatus `unsupported` flag was unaffected, since it is derived from the type
    //       rather than from the export path. A servicedSeq-keyed daemon is invisible to
    //       every check outside this phase. Reverted.
    const std::string unservicedVideo = errorNameFrom([&] { getFrameFd(proxy, kMonitorUnservicedVideo); });
    check(unservicedVideo == kErrUnsupported,
          "an unrenderable type is Unsupported from the moment it is recorded, before any worker has seen it");
    if (unservicedVideo != kErrUnsupported) {
        std::fprintf(stderr, "  an unserviced video monitor answered '%s'\n", unservicedVideo.c_str());
    }

    // GOAL: the premise both checks above rest on -- BOTH monitors of this phase are
    //       recorded and frameless, so the answers above are keyed on what was recorded
    //       rather than on what a render produced.
    // WHY-THIS-SHAPE: stated for both monitors rather than for the static one alone, which
    //       is what it used to be. The obvious single-monitor form leaves the S6.39-keying
    //       check able to decay into a duplicate of the bus tier's: an edit that routed
    //       only the video request through `setWallpaper` (the plausible "make this go
    //       through the real API" cleanup) would leave the static premise green while the
    //       video request was serviced, and a servicedSeq-keyed daemon would then pass here
    //       too. What is asserted about the video monitor is what GetStatus actually
    //       exposes -- recorded with this phase's own path, no frame, generation still 0.
    //       NOT-COVERED, stated rather than papered over: at HEAD, servicing a
    //       non-renderable request writes no frame, no generation and (deliberately) no
    //       lastError, so nothing on this wire distinguishes a serviced video monitor from
    //       an unserviced one. This block therefore rules out "it rendered" but cannot rule
    //       out "the worker passed over it"; the only guard against that is that the phase
    //       records through `WallpaperManager::requestWallpaper` and contains no
    //       `setWallpaper` call at all. If GetStatus ever publishes worker progress, this
    //       is the block to re-key onto it.
    // CONTROL: `manager.requestWallpaper("static", ...)` at the top of this function
    //       replaced by `setWallpaper(proxy, "static", imagePath, kMonitorUnservicedStatic)`
    //       plus a 3s settle -- the very edit this premise exists to catch -- observed 2
    //       FAILs: "a recorded-but-unrendered static monitor is reported as NoFrame" and
    //       "the unserviced static monitor really is unserviced: recorded, no frame, and no
    //       error". Reverted. The same substitution applied to the *video* request instead
    //       produced no FAIL anywhere in the run -- that is the measurement behind the
    //       NOT-COVERED note above, not a prediction. The video line has a control of its
    //       own: recording that monitor as "static" instead -> "FAIL: the unserviced video
    //       monitor is the request this phase recorded, and it produced no frame", beside
    //       the keying FAIL. Reverted.
    const StatusReply status = getStatusOrExplain(proxy);
    const nlohmann::json* pending = wallpaperStatus(status, std::to_string(kMonitorUnservicedStatic));
    check(pending != nullptr && !pending->value("has_frame", true) && !pending->contains("last_error"),
          "the unserviced static monitor really is unserviced: recorded, no frame, and no error");
    const nlohmann::json* pendingVideo = wallpaperStatus(status, std::to_string(kMonitorUnservicedVideo));
    check(pendingVideo != nullptr && pendingVideo->value("type", std::string()) == "video" &&
              pendingVideo->value("path", std::string()) == videoPath &&
              !pendingVideo->value("has_frame", true) && pendingVideo->value("generation", uint64_t{1}) == 0,
          "the unserviced video monitor is the request this phase recorded, and it produced no frame");
}

// --------------------------------------------------------------------------- //
// SetImportModifiers on the wire: what the method refuses, and which monitors an accepted
// report actually reaches.
//
// Runs on the shared manager and after every other bus phase, deliberately: a report that
// excludes the layout in use drops those monitors' images and re-renders them, so a phase
// running after this would be measuring monitors it did not put in that state.
//
// WHICH HALF OF THE CONTRACT A HOST CAN ANSWER depends on its export. An opaque fd carries no
// DRM format modifier, so no report can exclude it and the only observable is that nothing
// happens (S6.47). A dma_buf frame carries a real layout, so both directions are reachable
// (S6.45 and S6.46). The phase reads the committed frame's modifier off the wire and asks the
// question the host can answer, rather than pinning one of the two and failing on the other.
// --------------------------------------------------------------------------- //
void runImportModifierWireChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& imagePath,
                                 const Check& check) {
    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
    const std::vector<uint64_t> reported{wallpaperx::testing::kDrmModifierLinear};

    // GOAL: the daemon refuses a report it cannot act on -- another pixel format, an empty
    //       set, or a list long enough to be a way of sizing daemon state from the bus --
    //       and refuses it on the reply, as InvalidArgs.
    // WHY-THIS-SHAPE: all three are asserted, not one representative. They are three
    //       separate clauses in setImportModifiers and a check on one says nothing about
    //       the others; the empty case matters most, because an empty stored set reads
    //       identically to "nobody has reported yet" and would silently restore the
    //       unnegotiated behaviour rather than being refused.
    // CONTROL: each clause disabled in turn in WallpaperManager::setImportModifiers
    //       (backend/daemon/wallpaper_manager.cpp); see the report for the observed lines.
    check(errorNameFrom([&] { setImportModifiers(proxy, exportedFormat ^ 0xFFu, reported); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a modifier report for a format this daemon does not export is refused as InvalidArgs");
    check(errorNameFrom([&] { setImportModifiers(proxy, exportedFormat, {}); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "an empty modifier report is refused as InvalidArgs rather than stored");
    check(errorNameFrom([&] {
              setImportModifiers(proxy, exportedFormat,
                                 std::vector<uint64_t>(wallpaperx::WallpaperManager::kMaxImportModifiers + 1, 0));
          }) == "org.freedesktop.DBus.Error.InvalidArgs",
          "a modifier report longer than the daemon's limit is refused as InvalidArgs");

    // A monitor of this phase's own, driven to a known generation before anything is
    // reported. Everything below counts renders on it, and a threshold read from a monitor
    // another phase is still driving would not be a threshold at all.
    setWallpaper(proxy, "static", imagePath, kMonitorModifiersWire);
    if (!signals.wait(kMonitorModifiersWire, 1)) {
        // X.1: a lost signal abandons the phase with a reason. Falling through would leave
        // every count below measuring a monitor that never rendered, and the signal-count
        // check -- which asserts an exact number -- would then pass on the daemon having
        // done nothing at all.
        check(false, "the import-modifier wire phase's own monitor produced its first frame");
        return;
    }

    // Which layout the committed frame actually carries decides which half of the contract
    // this host can be asked about, so it is read off the wire rather than assumed. An opaque
    // fd publishes kDrmModifierInvalid, which is not a layout: no report can exclude it.
    uint64_t committed = wallpaperx::kDrmModifierInvalid;
    try {
        committed = getFrameFd(proxy, kMonitorModifiersWire).drmModifier;
    } catch (const sdbus::Error& e) {
        check(false, "the import-modifier wire phase's own monitor serves the frame it just announced");
        std::fprintf(stderr, "  GetFrameFd(monitor=%d) answered '%s'\n", kMonitorModifiersWire, e.getName().c_str());
        return;
    }
    const bool opaqueExport = committed == wallpaperx::kDrmModifierInvalid;

    // GOAL: S6.45 / S6.47 over the wire -- an accepted report leaves alone a monitor whose
    //       committed frame it does not exclude. On a dma_buf host that is a frame whose
    //       layout the report still names (S6.45); on an opaque-fd host it is every frame,
    //       because an opaque handle carries no layout at all (S6.47).
    // WHY-THIS-SHAPE: a negative ("no render happened") settled by a later positive on a
    //       SEPARATE monitor, and the separate monitor is load-bearing. Barriering on this
    //       monitor instead -- the obvious shape -- lets RenderWorker::post() collapse the
    //       re-render being looked for into the barrier's own queued entry, so the defect and
    //       the fix produce identical signal counts. Sleeping a fixed interval and then
    //       counting would grade the scheduler instead; the barrier monitor is set for the
    //       first time here, so its frame proves the worker has drained past the report.
    // COVERAGE-ASSERT: the exact signal count for this monitor, not a bare "no second
    //       signal", so an edit that stops this monitor rendering at all fails here rather
    //       than going quiet.
    // CONTROL: `if (stillReadable) continue;` disabled in
    //       WallpaperManager::adoptImportModifiersLocked (backend/daemon/
    //       wallpaper_manager.cpp) -- the pre-2026-08-30 "every renderable monitor is
    //       re-posted" behaviour. Observed red on both ICDs, this check reporting
    //       "monitor 22 announced 2 frames; expected exactly 1", together with the serve
    //       check below and the two manager-tier checks; 4 failures in all. Reverted.
    std::vector<uint64_t> keep{kUnavailableModifierA};
    if (!opaqueExport) keep.push_back(committed);
    setImportModifiers(proxy, exportedFormat, keep);
    setWallpaper(proxy, "static", imagePath, kMonitorModifiersBarrier);
    if (!signals.wait(kMonitorModifiersBarrier, 1)) {
        // X.1 again: without this the count below would be read while the report's work might
        // still be queued, and "no extra render happened" would be indistinguishable from
        // "it has not happened yet".
        check(false, "the barrier frame arrives after an import-modifier report");
        return;
    }
    const size_t afterKeep = signals.countFor(kMonitorModifiersWire);
    check(afterKeep == 1, opaqueExport
                              ? "a modifier report does not re-render a monitor whose frame is an opaque fd"
                              : "a modifier report does not re-render a monitor whose frame's layout it still names");
    if (afterKeep != 1) {
        std::fprintf(stderr, "  monitor %d announced %zu frames; expected exactly 1 -- its first render, and no "
                             "rebuild for a report that excludes nothing it holds\n",
                     kMonitorModifiersWire, afterKeep);
    }
    uint64_t servedAfterKeep = 0;
    try {
        servedAfterKeep = getFrameFd(proxy, kMonitorModifiersWire).generation;
    } catch (const sdbus::Error& e) {
        std::fprintf(stderr, "  GetFrameFd after the report answered '%s'\n", e.getName().c_str());
    }
    check(servedAfterKeep == 1, "the monitor a report did not exclude still serves the frame it already had");

    if (opaqueExport) {
        // Stated rather than silently skipped. The excluding half below needs a committed
        // frame that HAS a layout, and this host's export has none; the manager-tier phase
        // records the same fact through notVerified(), which is what makes ctest call the
        // run a skip, so repeating that call here would only double the line.
        std::printf("dbus_service_test: this host exports opaque fds, so the wire tier grades S6.47 -- a report "
                    "invalidates nothing -- and cannot reach S6.46, which needs a report that excludes the "
                    "committed layout\n");
        std::fflush(stdout);
        return;
    }

    // GOAL: S6.46 over the wire -- a report that excludes the committed frame's layout does
    //       reach that monitor, and the frame no reporting consumer could read is withdrawn.
    // WHY-THIS-SHAPE: polls GetStatus rather than waiting for FrameReady. On a driver whose
    //       only usable layout is the one just excluded the rebuild cannot succeed and no
    //       signal will ever arrive, so a FrameReady wait would spend the 30 s timeout and
    //       then have to treat the stall as data. The poll distinguishes the two endings the
    //       daemon may legitimately reach -- rebuilt at a newer generation, or failed with a
    //       recorded cause -- and fails outright if it reaches neither, so a wedged worker
    //       cannot be graded as either.
    // CONTROL: two, both on backend/daemon/wallpaper_manager.cpp, because the two endings
    //       fail to different defects. `it->second.frameModifier = target->drmModifier;`
    //       deleted from WallpaperManager::renderPending leaves every frame looking
    //       layout-less, so nothing is ever condemned -> the settled-state check goes red on
    //       Lavapipe (7 failures in that run). `state.hasFrame = false` deleted from
    //       applyImportModifiersLocked -- the defect 5fb5072 fixed -> the withdrawal check
    //       goes red (4 failures). Both reverted.
    // LINEAR is offered unless the committed layout already IS linear, so that on a driver
    // with more than one usable layout the report leaves something to rebuild into and the
    // rebuild is graded rather than skipped. A set of purely unallocatable layouts would make
    // every driver take the "nothing left to allocate" branch, which grades strictly less.
    std::vector<uint64_t> exclude{kUnavailableModifierA};
    if (committed != wallpaperx::testing::kDrmModifierLinear) {
        exclude.push_back(wallpaperx::testing::kDrmModifierLinear);
    }
    setImportModifiers(proxy, exportedFormat, exclude);

    uint64_t excludedGeneration = 0;
    bool excludedHasFrame = true;
    std::string excludedError;
    bool settled = false;
    for (int attempt = 0; attempt < 100 && !settled; ++attempt) {
        // The reply is bound to a named local: wallpaperStatus() returns a pointer into it, and
        // reading through a pointer into a temporary is undefined behaviour, not a lint.
        const StatusReply reply = getStatusOrExplain(proxy);
        const nlohmann::json* state = wallpaperStatus(reply, std::to_string(kMonitorModifiersWire));
        if (state != nullptr) {
            excludedHasFrame = state->value("has_frame", false);
            excludedGeneration = state->value("generation", uint64_t{0});
            excludedError = state->value("last_error", std::string());
            settled = (excludedHasFrame && excludedGeneration > 1) || !excludedError.empty();
        }
        if (!settled) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!settled) {
        // X.1: the stall is a failure, never data. Falling through would let a worker that
        // never picked the re-render up read as "the frame was correctly withdrawn".
        check(false, "the monitor whose layout a report excluded reaches a settled state");
        std::fprintf(stderr, "  monitor %d still reports has_frame=%d generation=%llu with no error after 5 s\n",
                     kMonitorModifiersWire, static_cast<int>(excludedHasFrame),
                     static_cast<unsigned long long>(excludedGeneration));
        return;
    }
    if (excludedError.empty()) {
        check(excludedGeneration > 1,
              "a report that excludes the committed frame's layout withdraws that frame and rebuilds it");
    } else {
        // Printed, not skipped. S6.46 is about the daemon withdrawing a frame no reporting
        // consumer could read, and that half IS asserted here -- the poll only leaves this
        // branch once the daemon has recorded a cause, so a worker that never picked the
        // re-render up fails above instead of landing here. What is missing is a stronger
        // assertion nobody can make on a single-layout driver: that the rebuilt image comes
        // back in one of the reported layouts. That is graded on the real GPU by the
        // renderer's consumer_modifier_probe.
        check(!excludedHasFrame, "a report that excludes the committed frame's layout withdraws that frame");
        std::printf("dbus_service_test: the withdrawn frame was not rebuilt -- this driver offers no layout in "
                    "the reported set: %s\n",
                    excludedError.c_str());
        std::fflush(stdout);
    }

    // The state recovers, which is what separates "withdrawn" from "lost". Without it the
    // check above is equally satisfied by a daemon that drops the monitor for good.
    setImportModifiers(proxy, exportedFormat, keep);
    bool recovered = false;
    for (int attempt = 0; attempt < 100 && !recovered; ++attempt) {
        const StatusReply reply = getStatusOrExplain(proxy);
        const nlohmann::json* state = wallpaperStatus(reply, std::to_string(kMonitorModifiersWire));
        recovered = state != nullptr && state->value("has_frame", false);
        if (!recovered) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(recovered, "a monitor a report withdrew renders again once a layout it can use is reported");
}

// --------------------------------------------------------------------------- //
// A departing consumer's set stops blocking, and something re-renders what it blocked.
//
// S6.50 is about the effective set widening when a reporter leaves the bus. S6.51 is the half
// that widening on its own does not give you: a monitor the departed set left with NO frame
// recovers only if the daemon hands it back AND something posts it. Two pieces of production
// code are involved and neither is visible from the other tier -- WallpaperManager::
// forgetImportModifiers() returns the monitors, and DbusService::watchClientExits()'s callback
// posts each one to the render worker -- so this drives the real daemon over the real bus.
//
// WHAT KEEPS THE MONITOR FRAMELESS IS THE IMAGE FILE, NOT THE MODIFIER SET, and that is what
// makes the phase answerable on every host. Using the set to block the render would need a
// report that excludes the layout in use, which does not exist where the export is an opaque fd
// -- the shipped path on this project's target GPU. An undecodable file blocks the render on
// any driver, so the modifier sets here only have to CHANGE, never to be unallocatable.
//
// Runs on the shared manager, after runImportModifierWireChecks: it re-reports a wider set from
// the shared proxy, so a phase running after it would be reading a set it did not choose.
// --------------------------------------------------------------------------- //
void runModifierWithdrawalRenderChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& imagePath,
                                       const Check& check) {
    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);

    // A reference frame of this phase's own, before anything is reported: its layout is what
    // every set below keeps, so nothing here invalidates a live image and the only monitor that
    // can be re-posted for a set change is the frameless one this phase blocks.
    setWallpaper(proxy, "static", imagePath, kMonitorWithdrawBarrier);
    if (!signals.wait(kMonitorWithdrawBarrier, 1)) {
        check(false, "the withdrawal phase's barrier monitor produced its first frame");
        return;
    }
    uint64_t committed = wallpaperx::kDrmModifierInvalid;
    try {
        committed = getFrameFd(proxy, kMonitorWithdrawBarrier).drmModifier;
    } catch (const sdbus::Error& e) {
        check(false, "the withdrawal phase's barrier monitor serves the frame it just announced");
        std::fprintf(stderr, "  GetFrameFd(monitor=%d) answered '%s'\n", kMonitorWithdrawBarrier, e.getName().c_str());
        return;
    }

    // Nested, so the departing consumer's set is a strict subset: the departure must WIDEN what
    // is in force, which is the only thing that hands a frameless monitor back. Both carry the
    // committed layout on a host whose export has one, so neither set can withdraw a frame --
    // this phase is about scheduling, not invalidation.
    std::vector<uint64_t> wide{kUnavailableModifierA, kUnavailableModifierB};
    std::vector<uint64_t> narrow{kUnavailableModifierA};
    if (committed != wallpaperx::kDrmModifierInvalid) {
        wide.push_back(committed);
        narrow.push_back(committed);
    }
    setImportModifiers(proxy, exportedFormat, wide);

    // Held at "no frame at all" by a file that cannot be decoded, then made decodable at a
    // moment this phase picks. SetWallpaper is not used to unblock it, because a SetWallpaper is
    // itself a post and would grade nothing about who posted the render.
    const std::string blockedPath =
        wallpaperx::testing::writeUndecodableFile(testAssetDir(), "dbus_test_withdraw.png");
    setWallpaper(proxy, "static", blockedPath, kMonitorWithdrawRender);
    bool blocked = false;
    for (int attempt = 0; attempt < 100 && !blocked; ++attempt) {
        const StatusReply reply = getStatusOrExplain(proxy);
        const nlohmann::json* state = wallpaperStatus(reply, std::to_string(kMonitorWithdrawRender));
        blocked = state != nullptr && !state->value("last_error", std::string()).empty();
        if (!blocked) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!blocked) {
        // X.1: the stall is abandoned with a reason, never fallen through. Every check below
        // measures a monitor this phase believes has no frame, and "the render has not run yet"
        // would satisfy them all while establishing nothing.
        check(false, "the withdrawal phase's blocked monitor records why it could not render");
        return;
    }

    // The departing consumer, on a connection of its own -- that is what gives it a unique bus
    // name to lose, and what makes destroying it a real departure the daemon is told about.
    auto leaver = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});
    setImportModifiers(*leaver, exportedFormat, narrow);

    // Drains the worker past the re-render the narrowing just posted, so the count below is read
    // from a settled daemon. A barrier on a DIFFERENT monitor, for the reason the phase above
    // gives: a barrier posted on the blocked monitor itself can be coalesced with the render
    // being counted.
    setWallpaper(proxy, "static", imagePath, kMonitorWithdrawBarrier);
    if (!signals.wait(kMonitorWithdrawBarrier, 2)) {
        check(false, "the withdrawal phase's barrier frame arrives after the narrowing report");
        return;
    }

    // Only now, with the worker drained, does the file become decodable -- so the ONLY render
    // that can produce a frame for this monitor is one posted after this line.
    writeSolidPng(blockedPath, 0, 255, 0);
    if (signals.countFor(kMonitorWithdrawRender) != 0) {
        check(false, "the withdrawal phase's blocked monitor announced no frame before the departure");
        std::fprintf(stderr, "  monitor %d announced %zu frame(s) while its file could not be decoded\n",
                     kMonitorWithdrawRender, signals.countFor(kMonitorWithdrawRender));
        return;
    }

    // GOAL: S6.51 end to end -- a consumer leaving the bus does not merely widen the effective
    //       set, it gets the monitors that set was blocking re-rendered.
    // WHY-THIS-SHAPE: the observable is a FRAME, not the set, because the set widening is
    //       exactly what S6.50 already grades and a daemon that widens while scheduling nothing
    //       passes every one of those checks. Nothing else in this phase can post this monitor:
    //       its file was made decodable after the worker was drained, and no SetWallpaper
    //       follows. The wait is bounded and reaching the bound FAILS with the monitor's own
    //       recorded state printed, so a daemon that never posts cannot be graded as a slow one.
    // CONTROL: two, both of which redden this check. `return adoptImportModifiersLocked(...)`
    //       in WallpaperManager::forgetImportModifiers (backend/daemon/wallpaper_manager.cpp)
    //       changed to `adoptImportModifiersLocked(...); return {};`, and separately the
    //       `for (...) m_worker.post(monitorId);` loop deleted from
    //       DbusService::watchClientExits (backend/daemon/dbus_service.cpp). Observed red on
    //       the NVIDIA host: the first mutation reddens this and the manager-tier check (2
    //       failures), the second reddens this one alone. Both printed the monitor's stale
    //       decode error, so no render had run. Both reverted.
    leaver.reset();
    const bool rendered = signals.wait(kMonitorWithdrawRender, 1);
    check(rendered, "a consumer leaving the bus gets the monitor its set was blocking re-rendered");
    if (!rendered) {
        const StatusReply reply = getStatusOrExplain(proxy);
        const nlohmann::json* state = wallpaperStatus(reply, std::to_string(kMonitorWithdrawRender));
        std::fprintf(stderr, "  monitor %d announced no frame after the reporting consumer left; has_frame=%d "
                             "last_error='%s'\n",
                     kMonitorWithdrawRender, state != nullptr && state->value("has_frame", false),
                     state != nullptr ? state->value("last_error", std::string()).c_str() : "(no status)");
    }
}

// --------------------------------------------------------------------------- //
// What a consumer's modifier report does to monitors that have already rendered.
//
// SetImportModifiers destroys the offscreen target of every monitor whose committed layout
// the new set excludes -- those images cannot be re-labelled -- and returns those monitors to
// re-post so each rebuilds one. Monitors the set still allows, and monitors exporting an
// opaque fd (which has no layout at all), keep the image they have. The re-renders happen one
// at a time on the render worker, so between the first and the last there is a window in
// which a condemned monitor has no target. What the daemon must not do in that window is keep
// claiming the monitor still has a frame: GetFrameFd trusts that flag and then reaches for a
// target that is gone.
//
// The phase therefore has two halves and a host decides which it can answer. It reads the
// committed frame's own DRM format modifier off a real export: kDrmModifierInvalid means an
// opaque fd, where only the "nothing is invalidated" half exists (S6.47) and the window is
// unreachable, so S6.44 and S6.46 are reported as unexercised rather than asserted weakly.
//
// WHY THIS DRIVES THE MANAGER DIRECTLY AND NOT THE BUS, which is the same reason
// runUnservicedRequestChecks gives. RenderWorker runs one FIFO for renders and exports
// alike, and DbusService::setImportModifiers posts every returned monitor's re-render from
// the event-loop thread before any later GetFrameFd can post its export. So over the wire
// the export is unconditionally behind both re-renders and the window is unreachable --
// racing one would be an assertion on the scheduler, which X.2 forbids. Calling
// renderPending() for one monitor and then exportCurrentFrame() for the other puts the
// daemon in exactly that state with no race at all.
// --------------------------------------------------------------------------- //
void runImportModifierChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                             const std::string& videoPath, const Check& check) {
    // Same shape as runWorkerCoalescingChecks': its own manager means its own Device, so
    // it needs its own validation grade, and the grade must run after the manager is
    // destroyed for the layer's teardown complaints to be countable.
    std::shared_ptr<const std::atomic<int>> counter;
    bool layerActive = false;
    struct GradeOnExit {
        const std::shared_ptr<const std::atomic<int>>& counter;
        const bool& layerActive;
        const Check& check;
        ~GradeOnExit() {
            int failures = 0;
            int ungraded = 0;
            {
                wallpaperx::testing::ValidationGrade grade("dbus_service_test (import-modifier phase)", failures);
                if (wallpaperx::testing::validationWaived()) grade.expectNoDevice();
                grade.reportUngradedTo(ungraded);
                grade.adopt(counter, layerActive);
            }
            if (ungraded > 0) {
                std::fprintf(stderr,
                             "dbus_service_test: the import-modifier phase's validation grade could not be made "
                             "-- VK_LAYER_KHRONOS_validation is not loaded. The phase itself is still judged.\n");
            }
            check(failures == 0, "the import-modifier phase's own device passed its validation grade");
        }
    } gradeOnExit{counter, layerActive, check};

    // Stands in for the DBus unique name the bus would assign the reporting consumer. Any
    // stable string does: the manager only uses it to tell reporters apart.
    const std::string kConsumerName = ":test.consumer";

    wallpaperx::WallpaperManager manager(config);
    struct CaptureOnExit {
        const wallpaperx::WallpaperManager& manager;
        std::shared_ptr<const std::atomic<int>>& counter;
        bool& layerActive;
        ~CaptureOnExit() {
            counter = manager.validationCounter();
            layerActive = manager.validationLayerActive();
        }
    } captureOnExit{manager, counter, layerActive};

    // What one export said, including the layout the frame carries: kDrmModifierInvalid means
    // an opaque fd, which has no layout for a report to exclude. Reading it here rather than
    // deriving it from the vendor keeps the phase honest on a host whose export policy
    // changes -- WALLPAPERX_FORCE_HANDLE_TYPE alone flips it on this GPU.
    struct Exported {
        bool hasFrame = false;
        wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;
        uint64_t modifier = wallpaperx::kDrmModifierInvalid;
    };

    // Closes the fd an export hands back. Every successful export here is a real
    // descriptor, and this phase performs several -- leaking them would eventually fail
    // the fd-count checks elsewhere in the binary rather than anything here.
    const auto exportOnce = [&manager](int32_t monitorId) {
        Exported out;
        wallpaperx::FrameExport got = manager.exportCurrentFrame(monitorId);
        out.hasFrame = got.handle.has_value();
        out.reason = got.reason;
        if (out.hasFrame) {
            out.modifier = got.handle->frame.modifier;
            if (got.handle->frame.fd >= 0) close(got.handle->frame.fd);
        }
        return out;
    };

    manager.requestWallpaper("static", imagePath, kMonitorModifiersRerendered);
    manager.requestWallpaper("static", imagePath, kMonitorModifiersPending);
    const wallpaperx::RenderOutcome firstRerendered = manager.renderPending(kMonitorModifiersRerendered);
    const wallpaperx::RenderOutcome firstPending = manager.renderPending(kMonitorModifiersPending);
    if (!firstRerendered.rendered || !firstPending.rendered) {
        wallpaperx::testing::notVerified(
            "S6.44 through S6.47",
            "neither monitor could be given a frame to begin with, so the states these goals are about -- a "
            "monitor that HAS a frame and a report that does or does not exclude its layout -- were never reached");
        return;
    }

    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);

    // Which layout the committed frame carries is read off a real export rather than derived
    // from the vendor: WALLPAPERX_FORCE_HANDLE_TYPE alone flips this GPU between the two, and
    // a phase that assumed one would grade the wrong half without saying so.
    const Exported firstExport = exportOnce(kMonitorModifiersRerendered);
    if (!firstExport.hasFrame) {
        check(false, "the monitor this phase just rendered exports the frame it committed");
        return;
    }
    const uint64_t committed = firstExport.modifier;
    const bool opaqueExport = committed == wallpaperx::kDrmModifierInvalid;

    // GOAL: S6.45 on a dma_buf host, S6.47 on an opaque-fd one -- an accepted report neither
    //       re-renders nor withdraws the frame of a monitor it does not exclude. Before
    //       2026-08-30 ANY accepted report destroyed EVERY monitor's image, so a consumer
    //       withdrawing one layout it had never been given blanked and rebuilt every screen.
    // WHY-THIS-SHAPE: two checks, not one, because the daemon decides in two places and only
    //       the second is what a consumer sees. adoptImportModifiersLocked() picks the
    //       monitors to re-post (the empty return below), and applyImportModifiersLocked()
    //       clears hasFrame and drops targets on the render thread. Asserting the return
    //       value alone would pass for a daemon that re-posts nothing and then blanks
    //       everything at the next render, which is the worse of the two failures. Forcing a
    //       render on the OTHER monitor is what carries the accepted set across to the GPU
    //       side, since a report that re-posts nobody applies nothing until something else
    //       renders -- without it the second check grades a set the render thread never saw.
    // COVERAGE-ASSERT: "the render that carries the accepted set to the GPU side ran" has no
    //       control of its own and is not meant to -- it exists so the survivor check below
    //       cannot pass on a render that never happened, and every mutation that falsifies it
    //       falsifies an earlier phase of this binary first.
    // CONTROL: `if (stillReadable) continue;` disabled in
    //       WallpaperManager::adoptImportModifiersLocked -> the re-post check goes red (4
    //       failures across this binary on both ICDs). Separately, `monitorIds` in
    //       WallpaperManager::applyImportModifiersLocked restored to every entry of
    //       m_monitors -> the survivor check goes red, with the wire tier's serve check (2
    //       failures). Both in backend/daemon/wallpaper_manager.cpp; both reverted.
    std::vector<uint64_t> keep{kUnavailableModifierA};
    if (!opaqueExport) keep.push_back(committed);
    const std::vector<int> keepRender = manager.setImportModifiers(kConsumerName, exportedFormat, keep).toRender;
    check(keepRender.empty(),
          opaqueExport ? "a modifier report re-posts no monitor whose frame is an opaque fd"
                       : "a modifier report re-posts no monitor whose frame's layout it still names");

    manager.requestWallpaper("static", imagePath, kMonitorModifiersRerendered);
    const wallpaperx::RenderOutcome carriedSet = manager.renderPending(kMonitorModifiersRerendered);
    check(carriedSet.rendered, "the render that carries the accepted set to the GPU side ran");
    const Exported survivor = exportOnce(kMonitorModifiersPending);
    check(survivor.hasFrame,
          opaqueExport ? "an opaque-fd frame survives a report being applied on the render thread"
                       : "a frame whose layout the report still names survives the report being applied");

    if (opaqueExport) {
        wallpaperx::testing::notVerified(
            "S6.44, S6.46, S6.53 and S6.54",
            "this host exports opaque file descriptors, which carry no DRM format modifier, so no report can "
            "exclude the committed frame and the state all four goals are about -- a monitor that HAD a frame and "
            "whose target a report then dropped -- cannot be reached here at all");
        return;
    }

    // GOAL: S6.46 -- a report that excludes the committed frame's layout does reach those
    //       monitors: both are returned for re-render, and the frame no reporting consumer
    //       could read is withdrawn rather than left published.
    // WHY-THIS-SHAPE: both monitors are asserted, not one. The window S6.44 is about only
    //       exists because the report condemns several monitors and the worker services them
    //       one at a time; a single-monitor form cannot express it.
    // CONTROL: `it->second.frameModifier = target->drmModifier;` deleted from
    //       WallpaperManager::renderPending (backend/daemon/wallpaper_manager.cpp), which
    //       leaves every committed frame looking layout-less so no report can exclude it.
    //       Observed red on Lavapipe -- this check plus six more, including the NotRendered
    //       window and both recovery checks. Reverted.
    // LINEAR unless the committed layout already is linear: on a driver with a second usable
    // layout that leaves something to rebuild into, so the rebuild is graded rather than
    // reported as unreachable. A set of purely unallocatable layouts grades strictly less.
    std::vector<uint64_t> exclude{kUnavailableModifierA};
    if (committed != wallpaperx::testing::kDrmModifierLinear) {
        exclude.push_back(wallpaperx::testing::kDrmModifierLinear);
    }
    const std::vector<int> toRender = manager.setImportModifiers(kConsumerName, exportedFormat, exclude).toRender;
    check(std::find(toRender.begin(), toRender.end(), kMonitorModifiersRerendered) != toRender.end() &&
              std::find(toRender.begin(), toRender.end(), kMonitorModifiersPending) != toRender.end(),
          "a report excluding the committed frame's layout returns both rendered monitors for re-render");

    // Only one of the two. This is the window: the modifier set is applied on the first
    // re-render, which drops BOTH monitors' targets, and the second monitor's re-render
    // has not happened.
    const wallpaperx::RenderOutcome reRendered = manager.renderPending(kMonitorModifiersRerendered);

    // GOAL: a monitor whose offscreen target the modifier report dropped, and whose
    //       re-render has not run yet, answers GetFrameFd with NoFrame -- it does not
    //       claim to still hold a frame it cannot produce.
    // WHY-THIS-SHAPE: the assertion is on the reply, not on the flag, because the defect
    //       is only harmful through the reply. exportCurrentFrame() reads hasFrame, finds
    //       it set, then discovers the target is gone and throws -- which reaches the
    //       client as a generic org.freedesktop.DBus.Error.Failed, not as NoFrame, so a
    //       polling consumer sees an error instead of "ask again". Checking
    //       snapshot().hasFrame instead would grade the same bit one layer earlier but
    //       would not show that the reply is the *right* one; NotRendered is asserted
    //       here precisely because that is the answer a consumer can act on.
    // CONTROL: `state.hasFrame = false;` deleted from
    //       WallpaperManager::applyImportModifiersLocked (backend/daemon/
    //       wallpaper_manager.cpp) -- the defect commit 5fb5072 fixed. Observed red on
    //       both ICDs with the exception text quoted below; every other check in this
    //       binary stayed green.
    bool threw = false;
    std::string threwWhat;
    Exported pending;
    try {
        pending = exportOnce(kMonitorModifiersPending);
    } catch (const std::exception& e) {
        threw = true;
        threwWhat = e.what();
    }
    check(!threw, "a monitor awaiting its re-render answers GetFrameFd rather than throwing");
    if (threw) {
        std::fprintf(stderr, "  the export threw: %s\n", threwWhat.c_str());
    } else {
        check(!pending.hasFrame && pending.reason == wallpaperx::NoFrameReason::NotRendered,
              "a monitor awaiting its re-render is NotRendered, not still holding its old frame");
    }

    // GOAL: the rebuild half of S6.46 -- the re-render the report scheduled produces an image
    //       in a layout the consumer reported, not merely any image.
    // WHY-THIS-SHAPE: branched on whether the rebuild succeeded rather than asserting that it
    //       does, because on a driver whose only usable layout is the one just excluded there
    //       is nothing left to allocate and the failure is the driver's, not the daemon's.
    //       Asserting success unconditionally would report a single-layout ICD as a daemon
    //       defect (X.3); dropping the assertion entirely would let a daemon that rebuilds
    //       into the layout it was told not to use pass. The failing branch is not a free
    //       pass either -- it asserts that the daemon recorded a cause, so a re-render that
    //       was posted and never serviced fails there rather than printing and moving on.
    // CONTROL: `modifiers = std::move(agreed);` deleted from Memory::createExportableImage
    //       (renderer/vulkan/memory.cpp), so the driver is offered every usable layout rather
    //       than the agreed ones, PLUS the chosen-modifier guard in populateDmaBufLayout
    //       (renderer/vulkan/frame_export.cpp) disabled -- without the second the guard throws
    //       first and this check is never reached. Observed under
    //       WALLPAPERX_FORCE_HANDLE_TYPE=dmabuf on the NVIDIA host: red, with the rebuild
    //       printing 0x300000000606013 where the reported set held only LINEAR. Both reverted.
    if (reRendered.rendered) {
        check(reRendered.generation > carriedSet.generation,
              "the monitor that was re-posted first rebuilds its image and commits a newer generation");
        const Exported rebuilt = exportOnce(kMonitorModifiersRerendered);
        check(rebuilt.hasFrame && std::find(exclude.begin(), exclude.end(), rebuilt.modifier) != exclude.end(),
              "the rebuilt frame carries a layout from the set the consumer reported");
        if (rebuilt.hasFrame) {
            std::printf("dbus_service_test: the excluded frame's layout 0x%llx was rebuilt as 0x%llx\n",
                        static_cast<unsigned long long>(committed),
                        static_cast<unsigned long long>(rebuilt.modifier));
            std::fflush(stdout);
        }
    } else {
        const wallpaperx::WallpaperManager::Snapshot snapshot = manager.snapshot();
        const auto entry = snapshot.monitors.find(kMonitorModifiersRerendered);
        const std::string why = entry != snapshot.monitors.end() ? entry->second.lastError : std::string();
        // Printed rather than skipped, and the assertion beside it is what keeps that honest:
        // a re-render that was never serviced records no cause, so a daemon that dropped the
        // work fails here rather than reaching a quiet print. What a single-layout driver
        // costs is only the stronger claim below -- that the rebuild lands inside the
        // reported set -- which the renderer's consumer_modifier_probe grades on the real GPU.
        check(!why.empty(), "a re-render that could not rebuild records why");
        std::printf("dbus_service_test: the re-render this report scheduled had nothing left to allocate, so the "
                    "rebuilt frame's layout was not graded: %s\n",
                    why.empty() ? "(no cause recorded)" : why.c_str());
        std::fflush(stdout);
    }

    // The state recovers, which is what separates "withdrawn" from "lost": once a layout this
    // GPU can use is reported again, both monitors render and export. Without this the checks
    // above are equally satisfied by a daemon that dropped both monitors for good.
    const std::vector<int> restored = manager.setImportModifiers(kConsumerName, exportedFormat, keep).toRender;
    check(std::find(restored.begin(), restored.end(), kMonitorModifiersPending) != restored.end(),
          "a later report re-posts a monitor the previous one left without a frame");
    const wallpaperx::RenderOutcome pendingReRendered = manager.renderPending(kMonitorModifiersPending);
    const wallpaperx::RenderOutcome rerenderedAgain = manager.renderPending(kMonitorModifiersRerendered);
    check(pendingReRendered.rendered && pendingReRendered.generation > firstPending.generation,
          "the monitor that waited rebuilds its image once a layout this GPU can use is reported");
    bool bothExportAgain = false;
    try {
        bothExportAgain = exportOnce(kMonitorModifiersPending).hasFrame &&
                          exportOnce(kMonitorModifiersRerendered).hasFrame && rerenderedAgain.rendered;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  the recovery export threw: %s\n", e.what());
    }
    check(bothExportAgain, "both monitors export a frame again once the withdrawn layout is replaced");

    // Two monitors of this phase's own, both holding a frame under the set in force, so the
    // blocks below start from a state neither of the monitors above is still being driven
    // through. `keep` is what allocated them, so their layout is one `keep` names.
    manager.requestWallpaper("static", imagePath, kMonitorModifiersUndoneA);
    manager.requestWallpaper("static", imagePath, kMonitorModifiersUndoneB);
    const wallpaperx::RenderOutcome undoneA = manager.renderPending(kMonitorModifiersUndoneA);
    const wallpaperx::RenderOutcome undoneB = manager.renderPending(kMonitorModifiersUndoneB);
    if (!undoneA.rendered || !undoneB.rendered) {
        check(false, "the pair S6.53 and S6.54 are graded on both hold a frame to begin with");
        return;
    }

    // Excludes every layout any driver can allocate, so what happens next does not depend on
    // how many layouts this one offers: a monitor condemned under it, and re-rendered under it,
    // can only fail -- which is exactly what makes "the old image was released" observable.
    const std::vector<uint64_t> unusable{kUnavailableModifierA};

    // GOAL: S6.54 -- the condemned set is re-derived from the set in force, not accumulated. A
    //       monitor condemned by a narrow report whose set widens again before the render thread
    //       has applied anything keeps the frame it already had.
    // WHY-THIS-SHAPE: two monitors, and the one that proves it is the one never re-rendered.
    //       Both are condemned by the narrow report, so both had their request sequence bumped;
    //       rendering A is what carries the widened set across to the render thread, and B is
    //       then read without ever being serviced -- so a frame B still has is a frame that was
    //       never withdrawn, not one that was withdrawn and rebuilt. Reading B through a single
    //       monitor would be impossible: the monitor that triggers the apply is the monitor that
    //       rebuilds, and a rebuild hides exactly the teardown this asks about.
    // CONTROL: the std::erase_if block deleted from
    //       WallpaperManager::adoptImportModifiersLocked (backend/daemon/wallpaper_manager.cpp).
    //       Observed red on Lavapipe -- "a monitor whose condemnation was undone before the
    //       render thread saw it keeps its frame", the only failure in that run. Reverted.
    const std::vector<int> condemned = manager.setImportModifiers(kConsumerName, exportedFormat, unusable).toRender;
    check(std::find(condemned.begin(), condemned.end(), kMonitorModifiersUndoneA) != condemned.end() &&
              std::find(condemned.begin(), condemned.end(), kMonitorModifiersUndoneB) != condemned.end(),
          "a report excluding both monitors' layout condemns both");
    manager.setImportModifiers(kConsumerName, exportedFormat, keep);
    const wallpaperx::RenderOutcome carriedUndo = manager.renderPending(kMonitorModifiersUndoneA);
    check(carriedUndo.rendered, "the render that carries the undone report to the GPU side ran");
    bool keptItsFrame = false;
    try {
        keptItsFrame = exportOnce(kMonitorModifiersUndoneB).hasFrame;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  the export of the monitor whose condemnation was undone threw: %s\n", e.what());
    }
    check(keptItsFrame, "a monitor whose condemnation was undone before the render thread saw it keeps its frame");

    // GOAL: S6.53 -- a monitor whose wallpaper type this build cannot draw still loses the image
    //       it holds when a report excludes that image's layout. The image is released, so it is
    //       not there to be reused when the monitor becomes renderable again.
    // WHY-THIS-SHAPE: the type is changed to `video` while the offscreen image from its static
    //       render is still allocated -- renderPending() releases a non-renderable monitor's
    //       image itself, so the state only exists between the request and the pass that
    //       services it, and nothing else in this suite reaches it. What is asserted is the
    //       re-render AFTER the monitor is renderable again: acquireTarget() reuses an image
    //       whose dimensions match, so a surviving image is silently drawn into and every export
    //       of it then carries a layout the consumer refused. Asserting "the report returned no
    //       monitor" instead would grade the wrong half -- that clause is true both with the fix
    //       and without it.
    // CONTROL: `if (!isRenderableType(state.type)) continue;` restored as the first statement of
    //       the loop body in WallpaperManager::adoptImportModifiersLocked (backend/daemon/
    //       wallpaper_manager.cpp). Observed red on Lavapipe: this check, the export check and
    //       the two below it; 4 failures, with the export throwing "exportFrame: the driver
    //       chose DRM format modifier 0, which the frame consumer did not report as importable"
    //       -- the reused image. Reverted. The "not asked to re-render" check just above has a
    //       control of its own: deleting the SECOND isRenderableType test, the one guarding the
    //       requestSeq bump, reddens it alone.
    manager.requestWallpaper("static", imagePath, kMonitorModifiersUnrenderable);
    const wallpaperx::RenderOutcome unrenderableFirst = manager.renderPending(kMonitorModifiersUnrenderable);
    if (!unrenderableFirst.rendered) {
        check(false, "the monitor S6.53 is graded on holds an image before its type changes");
        return;
    }
    manager.requestWallpaper("video", videoPath, kMonitorModifiersUnrenderable);

    const std::vector<int> condemnedAgain =
        manager.setImportModifiers(kConsumerName, exportedFormat, unusable).toRender;
    check(std::find(condemnedAgain.begin(), condemnedAgain.end(), kMonitorModifiersUnrenderable) ==
              condemnedAgain.end(),
          "a monitor whose type this build cannot draw is not asked to re-render");
    // Carries the report to the render thread, which is where the condemned images are actually
    // released. This render cannot succeed -- no driver can allocate the reported layout -- and
    // does not need to: the release happens before the allocation is attempted.
    manager.renderPending(kMonitorModifiersUndoneA);

    manager.requestWallpaper("static", imagePath, kMonitorModifiersUnrenderable);
    const wallpaperx::RenderOutcome reused = manager.renderPending(kMonitorModifiersUnrenderable);
    check(!reused.rendered,
          "a monitor that was unrenderable when the report arrived has no old image left to draw into");
    bool staleThrew = false;
    Exported stale;
    try {
        stale = exportOnce(kMonitorModifiersUnrenderable);
    } catch (const std::exception& e) {
        staleThrew = true;
        std::fprintf(stderr, "  exporting it threw, which is what a reused image does: %s\n", e.what());
    }
    check(!staleThrew && !stale.hasFrame,
          "it reports no frame rather than serving an image in a layout the consumer excluded");
    {
        const wallpaperx::WallpaperManager::Snapshot snapshot = manager.snapshot();
        const auto entry = snapshot.monitors.find(kMonitorModifiersUnrenderable);
        const std::string why = entry != snapshot.monitors.end() ? entry->second.lastError : std::string();
        // A render that was never serviced records no cause, so this is what keeps the negative
        // above from passing on work the daemon silently dropped.
        check(!why.empty(), "the render that found no image to reuse records why it could not make one");
        std::printf("dbus_service_test: the unrenderable monitor's image was released, so its re-render had to "
                    "allocate: %s\n",
                    why.empty() ? "(no cause recorded)" : why.c_str());
        std::fflush(stdout);
    }

    // Recovers, which separates "released" from "lost": with a usable layout reported again the
    // monitor renders and exports like any other.
    manager.setImportModifiers(kConsumerName, exportedFormat, keep);
    const wallpaperx::RenderOutcome unrenderableBack = manager.renderPending(kMonitorModifiersUnrenderable);
    bool exportsAgain = false;
    try {
        exportsAgain = unrenderableBack.rendered && exportOnce(kMonitorModifiersUnrenderable).hasFrame;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  the recovery export threw: %s\n", e.what());
    }
    check(exportsAgain, "that monitor renders and exports again once a usable layout is reported");
}

// --------------------------------------------------------------------------- //
// Whose report is it: the per-consumer bookkeeping behind SetImportModifiers.
//
// Several consumers may report at once -- one plugin instance per screen, plus anything else
// importing the frame -- and every one of them has to be able to read the image, so what
// takes effect is the INTERSECTION of what they reported. Two consequences follow, and
// neither is observable from a single consumer: a report that would leave that intersection
// empty must be refused rather than stored, and a list must die with the process that
// reported it, or the narrowest list any consumer ever sent binds every consumer after it --
// including the replacement for one that crashed.
//
// Drives the manager directly and creates no Device at all: the property is bookkeeping under
// m_stateMutex, and the observable it needs -- "did the effective set change" -- is the
// monitor list setImportModifiers returns, which is exact here and only implied over the bus.
// A monitor with a recorded request and no frame is re-posted whenever the effective set
// changes and never when it does not, which is what makes that list a usable probe. The wire
// tier below then grades the two things this tier cannot see: that the stored key is the
// caller's own bus name, and that a departure withdraws it.
//
// The modifiers are the deliberately-unavailable ones. Nothing renders here, so their being
// unallocatable costs nothing, and using them keeps the phase's verdict independent of which
// layouts the host's driver happens to offer.
// --------------------------------------------------------------------------- //
void runImportModifierClientChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                                   const Check& check) {
    wallpaperx::WallpaperManager manager(config);
    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);

    // Recorded, never rendered -- so it is re-posted exactly when the effective set changes.
    manager.requestWallpaper("static", imagePath, kMonitorModifiersIdle);

    struct Report {
        bool refused = false;
        bool changed = false;
        std::string why;
    };
    int probes = 0;
    const auto report = [&](const std::string& client, const std::vector<uint64_t>& set) {
        ++probes;
        Report out;
        try {
            out.changed = !manager.setImportModifiers(client, exportedFormat, set).toRender.empty();
        } catch (const wallpaperx::InvalidRequest& e) {
            out.refused = true;
            out.why = e.what();
        }
        return out;
    };

    // A separate name per probe, and that is load-bearing rather than tidiness: a consumer's
    // own next report overwrites whatever a refused one left behind, so re-using a name would
    // hide exactly the leftover entry the rollback probes exist to find. Measured -- with the
    // rollback deleted from setImportModifiers, the one-name form stayed green.
    const std::string consumerA = ":test.consumer.a";
    const std::string consumerB = ":test.consumer.b";
    const std::string consumerC = ":test.consumer.c";
    const std::string consumerD = ":test.consumer.d";
    const std::string consumerE = ":test.consumer.e";
    const std::string consumerF = ":test.consumer.f";
    const std::string consumerG = ":test.consumer.g";
    const std::string consumerH = ":test.consumer.h";
    constexpr uint64_t m1 = kUnavailableModifierA;
    constexpr uint64_t m2 = kUnavailableModifierB;
    constexpr uint64_t m3 = kUnavailableModifierC;

    const Report firstA = report(consumerA, {m1, m2});
    check(!firstA.refused && firstA.changed, "the first consumer's report takes effect");

    // The idempotency clause of S6.43, moved here from the GPU-backed phase, where the same
    // check went vacuous: with the new contract a repeat of a report that invalidated nothing
    // is indistinguishable from the report itself. A monitor with no frame is re-posted on
    // every real change, so here the two are distinguishable again.
    // CONTROL: the `if (m_importModifiers == modifiers) return {};` early-out disabled in
    //       WallpaperManager::adoptImportModifiersLocked (backend/daemon/
    //       wallpaper_manager.cpp). Observed red on both ICDs, with the two other
    //       unchanged-set checks in this phase; 3 failures. Reverted.
    check(!report(consumerA, {m1, m2}).changed, "an identical report from the same consumer changes nothing");

    // GOAL: S6.52 -- what a consumer reports is a SET. The same layouts sent in a different
    //       order, or with a duplicate added, are the same report: nothing is re-posted.
    // WHY-THIS-SHAPE: the permutation and the duplicate are asserted separately because the
    //       daemon needs two different operations to survive them -- sorting and deduplicating
    //       -- and either one alone leaves the other spelling observable. The consequence is
    //       not cosmetic: a change to the effective set bumps every frameless monitor's request
    //       sequence, which supersedes the render already in flight, so a consumer that
    //       permutes its list per frame keeps every monitor permanently without a picture.
    //       Asserting only the identical-order repeat above cannot see either.
    // CONTROL: the std::sort and std::unique lines deleted from
    //       WallpaperManager::setImportModifiers (backend/daemon/wallpaper_manager.cpp),
    //       keeping `std::vector<uint64_t> canonical = modifiers;`. Observed red on the NVIDIA
    //       host: exactly these two checks, and nothing else in the binary. Reverted.
    check(!report(consumerA, {m2, m1}).changed, "the same layouts reported in a different order change nothing");
    check(!report(consumerA, {m1, m2, m2}).changed, "the same layouts reported with a duplicate change nothing");

    // GOAL: S6.52's second clause -- the length limit is applied to the list as sent, so an
    //       over-long report cannot be admitted by collapsing its duplicates away first.
    // WHY-THIS-SHAPE: every entry is the SAME layout, so the list is over the limit as sent and
    //       one element long once deduplicated. A list of distinct layouts would be refused by
    //       either ordering of the two steps and would grade nothing about which came first.
    // CONTROL: the canonicalisation moved above the `modifiers.size() > kMaxImportModifiers`
    //       test in WallpaperManager::setImportModifiers (backend/daemon/
    //       wallpaper_manager.cpp). Observed red on the NVIDIA host: this check and the wire
    //       tier's length check, plus eight more that cascade from the two over-long probes
    //       being accepted instead of refused; 10 failures. Reverted.
    check(report(consumerA, std::vector<uint64_t>(wallpaperx::WallpaperManager::kMaxImportModifiers + 1, m1)).refused,
          "an over-long report of one repeated layout is refused rather than deduplicated into a short one");

    // GOAL: S6.48 -- what is in force with two consumers reporting is the intersection, not
    //       the most recent report and not the union.
    // WHY-THIS-SHAPE: three probes, because one cannot separate the three candidate rules. A
    //       reports {m1,m2} and B reports {m2,m3}, so the intersection is {m2}: a third
    //       consumer reporting {m3} would be ACCEPTED under "last report wins" and under
    //       "union", and reporting {m1} would be accepted under "first wins" and under
    //       "union". Both must be refused, and a report of exactly {m2} must be accepted and
    //       change nothing -- which is what pins the set to {m2} rather than to something
    //       narrower. Reading the set directly is not available and should not be added: it
    //       is private state, and X.6 forbids opening it for a test.
    // COVERAGE-ASSERT: the probe count asserted at the end of the phase, so an edit that
    //       deletes probes fails here instead of quietly narrowing what is separated.
    // CONTROL: the intersection in WallpaperManager::setImportModifiers (backend/daemon/
    //       wallpaper_manager.cpp) replaced by the caller's own list -- the last-report-wins
    //       rule. Observed red on both ICDs: both refusal probes above, the exact-set probe,
    //       and four more across this phase and the wire tier; 7 failures. Reverted.
    check(report(consumerB, {m2, m3}).changed, "a second consumer's report narrows what is in force");
    check(report(consumerC, {m3}).refused,
          "a layout only one consumer reported is refused as another consumer's whole set");
    check(report(consumerD, {m1}).refused,
          "a layout only the first consumer reported is refused as another consumer's whole set");
    const Report exact = report(consumerE, {m2});
    check(!exact.refused && !exact.changed,
          "a report of exactly the layouts in force is accepted and changes nothing");

    // GOAL: S6.49 -- a refused report leaves the daemon's effective set exactly as it was,
    //       for a caller with no earlier entry and for one with an earlier entry alike.
    // WHY-THIS-SHAPE: the rollback is invisible in the effective set, because the refusal is
    //       thrown before the set is adopted either way. What it is visible in is the stored
    //       per-consumer map, which the NEXT report intersects: if the two refused reports
    //       above had been left behind, the acceptance above could not have happened, and if
    //       A's refused {m1} is left behind, the benign report below cannot be accepted. So
    //       each rollback is graded by a later report from a DIFFERENT consumer that only
    //       succeeds if the map was restored. Asserting "the throw happened" would grade
    //       nothing about it, and re-probing with the refused consumer's own name would
    //       overwrite the leftover before it could be seen.
    // CONTROL: the two `m_importModifiersByClient` restore lines disabled in the empty-
    //       intersection branch of WallpaperManager::setImportModifiers (backend/daemon/
    //       wallpaper_manager.cpp). Observed red on both ICDs: both rollback checks here,
    //       the exact-set probe above, and both S6.50 probes below; 5 failures. Reverted.
    check(!exact.refused, "a refused report leaves a consumer that had no earlier entry unregistered");
    check(report(consumerA, {m1}).refused, "a report that would leave no shared layout is refused");
    const Report afterRollback = report(consumerF, {m1, m2});
    check(!afterRollback.refused && !afterRollback.changed,
          "a refused report restores the caller's earlier entry, so the set in force is untouched");

    // GOAL: S6.50 -- a consumer that goes away stops constraining the exporter, and the set
    //       widens back to the intersection of those that remain.
    // WHY-THIS-SHAPE: probed by re-offering the exact set that was refused while the departed
    //       consumer was present ({m3}, refused above), so the check is a before/after on one
    //       report rather than an assertion about internal state. The second half then
    //       forgets everyone and offers {m1}, which is disjoint from what the last remaining
    //       consumer had reported: accepted only if "no consumer reporting" really means
    //       unconstrained rather than "whatever the last one said".
    // CONTROL: `m_importModifiersByClient.erase(client)` replaced by a `count()` test in
    //       WallpaperManager::forgetImportModifiers (backend/daemon/wallpaper_manager.cpp),
    //       so nothing is ever forgotten. Observed red on both ICDs: both checks here and the
    //       wire tier's departure check; 3 failures. Reverted.
    manager.forgetImportModifiers(consumerA);
    manager.forgetImportModifiers(consumerE);
    manager.forgetImportModifiers(consumerF);
    const Report afterExit = report(consumerG, {m3});
    check(!afterExit.refused,
          "a layout a departed consumer excluded becomes reportable again once it is forgotten");
    manager.forgetImportModifiers(consumerB);
    manager.forgetImportModifiers(consumerG);
    check(!report(consumerH, {m1}).refused,
          "with no consumer left reporting, the exporter is unconstrained again");

    // GOAL: S6.56 -- a consumer that has reported before and is refused for an empty
    //       intersection is told how many OTHER consumers it shares no layout with.
    // WHY-THIS-SHAPE: staged from a known, small population rather than asserted against the
    //       phase's running total, so the expected number is a property of these four lines and
    //       not of how many probes ran above. Exactly one other consumer reports here, so the
    //       caller counting itself is the difference between "1" and "2" -- and the wire tier
    //       already checks the error NAME, which is identical either way, so only the text can
    //       carry this. The refusal itself is asserted too: a message check alone would pass on
    //       a daemon that stopped refusing, since the empty string contains no wrong number.
    // CONTROL: `others` replaced by `m_importModifiersByClient.size()` in the message built by
    //       WallpaperManager::setImportModifiers (backend/daemon/wallpaper_manager.cpp).
    //       Observed red on the NVIDIA host, the only failure in that run, with the refusal
    //       quoted as "...shares no layout with the 2 already reported...". Reverted.
    const std::string consumerI = ":test.consumer.i";
    check(!report(consumerI, {m1, m2}).refused, "a second consumer sharing a layout with the first is accepted");
    const Report miscounted = report(consumerI, {m2});
    check(miscounted.refused && miscounted.why.find("with the 1 already reported") != std::string::npos,
          "the refusal counts the other consumers, not the caller");
    if (!miscounted.why.empty() && miscounted.why.find("with the 1 already reported") == std::string::npos) {
        std::fprintf(stderr, "  the refusal said: %s\n  (one other consumer was reporting)\n",
                     miscounted.why.c_str());
    }

    // GOAL: S6.51 at the manager tier -- a departure does not merely widen the effective set,
    //       it hands back the monitors the departed set was blocking so something can re-render
    //       them. The idle monitor here has a recorded request and no frame at all, which is
    //       exactly the state a departed consumer's set can leave a monitor in.
    // WHY-THIS-SHAPE: asserts the RETURN VALUE of forgetImportModifiers, which the S6.50 checks
    //       above deliberately ignore -- they probe the set through a later report, and a
    //       withdrawal that widens the set while scheduling nothing satisfies every one of them.
    //       That is the whole gap: the monitor stays blank for the life of the daemon, because a
    //       static wallpaper sends no further request. A control that stops the set widening
    //       reddens the S6.50 checks instead and is not a control for this.
    // CONTROL: the last line of WallpaperManager::forgetImportModifiers (backend/daemon/
    //       wallpaper_manager.cpp) changed to `adoptImportModifiersLocked(...); return {};` --
    //       the set still widens, nothing is scheduled. Observed red on the NVIDIA host, with
    //       the wire tier's departure check and no S6.50 check; 2 failures. Reverted.
    manager.forgetImportModifiers(consumerI);
    const std::vector<int> unblocked = manager.forgetImportModifiers(consumerH).toRender;
    check(std::find(unblocked.begin(), unblocked.end(), kMonitorModifiersIdle) != unblocked.end(),
          "a departure hands back the frameless monitor its set was blocking, not just a wider set");

    check(probes == 15, "the per-consumer phase ran every probe it grades on");
    if (probes != 15) {
        std::fprintf(stderr, "  %d report probes ran; expected 15\n", probes);
    }
}

// --------------------------------------------------------------------------- //
// The same per-consumer contract over the real wire, which is where the two halves this
// daemon cannot see from the manager live: which name a report is filed under, and what
// removes it.
//
// The key is the SENDER's unique bus name, taken from the message rather than from any
// argument -- so a consumer cannot report on another's behalf, and the name is the one
// NameOwnerChanged later carries. Withdrawal is DbusService::watchClientExits(), which
// subscribes to the bus's own NameOwnerChanged and forgets a departed reporter.
//
// Its own manager, connection and service, on the connection's unique bus name rather than
// io.wallpaperx.Daemon: the shared phases own that name. Nothing renders here, so this
// manager builds no Device and has no validation grade to make. Each consumer is a proxy with
// a connection of its own, which is what gives it a distinct unique name and what makes
// destroying it a real departure from the bus.
// --------------------------------------------------------------------------- //
void runImportModifierClientWireChecks(const wallpaperx::WallpaperManager::Config& config, const Check& check) {
    wallpaperx::WallpaperManager manager(config);

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};

    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
    // Chosen so that each departure is observable in the next report's verdict, which is the
    // only channel a client has. Leaver{A,B} and Stayer{B,C} intersect to {B}, which excludes
    // Latecomer{C,D}; once the leaver is gone the intersection is {C}, which admits it. The
    // stayer then still excludes {D}, and only when the stayer has gone too does {D} pass --
    // so "forgot the wrong client" and "forgot everybody" are separable from "forgot the
    // right one". A single shared modifier could not separate them.
    const std::vector<uint64_t> setLeaver{kUnavailableModifierA, kUnavailableModifierB};
    const std::vector<uint64_t> setStayer{kUnavailableModifierB, kUnavailableModifierC};
    const std::vector<uint64_t> setLatecomer{kUnavailableModifierC, kUnavailableModifierD};
    const std::vector<uint64_t> setLast{kUnavailableModifierD};

    auto consumerA = sdbus::createProxy(daemonName, objectPath);
    auto consumerStayer = sdbus::createProxy(daemonName, objectPath);
    auto consumerB = sdbus::createProxy(daemonName, objectPath);

    // GOAL: a report is filed under the caller's own bus name, so consumers on separate
    //       connections are separate reporters and not one.
    // WHY-THIS-SHAPE: the refusal is the observable. A daemon keying every report under one
    //       constant name would treat each report as a REPLACEMENT for the last and accept
    //       the third, which is exactly what this refuses. Asserting instead that the daemon
    //       "stored three entries" would need state no client can see.
    // CONTROL: the `sender` argument in DbusService::registerVTable's SetImportModifiers
    //       handler (backend/daemon/dbus_service.cpp) replaced with a constant "". Observed
    //       red on the NVIDIA host, with the stayer check below; 2 failures. Reverted.
    setImportModifiers(*consumerA, exportedFormat, setLeaver);
    setImportModifiers(*consumerStayer, exportedFormat, setStayer);
    check(errorNameFrom([&] { setImportModifiers(*consumerB, exportedFormat, setLatecomer); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "a report sharing no layout with what the other consumers reported is refused as InvalidArgs");

    // GOAL: S6.50 end to end -- a consumer that leaves the bus stops constraining the
    //       exporter, through the daemon's own NameOwnerChanged subscription.
    // WHY-THIS-SHAPE: retries the report that was just refused, rather than sleeping once and
    //       asserting. The departure is announced asynchronously by the bus and handled on the
    //       daemon's event loop, so "how long until it is noticed" is not this test's to
    //       assert -- but the retry loop has a bound and reaching it FAILS the check rather
    //       than falling through, so a subscription that never fires cannot be graded as a
    //       slow one (X.1).
    // CONTROL: the `m_manager.forgetImportModifiers(name)` call deleted from
    //       DbusService::watchClientExits (backend/daemon/dbus_service.cpp). Observed red on
    //       the NVIDIA host, and the only failure in that run, with the diagnostic "the
    //       departed consumer's set was still in force after 100 attempts over 5 s".
    //       Reverted.
    consumerA.reset();
    bool acceptedAfterExit = false;
    int attempts = 0;
    for (; attempts < 100 && !acceptedAfterExit; ++attempts) {
        try {
            setImportModifiers(*consumerB, exportedFormat, setLatecomer);
            acceptedAfterExit = true;
        } catch (const sdbus::Error&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    check(acceptedAfterExit, "a consumer that leaves the bus stops constraining the exporter");
    if (!acceptedAfterExit) {
        std::fprintf(stderr, "  the departed consumer's set was still in force after %d attempts over 5 s\n",
                     attempts);
        return;
    }

    // GOAL: only the departing consumer is forgotten. Without this the check above is equally
    //       satisfied by a daemon that drops EVERY reporter whenever anybody leaves, which
    //       silently restores the unnegotiated behaviour the whole mechanism exists to
    //       replace -- and that daemon is not hypothetical, it is one line's difference.
    // WHY-THIS-SHAPE: the set offered here is disjoint from the stayer's and NOT from the
    //       late consumer's, so it is refused only while the stayer is still on the books.
    //       An arbitrary disjoint set would be refused either way and would grade nothing;
    //       measured -- with forgetImportModifiers() clearing the whole map, the arbitrary
    //       form stayed green and this one goes red.
    // CONTROL: `m_importModifiersByClient.clear();` added after the erase in
    //       WallpaperManager::forgetImportModifiers (backend/daemon/wallpaper_manager.cpp).
    //       Observed red on the NVIDIA host, and the only failure in that run. Reverted.
    auto consumerLast = sdbus::createProxy(daemonName, objectPath);
    check(errorNameFrom([&] { setImportModifiers(*consumerLast, exportedFormat, setLast); }) ==
              "org.freedesktop.DBus.Error.InvalidArgs",
          "the consumer that stayed still constrains the exporter after another one left");

    // No control of its own: every mutation that falsifies it falsifies the departure check
    // above first, which returns rather than falling through. What makes it more than a
    // repetition is the check between them -- the stayer was still constraining the exporter
    // one call ago, so this is the transition to nobody reporting, not a second look at one
    // departure.
    consumerStayer.reset();
    consumerB.reset();
    bool unconstrained = false;
    for (int attempt = 0; attempt < 100 && !unconstrained; ++attempt) {
        try {
            setImportModifiers(*consumerLast, exportedFormat, setLast);
            unconstrained = true;
        } catch (const sdbus::Error&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    check(unconstrained, "with every reporting consumer gone, the exporter is unconstrained again");
}

// --------------------------------------------------------------------------- //
// The daemon announces that the layouts in force have changed (S6.58, S6.59, S6.60).
//
// WHY THE SIGNAL EXISTS AT ALL, because it is what every check below is shaped around. A report
// can be refused for sharing no layout with ANOTHER consumer -- a verdict the caller cannot
// change by editing its own list. If that other consumer later leaves the bus and the set in
// force widens, a refused consumer with nothing to listen for never asks again and runs
// unnegotiated for the rest of the session. So this has to be an event, and it has to be exact
// in both directions: a signal that never fires leaves the consumer latched, and a signal that
// fires on every call is the poll it was built to replace.
//
// TWO TIERS, because the property splits and neither tier can see the other's half.
//
//   The manager decides WHETHER the set changed, and returns that as a flag beside the list of
//   monitors that have to be redrawn. Those two answers are deliberately different, and that
//   difference is invisible over the bus: "no signal" and "a signal withheld because nothing
//   needed redrawing" are the same silence on the wire.
//
//   The wire decides what the flag DOES. Only here is it observable that the flag gates an
//   emission at all, that a departure goes through the same path, and that an identical
//   re-report is silent.
//
// Its own manager, connection and service, on the connection's unique bus name rather than
// io.wallpaperx.Daemon, which the shared phases own. Nothing renders -- requestWallpaper only
// records a request -- so no Device is built here and this manager has no validation grade to
// make. The wire half deliberately drives a manager with NO monitors at all, so every change it
// makes is one that condemns nothing: that is the case S6.60 is about, and it is also the state
// a freshly started daemon is in, which is when a consumer is most likely to have been refused.
// --------------------------------------------------------------------------- //

// Counts ImportModifiersChanged. The signal carries no arguments, so the count is the entire
// observable, and every wait has a failure branch its caller must read (X.1).
class AnnouncementLog {
public:
    void record() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_count;
        }
        m_arrived.notify_all();
    }

    bool waitFor(int atLeast) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_arrived.wait_for(lock, std::chrono::seconds(5), [&] { return m_count >= atLeast; });
    }

    int count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_count;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_arrived;
    int m_count = 0;
};

void runImportModifierAnnouncementChecks(const wallpaperx::WallpaperManager::Config& config,
                                         const std::string& imagePath, const std::string& videoPath,
                                         const Check& check) {
    const uint32_t exportedFormat = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
    const std::string consumer = ":test.announce.consumer";

    {
        wallpaperx::WallpaperManager bookkeeping(config);

        // Recorded as a type this build cannot draw, so it is never handed back to re-render.
        // With it as the only monitor, every change below condemns nothing.
        bookkeeping.requestWallpaper("video", videoPath, kMonitorAnnounceUnrenderable);

        // GOAL: S6.60 -- a change that condemns no monitor is still reported as a change. The
        //       daemon has no frame to invalidate here, which is the state it starts in.
        // WHY-THIS-SHAPE: asserts the flag and the monitor list TOGETHER, and that pairing is
        //       the whole check: `changed` alone is satisfied by a daemon that returns "changed"
        //       whenever it returns anything, and the empty list is what pins this as the case
        //       where the two answers differ. The obvious alternative -- grading S6.60 over the
        //       bus by counting signals from a daemon with no monitors -- cannot distinguish
        //       "the set did not change" from "nothing needed redrawing", because both are
        //       silence.
        // CONTROL: the last line of WallpaperManager::adoptImportModifiersLocked
        //       (backend/daemon/wallpaper_manager.cpp) changed to
        //       `return ImportModifierUpdate{!toRender.empty(), std::move(toRender)};`.
        //       Observed red here and at the wire tier's first announcement, whose manager also
        //       has no monitor to condemn; 2 failures. The condemning check below stayed GREEN,
        //       which is what makes this property separable from "a change is announced at all".
        //       Reverted.
        const wallpaperx::ImportModifierUpdate condemnsNothing =
            bookkeeping.setImportModifiers(consumer, exportedFormat, {kUnavailableModifierA, kUnavailableModifierB});
        check(condemnsNothing.changed && condemnsNothing.toRender.empty(),
              "a report that changes the layouts in force but condemns no monitor is still reported as a change");
        if (!condemnsNothing.toRender.empty()) {
            std::fprintf(stderr, "  %zu monitor(s) were handed back for re-render; only a type this build cannot "
                                 "draw was registered, so this phase's premise is gone\n",
                         condemnsNothing.toRender.size());
        }

        // GOAL: S6.59 at the manager tier -- a report that leaves the set in force exactly as it
        //       was is not a change. This is what keeps the wire tier's silence from being an
        //       accident of the emission code.
        // CONTROL: the `if (m_importModifiers == modifiers) return {};` guard deleted from
        //       WallpaperManager::adoptImportModifiersLocked (backend/daemon/
        //       wallpaper_manager.cpp). Observed red here, at the wire tier's silence check, and
        //       at five pre-existing "changes nothing" checks; 7 failures. Reverted.
        const wallpaperx::ImportModifierUpdate unchanged =
            bookkeeping.setImportModifiers(consumer, exportedFormat, {kUnavailableModifierA, kUnavailableModifierB});
        check(!unchanged.changed && unchanged.toRender.empty(),
              "re-reporting the set already in force is not reported as a change");

        // GOAL: S6.60's other side -- a change that DOES condemn a monitor is reported as a
        //       change as well. Without it the check above is equally satisfied by a daemon that
        //       hardcodes `changed` to true, which would make S6.59 unenforceable.
        // WHY-THIS-SHAPE: the monitor added here is a renderable one with no frame at all, which
        //       is handed back on every change on every host. Using a monitor whose committed
        //       layout the report excludes -- the obvious way to condemn something -- would need
        //       a real render and would be unreachable wherever the export is an opaque fd, which
        //       is this project's target GPU.
        // CONTROL: `toRender.push_back(id);` deleted from the loop in
        //       WallpaperManager::adoptImportModifiersLocked (backend/daemon/
        //       wallpaper_manager.cpp). Observed red here, with four pre-existing checks that
        //       read the same list; 5 failures. Reverted.
        bookkeeping.requestWallpaper("static", imagePath, kMonitorAnnounceCondemned);
        const wallpaperx::ImportModifierUpdate condemns =
            bookkeeping.setImportModifiers(consumer, exportedFormat, {kUnavailableModifierA});
        check(condemns.changed && !condemns.toRender.empty(),
              "a report that condemns a monitor is reported as a change too");
    }

    wallpaperx::WallpaperManager announcer(config);
    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, announcer);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};

    // Declared before the proxy whose dispatch thread writes to it, so it outlives that thread.
    AnnouncementLog announced;
    // A subscriber that never reports anything, which is what a consumer already latched on a
    // refusal is: the whole point of the signal is that such a consumer hears about a change it
    // did not cause.
    auto listener = sdbus::createProxy(daemonName, objectPath);
    listener->uponSignal("ImportModifiersChanged")
        .onInterface(DbusService::kInterface)
        .call([&announced] { announced.record(); });

    // Each on a connection of its own: that is what gives it a distinct unique bus name, and
    // what makes destroying one a real departure the daemon is told about.
    auto reporter = sdbus::createProxy(daemonName, objectPath);
    auto departing = sdbus::createProxy(daemonName, objectPath);

    // GOAL: S6.58's first half -- a report that moves the intersection reaches subscribers as
    //       ImportModifiersChanged. The interface inventory elsewhere in this file asserts the
    //       signal is EXPORTED with no arguments, which says nothing about it ever being sent.
    // WHY-THIS-SHAPE: the subscriber is a third connection that reports nothing, not the
    //       reporting proxy itself. A consumer hearing its own call's effect would be graded on
    //       a delivery path a refused consumer never uses, and refused consumers are the entire
    //       audience for this signal. The wait is bounded and reaching the bound fails with a
    //       printed count rather than falling through into the count comparison below (X.1).
    // CONTROL: the `m_object->emitSignal("ImportModifiersChanged").onInterface(kInterface);`
    //       call deleted from DbusService::applyImportModifierUpdate (backend/daemon/
    //       dbus_service.cpp). Observed red here and the only failure in that run -- the checks
    //       below never ran, because this one abandons the phase rather than counting an
    //       announcement that cannot arrive. Reverted.
    setImportModifiers(*reporter, exportedFormat, {kUnavailableModifierA, kUnavailableModifierB});
    const bool announcedReport = announced.waitFor(1);
    check(announcedReport, "a report that changes the layouts in force is announced to a subscriber");
    if (!announcedReport) {
        std::fprintf(stderr, "  no ImportModifiersChanged arrived within 5 s of an accepted report\n");
        return;
    }

    // The identical re-report, then a report that really does narrow the set. The second is a
    // BARRIER, not decoration: signals from one emitter to one subscriber are delivered in the
    // order they were sent, and the re-report's own reply proves its handler had already run
    // when the barrier call was made. So once the barrier's signal has arrived, any signal the
    // re-report would have emitted is already counted. Sleeping and then counting is the
    // alternative, and it grades the host's timing rather than the daemon.
    //
    // The barrier's own check has no control of its own: every mutation that falsifies it
    // falsifies the announcement check above, which abandons the phase before this line is
    // reached. It is asserted rather than assumed because reaching its bound means the count
    // below has no barrier under it, and counting anyway would read a stall as silence.
    setImportModifiers(*reporter, exportedFormat, {kUnavailableModifierA, kUnavailableModifierB});
    setImportModifiers(*departing, exportedFormat, {kUnavailableModifierA});
    const bool announcedBarrier = announced.waitFor(2);
    check(announcedBarrier, "a second consumer's report narrowing the layouts in force is announced");
    if (!announcedBarrier) {
        std::fprintf(stderr, "  only %d announcement(s) arrived; the silence check below has no barrier and was "
                             "abandoned\n",
                     announced.count());
        return;
    }

    // GOAL: S6.59 on the wire -- re-reporting the set already in force announces nothing. False
    //       makes the signal the poll it replaces, and worse than a poll: a consumer wakes on
    //       it and reports, and that report is another change.
    // WHY-THIS-SHAPE: an exact count, taken only after the barrier above has landed, so the
    //       claim is "the re-report contributed nothing" rather than "nothing had arrived yet".
    //       Asserting `count() == 1` immediately after the re-report returns would rest on the
    //       reply overtaking a signal sent from the same connection a moment earlier, which is
    //       the opposite of the ordering D-Bus gives.
    // CONTROL: the `if (!update.changed) return;` line deleted from
    //       DbusService::applyImportModifierUpdate (backend/daemon/dbus_service.cpp), so every
    //       accepted call emits. Observed red on both ICDs -- "3 announcement(s) arrived across
    //       two changes and one identical re-report" -- and the only failure in either run.
    //       Reverted.
    check(announced.count() == 2, "re-reporting the set already in force announces nothing");
    if (announced.count() != 2) {
        std::fprintf(stderr, "  %d announcement(s) arrived across two changes and one identical re-report\n",
                     announced.count());
    }

    // GOAL: S6.58's second half -- a reporting consumer leaving the bus is announced too. This
    //       is the case the signal was added for: the departure widens the set in force, and a
    //       consumer refused for sharing no layout with the departed one has no other way to
    //       learn that its list would now be accepted.
    // WHY-THIS-SHAPE: graded on the SUBSCRIBER, which is a different consumer from the one that
    //       left and from the one that reported. Retrying the refused report until it is
    //       accepted -- which is how the departure itself is graded elsewhere in this file --
    //       establishes that the set widened and says nothing about anybody being told.
    // CONTROL: `applyImportModifierUpdate(m_manager.forgetImportModifiers(name));` in
    //       DbusService::watchClientExits (backend/daemon/dbus_service.cpp) replaced by a bare
    //       `m_manager.forgetImportModifiers(name);`, so the set still widens and nothing is
    //       announced. Observed red here -- "2 announcement(s) arrived ... 3 were expected" --
    //       alongside S6.51's re-render check, which the same line also posts; 2 failures. The
    //       two checks above stayed green. Reverted.
    departing.reset();
    const bool announcedDeparture = announced.waitFor(3);
    check(announcedDeparture, "a reporting consumer leaving the bus is announced to the others");
    if (!announcedDeparture) {
        std::fprintf(stderr, "  %d announcement(s) arrived within 5 s of the reporting consumer's departure; 3 were "
                             "expected\n",
                     announced.count());
    }
}

// --------------------------------------------------------------------------- //
// Coalescing, driven at the level the mechanisms actually live at.
//
// Two independent mechanisms collapse a burst, and both sit below DBus:
//
//   RenderWorker::m_queuedRenders  drops a monitor already sitting in the queue.
//   WallpaperManager::servicedSeq  makes renderPending a no-op for a request that has
//                                  already been serviced.
//
// So this drives the worker and the manager directly rather than through the bus. The
// point is not to avoid DBus for its own sake -- it is that over the bus the claim
// cannot be stated exactly. Counting the renders a burst produces measures how many
// completed inside a window whose length the OS scheduler sets, not the daemon: the
// worker erases a monitor from m_queuedRenders before rendering it, so every render that
// finishes mid-burst admits one more queue entry, and under `ctest -jN` the client's own
// calls are descheduled too. Any fixed bound is therefore either flaky or too loose to
// catch the thing it is bounding -- `<= 2` was reachable at 3.
//
// Holding the worker on a task that blocks removes the race instead of tolerating it:
// the worker runs one job at a time, so nothing can render until this thread releases
// the gate, and all five requests are provably recorded before the first render starts.
// The assertion becomes exact. Measured against the alternatives: with the gate held,
// either mechanism alone still yields 1, and both disabled yields 5.
//
// Uses its own WallpaperManager rather than the daemon's, and not for isolation's sake:
// Device pins itself to the thread that created it and aborts on a GPU call from any
// other, because Vulkan submission is not free-threaded and it shares one command pool.
// The daemon's Device belongs to the daemon's worker thread, so a second worker driving
// the same manager trips that guard by design. A second manager brings its own Device,
// created on and confined to the worker started below.
//
// The helper it calls last is declared first, below.
// --------------------------------------------------------------------------- //

// --------------------------------------------------------------------------- //
// Every task the worker is handed is answered exactly once, whatever happens to it.
//
// An export task owns the sdbus::Result its caller is blocked on. A task that is dropped
// -- because the queue is full, or because the worker is stopping -- answers nothing, and
// destroying an sdbus::Result sends no reply at all: the client then waits out sd-bus's
// full ~25 s timeout for a request the daemon decided in microseconds not to serve.
//
// Driven at the worker directly, for the same reason the coalescing phase above is: neither
// "the queue is at its limit" nor "a task is still queued when the worker stops" is a state
// any sequence of bus calls can stage. It takes the coalescing phase's manager rather than
// building another, because it posts no renders at all -- nothing here reaches the Device,
// so the thread-affinity guard that forbids a second rendering worker does not apply.
//
// Runs LAST in that phase, after its FrameReady counts are graded: this block stops a
// worker, and a stopped worker beside a live one is not a state those counts describe.
// --------------------------------------------------------------------------- //
void runWorkerTaskAnswerChecks(wallpaperx::WallpaperManager& manager, const Check& check) {
    using TaskState = wallpaperx::RenderWorker::TaskState;
    constexpr int kMax = static_cast<int>(wallpaperx::RenderWorker::kMaxQueuedTasks);

    // Every task posted below reports through this, so "answered exactly once" is a count
    // and not an impression. Declared ahead of the worker, and read only after the worker
    // has been joined and stop() has returned on both threads.
    struct Answers {
        std::atomic<int> invocations{0};
        std::atomic<int> ran{0};
        std::atomic<int> shuttingDown{0};
        std::atomic<int> busy{0};
        void record(TaskState state) {
            ++invocations;
            if (state == TaskState::Run) ++ran;
            else if (state == TaskState::ShuttingDown) ++shuttingDown;
            else ++busy;
        }
    } answers;
    int posted = 0;

    // Released on every path out of this function, including a throw: a worker left blocked
    // on a gate never observes stop(), so the join would hang instead of failing.
    struct Gate {
        std::promise<void> promise;
        std::atomic<bool> open{false};
        void release() {
            if (!open.exchange(true)) promise.set_value();
        }
        ~Gate() { release(); }
    };
    Gate holdOne;
    Gate holdTwo;
    const std::shared_future<void> gateOne = holdOne.promise.get_future().share();
    const std::shared_future<void> gateTwo = holdTwo.promise.get_future().share();

    std::promise<void> enteredOne;
    std::promise<void> enteredTwo;
    std::future<void> insideOne = enteredOne.get_future();
    std::future<void> insideTwo = enteredTwo.get_future();
    std::atomic<bool> secondTaskFinished{false};

    wallpaperx::RenderWorker worker(manager, [](int, uint64_t) {});

    // Held here so everything below is posted while the worker is provably inside a task and
    // therefore dequeuing nothing. Without this barrier every count below is a race.
    worker.postTask([&answers, &enteredOne, gateOne](TaskState state) mutable {
        answers.record(state);
        enteredOne.set_value();
        gateOne.wait();
    });
    ++posted;
    if (insideOne.wait_for(wallpaperx::testing::kSignalTimeout) != std::future_status::ready) {
        check(false, "the render worker picked up the task that holds it");
        std::fprintf(stderr, "dbus_service_test: the worker never entered the holding task, so the queue "
                             "states below cannot be staged; skipping them rather than measuring a worker "
                             "that is not running\n");
        return;
    }

    // GOAL: RenderWorker::post() ignores a negative monitor id, so it never puts a job on the
    //       FIFO that is neither a render nor a task. False if it queues one: run() treats
    //       every job with a negative id as a task and decrements the task count for it, so
    //       each such post silently spends one of the 64 slots that exist to stop a live bus
    //       message being pinned per queued export. Reachable from a hand-edited config, whose
    //       ids reach post() through DbusService::restoreSaved().
    // WHY-THIS-SHAPE: measured as the queue's REMAINING capacity after the phantom jobs have
    //       been dequeued, which is the only point at which the miscount exists. The obvious
    //       simpler check -- post(-1) and assert nothing rendered -- is vacuous: a job with a
    //       negative id and no task renders nothing in either build. Asserting the capacity
    //       before the gate opens is vacuous too, for the mirror reason: the count is only
    //       wrong once run() has processed those jobs.
    //       Expressed against kMaxQueuedTasks rather than 64, so raising the limit does not
    //       silently turn this into a check on a number nobody maintains.
    // CONTROL: `|| monitorId < 0` dropped from RenderWorker::post()
    //       (backend/daemon/render_worker.cpp) -> "FAIL: a negative monitor id costs the task
    //       queue none of its capacity", printing "4 task(s) fit where 1 should, after 3 post(s)
    //       with a negative id" -- one per phantom job, exactly as predicted. The shutdown check
    //       below goes red with it as a knock-on ("67 task(s) answered ShuttingDown, expected
    //       64"), since those three extra tasks are drained too. Observed 2026-08-19. Reverted.
    constexpr int kNegativePosts = 3;
    for (int i = 0; i < kNegativePosts; ++i) worker.post(-1 - i);

    // A second holding task, so there is a moment AFTER the phantom jobs have been dequeued at
    // which the worker is stopped again with tasks still queued behind it.
    worker.postTask([&answers, &enteredTwo, gateTwo, &secondTaskFinished](TaskState state) mutable {
        answers.record(state);
        enteredTwo.set_value();
        gateTwo.wait();
        // Deliberate, and only for the second stop() check far below: it makes the window in
        // which a caller that skipped the join has returned too early a real interval rather
        // than a few instructions. Costs this phase 200 ms once.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        secondTaskFinished.store(true);
    });
    ++posted;

    const auto fill = [&worker, &answers, &posted] {
        worker.postTask([&answers](TaskState state) { answers.record(state); });
        ++posted;
    };
    for (int i = 1; i < kMax; ++i) fill();  // the holding task above is the first of kMax

    // GOAL: a task posted when the queue is already at its limit is answered Busy, on the
    //       posting thread, rather than dropped. False if it is dropped: the export handler's
    //       sdbus::Result is destroyed unanswered and its client blocks for the reply timeout.
    // WHY-THIS-SHAPE: the answer is read immediately after postTask returns, because the claim
    //       is that it is answered INLINE -- a check that merely waited for the count to reach
    //       one would also pass for a build that answered it from the worker thread much later,
    //       which is the behaviour a blocked client cannot distinguish from a drop.
    // CONTROL: the `else if (m_queuedTasks >= kMaxQueuedTasks)` arm deleted from
    //       RenderWorker::postTask() (backend/daemon/render_worker.cpp) -> "FAIL: a task posted
    //       past the queue limit is answered Busy before postTask returns", printing "the task
    //       past the limit was answered 0 rather than Busy ... after 66 posts against a limit of
    //       64", with the capacity check and the shutdown count red beside it (the queue is
    //       unbounded, so nothing is refused anywhere). Observed 2026-08-19. Reverted.
    std::atomic<TaskState> overflowAnswer{TaskState::Run};
    worker.postTask([&answers, &overflowAnswer](TaskState state) {
        answers.record(state);
        overflowAnswer.store(state);
    });
    ++posted;
    const bool refusedInline = overflowAnswer.load() == TaskState::Busy;
    check(refusedInline, "a task posted past the queue limit is answered Busy before postTask returns");
    if (!refusedInline) {
        std::fprintf(stderr, "  the task past the limit was answered %d rather than Busy (Run=0, ShuttingDown=1, "
                             "Busy=2), after %d posts against a limit of %d\n",
                     static_cast<int>(overflowAnswer.load()), posted, kMax);
    }

    // Lets the worker drain the phantom jobs and reach the second holding task.
    holdOne.release();
    if (insideTwo.wait_for(wallpaperx::testing::kSignalTimeout) != std::future_status::ready) {
        check(false, "the render worker reached the second holding task");
        std::fprintf(stderr, "dbus_service_test: the worker did not reach the second holding task; the "
                             "remaining checks are skipped rather than measured against a stalled queue\n");
        holdTwo.release();
        return;
    }

    // One slot came free when the second holding task was dequeued, so exactly one more task
    // fits -- unless jobs that were never tasks have been counted as such.
    int accepted = 0;
    for (int i = 0; i < kNegativePosts + 2; ++i) {
        // shared_ptr, not a stack local: a task that is ACCEPTED here is answered later, from
        // stop()'s drain, long after this loop iteration's frame has gone. A reference to a
        // local was the first shape and segfaulted the binary on exactly that path.
        const auto answer = std::make_shared<std::atomic<TaskState>>(TaskState::Run);
        worker.postTask([&answers, answer](TaskState state) {
            answers.record(state);
            answer->store(state);
        });
        ++posted;
        if (answer->load() == TaskState::Busy) break;
        ++accepted;
    }
    check(accepted == 1, "a negative monitor id costs the task queue none of its capacity");
    if (accepted != 1) {
        std::fprintf(stderr, "  %d task(s) fit where 1 should, after %d post(s) with a negative id\n", accepted,
                     kNegativePosts);
    }

    // GOAL: stop() answers every task still on the queue with ShuttingDown, and every caller of
    //       stop() waits for the worker to be joined. False on the first count if the queue is
    //       simply discarded: up to 64 clients then block for the reply timeout on a daemon
    //       that is already gone. False on the second if a caller returns early because another
    //       thread had already set the stop flag: that caller then proceeds to tear down state
    //       the worker thread is still reading.
    // WHY-THIS-SHAPE: two threads, and the SECOND stop() is released by a probe rather than by
    //       a sleep. postTask cannot be queued here -- the queue is full, so every probe is
    //       answered inline -- and an inline ShuttingDown is the only observation that proves
    //       the flag is already set. Releasing the gate on a timer instead is the obvious
    //       simpler version and is a race: the worker would resume before the flag was set,
    //       run the queued tasks normally, and this phase would fail a correct build.
    //       Every wait has a failure branch, and the second-caller check is asserted ONLY when
    //       the probe actually observed the flag -- a run where it did not has staged nothing,
    //       and grading it would be grading the harness.
    // CONTROL: two, in RenderWorker (backend/daemon/render_worker.cpp), both observed 2026-08-19
    //       and both reverted. (a) the drain loop at the end of stop() replaced by
    //       `dropped.clear();` -> "FAIL: every task still queued when the worker stops is
    //       answered, not dropped" printing "0 task(s) answered ShuttingDown, expected 64", and
    //       "FAIL: every task this phase posted was invoked exactly once" printing "5
    //       invocation(s) for 69 posted task(s): 2 ran, 1 shutting down, 2 busy". (b) `if
    //       (m_stop) return;` restored at the top of stop() -> "FAIL: a second caller of stop()
    //       waits for the worker rather than returning on another thread's flag", printing
    //       "stop() returned on the second thread while the task the worker was running had not
    //       finished", and nothing else red -- three runs, red every time.
    // COVERAGE-ASSERT: `answers.invocations` is compared against the number of posts this
    //       function made, so a later edit that posts fewer tasks -- or one where a task is
    //       answered twice -- fails rather than quietly checking less.
    std::atomic<bool> sawStopping{false};
    std::atomic<bool> secondStopSawItFinished{false};
    {
        std::jthread releaser([&] {
            for (int i = 0; i < 2000 && !sawStopping.load(); ++i) {
                // shared_ptr, not a stack local: were the queue ever not full, this probe
                // would be queued and answered after the loop iteration had gone.
                const auto seen = std::make_shared<std::atomic<TaskState>>(TaskState::Run);
                worker.postTask([seen](TaskState state) { seen->store(state); });
                if (seen->load() == TaskState::ShuttingDown) {
                    sawStopping.store(true);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // Released whether or not the flag was seen: the gate must not be what wedges the
            // join. Whether it was seen decides which checks below are asked, not this.
            holdTwo.release();
            worker.stop();
            secondStopSawItFinished.store(secondTaskFinished.load());
        });
        worker.stop();
    }

    const int expectedShuttingDown = kMax;
    const bool answeredOnShutdown = answers.shuttingDown.load() == expectedShuttingDown;
    check(answeredOnShutdown, "every task still queued when the worker stops is answered, not dropped");
    if (!answeredOnShutdown) {
        std::fprintf(stderr, "  %d task(s) answered ShuttingDown, expected %d\n", answers.shuttingDown.load(),
                     expectedShuttingDown);
    }

    if (sawStopping.load()) {
        check(secondStopSawItFinished.load(),
              "a second caller of stop() waits for the worker rather than returning on another thread's flag");
        if (!secondStopSawItFinished.load()) {
            std::fprintf(stderr, "  stop() returned on the second thread while the task the worker was running "
                                 "had not finished\n");
        }
    } else {
        std::fprintf(stderr, "dbus_service_test: the stop flag was never observed from the second thread, so "
                             "whether that thread waits for the join was not established on this run\n");
    }

    // A task handed to a worker that has already stopped is the third way one can be dropped,
    // and the only one reachable without staging a queue at all.
    std::atomic<TaskState> afterStop{TaskState::Run};
    worker.postTask([&answers, &afterStop](TaskState state) {
        answers.record(state);
        afterStop.store(state);
    });
    ++posted;
    const bool answeredAfterStop = afterStop.load() == TaskState::ShuttingDown;
    check(answeredAfterStop, "a task posted to a worker that has stopped is answered ShuttingDown, not dropped");
    if (!answeredAfterStop) {
        std::fprintf(stderr, "  it was answered %d (Run=0, ShuttingDown=1, Busy=2)\n",
                     static_cast<int>(afterStop.load()));
    }

    const bool eachAnsweredOnce = answers.invocations.load() == posted;
    check(eachAnsweredOnce, "every task this phase posted was invoked exactly once");
    if (!eachAnsweredOnce) {
        std::fprintf(stderr, "  %d invocation(s) for %d posted task(s): %d ran, %d shutting down, %d busy\n",
                     answers.invocations.load(), posted, answers.ran.load(), answers.shuttingDown.load(),
                     answers.busy.load());
    }
}

// --------------------------------------------------------------------------- //
// Five images per monitor, and who is allowed to be drawn over.
//
// THREE CONSUMERS, and that is not decoration. A monitor keeps five images and one consumer may
// hold at most two, so it takes two consumers at their cap to bring the ring down to the image
// the producer must keep -- and a THIRD, holding nothing, to be the one that asks for it: either
// of the first two would be refused by the per-consumer cap whether the producer's reserve
// existed or not. A phase driven by fewer never reaches the state "only the reserve is free", and
// every check about refusal, refusal counting and release would pass without the state they
// describe ever existing.
//
// The manager tier and the wire tier are split into two phases with a manager each, and the
// split is forced rather than chosen: Device pins itself to the thread that created it, so a
// manager with a DbusService attached has its Device on that service's render worker and this
// thread may not call renderPending() on it. Anything that needs to drive a render pass
// synchronously -- which is everything about "was this render refused" -- has to be on a
// manager with no worker.
// --------------------------------------------------------------------------- //

// A phase that builds its own WallpaperManager builds its own Device, and the validation
// counter is per-Device: the handle captured around the outer manager says nothing about this
// one. Graded in a destructor so an exception out of the phase cannot skip it, and the counter
// is snapshotted separately (see CaptureGradeOnExit) because the layer's teardown complaints
// are emitted from ~WallpaperManager -- reading a live accessor before the manager is gone
// silently exempts the whole destruction half.
class PhaseDeviceGrade {
public:
    PhaseDeviceGrade(std::string label, const Check& check) : m_label(std::move(label)), m_check(check) {}

    PhaseDeviceGrade(const PhaseDeviceGrade&) = delete;
    PhaseDeviceGrade& operator=(const PhaseDeviceGrade&) = delete;

    // Must be called while `manager` is still alive.
    void capture(const wallpaperx::WallpaperManager& manager) {
        m_counter = manager.validationCounter();
        m_layerActive = manager.validationLayerActive();
    }

    // For a phase abandoning itself because this host could not render at all: it has already
    // said so through notVerified(), and a Device it never got to build must not be reported a
    // second time as a failure of the daemon.
    void expectNoDevice() { m_deviceOptional = true; }

    ~PhaseDeviceGrade() {
        int failures = 0;
        int ungraded = 0;
        {
            wallpaperx::testing::ValidationGrade grade("dbus_service_test (" + m_label + ")", failures);
            // The waiver covers a missing device here as well as a missing validator:
            // ValidationGrade waives only the latter, correctly for a probe, but a phase whose
            // manager was built with validation off legitimately has no counter at all.
            if (wallpaperx::testing::validationWaived() || m_deviceOptional) grade.expectNoDevice();
            // Its own bucket: an absent layer package says nothing about this daemon and must
            // not be charged to the check below. A package that IS present while the device
            // failed to activate it still counts as a failure, which is right.
            grade.reportUngradedTo(ungraded);
            grade.adopt(m_counter, m_layerActive);
        }
        if (ungraded > 0) {
            std::fprintf(stderr,
                         "dbus_service_test: %s's validation grade could not be made -- VK_LAYER_KHRONOS_validation "
                         "is not loaded. The phase itself is still judged.\n",
                         m_label.c_str());
        }
        // Named for what was actually judged: ValidationGrade has already printed the specific
        // verdict, so a label asserting "zero validation warnings" would contradict two of its
        // three outcomes.
        m_check(failures == 0, ("dbus_service_test: " + m_label + "'s own device passed its validation grade").c_str());
    }

private:
    std::string m_label;
    const Check& m_check;
    std::shared_ptr<const std::atomic<int>> m_counter;
    bool m_layerActive = false;
    bool m_deviceOptional = false;
};

// Snapshots the grade off `manager` as the manager's scope unwinds, on the exception path too.
// Declare it immediately after the manager it names, and the PhaseDeviceGrade before it.
struct CaptureGradeOnExit {
    PhaseDeviceGrade& grade;
    const wallpaperx::WallpaperManager& manager;
    ~CaptureGradeOnExit() { grade.capture(manager); }
};

// One accepted request plus one render pass, on this thread. Returns whether a frame
// committed. Both halves together, because "the daemon was asked and then given a chance to
// draw" is the unit every check below is stated in.
bool renderOnce(wallpaperx::WallpaperManager& manager, int32_t monitorId, const std::string& imagePath) {
    manager.requestWallpaper("static", imagePath, monitorId);
    return manager.renderPending(monitorId).rendered;
}

// What a hand-over produced: the image id, or 0 with the daemon's own reason for not handing
// one over. Both, because "no image" and "refused so the producer keeps one" are now different
// answers and a check that saw only the 0 could not tell them apart.
struct Handover {
    uint64_t bufferId = 0;
    // Meaningful only when bufferId is 0; the export writes it on every empty-handle path.
    wallpaperx::NoFrameReason reason = wallpaperx::NoFrameReason::NeverRequested;

    bool served() const { return bufferId != 0; }
    // The daemon publishes one reason for every refusal -- the producer's reserve, the
    // per-consumer cap and the holder limit are all NoFreeImage -- so this cannot say WHICH
    // bound refused. Each check below distinguishes them by the state it drove the monitor into
    // instead, and says so where it matters.
    bool refused() const { return bufferId == 0 && reason == wallpaperx::NoFrameReason::NoFreeImage; }
};

// Every descriptor the export produced is closed here. This phase fetches a dozen frames, and
// a leak would surface as an EMFILE in some unrelated later phase rather than as anything
// readable here.
Handover fetchHandover(wallpaperx::WallpaperManager& manager, int32_t monitorId, const std::string& client) {
    wallpaperx::FrameExport exported = manager.exportCurrentFrame(monitorId, client);
    if (!exported.handle) return Handover{0, exported.reason};
    if (exported.handle->frame.fd >= 0) close(exported.handle->frame.fd);
    if (exported.handle->acquireFd >= 0) close(exported.handle->acquireFd);
    return Handover{exported.handle->frame.bufferId, wallpaperx::NoFrameReason::NeverRequested};
}

// The id of the image `client` is handed, or 0 when there is none.
uint64_t fetchFrame(wallpaperx::WallpaperManager& manager, int32_t monitorId, const std::string& client) {
    return fetchHandover(manager, monitorId, client).bufferId;
}

// True when every id is a different image and none is 0. Used as the premise of the ring checks:
// hand-overs that repeat an id mean the ring never rotated, and the state the check is about was
// never built.
bool allDistinct(const std::vector<uint64_t>& ids) {
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == 0) return false;
        for (size_t j = i + 1; j < ids.size(); ++j) {
            if (ids[i] == ids[j]) return false;
        }
    }
    return true;
}

std::string joinIds(const std::vector<uint64_t>& ids) {
    std::string out;
    for (uint64_t id : ids) out += (out.empty() ? "" : ", ") + std::to_string(id);
    return out;
}

wallpaperx::MonitorStatus monitorStatus(const wallpaperx::WallpaperManager& manager, int32_t monitorId) {
    const wallpaperx::WallpaperManager::Snapshot snapshot = manager.snapshot();
    const auto it = snapshot.monitors.find(monitorId);
    return it == snapshot.monitors.end() ? wallpaperx::MonitorStatus{} : it->second;
}

void runBufferHoldChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                         const Check& check) {
    PhaseDeviceGrade grade("the buffer-hold phase", check);

    // Stand-ins for the unique bus names the daemon would record. Any two distinct strings do:
    // the manager only ever compares them.
    const std::string alice = ":test.hold.alice";
    const std::string bob = ":test.hold.bob";
    // The third is what makes the reserve reachable: with nothing ever taken back, alice and bob
    // at their cap account for every image but one, and only a consumer holding none of them can
    // ask for that last one without the per-consumer cap answering first.
    const std::string carol = ":test.hold.carol";

    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    const char* kHoldGoals = "the buffer-hold goals (a held image is not drawn over, every bound on the holds is a "
                             "refusal rather than a claim taken back, and the two ways a claim ends)";

    if (!renderOnce(manager, kMonitorHoldRing, imagePath)) {
        // A host with no working Vulkan never gets a Device built here, and the reason has
        // just been printed. Charging that to this phase's device grade as well would name the
        // daemon for a fault of the machine.
        grade.expectNoDevice();
        wallpaperx::testing::notVerified(
            kHoldGoals, "the first render on this host produced no frame at all, so no image was ever handed to a "
                        "consumer and none of the states these goals are about could be built");
        return;
    }
    const uint64_t firstId = fetchFrame(manager, kMonitorHoldRing, alice);
    if (firstId == 0) {
        wallpaperx::testing::notVerified(kHoldGoals,
                                         "the monitor committed a frame but could not export it, so no consumer "
                                         "ever came to hold an image");
        return;
    }

    // GOAL: a render never draws into an image a consumer is holding. With alice holding the
    //       monitor's first image, the next render still commits, and it commits into a
    //       different image.
    // WHY-THIS-SHAPE: both halves are asserted, because they fail in opposite directions and
    //       each alone is satisfied by a broken daemon. "It commits" alone is satisfied by the
    //       old single-image behaviour that repainted the frame in use; "a different id" alone
    //       is satisfied by a daemon that has stopped rendering, since the second fetch would
    //       then return nothing and 0 differs from firstId. Which image the pixels actually
    //       landed in is established at the renderer tier by target_ring_probe, whose readback
    //       works on the opaque-fd GPU where a CPU mapping of the exported frame cannot.
    // CONTROL: WallpaperManager::kTargetDepth (backend/daemon/wallpaper_manager.h) set to 1 ->
    //       "FAIL: a render commits while a consumer holds the monitor's first image" and
    //       "FAIL: that render drew into a different image from the one the consumer holds",
    //       with the ids printed as 1, 0, 0. Observed with this phase temporarily moved to the
    //       front of main(): at depth 1 the SHARED manager's only image is held by its own
    //       consumer, so the render phase above throws before this one would be reached. Both
    //       reverted.
    const bool secondCommitted = renderOnce(manager, kMonitorHoldRing, imagePath);
    check(secondCommitted, "a render commits while a consumer holds the monitor's first image");
    const uint64_t secondId = secondCommitted ? fetchFrame(manager, kMonitorHoldRing, alice) : 0;
    check(secondId != 0 && secondId != firstId,
          "that render drew into a different image from the one the consumer holds");

    // A second consumer takes its own fill, and this is what makes the rest of the phase
    // reachable: nothing is ever taken back, so two consumers at the per-consumer cap are the
    // only way to account for every image but the one the producer must keep.
    std::vector<uint64_t> ringIds{firstId, secondId};
    bool everyFillCommitted = secondCommitted;
    for (int i = 0; i < kExpectedHeldPerClient; ++i) {
        if (!everyFillCommitted) break;
        everyFillCommitted = renderOnce(manager, kMonitorHoldRing, imagePath);
        const uint64_t id = everyFillCommitted ? fetchFrame(manager, kMonitorHoldRing, bob) : 0;
        if (id == 0) everyFillCommitted = false;
        ringIds.push_back(id);
    }
    // The render that fills the last image. Nobody fetches it: it is the producer's reserve, and
    // the refusal below is the daemon declining to hand it over.
    const bool reserveRender = everyFillCommitted && renderOnce(manager, kMonitorHoldRing, imagePath);
    check(reserveRender, "a render commits with two consumers holding two images each");

    // COVERAGE-ASSERT: every check below describes a monitor whose last free image is the
    // producer's reserve. If the four hand-overs did not name four different images the
    // consumers are not holding four, the reserve is under no pressure, and the refusal below
    // would be grading a monitor with slack in it. Asserted before the refusal is read, so a run
    // that never built the state cannot report the refusal as established.
    // CONTROL: WallpaperManager::kFullConsumers (backend/daemon/wallpaper_manager.h) lowered to
    //       1, which shrinks the derived depth to three -> "FAIL: two consumers hold four of the
    //       monitor's five images, so exactly one is free", printing "the ids handed out were 1,
    //       2, 0", and the phase stops there so no reserve check below is reported as
    //       established. Reverted.
    // CONTROL, the other direction: that same constant raised to 3, which deepens the ring to
    //       seven -> this line stays GREEN, correctly, and the refusal check below goes red
    //       alone, printing "the third consumer was handed image 5". That is the right
    //       attribution: the premise held and the daemon's behaviour is what changed. Reverted.
    const bool ringFilled = reserveRender && ringIds.size() == static_cast<size_t>(2 * kExpectedHeldPerClient) &&
                            allDistinct(ringIds);
    check(ringFilled, "two consumers hold four of the monitor's five images, so exactly one is free");
    if (!ringFilled) {
        std::fprintf(stderr, "  the ids handed out were %s, reserve render committed: %s; the reserve checks below "
                             "have no pressure under them\n",
                     joinIds(ringIds).c_str(), reserveRender ? "yes" : "no");
        return;
    }

    // GOAL: the daemon keeps one image per monitor out of consumers' hands. A hand-over that
    //       would take the last free one is REFUSED, and refused with the reason that says so.
    // WHY-THIS-SHAPE: asked by a THIRD consumer holding nothing, so the per-consumer cap cannot
    //       be what answers -- alice and bob are both at their cap here, and asking either of
    //       them would be refused whether or not the reserve existed at all. The reason is
    //       asserted, not merely the absence of a handle: an empty handle alone is equally
    //       produced by a monitor that never rendered and by an export that failed.
    // CONTROL: the `m_targets->nextUnheld(monitorId, flattenHolds(next.holds)) == nullptr` half
    //       of the refusal in WallpaperManager::exportCurrentFrame (backend/daemon/
    //       wallpaper_manager.cpp) deleted, leaving only the cap and holder-limit half -> "FAIL:
    //       a hand-over that would take the producer's last free image is refused", printing the
    //       id the third consumer was handed instead. Reverted.
    const Handover refusedForCarol = fetchHandover(manager, kMonitorHoldRing, carol);
    check(refusedForCarol.refused(), "a hand-over that would take the producer's last free image is refused");
    if (!refusedForCarol.refused()) {
        std::fprintf(stderr, "  the third consumer was handed image %llu instead of being refused\n",
                     static_cast<unsigned long long>(refusedForCarol.bufferId));
    }

    // GOAL: a refusal is counted and published per monitor. Without it a consumer that has
    //       stopped giving images back is invisible: the daemon goes on rendering and committing,
    //       and every other field reads like a monitor that is keeping up.
    // WHY-THIS-SHAPE: an exact count of one, which is safe because this manager is driven from
    //       this thread alone and exactly one hand-over on this monitor has been refused so far.
    //       "Greater than zero" would also pass for a counter that never resets, and the reset is
    //       half of what the field means.
    // CONTROL: `++it->second.refusedHandovers;` deleted from WallpaperManager::exportCurrentFrame
    //       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: the refused hand-over was counted
    //       against the monitor", printing 0. Reverted.
    {
        const wallpaperx::MonitorStatus afterRefusal = monitorStatus(manager, kMonitorHoldRing);
        check(afterRefusal.refusedHandovers == 1, "the refused hand-over was counted against the monitor");
        if (afterRefusal.refusedHandovers != 1) {
            std::fprintf(stderr, "  the monitor reports %u refused hand-over(s) after exactly one refusal\n",
                         afterRefusal.refusedHandovers);
        }
    }

    // GOAL: refusing a consumer costs that consumer a frame, not the monitor its renders. The
    //       next request commits, it is discharged rather than left owed, and no render is
    //       deferred for want of an image.
    // WHY-THIS-SHAPE: three facts, because each alone is satisfied by a broken daemon. "It
    //       committed" alone is also true of one that drew over a held image; the generation
    //       moving says nothing about whether the request was discharged; and deferredRenders
    //       is the one that says the reserve, rather than luck, is why the producer had
    //       somewhere to draw. renderPending()'s deferral path still exists as a backstop and
    //       is meant to be unreachable now, so a non-zero count here is a report that a
    //       hand-over got past the reserve -- which is why it is asserted at zero rather than
    //       deleted along with the states it used to grade.
    // CONTROL: the `if (m_targets->nextUnheld(...) == nullptr)` refusal inside the admission
    //       block of WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp)
    //       deleted, so claims are still recorded but no hand-over is ever refused -> "FAIL: a
    //       request made after a refusal commits rather than being deferred", "FAIL: the frame
    //       counter moved by exactly that one render", "FAIL: the committed frame discharged the
    //       request" and "FAIL: no render was deferred -- the reserve kept an image free, so the
    //       backstop never fired", printing 1 deferral; 11 red in the run. Reverted. Deleting
    //       the whole admission block instead does NOT redden this check: with nothing recorded
    //       no image is ever held, so no render can defer and only the refusal checks go red.
    const uint64_t generationBefore = monitorStatus(manager, kMonitorHoldRing).generation;
    const bool fourthCommitted = renderOnce(manager, kMonitorHoldRing, imagePath);
    check(fourthCommitted, "a request made after a refusal commits rather than being deferred");
    {
        const wallpaperx::MonitorStatus after = monitorStatus(manager, kMonitorHoldRing);
        check(after.generation == generationBefore + 1, "the frame counter moved by exactly that one render");
        check(!after.renderOutstanding, "the committed frame discharged the request");
        check(after.deferredRenders == 0,
              "no render was deferred -- the reserve kept an image free, so the backstop never fired");
        if (after.deferredRenders != 0) {
            std::fprintf(stderr,
                         "  %u render(s) were refused for want of a free image, which the reserve exists to make "
                         "impossible\n",
                         after.deferredRenders);
        }
        // GOAL: the refusal count is reset by a served hand-over and by nothing else. A count a
        //       commit cleared would read zero for exactly the monitor it exists to report on --
        //       one that keeps rendering while no consumer can fetch.
        // WHY-THIS-SHAPE: read after a commit that produced a new generation, which is the event
        //       that resets the neighbouring deferredRenders. Asserting only that it is non-zero
        //       somewhere would not separate the two reset rules, and those rules are the whole
        //       difference between the two counters.
        // CONTROL: `it->second.refusedHandovers = 0;` moved out of
        //       WallpaperManager::exportCurrentFrame and into the commit path beside
        //       `state.deferredRenders = 0;` in WallpaperManager::renderPending (backend/daemon/
        //       wallpaper_manager.cpp) -> "FAIL: and a commit does not clear it -- only a served
        //       hand-over does", printing 0. Reverted.
        check(after.refusedHandovers == 1, "and a commit does not clear it -- only a served hand-over does");
        if (after.refusedHandovers != 1) {
            std::fprintf(stderr, "  the monitor reports %u refused hand-over(s) after a commit that reset nothing\n",
                         after.refusedHandovers);
        }
    }

    // GOAL: a consumer cannot release an image another consumer holds. The claim is recorded
    //       against the caller's own name, so naming somebody else's buffer frees nothing.
    // WHY-THIS-SHAPE: graded on what the release REPORTS, not on a later render. With the
    //       reserve in force no render is ever blocked, so the old shape -- "the monitor is
    //       still refused afterwards" -- is now true whatever the release did and would grade
    //       nothing. What a release that ended a claim produces instead is an announcement of
    //       the monitor's committed frame; one that ended nothing produces none.
    // CONTROL: the `hold.client == client` comparison in WallpaperManager::dropHoldLocked
    //       (backend/daemon/wallpaper_manager.cpp), which releaseBuffer calls, replaced by
    //       `true`, so any caller matches the first holder -> "FAIL: one consumer cannot
    //       release an image another consumer
    //       holds", printing "it freed something and announced monitor 0 generation 4", and
    //       downstream the departure check, whose consumer had already had its claim taken.
    //       Reverted.
    {
        std::vector<wallpaperx::FrameAvailable> announced;
        const std::vector<int> byTheWrongClient = manager.releaseBuffer(bob, kMonitorHoldRing, secondId, &announced);
        check(byTheWrongClient.empty() && announced.empty(),
              "one consumer cannot release an image another consumer holds");
        if (!announced.empty()) {
            std::fprintf(stderr, "  it freed something and announced monitor %d generation %llu\n",
                         announced.front().monitorId,
                         static_cast<unsigned long long>(announced.front().generation));
        }
    }

    // GOAL: a claim ending re-announces the monitor's committed frame. Nothing is going to
    //       redraw a monitor whose request is already serviced, so without this announcement a
    //       consumer that was refused has no trigger to ask again and its screen stays on the
    //       frame it had until the user sets another wallpaper.
    // WHY-THIS-SHAPE: the release is required to produce an announcement and NO render. The
    //       obvious form -- assert the monitor is named for a re-render -- describes the old
    //       contract: with the reserve in force there is no deferred render left to discharge,
    //       so a freed image has to be published some other way or not at all. The generation
    //       is compared against the committed one, because an announcement naming a generation
    //       the daemon has not committed sends every consumer after a frame that does not
    //       exist.
    // CONTROL: the `announce->push_back(...)` line in
    //       WallpaperManager::reportFreedImageLocked (backend/daemon/wallpaper_manager.cpp)
    //       deleted -> "FAIL: releasing an image re-announces the monitor's committed frame",
    //       printing 0 announcements. Reverted.
    const uint64_t committed = monitorStatus(manager, kMonitorHoldRing).generation;
    {
        std::vector<wallpaperx::FrameAvailable> announced;
        const std::vector<int> toRender = manager.releaseBuffer(alice, kMonitorHoldRing, firstId, &announced);
        const bool announcedOnce = announced.size() == 1 && announced.front().monitorId == kMonitorHoldRing &&
                                   announced.front().generation == committed;
        check(announcedOnce, "releasing an image re-announces the monitor's committed frame");
        check(toRender.empty(), "and asks for no render, because the monitor owes none");
        if (!announcedOnce) {
            std::fprintf(stderr, "  %zu announcement(s) for a monitor whose committed frame is generation %llu\n",
                         announced.size(), static_cast<unsigned long long>(committed));
        }
    }

    // GOAL: a refusal is temporary. The consumer that was refused is served once a release has
    //       put an image back, and the monitor's refusal count goes back to zero -- so being
    //       refused costs that consumer a frame and not its screen.
    // WHY-THIS-SHAPE: asked of the SAME consumer that was refused above, with nothing in between
    //       but one release. A fresh consumer would be served whatever the release did, since a
    //       client holding nothing is never the one keeping the ring full. The reset is checked
    //       here rather than beside the increment because a served hand-over is the only thing
    //       that performs it, and this is the first served hand-over since the refusal.
    // CONTROL: `std::erase(holder->buffers, bufferId)` in WallpaperManager::dropHoldLocked
    //       (backend/daemon/wallpaper_manager.cpp) changed to a `std::count(...)` that reports
    //       the claim without removing it -> "FAIL: the refused consumer is served once a
    //       release has put an image back". Reverted.
    // CONTROL, the reset leg: `it->second.refusedHandovers = 0;` deleted from
    //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp) -> "FAIL:
    //       and the served hand-over cleared the monitor's refusal count", with the service check
    //       above green. Reverted.
    const Handover carolAtLast = fetchHandover(manager, kMonitorHoldRing, carol);
    check(carolAtLast.served(), "the refused consumer is served once a release has put an image back");
    if (!carolAtLast.served()) {
        // Abandoned rather than graded: the reset is a property OF a served hand-over, so with
        // nothing served this check would report a second failure for the first one's cause.
        std::fprintf(stderr, "  it was refused again after the release, so nothing came back into the ring, and "
                             "the refusal-count reset had no served hand-over to be graded against\n");
        return;
    }
    check(monitorStatus(manager, kMonitorHoldRing).refusedHandovers == 0,
          "and the served hand-over cleared the monitor's refusal count");

    // GOAL: a consumer leaving the bus gives its images back, and that is announced too.
    //       Without the first half a consumer that crashed holding a frame keeps that image out
    //       of the ring for the life of the daemon -- plasmashell restarts are routine while
    //       this daemon does not.
    // WHY-THIS-SHAPE: the announcement alone would not establish that anything was dropped, so
    //       the hand-over after it is the second leg -- and the ring is driven back down to its
    //       reserve BEFORE the departure, which is what stops that second leg from being vacuous.
    //       Without the render and the gate below, the last committed image is one carol already
    //       holds, so her fetch would be answered from her own claim whether or not the departure
    //       freed anything.
    // CONTROL: `std::erase_if(state.holds, ...)` in WallpaperManager::forgetHeldBuffers
    //       (backend/daemon/wallpaper_manager.cpp) replaced by a `std::count_if` that reports
    //       the monitor without dropping the claim -> "FAIL: the departed consumer's images are
    //       really back in the ring", printing that the surviving consumer was refused. The
    //       announcement check stayed green, which is the measurement showing the two are not one
    //       fact. Reverted.
    // CONTROL, the other leg: the `announce` argument of the reportFreedImageLocked call in
    //       WallpaperManager::forgetHeldBuffers (backend/daemon/wallpaper_manager.cpp) passed
    //       as `nullptr`, so claims are dropped but nothing is published -> "FAIL: a departure
    //       announces the monitor's committed frame", with the ring check green. Reverted.
    {
        check(renderOnce(manager, kMonitorHoldRing, imagePath), "a render commits into the one image left free");
        // NO CONTROL, and this is the reason rather than an omission: it is a gate on the check
        // below, not a claim of its own. Every production mutation that would make it false --
        // the reserve deleted, the ring deepened -- also reddens the reserve refusal earlier in
        // this phase. What it guards is a future edit to THIS file that leaves the ring with
        // slack here, which would make the post-departure hand-over succeed for free.
        const Handover gate = fetchHandover(manager, kMonitorHoldRing, carol);
        check(gate.refused(), "and the ring is down to its reserve again before the departure");
        if (!gate.refused()) {
            std::fprintf(stderr, "  the consumer was handed image %llu, so the departure below had a free image "
                                 "under it and would grade nothing\n",
                         static_cast<unsigned long long>(gate.bufferId));
            return;
        }

        std::vector<wallpaperx::FrameAvailable> announced;
        const std::vector<int> toRender = manager.forgetHeldBuffers(alice, &announced);
        check(announced.size() == 1 && announced.front().monitorId == kMonitorHoldRing,
              "a departure announces the monitor's committed frame");
        check(toRender.empty(), "and asks for no render either");
        const Handover afterDeparture = fetchHandover(manager, kMonitorHoldRing, carol);
        check(afterDeparture.served() && afterDeparture.bufferId != carolAtLast.bufferId,
              "the departed consumer's images are really back in the ring");
        if (!afterDeparture.served()) {
            std::fprintf(stderr, "  the surviving consumer was refused, so the departed one's claims are still "
                                 "holding the ring\n");
        }
    }

    // ---------------------------------------------------------------- //
    // A caller with no bus name, on a monitor of its own.
    // ---------------------------------------------------------------- //

    // GOAL: a frame exported to a caller the daemon cannot name takes no image out of the
    //       ring. A claim recorded under an empty name could never be withdrawn -- no
    //       ReleaseBuffer names it, no departure announces it -- so it would cost the monitor
    //       an image for the life of the daemon.
    // WHY-THIS-SHAPE: the unnamed caller is asked for a WHOLE RING'S WORTH of hand-overs, which
    //       is more than twice its per-consumer cap, and every one is required to name a
    //       different image. The obvious shape -- two exports and then "a render still commits"
    //       -- stops one hand-over short of the cap at this depth and would commit either way.
    //       A single loop is enough because both bounds a recorded claim could hit are inside
    //       it: charged with claims, the caller is refused at the third hand-over by the cap,
    //       and if the cap were gone as well the reserve refuses the fifth. Measured: with the
    //       guard removed it stopped after two, and with the cap disabled as well after four.
    // CONTROL: the `if (!client.empty())` guard around the hand-over record in
    //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp)
    //       removed, so an empty name is recorded like any other -> "FAIL: a caller with no bus
    //       name is served for a whole ring's worth of renders", printing "it was handed [6, 7]
    //       before stopping". The only red line in the run. Reverted.
    // COVERAGE-ASSERT: the hand-overs are required to name kExpectedRingDepth DIFFERENT images,
    //       not merely to succeed, so a daemon that answered every one from a ring that had
    //       stopped rotating could not pass this.
    {
        std::vector<uint64_t> unnamedIds;
        bool servedThroughout = true;
        for (int i = 0; i < kExpectedRingDepth; ++i) {
            if (!renderOnce(manager, kMonitorHoldUnnamed, imagePath)) {
                servedThroughout = false;
                break;
            }
            const uint64_t id = fetchFrame(manager, kMonitorHoldUnnamed, "");
            if (id == 0) {
                servedThroughout = false;
                break;
            }
            unnamedIds.push_back(id);
        }
        servedThroughout = servedThroughout && unnamedIds.size() == static_cast<size_t>(kExpectedRingDepth) &&
                           allDistinct(unnamedIds);
        static_assert(kExpectedRingDepth > kExpectedHeldPerClient,
                      "the loop must exceed the per-consumer cap, or a daemon charging the unnamed caller with "
                      "claims would never be refused and the check would grade nothing");
        check(servedThroughout, "a caller with no bus name is served for a whole ring's worth of renders");
        if (!servedThroughout) {
            std::fprintf(stderr, "  it was handed [%s] before stopping, so a claim was charged to a name nothing "
                                 "could withdraw\n",
                         joinIds(unnamedIds).c_str());
        }
    }

    // ---------------------------------------------------------------- //
    // The holder limit, on a monitor of its own: five consumers.
    // ---------------------------------------------------------------- //

    // GOAL: with the holder limit reached, a consumer the daemon has never served is REFUSED,
    //       and every holder keeps everything it had. False if the daemon makes room by
    //       forgetting somebody's claim: that consumer is still sampling the image, and the very
    //       next render draws into it.
    // WHY-THIS-SHAPE: which holders survived is read through releaseBuffer, because the hold
    //       list is private and a release is the daemon's own report of whether a claim was
    //       there to end -- it frees something for a holder it still has and nothing at all for
    //       one it has forgotten. "The fifth was refused" alone would not establish the second
    //       half: a daemon that forgot a holder AND refused the newcomer passes it. An existing
    //       holder is re-served first, so the limit is also required not to refuse the consumers
    //       already inside it.
    // CONTROL: `if (holds.size() >= kMaxHoldersPerMonitor) return {};` deleted from
    //       WallpaperManager::withHandover (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a
    //       fifth consumer past the holder limit is refused", printing the image it was handed.
    //       Reverted.
    // CONTROL, the survivors leg: that same line replaced by the eviction it took the place of,
    //       `if (holds.size() >= kMaxHoldersPerMonitor) holds.erase(holds.begin());` -> "FAIL: a
    //       fifth consumer past the holder limit is refused" and "FAIL: and every holder still
    //       has the claim it had", printing the survivor list without the evicted consumer.
    //       Reverted.
    // COVERAGE-ASSERT: every premise -- four holders on one image, the re-service landing on
    //       that same image -- is asserted before any survivor is read, so a run that never
    //       reached the limit cannot report the refusal as established. The probe count beside
    //       them guards this file rather than the daemon; see the comment on it.
    {
        const std::string reServed = ":test.limit.re-served";
        const std::string idle = ":test.limit.idle";
        const std::string bystanderA = ":test.limit.bystander-a";
        const std::string bystanderB = ":test.limit.bystander-b";
        const std::string newcomer = ":test.limit.newcomer";
        static_assert(kExpectedHoldersPerMonitor == 4,
                      "the four names below fill the holder limit exactly; a different limit needs a different "
                      "number of them or the fifth hand-over would not be refused");

        bool builtTheState = renderOnce(manager, kMonitorHoldLimit, imagePath);
        // Every holder claims the SAME image: nothing renders in between, so each hand-over
        // names the monitor's one committed frame. That keeps the producer's reserve out of
        // this -- four claims on one image still leave four of the five free -- so a refusal
        // here is the holder bookkeeping and not the ring running out.
        uint64_t shared = 0;
        int filled = 0;
        for (const std::string& holder : {reServed, idle, bystanderA, bystanderB}) {
            if (!builtTheState) break;
            const Handover handed = fetchHandover(manager, kMonitorHoldLimit, holder);
            if (!handed.served() || (shared != 0 && handed.bufferId != shared)) {
                builtTheState = false;
                break;
            }
            shared = handed.bufferId;
            ++filled;
        }
        builtTheState = builtTheState && filled == kExpectedHoldersPerMonitor;
        check(builtTheState, "four consumers hold the same image, which fills the monitor's holder limit");

        // Each step is graded only once the one before it held, so one unbuilt premise produces
        // one red line naming the thing that actually did not happen rather than three.
        bool reServedAgain = false;
        if (builtTheState) {
            const Handover again = fetchHandover(manager, kMonitorHoldLimit, reServed);
            reServedAgain = again.served() && again.bufferId == shared;
            check(reServedAgain, "a consumer already inside the limit is served again, so the limit refuses arrivals "
                                 "and not the holders it already has");
        }

        if (!reServedAgain) {
            std::fprintf(stderr, "  the holder limit was never reached with one holder re-served, so the refusal "
                                 "checks below were not graded\n");
        } else {
            const Handover fifth = fetchHandover(manager, kMonitorHoldLimit, newcomer);
            check(fifth.refused(), "a fifth consumer past the holder limit is refused");
            if (!fifth.refused()) {
                std::fprintf(stderr, "  it was handed image %llu, on a monitor with four images still free -- so "
                                     "the limit gave way rather than refusing\n",
                             static_cast<unsigned long long>(fifth.bufferId));
            }

            std::vector<std::string> survivors;
            const std::vector<std::string> probed = {reServed, idle, bystanderA, bystanderB, newcomer};
            for (const std::string& who : probed) {
                std::vector<wallpaperx::FrameAvailable> announced;
                const std::vector<int> toRender = manager.releaseBuffer(who, kMonitorHoldLimit, shared, &announced);
                if (!toRender.empty() || !announced.empty()) survivors.push_back(who);
            }
            // Guards this file, not the daemon: no production change can redden it. Dropping
            // `idle` from the probe list would leave `survivors` matching `expected` exactly and
            // the check below green while nothing asked about the holder an eviction would take.
            check(static_cast<int>(probed.size()) == kExpectedHoldersPerMonitor + 1,
                  "every consumer that could be holding was asked whether the daemon still has a claim for it");

            const std::vector<std::string> expected = {reServed, idle, bystanderA, bystanderB};
            check(survivors == expected, "and every holder still has the claim it had, because nothing was taken "
                                         "back to make room");
            if (survivors != expected) {
                std::string names;
                for (const std::string& who : survivors) names += (names.empty() ? "" : ", ") + who;
                std::fprintf(stderr, "  the survivors were [%s]; all four holders were expected, and the refused "
                                     "newcomer %s was expected to hold nothing\n",
                             names.c_str(), newcomer.c_str());
            }
        }
    }

    // ---------------------------------------------------------------- //
    // The other case, on a monitor of its own: one consumer, never releasing.
    // ---------------------------------------------------------------- //

    // GOAL: a consumer that never releases is REFUSED past the per-consumer cap rather than
    //       served by taking one of its own images back, and the monitor goes on rendering
    //       regardless. False if a third hand-over drops that consumer's oldest claim: it has
    //       given no sign of having stopped reading that image, and the next render draws into
    //       it.
    // WHY-THIS-SHAPE: on a monitor of its own with a SINGLE consumer, so the refusal can only be
    //       the cap. The producer's reserve cannot be what answers here -- one consumer at a cap
    //       of two leaves three of the five images free -- which is exactly what the free-image
    //       check below asserts, and without it this check would pass equally for a daemon whose
    //       ring had simply run out. The refusal is also required to leave the consumer's
    //       existing claims alone, read back through the release probe.
    // CONTROL: `if (holder->buffers.size() >= kMaxHeldPerClient) return {};` deleted from
    //       WallpaperManager::withHandover (backend/daemon/wallpaper_manager.cpp), restoring the
    //       trim it replaced -> "FAIL: a consumer at its cap is refused rather than having its
    //       oldest claim taken back", printing the image it was handed. Reverted.
    // COVERAGE-ASSERT: the ids handed out before the cap can bite are required to name different
    //       images, so a run where the ring never rotated cannot reach the refusal and report it
    //       as established.
    std::vector<uint64_t> handedOut;
    bool everyRenderCommitted = true;
    for (int i = 0; i < kExpectedHeldPerClient; ++i) {
        if (!renderOnce(manager, kMonitorHoldNeverReleases, imagePath)) {
            everyRenderCommitted = false;
            break;
        }
        const uint64_t id = fetchFrame(manager, kMonitorHoldNeverReleases, alice);
        if (id == 0) {
            everyRenderCommitted = false;
            break;
        }
        handedOut.push_back(id);
    }
    const bool capFilled = everyRenderCommitted && handedOut.size() == static_cast<size_t>(kExpectedHeldPerClient) &&
                           allDistinct(handedOut);
    check(capFilled, "a consumer that never releases is handed one image per render up to its cap");
    if (!capFilled) {
        std::fprintf(stderr, "  it was handed [%s] of an expected %d different images; the cap checks below were "
                             "abandoned\n",
                     joinIds(handedOut).c_str(), kExpectedHeldPerClient);
        return;
    }

    check(renderOnce(manager, kMonitorHoldNeverReleases, imagePath),
          "the monitor still commits a render while that consumer sits at its cap");
    const Handover pastTheCap = fetchHandover(manager, kMonitorHoldNeverReleases, alice);
    check(pastTheCap.refused(), "a consumer at its cap is refused rather than having its oldest claim taken back");
    if (!pastTheCap.refused()) {
        std::fprintf(stderr, "  it was handed image %llu\n", static_cast<unsigned long long>(pastTheCap.bufferId));
    }

    // GOAL: that refusal is the CAP and not the ring running out. Without this the check above
    //       would read the same on a daemon with no cap at all whose images had merely been
    //       exhausted.
    // WHY-THIS-SHAPE: a second consumer, holding nothing, asks the same monitor and must be
    //       served. It shares the reserve with the first, so if the ring were the thing refusing
    //       it would be refused too.
    // NO CONTROL, and this is the reason rather than an omission: it is a gate on the check
    //       above, not a claim of its own. The only production change that makes it false is a
    //       ring too shallow for one consumer's cap plus the reserve, and that reddens the ring
    //       premise earlier in this phase, which abandons before this line. Measured twice:
    //       WallpaperManager::kFullConsumers lowered to 1 and, separately,
    //       WallpaperManager::kTargetDepth set to `1 + kMaxHeldPerClient` (both backend/daemon/
    //       wallpaper_manager.h) each produced "FAIL: two consumers hold four of the monitor's
    //       five images, so exactly one is free" and nothing below it. Both reverted.
    const Handover byAnother = fetchHandover(manager, kMonitorHoldNeverReleases, bob);
    check(byAnother.served(), "and the monitor still had images free, so it was the cap that refused and not the "
                              "ring");
    if (!byAnother.served()) {
        std::fprintf(stderr, "  a consumer holding nothing was refused too, so the ring had run out and the cap "
                             "was not what answered\n");
    }

    // The refusal took nothing back: both of the consumer's claims are still there to end.
    // Read through releaseBuffer, which is the daemon's own report of whether a claim existed.
    int stillHeld = 0;
    for (uint64_t id : handedOut) {
        std::vector<wallpaperx::FrameAvailable> announced;
        const std::vector<int> toRender = manager.releaseBuffer(alice, kMonitorHoldNeverReleases, id, &announced);
        if (!toRender.empty() || !announced.empty()) ++stillHeld;
    }
    check(stillHeld == kExpectedHeldPerClient, "and both of its images were still claimed after the refusal");
    if (stillHeld != kExpectedHeldPerClient) {
        std::fprintf(stderr, "  %d of %d claims were still there to release; the rest had been taken back\n",
                     stillHeld, kExpectedHeldPerClient);
    }
}

// One unsigned counter GetStatus reports for one monitor, read once.
//
// Read rather than waited for. Both counters this is used for are expected at an exact value the
// moment the read is made -- the monitor is quiesced by then -- and a wait for a value would have
// a success path reachable by a timeout on the way to a number that never changed.
//
// Every failure to read it is a distinct, named outcome -- a reply that never arrived, an entry
// the reply does not carry, a key the entry does not carry. A caller that only looked at `read`
// is still told which, because `error` is filled for every one of them, and `seen` is
// meaningless unless `read` is true.
struct CounterReading {
    bool read = false;
    uint32_t seen = 0;
    std::string error;
};

CounterReading readCounter(sdbus::IProxy& proxy, int32_t monitorId, const std::string& key) {
    const std::string monitor = std::to_string(monitorId);
    CounterReading out;
    const wallpaperx::testing::StatusReply status = wallpaperx::testing::getStatus(proxy);
    if (!status.ok()) {
        out.error = status.error();
        return out;
    }
    const nlohmann::json* entry = wallpaperx::testing::wallpaperStatus(status, monitor);
    if (entry == nullptr) {
        out.error = "GetStatus carries no entry for monitor " + monitor;
        return out;
    }
    if (!entry->contains(key) || !(*entry)[key].is_number_unsigned()) {
        out.error = "GetStatus's entry for monitor " + monitor + " carries no unsigned " + key +
                    " key, so that count cannot be read from the wire at all";
        return out;
    }
    out.seen = entry->value(key, uint32_t{0});
    out.read = true;
    return out;
}

void runBufferReleaseWireChecks(const wallpaperx::WallpaperManager::Config& config, const std::string& imagePath,
                                const Check& check) {
    PhaseDeviceGrade grade("the buffer-release wire phase", check);

    // Declared before the proxy whose dispatch thread writes to it, so it outlives that thread.
    wallpaperx::testing::FrameReadyLog signals;

    wallpaperx::WallpaperManager manager(config);
    CaptureGradeOnExit captureGrade{grade, manager};

    auto serviceConn = sdbus::createSessionBusConnection();
    DbusService service(*serviceConn, manager);
    serviceConn->enterEventLoopAsync();
    const sdbus::ServiceName daemonName{serviceConn->getUniqueName()};
    const sdbus::ObjectPath objectPath{DbusService::kObjectPath};

    auto listener = sdbus::createProxy(daemonName, objectPath);
    listener->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

    // Each on a connection of its own: that is what gives it a distinct unique bus name, and
    // a unique name is what the daemon records a hand-over against and what a departure
    // withdraws. Two proxies on one connection would be one consumer as far as the daemon can
    // tell, and this whole phase needs two.
    auto alice = sdbus::createProxy(daemonName, objectPath);
    auto bob = sdbus::createProxy(daemonName, objectPath);
    // The one that is refused, and it has to be a third connection: alice and bob are both at
    // their per-consumer cap by the time the reserve is reached, so asking either of them would
    // be refused by the cap whether or not the reserve existed.
    auto carol = sdbus::createProxy(daemonName, objectPath);

    const char* kWireGoals = "the buffer-release wire goals (the reserve's refusal on the wire, ReleaseBuffer over "
                             "the bus, the re-announcement a freed image produces, and a departing consumer's "
                             "claims)";

    // Brings the ring down to its last free image: one frame per image, each fetched by alice
    // and then bob until both are at their cap, and the last left for the producer. Each
    // SetWallpaper is followed by a wait for its exact generation, which is safe here because
    // nothing else drives this manager -- one monitor, one FIFO render thread, so generations
    // run 1..kExpectedRingDepth.
    uint64_t heldByAlice = 0;
    uint64_t generation = 0;
    for (int i = 0; i < kExpectedRingDepth; ++i) {
        wallpaperx::testing::setWallpaper(*alice, "static", imagePath, kMonitorReleaseWire);
        ++generation;
        if (!signals.wait(kMonitorReleaseWire, generation)) {
            // The first frame never arriving is what a host with no working Vulkan produces,
            // and the reason has just been printed; charging its missing Device to this phase
            // too would name the daemon for a fault of the machine.
            if (generation == 1) grade.expectNoDevice();
            wallpaperx::testing::notVerified(
                kWireGoals, "the daemon did not report frame " + std::to_string(generation) +
                                " on this host, so the ring was never brought down to its last free image and none "
                                "of the states these goals are about could be built");
            return;
        }
        // alice takes her cap, then bob takes his; the last frame is fetched by nobody, because
        // it is the image the producer must keep and the refusal below is what grades that.
        if (i >= 2 * kExpectedHeldPerClient) break;
        sdbus::IProxy& taker = i < kExpectedHeldPerClient ? *alice : *bob;
        const wallpaperx::testing::Frame frame = wallpaperx::testing::getFrameFd(taker, kMonitorReleaseWire);
        if (frame.bufferId == 0) {
            wallpaperx::testing::notVerified(kWireGoals,
                                             "a frame came back over the bus with no buffer id, so no hand-over "
                                             "could be recorded against a consumer");
            return;
        }
        if (i == 0) heldByAlice = frame.bufferId;
    }

    // GOAL: the reserve's refusal reaches the client as its own named error, not as "there is
    //       no frame". A consumer that cannot tell the two apart either stops asking or polls.
    // WHY-THIS-SHAPE: the error NAME is compared, because that is all a client can branch on --
    //       the message is prose and the plugin matches on the name alone. Asserting merely
    //       that the call failed would be satisfied by NoFrame, which is exactly the answer
    //       this error exists to be distinguishable from. Asked by a third connection holding
    //       nothing, so the per-consumer cap cannot be what answers.
    // CONTROL: the `if (exported.reason == NoFrameReason::NoFreeImage)` branch in
    //       DbusService::completeGetFrameFd (backend/daemon/dbus_service.cpp) deleted, so the
    //       refusal falls through to the generic no-frame reply -> "FAIL: a hand-over that
    //       would take the producer's last image is refused as NoFreeImage on the wire",
    //       printing "it answered 'io.wallpaperx.Daemon.Error.NoFrame'". Reverted.
    const std::string refusedName = wallpaperx::testing::errorNameFrom(
        [&carol] { wallpaperx::testing::getFrameFd(*carol, kMonitorReleaseWire); });
    check(refusedName == "io.wallpaperx.Daemon.Error.NoFreeImage",
          "a hand-over that would take the producer's last image is refused as NoFreeImage on the wire");
    if (refusedName != "io.wallpaperx.Daemon.Error.NoFreeImage") {
        std::fprintf(stderr, "  it answered '%s'\n", refusedName.c_str());
        wallpaperx::testing::notVerified(kWireGoals, "the third consumer was not refused, so the states the goals "
                                                     "below are about were never built");
        return;
    }

    // GOAL: a refusal announces nothing. False for a daemon that emitted FrameReady on the way
    //       out of a refused hand-over, which would make every refused consumer re-ask forever.
    // WHY-THIS-SHAPE: an exact count, which is safe only because the monitor is quiesced -- every
    //       render has been waited for by its exact generation and the refusal above has already
    //       returned, so no further signal can be in flight. Also the anti-vacuity barrier for
    //       the re-announcement check further down: with this count pinned at one per render, the
    //       repeats there cannot be leftovers from the fill.
    // CONTROL: `emitFrameReady(monitorId, m_manager.snapshot().monitors.at(monitorId)
    //       .generation);` added at the top of the NoFreeImage branch in
    //       DbusService::completeGetFrameFd (backend/daemon/dbus_service.cpp) -> "FAIL: the
    //       refused hand-over announced nothing -- one FrameReady per render, and no more", and
    //       the barrier before the departure below red for the same reason. Reverted.
    check(signals.countFor(kMonitorReleaseWire) == static_cast<size_t>(kExpectedRingDepth),
          "the refused hand-over announced nothing -- one FrameReady per render, and no more");

    // GOAL: deferred_renders is on the wire, and reads zero even in the state that used to
    //       produce a deferral. The key still has to be readable -- a client cannot otherwise
    //       tell a producer that is quietly drawing less often than asked from one keeping up --
    //       and it reads zero because the reserve is what now keeps an image free.
    // WHY-THIS-SHAPE: the key's PRESENCE and its value are asserted separately from each other,
    //       through a reader that names each way the read can fail. A bare
    //       `value("deferred_renders", 0) == 0` would pass just as happily for a daemon that
    //       stopped publishing the key at all, which is the whole failure the presence half is
    //       about. renderPending()'s deferral path is kept as a backstop, so a non-zero count
    //       here is a report that a hand-over got past the reserve.
    // CONTROL: two, both reverted. `entry["deferred_renders"] = monitor.deferredRenders;` in
    //       DbusService::getStatus (backend/daemon/dbus_service.cpp) guarded by
    //       `if (monitor.deferredRenders > 0)` -> "FAIL: GetStatus carries a deferred-render
    //       count for this monitor", printing the reader's own reason. And the `if
    //       (!client.empty())` admission block deleted from
    //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp) -> the
    //       refusal check above goes red first and abandons the phase, so this one is graded by
    //       the manager-tier check of the same name instead.
    {
        const CounterReading deferrals = readCounter(*alice, kMonitorReleaseWire, "deferred_renders");
        check(deferrals.read, "GetStatus carries a deferred-render count for this monitor");
        if (!deferrals.read) {
            std::fprintf(stderr, "  %s\n", deferrals.error.c_str());
        } else {
            check(deferrals.seen == 0, "and it is zero -- the reserve kept an image free rather than deferring");
            if (deferrals.seen != 0) {
                std::fprintf(stderr, "  it reads %u, so a hand-over got past the reserve\n", deferrals.seen);
            }
        }
    }

    // GOAL: refused_handovers is on the wire, and it is what tells an operator that a consumer
    //       has stopped giving images back. The count beside it cannot: deferred_renders reads
    //       zero for exactly this monitor, because the reserve refuses the hand-over instead of
    //       letting a render defer.
    // WHY-THIS-SHAPE: the key's PRESENCE and its value are asserted separately, through a reader
    //       that names each way the read can fail -- a bare `value("refused_handovers", 0) == 1`
    //       would pass for a daemon that had stopped publishing the key and been read as its
    //       default. The exact value is safe because the monitor is quiesced and exactly one
    //       hand-over on it has been refused. Deliberately read AFTER the deferred_renders check
    //       above, so the pair is graded in the same state: the two reading differently is the
    //       whole reason the second field exists.
    // CONTROL: `entry["refused_handovers"] = monitor.refusedHandovers;` deleted from
    //       DbusService::getStatus (backend/daemon/dbus_service.cpp) -> "FAIL: GetStatus carries
    //       a refused-hand-over count for this monitor", printing the reader's own reason.
    //       Reverted.
    // CONTROL, the value leg: `entry.refusedHandovers = state.refusedHandovers;` in
    //       WallpaperManager::snapshot (backend/daemon/wallpaper_manager.cpp) deleted, so the
    //       field is published but always zero -> "FAIL: and it counts the hand-over the reserve
    //       refused, which the deferral count cannot", printing 0. Reverted.
    {
        const CounterReading refusals = readCounter(*alice, kMonitorReleaseWire, "refused_handovers");
        check(refusals.read, "GetStatus carries a refused-hand-over count for this monitor");
        if (!refusals.read) {
            std::fprintf(stderr, "  %s\n", refusals.error.c_str());
        } else {
            check(refusals.seen == 1,
                  "and it counts the hand-over the reserve refused, which the deferral count cannot");
            if (refusals.seen != 1) {
                std::fprintf(stderr, "  it reads %u after exactly one refused hand-over on this monitor\n",
                             refusals.seen);
            }
        }
    }

    // GOAL: refusing a consumer does not stop the monitor rendering. The next request commits
    //       and is announced, so the producer keeps working while one consumer is going without.
    // WHY-THIS-SHAPE: waits for the exact next generation rather than "any newer frame". The
    //       monitor is quiesced -- one client driving it, one FIFO render thread -- so the
    //       generation this request lands on is determined, and an at-least wait would also be
    //       satisfied by a frame that had already arrived before the request was made.
    // CONTROL: the loop bound in OffscreenTargetManager::nextUnheld
    //       (renderer/vulkan/offscreen.cpp) narrowed from `first + count` to
    //       `first + count - 1`, so the producer will not reuse the slot it just published and
    //       has nowhere to draw once two images are held -> "FAIL: the monitor keeps rendering
    //       while a consumer is being refused", printing that no FrameReady for generation 4
    //       arrived. Chosen over forcing renderPending's deferral branch unconditionally: that
    //       stops the very first render, so this phase is never reached and the check would be
    //       unattested. Reverted.
    wallpaperx::testing::setWallpaper(*alice, "static", imagePath, kMonitorReleaseWire);
    const bool keptRendering = signals.wait(kMonitorReleaseWire, generation + 1);
    check(keptRendering, "the monitor keeps rendering while a consumer is being refused");
    if (!keptRendering) {
        std::fprintf(stderr, "  no FrameReady for generation %llu arrived within the timeout\n",
                     static_cast<unsigned long long>(generation + 1));
        return;
    }
    ++generation;

    // GOAL: ReleaseBuffer over the bus ends the caller's claim, and the daemon re-announces the
    //       monitor's committed frame so the refused consumer learns it may ask again. Which
    //       also establishes that the release is attributed to the CALLER: released under any
    //       name but the sender's, alice's call would match no claim and nothing would be
    //       announced.
    // WHY-THIS-SHAPE: a REPEAT of a generation already announced is what is waited for, so the
    //       count of that exact (monitor, generation) is asserted to be 1 first and then waited
    //       up to 2. The obvious form -- wait for the next generation -- describes the old
    //       contract, where a release discharged a deferred render; nothing is going to render
    //       here, and a plain wait() would be satisfied by the announcement the commit above
    //       already made.
    // CONTROL: `releaseBuffer(sender ? sender : "", monitorId, bufferId)` in the ReleaseBuffer
    //       handler in DbusService::registerVTable (backend/daemon/dbus_service.cpp) changed to
    //       `releaseBuffer("", monitorId, bufferId)` -> "FAIL: releasing an image re-announces
    //       the monitor's committed frame, so a refused consumer learns it may ask again".
    //       Reverted.
    // CONTROL: `announceAvailableFrames(toAnnounce);` deleted from DbusService::releaseBuffer
    //       (backend/daemon/dbus_service.cpp) -> the same line red. Reverted.
    check(signals.countOf(kMonitorReleaseWire, generation) == 1,
          "the committed frame has been announced exactly once so far");
    wallpaperx::testing::releaseBuffer(*alice, kMonitorReleaseWire, heldByAlice);
    const bool reannounced = signals.waitForRepeat(kMonitorReleaseWire, generation, 2);
    check(reannounced,
          "releasing an image re-announces the monitor's committed frame, so a refused consumer learns it may ask "
          "again");
    if (!reannounced) {
        std::fprintf(stderr, "  generation %llu was never announced a second time within the timeout\n",
                     static_cast<unsigned long long>(generation));
        return;
    }

    // GOAL: the refused consumer is actually served once an image has come free -- the refusal
    //       was temporary, not a consumer permanently locked out of a monitor.
    // WHY-THIS-SHAPE: asked of the SAME consumer that was refused above, on the same monitor,
    //       with nothing in between but the release. A fresh consumer would be served whatever
    //       the release did, since a client with no claims is never the one holding the ring.
    // CONTROL: `std::erase(holder->buffers, bufferId)` in WallpaperManager::dropHoldLocked
    //       (backend/daemon/wallpaper_manager.cpp), which releaseBuffer calls, changed to a
    //       `std::count(...)` that reports
    //       the claim without removing it -> "FAIL: the consumer that was refused is served
    //       once an image has come free", printing "it was answered
    //       'io.wallpaperx.Daemon.Error.NoFreeImage' instead", and the only red line in the
    //       run -- the re-announcement check above stayed green, which is the measurement
    //       showing announcing and freeing are two facts and not one. Reverted.
    const std::string servedName =
        wallpaperx::testing::errorNameFrom([&carol] { wallpaperx::testing::getFrameFd(*carol, kMonitorReleaseWire); });
    check(servedName.empty(), "the consumer that was refused is served once an image has come free");
    if (!servedName.empty()) {
        std::fprintf(stderr, "  it was answered '%s' instead\n", servedName.c_str());
    }

    // GOAL: the refusal count on the wire goes back to zero once this monitor serves a
    //       hand-over. A count that only ever rose would name every monitor that had ever
    //       refused anything, which is no diagnosis at all.
    // WHY-THIS-SHAPE: read after the served hand-over above and asserted at exactly zero. The
    //       same reading was 1 earlier in this phase, so the pair together says the field moves
    //       in both directions rather than merely being present.
    // CONTROL: `it->second.refusedHandovers = 0;` deleted from
    //       WallpaperManager::exportCurrentFrame (backend/daemon/wallpaper_manager.cpp) ->
    //       "FAIL: and a served hand-over puts the refusal count back to zero", printing 1.
    //       Reverted.
    if (servedName.empty()) {
        const CounterReading afterService = readCounter(*alice, kMonitorReleaseWire, "refused_handovers");
        check(afterService.read && afterService.seen == 0,
              "and a served hand-over puts the refusal count back to zero");
        if (!afterService.read) {
            std::fprintf(stderr, "  %s\n", afterService.error.c_str());
        } else if (afterService.seen != 0) {
            std::fprintf(stderr, "  it still reads %u after a hand-over was served\n", afterService.seen);
        }
    }

    // GOAL: a consumer leaving the bus gives its images back, and that is announced too --
    //       graded through DbusService's own NameOwnerChanged handler, which the manager tier
    //       cannot reach.
    // WHY-THIS-SHAPE: three steps, and the first two are what stop the last from being vacuous.
    //       A render is driven first so the monitor has a fresh committed frame with no claim on
    //       it; carol is then REFUSED, which is what establishes that alice's remaining claim is
    //       the thing holding the ring; only then does alice leave. Without the refusal step,
    //       carol's later success would be equally true of a daemon whose departure handler drops
    //       nothing at all. carol and not bob, because bob is at his per-consumer cap here and
    //       would be refused whatever the ring had free.
    // CONTROL: the `announce` argument of the reportFreedImageLocked call in
    //       WallpaperManager::forgetHeldBuffers (backend/daemon/wallpaper_manager.cpp) passed
    //       as `nullptr`, so a departure drops claims but publishes nothing -> "FAIL: a
    //       consumer leaving the bus re-announces the monitor's committed frame", printing
    //       "generation 5 was never announced a second time after the consumer left", while
    //       the ring check below stayed green. Reverted.
    // CONTROL, the other leg: that same `std::erase_if` replaced by a `std::count_if` that
    //       reports the monitor without dropping the claim -> "FAIL: the departed consumer's
    //       images are really back in the ring", printing the NoFreeImage the surviving
    //       consumer was answered, while the announcement check stayed green. Reverted.
    wallpaperx::testing::setWallpaper(*bob, "static", imagePath, kMonitorReleaseWire);
    if (!signals.wait(kMonitorReleaseWire, generation + 1)) {
        std::fprintf(stderr, "  no FrameReady for generation %llu arrived, so the departure checks had no "
                             "committed frame under them\n",
                     static_cast<unsigned long long>(generation + 1));
        wallpaperx::testing::notVerified(kWireGoals, "the monitor stopped rendering before the departure checks");
        return;
    }
    ++generation;
    const std::string beforeDeparture = wallpaperx::testing::errorNameFrom(
        [&carol] { wallpaperx::testing::getFrameFd(*carol, kMonitorReleaseWire); });
    // NO CONTROL, and this is the reason rather than an omission: it is a gate, not a claim.
    // Every mutation that would make it false -- the reserve removed, the hand-over cap widened,
    // the ring deepened -- also makes the FIRST refusal check above false, and that one abandons
    // the phase before this line is reached. What it protects against is a future edit to THIS
    // file that reorders the fill so the ring still has slack here; that lands in the else
    // branch, which abandons rather than grading the departure against a free image.
    check(beforeDeparture == "io.wallpaperx.Daemon.Error.NoFreeImage",
          "with the consumers holding images the monitor is down to its reserve again");
    if (beforeDeparture != "io.wallpaperx.Daemon.Error.NoFreeImage") {
        std::fprintf(stderr, "  it answered '%s', so the departure below had a free image under it and would "
                             "grade nothing\n",
                     beforeDeparture.c_str());
        wallpaperx::testing::notVerified(kWireGoals, "the ring was not down to its reserve before the departure");
        return;
    }
    check(signals.countOf(kMonitorReleaseWire, generation) == 1,
          "the newest frame has been announced exactly once before the departure");
    alice.reset();
    const bool departureAnnounced = signals.waitForRepeat(kMonitorReleaseWire, generation, 2);
    check(departureAnnounced, "a consumer leaving the bus re-announces the monitor's committed frame");
    if (!departureAnnounced) {
        std::fprintf(stderr, "  generation %llu was never announced a second time after the consumer left\n",
                     static_cast<unsigned long long>(generation));
    }
    const std::string afterDeparture = wallpaperx::testing::errorNameFrom(
        [&carol] { wallpaperx::testing::getFrameFd(*carol, kMonitorReleaseWire); });
    check(afterDeparture.empty(), "the departed consumer's images are really back in the ring");
    if (!afterDeparture.empty()) {
        std::fprintf(stderr, "  the surviving consumer was answered '%s' after the other one left\n",
                     afterDeparture.c_str());
    }

    // GOAL: ReleaseBuffer rejects a monitor id the daemon does not accept, on the reply, under
    //       the same error name every other malformed request gets.
    // CONTROL: `requireValidMonitorId(monitorId);` deleted from
    //       WallpaperManager::releaseBuffer (backend/daemon/wallpaper_manager.cpp) -> "FAIL:
    //       ReleaseBuffer refuses a negative monitor id as InvalidArgs", printing the empty
    //       name the call returned instead. Reverted.
    const std::string badId = wallpaperx::testing::errorNameFrom(
        [&bob] { wallpaperx::testing::releaseBuffer(*bob, -1, 1); });
    check(badId == "org.freedesktop.DBus.Error.InvalidArgs",
          "ReleaseBuffer refuses a negative monitor id as InvalidArgs");
    if (badId != "org.freedesktop.DBus.Error.InvalidArgs") {
        std::fprintf(stderr, "  it answered '%s'\n", badId.c_str());
    }
}

void runWorkerCoalescingChecks(const wallpaperx::WallpaperManager::Config& config,
                               const std::vector<std::string>& burstPaths, const Check& check) {
    // This phase builds a manager, and therefore a Device, of its own. The counter is
    // per-Device now, so the handle captured around the outer manager says nothing about
    // this one -- when the oracle was process-wide it covered both, and the migration
    // silently left this phase with no validation oracle at all. Declared before the
    // manager so it is destroyed after it, which is what makes teardown complaints
    // countable; graded in a destructor so an exception out of the phases below cannot
    // skip it.
    std::shared_ptr<const std::atomic<int>> counter;
    bool layerActive = false;
    struct GradeOnExit {
        const std::shared_ptr<const std::atomic<int>>& counter;
        const bool& layerActive;
        const Check& check;
        ~GradeOnExit() {
            // The same ladder ValidationGrade encodes, expressed once there rather than
            // open-coded here: waiver, then no-device, then no-validator, then the count.
            // Two copies of that policy in one suite is how the two tiers come to
            // disagree about what a clean run means -- and the waiver semantics changed
            // under both of them twice in as many rounds.
            // The waiver covers a missing device here as well as a missing validator.
            // ValidationGrade only waives the latter -- correctly, since a null counter
            // in a probe means something went wrong -- but this phase legitimately has no
            // device when the waived TSan tier builds its manager with validation off.
            // Saying so explicitly is what keeps the scoped waiver from narrowing a case
            // this call site had been relying on.
            int failures = 0;
            int ungraded = 0;
            {
                wallpaperx::testing::ValidationGrade grade(
                    "dbus_service_test (worker-coalescing phase)", failures);
                if (wallpaperx::testing::validationWaived()) grade.expectNoDevice();
                // Its own bucket: an absent layer PACKAGE says nothing about this daemon, so it
                // must not be charged to the check below. ValidationGrade still charges failures
                // when the package is present and the device simply did not activate it, which
                // is a defect here.
                grade.reportUngradedTo(ungraded);
                grade.adopt(counter, layerActive);
            }
            if (ungraded > 0) {
                std::fprintf(stderr,
                             "dbus_service_test: the worker-coalescing phase's validation grade could not be made "
                             "-- VK_LAYER_KHRONOS_validation is not loaded. The phase itself is still judged.\n");
            }
            // Named for what was actually judged. ValidationGrade has already printed the
            // specific verdict -- no device, no validator, or a nonzero count -- so a
            // single label asserting "zero validation warnings" would contradict two of
            // the three and point the operator at the GPU path for a missing package.
            check(failures == 0, "the worker-coalescing phase's own device passed its validation grade");
        }
    } gradeOnExit{counter, layerActive, check};

    wallpaperx::WallpaperManager manager(config);
    struct CaptureOnExit {
        const wallpaperx::WallpaperManager& manager;
        std::shared_ptr<const std::atomic<int>>& counter;
        bool& layerActive;
        ~CaptureOnExit() {
            counter = manager.validationCounter();
            layerActive = manager.validationLayerActive();
        }
    } captureOnExit{manager, counter, layerActive};
    std::atomic<int> frameReadySignals{0};

    // Released on every path, including an exception between here and the release below.
    // A worker left blocked on this gate would never observe stop(), so ~RenderWorker
    // would join a thread that never returns and the test would hang instead of failing.
    struct Gate {
        std::promise<void> promise;
        bool open = false;
        void release() {
            if (!open) {
                open = true;
                promise.set_value();
            }
        }
        ~Gate() { release(); }
    } gate;
    const std::shared_future<void> gateOpen = gate.promise.get_future().share();

    wallpaperx::RenderWorker worker(manager, [&](int, uint64_t) { ++frameReadySignals; });
    worker.postTask([gateOpen](wallpaperx::RenderWorker::TaskState) { gateOpen.wait(); });

    for (const std::string& path : burstPaths) {
        manager.requestWallpaper("static", path, kMonitorWorkerCoalesce);
        worker.post(kMonitorWorkerCoalesce);
    }

    // Queued behind the burst on the same FIFO, so once it runs every render the burst
    // produced has completed. stop() deliberately does not drain the queue, so it cannot
    // be used as the barrier here.
    //
    // GOAL: X.1 -- a wedged worker must fail this phase, not stall it.
    // WHY-THIS-SHAPE: the plain future::wait() this replaces had no deadline at all. A
    //       worker that never dequeued would hang here until ctest's own timeout fired,
    //       which reports the binary as a timeout and prints none of the FAIL lines the
    //       run had already accumulated -- destroying exactly the diagnostics the phase
    //       exists to produce. The bounded wait is only half the fix, and the half that
    //       does not work on its own: returning from here unwinds into ~RenderWorker,
    //       whose stop() calls m_thread.join() unconditionally -- on the very thread the
    //       timeout just proved is not progressing. A bounded wait followed by an
    //       unbounded join is still unbounded, so the timeout would hand the hang
    //       straight back one scope exit later. _Exit runs no destructors, so no join is
    //       attempted and ctest sees a failed process rather than a timeout. It also
    //       abandons the phase, which is wanted independently: every assertion below
    //       reads a snapshot that is only meaningful once the queue is empty.
    // CONTROL: the barrier task replaced by one that never returns
    //       (`worker.postTask([]{ for (;;) std::this_thread::sleep_for(std::chrono::hours(1)); })`
    //       posted ahead of it, wedging the FIFO the way a real dequeue regression
    //       would) -> "FAIL: the render worker drains the coalescing burst" followed by
    //       the "ending the run here" line, process gone at 30.5 s. The _Exit is load
    //       bearing and was measured to be: with `return` in its place, the same
    //       mutation was still running when a 75 s hard kill fired (SIGKILL, 137) --
    //       which is the state this shape exists to rule out. An earlier CONTROL here
    //       leaked the promise instead; that never reached the join, so it could not
    //       have observed either behaviour and its no-hang claim was unfounded. Both
    //       mutations reverted. An intermediate revision of this comment referred to a
    //       stderr-capture library and a drain call that had to run before the _Exit;
    //       both were deleted when the validation counter moved onto Device, so stderr
    //       is unbuffered and straight to the terminal again and the FAIL line above
    //       needs nothing extra to survive.
    const auto drain = [&worker](const char* what, const Check& c) {
        std::promise<void> done;
        std::future<void> future = done.get_future();
        // Moved into the task: a promise left on this stack would be destroyed while
        // the worker still held a reference to it if the wait below timed out.
        worker.postTask([done = std::move(done)](wallpaperx::RenderWorker::TaskState) mutable { done.set_value(); });
        if (future.wait_for(wallpaperx::testing::kSignalTimeout) == std::future_status::ready) return;
        c(false, what);
        std::fprintf(stderr,
                     "dbus_service_test: the render worker did not progress within the signal "
                     "timeout; ending the run here rather than joining a wedged thread\n");
        // Mandatory before _Exit: stdout is fully buffered under ctest, so every
        // progress and WARNING line written so far is still in the buffer _Exit
        // discards. stderr needs no such care -- it is unbuffered and goes straight to
        // the terminal, which is what makes the FAIL line above survive.
        std::fflush(nullptr);
        std::_Exit(1);
    };

    gate.release();
    drain("the render worker drains the coalescing burst", check);

    const wallpaperx::WallpaperManager::Snapshot snapshot = manager.snapshot();
    const auto entry = snapshot.monitors.find(kMonitorWorkerCoalesce);
    const bool present = entry != snapshot.monitors.end();
    check(present, "the monitor the coalescing burst targeted appears in the manager's snapshot");
    if (!present) return;

    check(entry->second.generation == 1,
          "five requests posted while the render thread is held collapse into exactly one render");
    check(frameReadySignals.load() == 1, "exactly one FrameReady callback fires for the collapsed burst");
    check(entry->second.path == burstPaths.back(), "the one render that happens serves the LAST request of the burst");

    if (entry->second.generation != 1) {
        std::fprintf(stderr, "  generation is %llu after a burst of %d, expected 1\n",
                     static_cast<unsigned long long>(entry->second.generation), kBurstSize);
    }

    // GOAL: S6.25 -- a render pass is idempotent per *request*: one SetWallpaper never
    //       produces two renders, two generations or two FrameReady signals, however
    //       many times the monitor is queued.
    // WHY-THIS-SHAPE: the burst above does not test this and cannot. Two mechanisms
    //       collapse a burst, and with the gate held only one of them is in play:
    //       RenderWorker::post() refuses to queue a monitor already in m_queuedRenders,
    //       so five posts become one queue entry and WallpaperManager::servicedSeq is
    //       never consulted. What servicedSeq guards is the *other* shape -- a monitor
    //       posted again after it was already dequeued and serviced -- which only
    //       happens with no new request behind it. Posting without a preceding
    //       requestWallpaper is the only way to produce that state deliberately, and it
    //       is reachable from here because this phase drives the worker directly; over
    //       the bus every post has a request in front of it.
    // CONTROL: the `servicedSeq == requestSeq` early return in
    //       WallpaperManager::renderPending() disabled. Before this block the entire
    //       suite stayed green with it disabled -- both tiers, both ICDs -- which is
    //       what made the mechanism untested. With it: "FAIL: re-posting a monitor
    //       whose request was already serviced renders nothing again" and "FAIL:
    //       a re-post emits no second FrameReady", generation 2 against 1. Reverted.
    worker.post(kMonitorWorkerCoalesce);
    drain("the render worker drains the idempotence re-post", check);

    const wallpaperx::WallpaperManager::Snapshot after = manager.snapshot();
    const auto repeated = after.monitors.find(kMonitorWorkerCoalesce);
    const bool stillPresent = repeated != after.monitors.end();
    check(stillPresent, "the re-posted monitor is still in the manager's snapshot");
    if (!stillPresent) return;

    check(repeated->second.generation == 1,
          "re-posting a monitor whose request was already serviced renders nothing again");
    check(frameReadySignals.load() == 1, "a re-post emits no second FrameReady");
    if (repeated->second.generation != 1) {
        std::fprintf(stderr, "  generation went from 1 to %llu on a re-post with no new request\n",
                     static_cast<unsigned long long>(repeated->second.generation));
    }

    runWorkerTaskAnswerChecks(manager, check);
}

// --------------------------------------------------------------------------- //
// Coalescing over the bus: when requests for one monitor arrive faster than the GPU can
// serve them, the daemon must render the LAST one.
//
// This exists because the claim was previously asserted on evidence that could not
// carry it. Five requests were fired at one monitor, the generation counter read 1, and
// that was taken as proof. All five named the *same image* -- so a daemon that served
// the first request and discarded the rest produces an identical observation, as does
// one that served the last. The counter distinguishes "how many renders committed",
// which was never the question; the question is "which request's pixels won".
//
// Two independent answers below. The outcome checks decide it from whether a frame
// exists at all, which needs no access to the frame's contents and therefore runs on
// every ICD. The content check decides it from the pixels, which needs a CPU-mappable
// export and therefore runs only on the DMA_BUF + LINEAR path.
// --------------------------------------------------------------------------- //
void runCoalescingChecks(sdbus::IProxy& proxy, FrameReadyLog& signals, const std::string& imagePath,
                         const std::string& corruptPath, const std::vector<std::string>& burstPaths,
                         const Check& check) {
    // The burst monitors are drained by the time the barrier monitor's frame lands: the
    // daemon does all GPU work on one FIFO thread, and every burst request has already
    // been recorded and posted by the time SetWallpaper returns to this thread, so the
    // barrier request is unconditionally behind them in the queue. No sleep, no flake.
    int barrierGeneration = 0;
    const auto barrier = [&](const char* what) {
        ++barrierGeneration;
        setWallpaper(proxy, "static", imagePath, kMonitorCoalesceBarrier);
        check(signals.wait(kMonitorCoalesceBarrier, static_cast<uint64_t>(barrierGeneration)), what);
    };

    // --- Outcome, on every ICD: a burst ending in a request that cannot render. ---
    //
    // If the last request wins, the monitor ends with no frame and a recorded error. If
    // the first request won instead, it ends holding a perfectly good frame. Nothing
    // about the *number* of renders is asserted -- an intermediate valid request may or
    // may not commit and emit its own FrameReady depending on how fast the worker
    // drains, and asserting on that would be asserting on the scheduler.
    for (int i = 0; i < kBurstSize - 1; ++i) {
        setWallpaper(proxy, "static", imagePath, kMonitorBurstFailLast);
    }
    setWallpaper(proxy, "static", corruptPath, kMonitorBurstFailLast);
    barrier("the barrier frame arrives after a burst ending in an unrenderable request");

    // RenderFailed rather than NoFrame is itself part of the claim: it is only reachable
    // when a render actually ran and failed, so it rules out "the burst was dropped
    // entirely" in a way a bare NoFrame cannot.
    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorBurstFailLast); }) ==
              "io.wallpaperx.Daemon.Error.RenderFailed",
          "a burst ending in an unrenderable request leaves no frame -- the last request is served, not the first");
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* monitor = wallpaperStatus(status, std::to_string(kMonitorBurstFailLast));
        const bool reported = monitor != nullptr && monitor->value("path", std::string()) == corruptPath &&
                              !monitor->value("has_frame", true) && monitor->contains("last_error");
        check(reported, "GetStatus attributes the burst's outcome to the last request in it");
    }

    // --- Outcome, the mirror image: a burst ending in a request that *can* render. ---
    //
    // Runs the same shape in reverse so neither direction can pass for the wrong reason.
    // Without it, a daemon that simply dropped every request after the first would fail
    // the check above and pass nothing else -- but so would one that dropped every
    // request, full stop, which is a different bug with a different fix.
    for (int i = 0; i < kBurstSize - 1; ++i) {
        setWallpaper(proxy, "static", corruptPath, kMonitorBurstRenderLast);
    }
    setWallpaper(proxy, "static", imagePath, kMonitorBurstRenderLast);
    barrier("the barrier frame arrives after a burst ending in a renderable request");

    check(errorNameFrom([&] { getFrameFd(proxy, kMonitorBurstRenderLast); }).empty(),
          "a burst ending in a renderable request leaves a frame, even after four failures");
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* monitor = wallpaperStatus(status, std::to_string(kMonitorBurstRenderLast));
        const bool served = monitor != nullptr && monitor->value("path", std::string()) == imagePath &&
                            monitor->value("has_frame", false);
        check(served, "the burst's last request is the one reflected in GetStatus");
    }

    // --- Error recovery, deliberately NOT a burst. ---
    //
    // Whether a successful render clears a previous failure's last_error cannot be asked
    // of a burst at all. Coalescing means the four corrupt requests above are never
    // rendered -- the worker reads the latest request when the monitor is dequeued, so
    // exactly one render runs and it is the valid one. last_error is therefore never set
    // for that monitor, and asserting its absence afterwards holds no matter what the
    // clearing code does. The check used to live there, passing for a monitor that had
    // never failed.
    //
    // Sequencing the two requests, each behind its own barrier, is what makes the
    // corrupt render actually execute and record an error for the next one to clear.
    setWallpaper(proxy, "static", corruptPath, kMonitorErrorRecovery);
    barrier("the barrier frame arrives after a standalone failing request");
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* monitor = wallpaperStatus(status, std::to_string(kMonitorErrorRecovery));
        const bool failed = monitor != nullptr && !monitor->value("has_frame", true) &&
                            !monitor->value("last_error", std::string()).empty();
        // Without this the clearing check below is unfalsifiable again, one step further
        // out: nothing else here proves the error was ever recorded in the first place.
        check(failed, "a standalone failing render records last_error, so there is an error to clear");
    }

    setWallpaper(proxy, "static", imagePath, kMonitorErrorRecovery);
    check(signals.wait(kMonitorErrorRecovery, 1), "the recovering render emits FrameReady");
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* monitor = wallpaperStatus(status, std::to_string(kMonitorErrorRecovery));
        const bool cleared = monitor != nullptr && monitor->value("path", std::string()) == imagePath &&
                             monitor->value("has_frame", false) && !monitor->contains("last_error");
        // Pins the outcome, not one particular line. WallpaperManager clears lastError
        // twice -- once in requestWallpaper() when the request is recorded, once in
        // renderPending() when a render commits -- and either alone is sufficient here,
        // so removing just one does not make this fail. Removing both does (verified).
        // That redundancy is worth knowing about: a future edit that deletes one of them
        // will not be caught by this check, and should not be assumed safe because it is.
        check(cleared, "a successful render leaves no last_error behind from an earlier failed one");
    }

    // --- Content: which request's pixels actually reached the exported frame. ---
    for (const std::string& path : burstPaths) {
        setWallpaper(proxy, "static", path, kMonitorBurstContent);
    }
    barrier("the barrier frame arrives after a burst of five different images");

    const Frame frame = getFrameFd(proxy, kMonitorBurstContent);
    {
        const StatusReply status = getStatusOrExplain(proxy);
        const nlohmann::json* monitor = wallpaperStatus(status, std::to_string(kMonitorBurstContent));
        check(monitor != nullptr && monitor->value("path", std::string()) == burstPaths.back(),
              "GetStatus reports the last image of the burst as the monitor's wallpaper");
    }
    // Reported, not asserted. How many renders a burst produces over the bus depends on
    // how many complete before the next request lands, which is the scheduler's decision
    // and not the daemon's -- runWorkerCoalescingChecks asserts the collapse exactly, by
    // holding the render thread so there is no window to race with. Printed as both
    // numbers rather than their difference: generation is uint64_t and kBurstSize is int,
    // so subtracting produced 18446744073709551615 in precisely the broken case the line
    // was there to explain.
    std::printf("dbus_service_test: %d rapid requests over the bus produced generation %llu\n", kBurstSize,
                static_cast<unsigned long long>(frame.generation));

    // GOAL: S6.24's content half -- that the *pixels* of the exported frame are the last
    //       burst request's, not the first's -- is either decided or reported as
    //       undecided. It must never exit 0 having read no pixels.
    // WHY-THIS-SHAPE: notVerified() rather than a print, and scoped to the content half
    //       alone. The outcome checks above decide first-wins vs last-wins without
    //       touching the frame's bytes and run on every ICD, so they are not in question
    //       here; only the pixel comparison is. A check(false) would be wrong in the
    //       other direction -- an OPAQUE_FD device is a correct daemon on hardware this
    //       comparison cannot be performed on at all.
    // CONTROL: `frame.handleType != 1` forced true on the Lavapipe tier, which normally
    //       exports DMA_BUF/LINEAR and reaches the pixel loop. Observed: "NOT VERIFIED --
    //       S6.24's content half ... this device exports OPAQUE_FD", "1 goal(s) could not
    //       be exercised on this host", exit 77 where the unmutated run exits 0, and
    //       ctest reporting dbus_service_test_lavapipe as Skipped. Reverted.
    const char* kContentGoal = "S6.24's content half -- which request's pixels reached the exported frame "
                               "(its outcome half ran and held above)";
    if (frame.handleType != 1) {
        wallpaperx::testing::notVerified(
            kContentGoal,
            "this device exports OPAQUE_FD, whose bytes are importable only by another Vulkan/GL context on "
            "the same device");
        return;
    }
    if (frame.drmModifier != wallpaperx::testing::kDrmModifierLinear) {
        char why[160];
        std::snprintf(why, sizeof(why),
                      "modifier 0x%llx is tiled, so its rows are not CPU-addressable and a byte comparison "
                      "would fail a correct export",
                      static_cast<unsigned long long>(frame.drmModifier));
        wallpaperx::testing::notVerified(kContentGoal, why);
        return;
    }
    if (frame.planes.empty()) {
        check(false, "a LINEAR DMA_BUF frame carries a plane layout");
        return;
    }

    // The fd must really be at least memorySize bytes before it is mapped. MappedFrame
    // bounds-checks rows against memorySize -- the value the daemon *claims* -- not
    // against the object's real size, so a daemon publishing a too-large memorySize
    // yields a mapping whose tail has no backing pages and the pixel loop below takes a
    // SIGBUS. frame_export_probe gained exactly this gate; the equivalent check here did
    // not. The nearest existing assertion is a non-fatal check() on a different monitor,
    // which records a FAIL and then falls straight through into this mmap.
    const off_t fdSize = lseek(frame.fd.get(), 0, SEEK_END);
    if (fdSize < 0) {
        // "Could not determine the size" is not "the size is fine". Falling through here
        // is what the gate exists to prevent: mmap does not validate length against the
        // object, so a shorter object maps cleanly and the pixel loop below dies of
        // SIGBUS with no FAIL line at all. Losing the content check on a non-seekable
        // exporter is a stated gap; a crash is not.
        char why[256];
        std::snprintf(why, sizeof(why),
                      "lseek on the exported fd failed (%s), so its real size cannot be confirmed against the "
                      "published memory_size of %llu before mapping it",
                      std::strerror(errno), static_cast<unsigned long long>(frame.memorySize));
        wallpaperx::testing::notVerified(kContentGoal, why);
        return;
    }
    if (static_cast<uint64_t>(fdSize) < frame.memorySize) {
        check(false, "the exported fd is at least as large as the memory_size the daemon published");
        std::fprintf(stderr, "  fd is %lld bytes but memory_size is %llu -- not mapping it\n",
                     static_cast<long long>(fdSize), static_cast<unsigned long long>(frame.memorySize));
        return;
    }

    const wallpaperx::testing::MappedFrame mapping(frame.fd.get(), frame.memorySize);
    if (!mapping.valid()) {
        wallpaperx::testing::notVerified(kContentGoal, "mmap of the exported fd failed: " + mapping.error());
        return;
    }

    // The target is a solid fill of the last burst colour, in B8G8R8A8 order.
    const unsigned char* rgb = kBurstColors[kBurstColorCount - 1];
    const unsigned char expectedB = rgb[2];
    const unsigned char expectedG = rgb[1];
    const unsigned char expectedR = rgb[0];
    constexpr int kTolerance = 6;

    const uint64_t offset = frame.planes[0].get<0>();
    const uint64_t pitch = frame.planes[0].get<1>();
    size_t badPixels = 0;
    unsigned char firstBad[4] = {0, 0, 0, 0};
    for (uint32_t y = 0; y < kTargetHeight; ++y) {
        const unsigned char* row = mapping.row(offset, pitch, y, static_cast<size_t>(kTargetWidth) * 4);
        if (row == nullptr) {
            badPixels += kTargetWidth;
            continue;
        }
        for (uint32_t x = 0; x < kTargetWidth; ++x) {
            const unsigned char* px = row + static_cast<size_t>(x) * 4;
            if (std::abs(int(px[0]) - int(expectedB)) > kTolerance ||
                std::abs(int(px[1]) - int(expectedG)) > kTolerance ||
                std::abs(int(px[2]) - int(expectedR)) > kTolerance) {
                if (badPixels == 0) std::memcpy(firstBad, px, 4);
                ++badPixels;
            }
        }
    }

    check(badPixels == 0,
          "the exported frame's pixels are the LAST image of the burst, read back through its own fd");
    if (badPixels != 0) {
        std::fprintf(stderr,
                     "  %zu of %u pixels differ; expected B,G,R = %d,%d,%d, first mismatch was %d,%d,%d "
                     "(the burst's first colour is B,G,R = %d,%d,%d)\n",
                     badPixels, kTargetWidth * kTargetHeight, expectedB, expectedG, expectedR, firstBad[0],
                     firstBad[1], firstBad[2], kBurstColors[0][2], kBurstColors[0][1], kBurstColors[0][0]);
    } else {
        std::printf("dbus_service_test: exported frame is solid B,G,R = %d,%d,%d -- the burst's last image\n",
                    expectedB, expectedG, expectedR);
    }
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "dbus_service_test";

    // Wrapped in a Check rather than called directly because every phase below takes one:
    // the std::function is what lets a phase be handed the grader without this file's
    // reporting internals.
    const Check check = wallpaperx::testing::check;

    // Scoped so the manager, and with it the render thread, the Vulkan Device, the
    // offscreen targets and Memory's leaked-allocation check, are all destroyed before
    // the validation counter is read below. Destroying an image still owned by an
    // external queue family, or leaking device memory, only raises validation errors
    // during teardown -- sampling the counter while the manager is still alive silently
    // exempts the whole destruction half of the code under test.
    // Filled as the manager scope closes; see CaptureOnExit below. Declared out here
    // because they are graded after the try block, once everything is destroyed.
    std::shared_ptr<const std::atomic<int>> validationCounter;
    bool validationLayerWasActive = false;

    try {
        const std::string imagePath = writeTestImage();
        const std::string corruptPath =
            wallpaperx::testing::writeUndecodableFile(testAssetDir(), "dbus_test_corrupt.png");
        // A file that exists, is a regular file and is readable -- and nothing more.
        // Needed because SetWallpaper applies the readable-regular-file check to *every*
        // known type now (S6.38), so the old "/tmp/wallpaperx-test.mp4", which was never
        // created, is rejected on the reply and never reaches the recorded-but-
        // unrenderable state the video checks are about. The shared helper's name says
        // "undecodable" and that is beside the point here: this build has no video
        // renderer, so nothing ever opens the file and its contents are irrelevant.
        const std::string videoPath =
            wallpaperx::testing::writeUndecodableFile(testAssetDir(), "dbus_test_video.mp4");
        const std::vector<std::string> burstPaths = writeBurstImages();

        // Declared before everything it outlives. The signal callback below captures it,
        // and that callback runs on the proxy's dispatch thread -- so if the log were
        // declared after the proxy it would be destroyed first, and any FrameReady still
        // in flight during an unwinding stack would lock a destroyed mutex. Reverse
        // declaration order is what guarantees proxy-then-service-then-log teardown.
        FrameReadyLog signals;

        wallpaperx::WallpaperManager::Config config;
        config.width = kTargetWidth;
        config.height = kTargetHeight;
        // Validation on: it is the correctness oracle for the export path's barriers and
        // queue-family transfers, none of which this test can observe directly.
        config.enableValidation = !validationWaived();
        wallpaperx::WallpaperManager manager(config);
        // Captured as this scope unwinds, in a destructor so it runs on the exception
        // path too -- a throw out of the phases below must not silently leave the
        // validation oracle reading "0 warnings" from a null handle.
        struct CaptureOnExit {
            const wallpaperx::WallpaperManager& manager;
            std::shared_ptr<const std::atomic<int>>& counter;
            bool& layerActive;
            ~CaptureOnExit() {
                counter = manager.validationCounter();
                layerActive = manager.validationLayerActive();
            }
        } captureOnExit{manager, validationCounter, validationLayerWasActive};
        // Taken while the manager is alive and graded after this scope closes. The
        // layer's teardown complaints -- a resource still in flight, an object destroyed
        // twice -- are emitted from ~WallpaperManager and ~Device, which is exactly what
        // "the render, export and teardown path produced zero warnings" claims to cover.
        // Reading a live accessor before the scope ends would silently drop that half.

        // Service side: requests the name and serves on a background event loop.
        auto serviceConn = sdbus::createSessionBusConnection(sdbus::ServiceName{DbusService::kBusName});
        DbusService service(*serviceConn, manager);
        serviceConn->enterEventLoopAsync();

        // Client side: its own connection + loop, managed by the proxy.
        auto proxy =
            sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName}, sdbus::ObjectPath{DbusService::kObjectPath});

        // Subscribed before the first request, so no signal can be missed.
        proxy->uponSignal("FrameReady")
            .onInterface(DbusService::kInterface)
            .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

        runHostChecks(*proxy, imagePath, check);
        // Before the render phases, and on the same manager: naming a screen needs no GPU,
        // and running it first keeps it reachable on a host whose Vulkan stack cannot come
        // up -- the same reason runHostChecks is first.
        //
        // Guarded, unlike its neighbours: it runs ahead of every GPU phase, and one call in
        // it is graded on a third-party library's throwing behaviour (sd-bus refusing to
        // serialize a non-UTF-8 argument). An exception under any type its own catch does
        // not name would otherwise reach main's catch-all and cancel the render, export and
        // coalescing phases outright, so a dependency upgrade would delete the GPU coverage
        // of this binary while printing only a connector-name failure. Counted as a failure
        // so the run still cannot report green.
        try {
            runOutputNameChecks(*proxy, check);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: the connector-name phase aborted, later phases still ran: %s\n", e.what());
            ++wallpaperx::testing::g_failures;
        }
        runRenderChecks(*proxy, signals, imagePath, corruptPath, videoPath, check);
        runCoalescingChecks(*proxy, signals, imagePath, corruptPath, burstPaths, check);
        // Last two of the shared-manager phases, on purpose and in this order: an accepted
        // report rebuilds the images the set excludes, so anything after them would be reading
        // state it did not set, and the second re-reports a wider set than the first leaves.
        runImportModifierWireChecks(*proxy, signals, imagePath, check);
        runModifierWithdrawalRenderChecks(*proxy, signals, imagePath, check);

        // Its own manager, connection and service; no GPU work at all. Placed before the
        // worker-coalescing phase only for reading order -- it shares no state with
        // either neighbour.
        runUnservicedRequestChecks(config, imagePath, videoPath, check);

        // Neither of these renders anything, so neither builds a Device: they grade the
        // per-consumer bookkeeping behind SetImportModifiers, which is pure state. Placed
        // before the GPU-backed phase below so that a host whose Vulkan stack cannot come up
        // still answers for them.
        runImportModifierClientChecks(config, imagePath, check);
        runImportModifierClientWireChecks(config, check);
        runImportModifierAnnouncementChecks(config, imagePath, videoPath, check);

        // Its own manager and Device, both created and destroyed on this thread -- the
        // phase drives renderPending() directly, so there is no worker thread for the
        // Device to be pinned to.
        runImportModifierChecks(config, imagePath, videoPath, check);

        // Its own manager and Device, both on this thread: the phase drives renderPending()
        // directly, so there must be no worker for the Device to be pinned to.
        runBufferHoldChecks(config, imagePath, check);

        // Its own manager, connection, service and worker, because this half is driven
        // entirely over the bus. Placed after the phase above for reading order only -- it
        // shares no state with it.
        runBufferReleaseWireChecks(config, imagePath, check);

        // Its own manager and Device, scoped inside the call so both are torn down
        // before the validation warning count is read below.
        runWorkerCoalescingChecks(config, burstPaths, check);
    } catch (const std::exception& e) {
        // Without this the first unhandled sdbus::Error -- which is what a host with no
        // usable Vulkan ICD produces -- would escape main and abort the process, so
        // ctest would report an opaque SIGABRT instead of a named failure, and none of
        // the FAIL lines above would ever be printed.
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++wallpaperx::testing::g_failures;
    }

    // Asking for validation is not the same as getting it: Device degrades silently when
    // the layer package is absent, and the warning count then stays 0 no matter what the
    // GPU actually did. Without this check the suite's only GPU-side oracle reports green
    // on any machine without vulkan-validation-layers installed.
    if (validationWaived()) {
        std::printf(
            "dbus_service_test: WARNING -- running with WALLPAPERX_ALLOW_NO_VALIDATION=1. The GPU-side "
            "correctness oracle is DISABLED; this run proves only that the DBus and threading behaviour "
            "holds. Do not read a pass here as the suite being green.\n");
    } else if (!validationCounter) {
        // Distinct from "the layer is missing", and it used to be reported as that. The
        // manager builds its Device lazily on the render worker, so if every SetWallpaper
        // failed before ensureGpu committed there is no counter at all -- and grading a
        // null handle as 0 warnings passed the oracle on a run where no GPU work happened.
        std::fprintf(stderr,
                     "FAIL: no Vulkan device was ever created, so the validation oracle graded "
                     "nothing -- see the render failures above for the cause.\n");
        ++wallpaperx::testing::g_failures;
    } else if (!validationLayerWasActive) {
        // Two causes, opposite verdicts. The package missing from the host says nothing about
        // this daemon; the package present while the daemon's own Device failed to activate it
        // is a defect here, and reporting that as a host fact retires the GPU-side oracle
        // silently. Either way this is not a ctest skip: the rest of the run was judged.
        if (wallpaperx::testing::validationLayerAvailable()) {
            std::fprintf(stderr,
                         "FAIL: VK_LAYER_KHRONOS_validation is installed but the daemon's Device did not activate "
                         "it -- the zero-warning check below would have validated nothing.\n");
        } else {
            std::fprintf(stderr,
                         "FAIL: VK_LAYER_KHRONOS_validation is not installed on this host, so the zero-warning "
                         "check below would have passed without validating anything. Install the Vulkan "
                         "validation layers, or set WALLPAPERX_ALLOW_NO_VALIDATION=1 to accept the loss.\n");
        }
        ++wallpaperx::testing::g_failures;
    } else {
        const int warnings = validationCounter->load();
        check(warnings == 0, "the render, export and teardown path produced zero Vulkan validation warnings");
        if (warnings > 0) {
            std::fprintf(stderr, "  %d validation warning(s)\n", warnings);
        }
    }

    // Asked before the pass/skip summary, so a run that is both broken and incomplete
    // reports as broken rather than hiding behind ctest's "Skipped".
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
