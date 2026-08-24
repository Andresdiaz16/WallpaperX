// SPDX-License-Identifier: GPL-2.0-or-later

// The one thing nothing else in this suite does: a frame rendered by the REAL daemon, in its own
// process, imported into a GL context in THIS process, and read back as pixels.
//
// Why a third tier was needed rather than a phase in one of the two that exist:
//
//   frame_import_probe   exports and imports inside one process. The descriptor never crosses a
//                        process boundary, so nothing about the cross-process contract is
//                        exercised -- and it is the boundary that hid the missing
//                        GL_DEDICATED_MEMORY_OBJECT_EXT flag, which made every import succeed and
//                        sample blank while that probe stayed green.
//   daemon_system_test   crosses the boundary, but its pixel check reads the frame by mmap, which
//                        only a LINEAR dma_buf allows. On the machine this ships to the daemon
//                        exports OPAQUE_FD, so that check skips exactly on the path that ships.
//
// What is graded here, and it is deliberately the shipping configuration: NVIDIA, OPAQUE_FD,
// GL_EXT_memory_object_fd, through backend/kde/frame_import.cpp -- the same importer the Plasma
// plugin calls from updatePaintNode(). Everything else reports a named skip rather than a pass;
// a dma_buf daemon is covered by daemon_system_lavapipe and by frame_import_probe_lavapipe.

#include "frame_import.h"

#include "support/forked_child.h"
#include "support/test_image.h"
#include "tests/client/daemon_client.h"
#include "tests/client/scratch_dir.h"
#include "tests/gl_import_harness.h"
#include "tests/spawned_daemon.h"
#include "tests/test_report.h"

#include <vulkan/vulkan.h>

#include <sdbus-c++/sdbus-c++.h>

#include <fcntl.h>
#include <signal.h>
#include <linux/kcmp.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

using wallpaperx::DbusService;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::openFdCount;
using wallpaperx::testing::Clock;
using wallpaperx::testing::SpawnedDaemon;

constexpr const char* kGoals =
    "S12.1 and S12.3, cross-process half (a frame the daemon exported in another process imports "
    "in this one as the pixels it was told to render, and neither an accepted nor a refused frame "
    "leaks a descriptor)";

// Not monitor 0: the daemon's own defaults live there in other tiers, and a distinct id keeps
// this test's frame identifiable in the daemon's log.
constexpr int32_t kMonitor = 3;

// ---------------------------------------------------------------------------- //
// The fixture: an image whose four quadrants are different colours.
// ---------------------------------------------------------------------------- //
//
// A solid fill -- what daemon_system_test uses -- cannot fail for a transposed read, a wrong
// stride or a vertical flip, because every wrong answer is the same colour as the right one.
// Four distinct quadrants fail for all three. The colours are chosen so that the red/blue
// transposition the importer's swizzle exists to prevent moves each quadrant to a colour no
// other quadrant has.
struct Rgba {
    unsigned char r;
    unsigned char g;
    unsigned char b;
    unsigned char a;
};

constexpr Rgba kTopLeft{230, 20, 30, 64};
constexpr Rgba kTopRight{20, 220, 40, 64};
constexpr Rgba kBottomLeft{30, 40, 210, 64};
constexpr Rgba kBottomRight{240, 200, 20, 64};

constexpr std::array<Rgba, 4> kQuadrants{kTopLeft, kTopRight, kBottomLeft, kBottomRight};

constexpr bool differsInRgb(const Rgba& a, const Rgba& b) { return a.r != b.r || a.g != b.g || a.b != b.b; }
constexpr Rgba redBlueSwapped(const Rgba& c) { return Rgba{c.b, c.g, c.r, c.a}; }

// Looped rather than enumerated: six pairs written out by hand had two missing, and nothing tied
// the list to the colours, so retuning a quadrant did not force the pairs to be revisited.
constexpr bool allQuadrantsDistinct() {
    for (size_t i = 0; i < kQuadrants.size(); ++i)
        for (size_t j = i + 1; j < kQuadrants.size(); ++j)
            if (!differsInRgb(kQuadrants[i], kQuadrants[j])) return false;
    return true;
}

// A quadrant whose red and blue are exchanged must match no quadrant at all -- itself included,
// or the swizzle is invisible where it happens; any other, or a transposed frame reads as a
// correctly laid out one.
constexpr bool noSwapCollidesWithAnyQuadrant() {
    for (const Rgba& c : kQuadrants)
        for (const Rgba& other : kQuadrants)
            if (!differsInRgb(redBlueSwapped(c), other)) return false;
    return true;
}

// The quadrant colours must be distinct in EVERY channel pair that a layout error could
// exchange, or the check that compares them would pass for a frame that is laid out wrongly.
static_assert(allQuadrantsDistinct());
static_assert(noSwapCollidesWithAnyQuadrant());

const Rgba& quadrantColour(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const bool right = x >= width / 2;
    const bool bottom = y >= height / 2;
    if (!bottom) return right ? kTopRight : kTopLeft;
    return right ? kBottomRight : kBottomLeft;
}

// 16:9, so the daemon's 1920x1080 default target is an exact integer upscale of it and no
// aspect-fit policy can rotate or letterbox the quadrants.
std::string writeQuadrantPng(const std::string& path, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Rgba& colour = quadrantColour(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                                                static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            unsigned char* px = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            px[0] = colour.r;
            px[1] = colour.g;
            px[2] = colour.b;
            px[3] = colour.a;
        }
    }
    wallpaperx::testing::detail::writePngOrThrow(path, width, height, pixels);
    return path;
}


// ---------------------------------------------------------------------------- //
// Finding the GPU the daemon rendered on.
// ---------------------------------------------------------------------------- //

struct GpuUuid {
    bool found = false;
    std::array<uint8_t, 16> uuid{};
    std::string why;
};

// The UUID of the Vulkan physical device whose name is `wanted` -- which is what the GL context
// must be created on, because an opaque fd is importable only by the driver that produced it.
//
// Matched on the name the daemon itself published through GetStatus, rather than by re-deriving
// the daemon's device-selection policy here: two copies of that policy would agree until one
// changed, and the failure would look like a driver mismatch. An ambiguous name is reported
// rather than guessed.
GpuUuid vulkanUuidForDeviceNamed(const std::string& wanted) {
    GpuUuid out;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "wallpaperx-cross-process-import-test";
    appInfo.apiVersion = VK_API_VERSION_1_1;  // VkPhysicalDeviceIDProperties is core from 1.1

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        out.why = "this process could not create a Vulkan instance to ask which GPU that name is";
        return out;
    }

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        vkDestroyInstance(instance, nullptr);
        return out;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return out;
    }
    devices.resize(count);

    int matches = 0;
    std::string seen;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        vkGetPhysicalDeviceProperties2(device, &properties);

        if (!seen.empty()) seen += ", ";
        seen += properties.properties.deviceName;
        if (wanted != properties.properties.deviceName) continue;
        ++matches;
        out.uuid = wallpaperx::testing::vulkanDeviceUuid(device);
    }
    vkDestroyInstance(instance, nullptr);

    if (matches == 1) {
        out.found = true;
        return out;
    }
    out.why = matches == 0 ? "no Vulkan device in this process is called \"" + wanted + "\" (this process sees: " +
                                 (seen.empty() ? "none" : seen) + ")"
                           : "\"" + wanted + "\" names " + std::to_string(matches) +
                                 " Vulkan devices here, so which one the daemon rendered on is not decidable";
    return out;
}

// ---------------------------------------------------------------------------- //
// Importing, the way WallpaperXItem does.
// ---------------------------------------------------------------------------- //

// What one staged-to-fail import did with the descriptor it was given. Three states and not two:
// "was it refused" and "did it hand the descriptor back" are separate questions, and conflating
// them is what made an earlier version of the refusal phase unable to fail for its own defect.
// NeverAttempted is a fourth state because a harness that could not duplicate the descriptor
// established nothing: counting it as a refusal would grade the importer for this file's failure.
enum class Outcome { Imported, RefusedAndHandedBack, RefusedAndConsumed, NeverAttempted };

// Fills a Request from a GetFrameFd reply. The descriptor is DUPLICATED, exactly as
// WallpaperXItem::onFrameReply does with the one QDBusUnixFileDescriptor hands it: the reply
// object owns its own copy and closes it, and the importer's contract is about the copy it is
// given.
// Whether this kernel answers kcmp at all. FrameImport uses it as its only means of deciding who
// owns a refused frame's descriptor, and where it is missing (no CONFIG_KCMP) or blocked (a
// seccomp policy) the importer leaks one per refusal deliberately -- so the leak check below has
// to know, or it grades that decision as a defect.
bool kcmpUsable() {
    static const bool usable = [] {
        // A descriptor this process opened, not 0: stdin belongs to whoever launched the test,
        // and a closed one would answer EBADF and be read as "this kernel cannot do kcmp".
        const int probe = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (probe < 0) {
            harnessFailure("could not open /dev/null to ask whether this kernel answers kcmp");
            return false;
        }
        const long self = ::syscall(SYS_kcmp, ::getpid(), ::getpid(), KCMP_FILE, probe, probe);
        const int failure = errno;
        ::close(probe);
        // Only these two mean the kernel will not answer. Anything else is about the operands.
        return self >= 0 || (failure != ENOSYS && failure != EPERM);
    }();
    return usable;
}

wallpaperx::FrameImport::Request requestFromFrame(const wallpaperx::testing::Frame& frame) {
    wallpaperx::FrameImport::Request request;
    request.fd = ::dup(frame.fd.get());
    // Reported here rather than at each call site: two of the three forgot, and a descriptor this
    // file failed to duplicate is indistinguishable from one the importer refused to take.
    if (request.fd < 0) {
        harnessFailure(std::string("the frame's descriptor could not be duplicated: ") + std::strerror(errno));
    }
    request.width = frame.width;
    request.height = frame.height;
    request.memorySize = frame.memorySize;
    request.handleType = frame.handleType == 1 ? wallpaperx::FrameImport::HandleType::DmaBuf
                                               : wallpaperx::FrameImport::HandleType::OpaqueFd;
    request.drmFormat = frame.drmFormat;
    request.drmModifier = frame.drmModifier;
    for (const auto& plane : frame.planes) request.planes.emplace_back(plane.get<0>(), plane.get<1>());
    return request;
}

// ---------------------------------------------------------------------------- //
// The run.
// ---------------------------------------------------------------------------- //

bool runChecks() {
    const std::string imagePath =
        writeQuadrantPng(std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/cross_process_quadrants.png", 640, 360);

    wallpaperx::testing::ScratchDir configHome("xproc");
    if (!configHome.ok()) {
        notVerified(kGoals, "no scratch directory could be created for the daemon's config, so no daemon of this "
                            "test's own could be started");
        return true;
    }

    SpawnedDaemon daemon({configHome.env(), configHome.stateEnv()});
    if (!daemon.waitForOutput(std::string("serving ") + DbusService::kBusName, std::chrono::seconds(45))) {
        if (const char* fault = daemon.startupFault()) {
            harnessFailure(std::string("the daemon child never ran: ") + fault);
            return false;
        }
        // A daemon that died is the daemon failing, not the host refusing to run the test. Only a
        // child still running -- one that simply never printed the line -- can be a skip.
        int signo = 0;
        const SpawnedDaemon::Departure went = daemon.departure(signo);
        if (went == SpawnedDaemon::Departure::QuitOnItsOwn) {
            check(false, "the daemon started successfully enough to announce the bus name");
            std::fprintf(stderr, "  it exited on its own before announcing the bus name; its output above says why\n");
            return false;
        }
        if (went == SpawnedDaemon::Departure::KilledFromOutside) {
            notVerified(kGoals, "the daemon this test started was killed by signal " + std::to_string(signo) +
                                    " by something outside this test before it announced the bus name");
            return true;
        }
        notVerified(kGoals, "the daemon this test started never announced that it was serving the bus name, so "
                            "there was nothing to ask for a frame");
        return true;
    }

    // GOAL: the frame graded below came from the daemon THIS test started, not from a daemon the
    //       developer already had running. False if the well-known name is owned by anyone else.
    // WHY-THIS-SHAPE: asked of the bus, not inferred from the child having printed "serving".
    //       Both daemons print it, and the loser of a name race prints it never -- but this test
    //       is normally wrapped in dbus-run-session, where a stale daemon on the developer's real
    //       bus is invisible, so the cheap version would be right for the wrong reason and wrong
    //       the day someone runs the binary by hand. A mismatch is a skip and not a failure: it
    //       says this host could not give the test a daemon of its own.
    // CONTROL: a shell script standing in for WALLPAPERX_DAEMON_BINARY -- it prints the daemon's
    //       "serving io.wallpaperx.Daemon" line and then sleeps, so the wait above is satisfied
    //       by a process that never took the name -- with a real daemon started on the same bus
    //       first -> "NOT VERIFIED ... io.wallpaperx.Daemon is owned by pid 448568, not by the
    //       daemon this test started (pid 448610) -- refusing to measure someone else's daemon",
    //       exit 77. Observed 2026-08-22; the stand-in was reverted.
    uint32_t ownerPid = 0;
    try {
        auto bus = sdbus::createProxy(sdbus::ServiceName{"org.freedesktop.DBus"},
                                      sdbus::ObjectPath{"/org/freedesktop/DBus"});
        bus->callMethod("GetConnectionUnixProcessID")
            .onInterface("org.freedesktop.DBus")
            .withArguments(std::string(DbusService::kBusName))
            .storeResultsTo(ownerPid);
    } catch (const sdbus::Error& e) {
        notVerified(kGoals, std::string("the bus would not say which process owns ") + DbusService::kBusName + " (" +
                                e.getName() + "), so the daemon under test could not be identified");
        return true;
    }
    if (static_cast<pid_t>(ownerPid) != daemon.pid()) {
        notVerified(kGoals, std::string(DbusService::kBusName) + " is owned by pid " + std::to_string(ownerPid) +
                                ", not by the daemon this test started (pid " + std::to_string(daemon.pid()) +
                                ") -- refusing to measure someone else's daemon");
        return true;
    }

    wallpaperx::testing::FrameReadyLog signals;
    auto proxy = sdbus::createProxy(sdbus::ServiceName{DbusService::kBusName},
                                    sdbus::ObjectPath{DbusService::kObjectPath});
    proxy->uponSignal("FrameReady")
        .onInterface(DbusService::kInterface)
        .call([&signals](int32_t monitorId, uint64_t generation) { signals.record(monitorId, generation); });

    // Which GPU the daemon is on is the daemon's answer, not this process's guess.
    const wallpaperx::testing::StatusReply status = wallpaperx::testing::getStatus(*proxy);
    if (!status.ok()) {
        notVerified(kGoals, "the daemon would not report its status (" + status.error() +
                                "), so the GPU it renders on is unknown");
        return true;
    }
    const auto& gpu = status.json().value("gpu", nlohmann::json::object());
    if (!gpu.value("initialized", false)) {
        // The GPU comes up on the first SetWallpaper, so it is expected to be absent here; the
        // name is read again after the render below.
        std::printf("cross_process_import_test: the daemon has not brought its GPU up yet, which is normal before "
                    "the first SetWallpaper\n");
    }

    wallpaperx::testing::setWallpaper(*proxy, "static", imagePath, kMonitor);
    if (!signals.wait(kMonitor, 1)) {
        // Diagnosed, not skipped. SetWallpaper already returned, so the daemon accepted the
        // request; whether it then rendered is the behaviour under test, not a host capability.
        // Its own status says which: a daemon with no GPU could not have rendered anywhere, but
        // one that has a GPU and still produced nothing is a defect this entry must report.
        const wallpaperx::testing::StatusReply why = wallpaperx::testing::getStatus(*proxy);
        const bool gpuUp =
            why.ok() && why.json().value("gpu", nlohmann::json::object()).value("initialized", false);
        if (!gpuUp) {
            notVerified(kGoals, "the daemon never brought a GPU up, so it could not render a frame to import" +
                                    (why.ok() ? std::string() : std::string(" (and it would not report its status)")));
            return true;
        }
        check(false, "the daemon renders a frame for a wallpaper it accepted");
        std::fprintf(stderr, "  the daemon has a GPU but signalled no FrameReady for monitor %d; its status says %s\n",
                     kMonitor, why.json().dump().c_str());
        return false;
    }

    const wallpaperx::testing::StatusReply rendered = wallpaperx::testing::getStatus(*proxy);
    const std::string gpuName =
        rendered.ok() ? rendered.json().value("gpu", nlohmann::json::object()).value("name", std::string())
                      : std::string();
    if (gpuName.empty()) {
        notVerified(kGoals, "the daemon rendered a frame but named no GPU in its status, so no GL context could be "
                            "created on the device that produced it");
        return true;
    }

    const GpuUuid wanted = vulkanUuidForDeviceNamed(gpuName);
    if (!wanted.found) {
        notVerified(kGoals, "the daemon rendered on \"" + gpuName + "\" but " + wanted.why);
        return true;
    }

    wallpaperx::testing::HeadlessGl gl;
    std::string why;
    if (!gl.createMatching(wanted.uuid, why)) {
        notVerified(kGoals, "no GL context could be created on the GPU the daemon rendered on (\"" + gpuName +
                                "\") -- " + why);
        return true;
    }

    const wallpaperx::testing::Frame frame = wallpaperx::testing::getFrameFd(*proxy, kMonitor);
    std::printf("cross_process_import_test: daemon pid %d on \"%s\" -> %ux%u, memory_size=%llu, handle_type=%d, "
                "fourcc=0x%08x; importing on \"%s\"\n",
                static_cast<int>(daemon.pid()), gpuName.c_str(), frame.width, frame.height,
                static_cast<unsigned long long>(frame.memorySize), frame.handleType, frame.drmFormat,
                gl.renderer().c_str());

    if (frame.handleType != 0) {
        notVerified(kGoals, "the daemon exported handle_type=" + std::to_string(frame.handleType) +
                                "; this test grades the OPAQUE_FD path that ships on a real GPU, and the dma_buf "
                                "path is covered by daemon_system_lavapipe and frame_import_probe_lavapipe");
        return true;
    }
    if (!wallpaperx::FrameImport::supports(wallpaperx::FrameImport::HandleType::OpaqueFd, &why)) {
        notVerified(kGoals, "this GL context cannot import an opaque fd -- " + why);
        return true;
    }

    // GOAL: an fd that crossed a process boundary imports into a GL context that never allocated
    //       it. False if the import returns no texture.
    // WHY-THIS-SHAPE: graded as a FAILURE and not a skip. Every reason an import could
    //       legitimately be impossible here has already been ruled out above -- the GL device's
    //       UUID equals that of the Vulkan device the daemon named, the context advertises the
    //       extensions the path needs, and the handle type is the one this path is for. Treating
    //       a failure past this point as "could not be exercised" is how a broken importer
    //       reports itself as a green skip.
    // CONTROL: GL_HANDLE_TYPE_OPAQUE_FD_EXT replaced by GL_HANDLE_TYPE_OPAQUE_WIN32_EXT at the
    //       glImportMemoryFdEXT call in backend/kde/frame_import.cpp -> "FAIL: a frame exported
    //       by the daemon process imports into a GL context in this one", printing "on NVIDIA
    //       GeForce RTX 3080 Ti/PCIe/SSE2: glImportMemoryFdEXT rejected the exported memory".
    //       Observed 2026-08-22. Reverted.
    wallpaperx::FrameImport::Request request = requestFromFrame(frame);
    if (request.fd < 0) {
        return true;
    }
    const wallpaperx::FrameImport::Result imported = wallpaperx::FrameImport::import(request);
    const bool importedOk = imported.texture != 0;
    check(importedOk, "a frame exported by the daemon process imports into a GL context in this one");
    if (!importedOk) {
        std::fprintf(stderr, "  on %s: %s\n", gl.renderer().c_str(), imported.error.c_str());
        if (request.fd >= 0) close(request.fd);
        return true;
    }

    std::string readWhy;
    const std::vector<unsigned char> sampled =
        wallpaperx::testing::readSampled(imported.texture, frame.width, frame.height, readWhy);
    if (sampled.empty()) {
        // This harness's own machinery, not the importer's: reported as a skip so a broken
        // harness cannot print as a failure of the code under test.
        wallpaperx::FrameImport::release(imported);
        notVerified(kGoals, "the imported texture could not be sampled -- " + readWhy);
        return true;
    }

    // GOAL: what a compositor SAMPLES from the imported texture is the picture the daemon was
    //       told to draw -- right colours, right way up, right way round. False if the texture
    //       samples blank, if red and blue arrive transposed, or if the rows or columns are
    //       exchanged.
    // WHY-THIS-SHAPE: four differently-coloured quadrants compared per pixel, rather than the
    //       solid fill daemon_system_test uses. A solid fill cannot fail for a transposed read,
    //       a wrong stride or a vertical flip -- every wrong answer is the colour of the right
    //       one. Sampled through a shader rather than read out of the texture's own memory,
    //       because GL_TEXTURE_SWIZZLE_R/B applies to LOOKUPS only: a framebuffer read of the
    //       texture returns the same bytes whether the swizzle is set or not, so it cannot see
    //       the channel plumbing at all.
    //       Pixels within kQuadrantMargin of the two mid-lines are excluded because the daemon
    //       filters the source image up to the target size, so those are genuinely a blend of
    //       two quadrants. That band is the only place a blend can happen -- everywhere else all
    //       four filter taps hold the same colour, so the comparison is EXACT (kTolerance 0) and
    //       not a window widened until the numbers fit. Measured: with tolerance 0, all 2025856
    //       compared pixels match byte for byte.
    // CONTROL, the colour half: the GL_TEXTURE_SWIZZLE_R/B block in FrameImport::import in
    //       backend/kde/frame_import.cpp disabled (`if (false && swapRedAndBlue)`) -> "FAIL: the
    //       imported frame samples as the picture the daemon was told to render...", printing
    //       "2025856 of 2025856 compared pixels differ, first at (0,0): sampled 30,20,230,255;
    //       the daemon was told to render 230,20,30 there". Observed 2026-08-22. Reverted.
    // CONTROL, the orientation half: this file's fixture written upside down (the row index
    //       mirrored in writeQuadrantPng, so the daemon renders a picture the check does not
    //       expect, while the expectation is untouched) -> the same FAIL, "sampled 30,40,210" --
    //       the bottom-left quadrant's colour arriving at (0,0). Observed 2026-08-22. Reverted.
    // NOT A CONTROL, and worth recording: deleting the GL_DEDICATED_MEMORY_OBJECT_EXT parameter
    //       -- the historic defect that made an import succeed and sample blank -- does NOT red
    //       this check on this driver. Measured 2026-08-22: all checks still passed. So this
    //       check's power over the blank-texture defect is unproven here; what it does prove is
    //       that the pixels sampled are the ones the daemon rendered, by the two controls above.
    // COVERAGE-ASSERT: the number of pixels actually compared is asserted, per quadrant. Without
    //       it a margin or a bounds slip could leave the sweep comparing a handful of pixels --
    //       or none -- and the check would go quiet rather than red. Measured: 2025856 of the
    //       frame's 2073600 pixels are compared, 97.7%.
    // CONTROL (the coverage assertion): kQuadrantMargin raised to 600, which swallows the whole
    //       frame -> "FAIL: the colour comparison above swept every quadrant of the frame, so it
    //       cannot pass by comparing nothing", printing "compared 0 pixels: 0 / 0 / 0 / 0 per
    //       quadrant, wanted at least 259200 each", and the colour check red beside it rather
    //       than passing on an empty sweep. Observed 2026-08-22. Reverted.
    constexpr uint32_t kQuadrantMargin = 8;
    constexpr int kTolerance = 0;
    size_t compared = 0;
    std::array<size_t, 4> perQuadrant{};
    size_t differing = 0;
    size_t firstBadIndex = sampled.size();
    for (uint32_t y = 0; y < frame.height; ++y) {
        const uint32_t dy = y < frame.height / 2 ? frame.height / 2 - y : y - frame.height / 2 + 1;
        if (dy <= kQuadrantMargin) continue;
        for (uint32_t x = 0; x < frame.width; ++x) {
            const uint32_t dx = x < frame.width / 2 ? frame.width / 2 - x : x - frame.width / 2 + 1;
            if (dx <= kQuadrantMargin) continue;
            const size_t at = (static_cast<size_t>(y) * frame.width + x) * 4;
            if (at + 3 >= sampled.size()) continue;
            const Rgba& want = quadrantColour(x, y, frame.width, frame.height);
            ++compared;
            perQuadrant[(y >= frame.height / 2 ? 2u : 0u) + (x >= frame.width / 2 ? 1u : 0u)] += 1;
            const bool ok = std::abs(int(sampled[at + 0]) - int(want.r)) <= kTolerance &&
                            std::abs(int(sampled[at + 1]) - int(want.g)) <= kTolerance &&
                            std::abs(int(sampled[at + 2]) - int(want.b)) <= kTolerance;
            if (!ok) {
                if (differing == 0) firstBadIndex = at;
                ++differing;
            }
        }
    }
    const bool coloursMatch = compared > 0 && differing == 0;
    check(coloursMatch, "the imported frame samples as the picture the daemon was told to render, in the right "
                        "colours and the right orientation");
    if (!coloursMatch && firstBadIndex + 3 < sampled.size()) {
        const size_t pixel = firstBadIndex / 4;
        const uint32_t x = static_cast<uint32_t>(pixel % frame.width);
        const uint32_t y = static_cast<uint32_t>(pixel / frame.width);
        const Rgba& want = quadrantColour(x, y, frame.width, frame.height);
        std::fprintf(stderr,
                     "  %zu of %zu compared pixels differ, first at (%u,%u): sampled %u,%u,%u,%u; the daemon was "
                     "told to render %u,%u,%u there -- on %s\n",
                     differing, compared, x, y, sampled[firstBadIndex], sampled[firstBadIndex + 1],
                     sampled[firstBadIndex + 2], sampled[firstBadIndex + 3], want.r, want.g, want.b,
                     gl.renderer().c_str());
    }

    const size_t quadrantFloor = (static_cast<size_t>(frame.width) * frame.height) / 8;
    const bool sweptEverything = perQuadrant[0] >= quadrantFloor && perQuadrant[1] >= quadrantFloor &&
                                 perQuadrant[2] >= quadrantFloor && perQuadrant[3] >= quadrantFloor;
    check(sweptEverything, "the colour comparison above swept every quadrant of the frame, so it cannot pass by "
                           "comparing nothing");
    if (!sweptEverything) {
        std::fprintf(stderr, "  compared %zu pixels: %zu / %zu / %zu / %zu per quadrant, wanted at least %zu each\n",
                     compared, perQuadrant[0], perQuadrant[1], perQuadrant[2], perQuadrant[3], quadrantFloor);
    }

    wallpaperx::FrameImport::release(imported);

    // ------------------------------------------------------------------ //
    // Descriptor accounting: the accepted path.
    // ------------------------------------------------------------------ //
    //
    // GOAL: S12.3 -- fetching and importing frame after frame does not accumulate descriptors.
    //       False if any of GetFrameFd's reply, the duplicate handed to the driver, or the
    //       importer's own copy is left open per frame; a wallpaper that changes every few
    //       seconds turns one leaked descriptor per frame into an EMFILE by evening.
    // WHY-THIS-SHAPE: growth is measured from AFTER the first cycle rather than from before it.
    //       A driver may open descriptors of its own the first time it imports and hold them for
    //       the life of the context -- measured, NVIDIA does -- and a single import cannot tell
    //       that apart from a leak. Repetition can.
    // CONTROL: the `::close(request.fd)` on the successful path of FrameImport::import in
    //       backend/kde/frame_import.cpp deleted, leaving `request.fd = -1` so the caller cannot
    //       close it either -> "FAIL: fetching and importing frame after frame does not
    //       accumulate file descriptors", printing "37 open fds after the first cycle, 42 after
    //       6" -- one per frame. Observed 2026-08-22. Reverted.
    // CONTROL, the coverage half: this file's later cycles made to import a wrong memory size ->
    //       "FAIL: every repeated fetch-and-import cycle succeeded...", printing "3 of 6 cycles
    //       imported; the first failure was: glImportMemoryFdEXT rejected the exported memory",
    //       and the count comparison correctly withheld. Observed 2026-08-22. Reverted.
    // COVERAGE-ASSERT: the number of cycles that actually imported is asserted, and a run that
    //       could not read /proc/self/fd reports itself unexercised rather than passing.
    //       openFdCount() answers 0 for a directory it cannot read, and 0 <= 0 is a passing
    //       comparison of nothing. The procfs attribution is reached only once every cycle
    //       imported: an early break leaves the same 0 behind, and would otherwise be blamed
    //       on the host.
    // CONTROL (that guard): the directory openFdCount() walks in tests/client/daemon_client.h
    //       pointed at a path that does not exist -> both descriptor phases reported NOT
    //       VERIFIED naming /proc/self/fd, and the binary exited 77 instead of 0. Observed
    //       2026-08-22. Reverted.
    constexpr int kCycles = 6;
    int importedCycles = 0;
    bool dupFailed = false;
    size_t afterFirstCycle = 0;
    std::string cycleError;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        {
            const wallpaperx::testing::Frame again = wallpaperx::testing::getFrameFd(*proxy, kMonitor);
            wallpaperx::FrameImport::Request request2 = requestFromFrame(again);
            // Breaks the loop, never returns: a return here would also delete the refusal phase
            // below and the daemon's ordered shutdown.
            if (request2.fd < 0) {
                dupFailed = true;
                break;
            }
            const wallpaperx::FrameImport::Result round = wallpaperx::FrameImport::import(request2);
            if (round.texture == 0) {
                if (request2.fd >= 0) close(request2.fd);
                cycleError = round.error;
                break;
            }
            wallpaperx::FrameImport::release(round);
            if (request2.fd >= 0) close(request2.fd);
            ++importedCycles;
        }
        // Counted with the reply object destroyed, so the descriptor sdbus holds for the reply is
        // not read as growth.
        if (cycle == 0) afterFirstCycle = openFdCount();
    }
    const size_t afterAllCycles = openFdCount();
    // Withheld when this file's own dup() failed: the cycles did not fall short because the
    // importer refused them, and requestFromFrame has already reported the real cause.
    if (!dupFailed) {
        check(importedCycles == kCycles,
              "every repeated fetch-and-import cycle succeeded, so the descriptor count below was measured over a "
              "known number of real imports");
        if (importedCycles != kCycles) {
            std::fprintf(stderr, "  %d of %d cycles imported; the first failure was: %s\n", importedCycles, kCycles,
                         cycleError.empty() ? "(none recorded)" : cycleError.c_str());
        }
    }
    if (dupFailed || importedCycles != kCycles) {
        // Taken first, because a run that broke out of the loop early never reached the first
        // count and leaves it 0 -- the same value openFdCount() returns for a directory it
        // cannot read. Attributing that to procfs names the wrong cause; the check above is
        // already red with the real one.
    } else if (afterFirstCycle == 0 || afterAllCycles == 0) {
        // A host whose /proc/self/fd cannot be read has not exercised this, and must not report
        // that it has: openFdCount() answers 0 there, and 0 <= 0 is a passing comparison of
        // nothing. Attributed as unexercised rather than as a failure -- it is a property of the
        // host, not of the importer.
        notVerified(kGoals, "/proc/self/fd could not be read while importing frame after frame, so no descriptor "
                            "was counted");
    } else {
        // Bounded by a constant, not by zero. The property is that growth does not scale with the
        // number of frames; a driver is free to open a fixed number of descriptors of its own
        // lazily, and not necessarily all on the first import. A per-frame leak clears this bound
        // as soon as the cycle count exceeds it, which is what the count check above guarantees.
        constexpr size_t kLazyDriverAllowance = 2;
        const size_t grew = afterAllCycles > afterFirstCycle ? afterAllCycles - afterFirstCycle : 0;
        const bool steady = grew <= kLazyDriverAllowance;
        check(steady, "fetching and importing frame after frame does not accumulate file descriptors");
        if (!steady) {
            std::fprintf(stderr, "  %zu open fds after the first cycle, %zu after %d -- grew by %zu over %d further "
                                 "imports, which is more than a fixed lazy allocation can explain\n",
                         afterFirstCycle, afterAllCycles, kCycles, grew, kCycles - 1);
        }
    }

    // ------------------------------------------------------------------ //
    // Descriptor accounting: the refused path.
    // ------------------------------------------------------------------ //
    //
    // GOAL: S12.3 -- a frame the importer REFUSES leaks no descriptor either. False if the fd
    //       handed to the driver is neither taken by it nor closed here; that is the defect
    //       fixed on 2026-08-22 (a294cd5), which leaked one descriptor per rejected frame and
    //       was invisible to every test in the suite.
    // WHY-THIS-SHAPE: the refusal is staged INSIDE glImportMemoryFdEXT, by publishing a memory
    //       size the driver will not accept for that allocation, and the staging is verified by
    //       matching the importer's own error text before anything is counted. The obvious
    //       simpler alternative -- refuse the frame earlier, e.g. with width 0 -- cannot fail:
    //       those paths return before any descriptor is handed over, so they would grade a
    //       branch the fix never touched and stay green with the defect restored.
    //       An import that unexpectedly SUCCEEDS is a harness fault and not a skip: reported as
    //       a skip it would silently delete this whole phase.
    // CONTROL: the F_DUPFD_CLOEXEC duplication removed from FrameImport::import in
    //       backend/kde/frame_import.cpp, restoring the shape that leaked -- the caller's
    //       descriptor surrendered to the driver and `request.fd` cleared before the call ->
    //       "FAIL: a frame the importer refuses leaks no file descriptor", printing "36 open fds
    //       before 32 refused imports, 68 after -- 32 leaked", one per refused frame, alongside
    //       "FAIL: a refused frame leaves its file descriptor with the caller...". Observed
    //       2026-08-22. Reverted.
    // CONTROL, the staging guard: the staged size set to half the allocation instead of over it
    //       -> "FAIL (harness): the staged frame was refused before its descriptor reached the
    //       driver (\"glTexStorageMem2DEXT could not back a texture with the imported memory\")",
    //       which is this phase refusing to grade a branch it did not reach. Observed 2026-08-22.
    // COVERAGE-ASSERT: the number of attempts REFUSED AT THE HAND-OVER is asserted, and that
    //       count -- not "refused and handed the descriptor back" -- is what gates the leak
    //       comparison. The distinction is not pedantic: gating on the stronger property made
    //       the leak check unreachable under the very defect it grades, because the leaking
    //       shape also stops handing the descriptor back. Measured, not reasoned: the first
    //       version of this block passed its control by failing a different check.
    {
        const wallpaperx::testing::Frame doomed = wallpaperx::testing::getFrameFd(*proxy, kMonitor);

        // A size the driver cannot match against the allocation behind the fd. Derived from the
        // real one so the refusal is about the size and not about a fourcc or a plane layout,
        // which are checked before the descriptor is handed over.
        const auto stageOnce = [&doomed](std::string& error) -> Outcome {
            wallpaperx::FrameImport::Request rejected = requestFromFrame(doomed);
            if (rejected.fd < 0) return Outcome::NeverAttempted;
            rejected.memorySize = doomed.memorySize * 4 + 4096;
            const wallpaperx::FrameImport::Result result = wallpaperx::FrameImport::import(rejected);
            error = result.error;
            if (result.texture != 0) {
                wallpaperx::FrameImport::release(result);
                if (rejected.fd >= 0) close(rejected.fd);
                return Outcome::Imported;
            }
            // A refusal that did not come from the hand-over never reached the branch this phase
            // grades. Counting it would let the leak check pass over a sweep in which the leaking
            // code ran zero times -- and a descriptor-pressure host, which is what a leak
            // produces, refuses at the duplication instead.
            if (error != wallpaperx::FrameImport::kImportRejectedError) {
                if (rejected.fd >= 0) close(rejected.fd);
                harnessFailure("the staged frame was refused before its descriptor reached the driver (\"" + error +
                               "\"), which is not the path this phase grades");
                return Outcome::NeverAttempted;
            }
            // The importer's contract: a refused frame leaves the descriptor with the caller.
            if (rejected.fd >= 0) {
                close(rejected.fd);
                return Outcome::RefusedAndHandedBack;
            }
            return Outcome::RefusedAndConsumed;
        };

        std::string stagedError;
        const Outcome firstOutcome = stageOnce(stagedError);
        // Matched against the importer's own words for the failure at the descriptor hand-over.
        // Any other refusal happened before the fd was given away, which is a branch this phase
        // does not grade -- so it is reported as this harness failing to set the case up.
        const bool stagedAtHandover = stagedError == wallpaperx::FrameImport::kImportRejectedError;
        if (firstOutcome == Outcome::NeverAttempted) {
            // requestFromFrame has already reported why; nothing below is measurable.
        } else if (firstOutcome == Outcome::Imported) {
            harnessFailure("the frame staged to be refused imported successfully, so nothing was established about "
                           "what a refused import does with its descriptor");
        } else if (!stagedAtHandover) {
            harnessFailure("the staged frame was refused before its descriptor reached the driver (\"" + stagedError +
                           "\"), which is not the path this phase grades");
        } else {
            constexpr int kRefusals = 32;
            const size_t beforeRefusals = openFdCount();
            int refusedAtHandover = 0;
            int handedBack = 0;
            bool neverAttempted = false;
            std::string lastError;
            for (int i = 0; i < kRefusals; ++i) {
                const Outcome outcome = stageOnce(lastError);
                if (outcome == Outcome::NeverAttempted) {
                    neverAttempted = true;
                    break;
                }
                // Only a refusal that reached the driver counts. An import this file never made
                // is not evidence about what the importer does with a descriptor.
                if (outcome == Outcome::RefusedAndHandedBack || outcome == Outcome::RefusedAndConsumed) {
                    ++refusedAtHandover;
                }
                if (outcome == Outcome::RefusedAndHandedBack) ++handedBack;
            }
            const size_t afterRefusals = openFdCount();

            // Not a return: the daemon's ordered shutdown below must still run.
            if (!neverAttempted) {

            check(refusedAtHandover == kRefusals,
                  "every staged frame in the refusal sweep really was refused, so the descriptor count below was "
                  "measured over a known number of real refusals");
            if (refusedAtHandover != kRefusals) {
                std::fprintf(stderr, "  %d of %d were refused; the last error was: %s\n", refusedAtHandover,
                             kRefusals, lastError.c_str());
            }
            // The importer's other half of the same contract, asserted separately rather than
            // folded into the gate above: a refusal that swallowed the caller's descriptor is a
            // defect in its own right, and gating the leak comparison on it would make that
            // comparison unreachable exactly when it matters.
            check(handedBack == kRefusals,
                  "a refused frame leaves its file descriptor with the caller, rather than reporting it consumed");
            if (handedBack != kRefusals) {
                std::fprintf(stderr, "  %d of %d refusals handed the descriptor back; %d reported it consumed\n",
                             handedBack, kRefusals, refusedAtHandover - handedBack);
            }
            if (!kcmpUsable()) {
                // The importer can only reclaim the duplicate where it can decide descriptor
                // identity, and kcmp is its only means of doing so. Without it, production leaks
                // one per refusal by documented decision -- grading that as a defect here would
                // report a deliberate trade as a bug, with nothing naming kcmp.
                notVerified(kGoals, "kcmp is unavailable on this host, so the importer cannot decide who owns a "
                                    "refused frame's descriptor and leaks it by design");
            } else if (beforeRefusals == 0 || afterRefusals == 0) {
                notVerified(kGoals, "/proc/self/fd could not be read around the refusal sweep, so no descriptor was "
                                    "counted");
            } else if (refusedAtHandover == kRefusals) {
                const bool steady = afterRefusals <= beforeRefusals;
                check(steady, "a frame the importer refuses leaks no file descriptor");
                if (!steady) {
                    std::fprintf(stderr, "  %zu open fds before %d refused imports, %zu after -- %zu leaked\n",
                                 beforeRefusals, kRefusals, afterRefusals, afterRefusals - beforeRefusals);
                }
            }
            }
        }
    }

    // Torn down before the GL context, and before this function returns: the daemon holds the
    // exported image, and a test that walked away from it would leave the next entry in the run
    // sharing a bus name with a process nobody is managing.
    const int exitCode = daemon.shutdown(SIGTERM);
    const char* kNotGraded = " (not graded here -- daemon_system_test owns the shutdown contract)\n";
    if (daemon.timedOut()) {
        std::printf("cross_process_import_test: the daemon did not exit within the harness deadline of SIGTERM and "
                    "had to be SIGKILLed%s",
                    kNotGraded);
    } else if (daemon.termSignal() != 0) {
        std::printf("cross_process_import_test: the daemon died on signal %d after SIGTERM%s", daemon.termSignal(),
                    kNotGraded);
    } else if (exitCode != 0) {
        std::printf("cross_process_import_test: the daemon exited with %d after SIGTERM%s", exitCode, kNotGraded);
    }
    return true;
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "cross_process_import_test";
    // Everything this drives reports failure by throwing, and without a boundary a host with no
    // bus or no Vulkan ends in std::terminate -- a failure carrying no reason, and no diagnostics
    // from the checks that did run.
    bool ok = false;
    try {
        ok = runChecks();
    } catch (const std::exception& e) {
        // Named as unattributed on purpose. A throw out of here is either this harness failing to
        // set up or the daemon failing to answer a call, and the exception alone does not say
        // which -- an sdbus timeout is exactly what a daemon wedged in its own handler produces.
        harnessFailure(std::string("the run ended in an error nothing caught, so the phases after it never ran: ") +
                       e.what());
    }
    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon or the "
                                                                 "importer");
    if (!ok || failed || faulted) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
