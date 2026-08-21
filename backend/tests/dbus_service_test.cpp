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
#include <cstdint>
#include <future>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
using wallpaperx::testing::setWallpaper;
using wallpaperx::testing::StatusReply;
using wallpaperx::testing::validationWaived;
using wallpaperx::testing::wallpaperStatus;

// The pass / fail contract lives in tests/test_report.h, shared with the other five test
// binaries so the wording and the exit codes cannot drift apart.
//
// This file uses only two of its three channels. It has no notVerified() call and its four
// ctest entries in backend/CMakeLists.txt carry no SKIP_RETURN_CODE, so exit 77 would read
// to ctest as a failure: the skip channel has to be enabled there before it can be used
// here. reportNotVerifiedOrPassed() below therefore always takes its all-passed branch, and
// is called for the failure guard it applies rather than for a skip this file can produce.

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

std::string writeTestImage() {
    std::filesystem::create_directories(testAssetDir());
    const std::string path = testAssetDir() + "/dbus_test_blue.png";

    const int w = 8;
    const int h = 8;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        pixels[i * 4 + 0] = 0;
        pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 255;
        pixels[i * 4 + 3] = 255;
    }

    if (!stbi_write_png(path.c_str(), w, h, 4, pixels.data(), w * 4)) {
        throw std::runtime_error("dbus_service_test: failed to write test image");
    }
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

    // Skipped rather than asserted when the test runs as root, where mode 000 is still
    // readable: a root run would otherwise report a daemon defect for a property of the
    // caller's uid. Announced, because an unnoticed skip here is a silent loss of the
    // access(R_OK) half.
    const std::string unreadablePath = testAssetDir() + "/dbus_test_unreadable.png";
    {
        std::ofstream out(unreadablePath, std::ios::binary);
        out << "not readable";
    }
    std::error_code permEc;
    std::filesystem::permissions(unreadablePath, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, permEc);
    if (permEc || ::access(unreadablePath.c_str(), R_OK) == 0) {
        std::printf(
            "dbus_service_test: unreadable-file check skipped -- this process can still read a mode-000 file "
            "(running as root?), so its acceptance would say nothing about the daemon.\n");
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
                 {"out", "a(tt):planes"}}},
        // CONTROL: the second name in `.withInputParamNames("monitor_id", "output_name")` in
        //       DbusService::registerVTable (backend/daemon/dbus_service.cpp) changed to
        //       "output" -> "FAIL: wire signature of method SetMonitorOutput is unchanged",
        //       printing expected `s:output_name(in)` against actual `s:output(in)`.
        //       Reverted. Dropping the name entirely was tried first and is NOT a usable
        //       control: sd-bus refuses a vtable whose parameter-name count disagrees with
        //       the signature, so the daemon fails to register at all and the run reports an
        //       exception instead of this line.
        {"method", "SetMonitorOutput", ArgList{{"in", "i:monitor_id"}, {"in", "s:output_name"}}},
        {"method", "GetStatus", ArgList{{"out", "s:status"}}},
        // A signal's args carry no direction attribute, hence the empty first field.
        {"signal", "FrameReady", ArgList{{"", "i:monitor_id"}, {"", "t:frame_generation"}}},
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
        check(frame.syncFds.empty(), "sync_fds is empty (reserved slot)");
        check(frame.handleType == 0 || frame.handleType == 1, "handle_type is OPAQUE_FD (0) or DMA_BUF (1)");

        // memory_size must be the exact allocation size, not merely big enough:
        // glImportMemoryFdEXT and EGL both take it as *the* size of the imported object,
        // so a value that is too large fails the import and one that is too small is
        // read as a smaller buffer. A dma_buf reports its true size via SEEK_END, which
        // is an oracle independent of whatever the daemon chose to put on the wire.
        if (frame.handleType == 1) {
            const off_t seekSize = lseek(frame.fd.get(), 0, SEEK_END);
            check(seekSize > 0 && static_cast<uint64_t>(seekSize) == frame.memorySize,
                  "memory_size equals the dma_buf's actual size");
            if (seekSize > 0 && static_cast<uint64_t>(seekSize) != frame.memorySize) {
                std::fprintf(stderr, "  memory_size=%llu but the dma_buf is %lld bytes\n",
                             static_cast<unsigned long long>(frame.memorySize),
                             static_cast<long long>(seekSize));
            }
        }
        check(frame.memorySize >= static_cast<uint64_t>(kTargetWidth) * kTargetHeight * 4,
              "memory_size covers at least the frame's pixels");

        // The three layout fields are populated together or not at all. A half-populated
        // set is the failure that matters: an importer given a fourcc but no modifier,
        // or planes with no fourcc, builds a wrong image rather than failing.
        const bool populated = frame.drmModifier != wallpaperx::kDrmModifierInvalid;
        if (populated) {
            check(frame.handleType == 1, "layout metadata is only published on the DMA_BUF path");
            check(frame.drmFormat != 0, "a populated frame carries a DRM fourcc");
            check(!frame.planes.empty(), "a populated frame carries at least one plane");
            for (const auto& plane : frame.planes) {
                check(plane.get<1>() >= static_cast<uint64_t>(kTargetWidth) * 4,
                      "each plane's rowPitch covers a row of pixels");
            }
        } else {
            // The fourcc is channel order, not layout, and an opaque fd has one too -- see
            // Step 12. Only the layout must be absent.
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
            {
                wallpaperx::testing::ValidationGrade grade(
                    "dbus_service_test (worker-coalescing phase)", failures);
                if (wallpaperx::testing::validationWaived()) grade.expectNoDevice();
                grade.adopt(counter, layerActive);
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

    if (frame.handleType != 1) {
        std::printf(
            "dbus_service_test: content check skipped -- this device exports OPAQUE_FD, whose bytes are "
            "importable only by another Vulkan/GL context on the same device. The outcome checks above still "
            "ran, and they decide first-wins vs last-wins on their own.\n");
        return;
    }
    if (frame.drmModifier != wallpaperx::testing::kDrmModifierLinear) {
        std::printf(
            "dbus_service_test: content check skipped -- modifier 0x%llx is tiled, so its rows are not "
            "CPU-addressable and a byte comparison would fail a correct export.\n",
            static_cast<unsigned long long>(frame.drmModifier));
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
        std::printf(
            "dbus_service_test: content check skipped -- lseek on the exported fd failed (%s), so its real size "
            "cannot be confirmed against the published memory_size of %llu before mapping it.\n",
            std::strerror(errno), static_cast<unsigned long long>(frame.memorySize));
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
        std::printf("dbus_service_test: content check skipped -- mmap of the exported fd failed (%s).\n",
                    mapping.error().c_str());
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

        // Its own manager, connection and service; no GPU work at all. Placed before the
        // worker-coalescing phase only for reading order -- it shares no state with
        // either neighbour.
        runUnservicedRequestChecks(config, imagePath, videoPath, check);

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
        std::fprintf(stderr,
                     "FAIL: VK_LAYER_KHRONOS_validation was requested but is not loaded -- the "
                     "zero-warning check below would pass without validating anything. "
                     "Install the Vulkan validation layers.\n");
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
