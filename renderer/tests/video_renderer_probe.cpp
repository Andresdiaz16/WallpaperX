// SPDX-License-Identifier: GPL-2.0-or-later
//
// The video renderer -- what it puts on a monitor, what it leaves behind, and what it
// refuses -- and the modifier filter's new tiling-feature requirement, which that renderer
// is the reason for.
//
// WHY THIS FILE EXISTS AT ALL. VideoRenderer has no caller anywhere in the tree: nothing
// decodes video yet, so no shipping path constructs a VideoFrame. Every line of it is
// unexercised until something builds a frame by hand, which is what this probe does -- an
// image whose pixels say where in the image they came from, lent to the renderer exactly as
// a decoder would lend one. Without that, three review passes could only grade findings
// against this code "plausible", because no reviewer could reach it.
//
// TWO ENTRIES, ONE BINARY. `render` grades the renderer; `modifiers` grades
// usableDrmModifiers()'s tiling-feature filter. They are separate ctest entries because they
// can be answered on different drivers: the render half needs nothing beyond a Device, while
// the "a modifier lacking the feature is dropped while one having it survives" half needs a
// driver offering two modifiers that differ in some feature, which Lavapipe (one modifier,
// LINEAR) cannot provide. One verdict for both would either claim that half ran where it did
// not, or mark a demonstrably working renderer unverified.
//
// HOW THE GEOMETRY IS READ BACK. The lent frame is a ramp: red rises with x, green with y,
// blue is constant. A destination pixel's red therefore names the source column it was
// sampled from, so the crop rectangle can be recovered from the rendered frame instead of
// being asserted through a byte-exact reference image, which would be a claim about the
// driver's filter rather than about the crop. Read back through Vulkan, not through a CPU
// mapping of an exported fd: a mapping needs DMA_BUF with a LINEAR layout, and the GPU this
// project targets exports opaque file descriptors.

#include "renderers/video_renderer.h"

#include "vulkan/device.h"
#include "vulkan/gpu_fault.h"
#include "vulkan/memory.h"
#include "vulkan/offscreen.h"

#include "support/check.h"
#include "support/cover_expectation.h"
#include "support/gpu_readback.h"
#include "support/lent_frame.h"
#include "support/validation_grade.h"

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wallpaperx::AllocatedBuffer;
using wallpaperx::AllocatedImage;
using wallpaperx::Device;
using wallpaperx::HostAccess;
using wallpaperx::OffscreenTarget;
using wallpaperx::OffscreenTargetManager;
using wallpaperx::VideoFrame;
using wallpaperx::VideoRenderer;
using wallpaperx::VideoSource;
using wallpaperx::testing::check;
using wallpaperx::testing::CropRect;
using wallpaperx::testing::expectedCover;
using wallpaperx::testing::fail;

constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

// ------------------------------------------------------------------------- //
// What the renderer actually submitted, read off the barriers themselves.
// ------------------------------------------------------------------------- //

struct RecordedBarrier {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED;
    uint64_t seq = 0;

    bool isOwnershipTransfer() const { return srcFamily != dstFamily; }
};

std::mutex g_barrierMutex;
std::vector<RecordedBarrier> g_barriers;
bool g_recordingBarriers = false;
uint64_t g_barrierSeq = 0;

void startBarrierRecording() {
    std::lock_guard lock(g_barrierMutex);
    g_barriers.clear();
    g_recordingBarriers = true;
}

void stopBarrierRecording() {
    std::lock_guard lock(g_barrierMutex);
    g_recordingBarriers = false;
}

std::vector<RecordedBarrier> barriersFor(VkImage image) {
    std::lock_guard lock(g_barrierMutex);
    std::vector<RecordedBarrier> matching;
    for (const RecordedBarrier& barrier : g_barriers) {
        if (barrier.image == image) matching.push_back(barrier);
    }
    return matching;
}

std::string layoutName(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC_OPTIMAL";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST_OPTIMAL";
        case VK_IMAGE_LAYOUT_PREINITIALIZED: return "PREINITIALIZED";
        default: return "VkImageLayout(" + std::to_string(static_cast<int>(layout)) + ")";
    }
}

// ------------------------------------------------------------------------- //
// The lent frame.
// ------------------------------------------------------------------------- //

constexpr unsigned char kConstantBlue = 128;

unsigned char ramp(uint32_t coordinate, uint32_t extent) {
    if (extent <= 1) return 0;
    return static_cast<unsigned char>(std::lround(255.0 * coordinate / (extent - 1)));
}

// A frame whose pixels name their own position: red rises with x, green with y, blue is a
// constant every pixel carries. Owned here and lent to the renderer, which is the ownership
// split VideoFrame documents -- the renderer allocates nothing and frees nothing.
std::vector<unsigned char> rampPixels(uint32_t width, uint32_t height) {
    std::vector<unsigned char> bgra(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            unsigned char* pixel = &bgra[(static_cast<size_t>(y) * width + x) * 4];
            pixel[0] = kConstantBlue;
            pixel[1] = ramp(y, height);
            pixel[2] = ramp(x, width);
            pixel[3] = 255;
        }
    }
    return bgra;
}

class RampFrame : public wallpaperx::testing::LentFrame {
public:
    RampFrame(Device& device, uint32_t width, uint32_t height, VkImageLayout declaredLayout)
        : LentFrame(device, width, height, declaredLayout, rampPixels(width, height), "video-probe-frame") {}
};

// ------------------------------------------------------------------------- //
// Where the pixels on the monitor came from.
// ------------------------------------------------------------------------- //


constexpr int kSampleAxisPoints = 5;
constexpr int kSamplesPerCase = kSampleAxisPoints * kSampleAxisPoints;

uint32_t samplePosition(int index, uint32_t extent) {
    constexpr double kFractions[kSampleAxisPoints] = {0.0, 0.25, 0.5, 0.75, 1.0};
    const auto position = static_cast<uint32_t>(kFractions[index] * (extent - 1) + 0.5);
    return std::min(position, extent - 1);
}

struct CoverReport {
    int compared = 0;
    int matchedX = 0;
    int matchedY = 0;
    int carriedFrameConstants = 0;
    double worstX = 0;
    double worstY = 0;
    double toleranceX = 0;
    double toleranceY = 0;
};

// Recovers, for a grid of destination pixels, which source coordinate each was sampled from,
// and compares that against the cover rule.
//
// The tolerance is built rather than picked: one destination pixel's worth of source (the
// footprint a linear filter averages over), plus half a quantisation step of the ramp (the
// most the 8-bit encoding can lose), plus 1.5 pixels of slack for the filter's tap choice on
// a downscale. Everything it must catch is far outside that -- for the 256x128 frame on a
// square monitor the crop starts at column 64 and a stretch starts at column 0.
CoverReport sampleCover(const std::vector<unsigned char>& pixels, uint32_t frameWidth, uint32_t frameHeight,
                        uint32_t targetWidth, uint32_t targetHeight) {
    const CropRect crop = expectedCover(frameWidth, frameHeight, targetWidth, targetHeight);

    CoverReport report;
    report.toleranceX = 1.5 + crop.width / targetWidth + 0.5 * (frameWidth - 1) / 255.0;
    report.toleranceY = 1.5 + crop.height / targetHeight + 0.5 * (frameHeight - 1) / 255.0;

    for (int yi = 0; yi < kSampleAxisPoints; ++yi) {
        for (int xi = 0; xi < kSampleAxisPoints; ++xi) {
            const uint32_t x = samplePosition(xi, targetWidth);
            const uint32_t y = samplePosition(yi, targetHeight);
            const size_t offset = (static_cast<size_t>(y) * targetWidth + x) * 4;
            if (offset + 4 > pixels.size()) return report;  // compared < kSamplesPerCase: the caller fails
            ++report.compared;

            // The continuous source coordinate this destination pixel's centre maps to, and the
            // ramp index a linear filter reads there (ramp value i sits at continuous i + 0.5).
            const double u = crop.x0 + (x + 0.5) * crop.width / targetWidth;
            const double v = crop.y0 + (y + 0.5) * crop.height / targetHeight;
            const double expectedX = std::clamp(u - 0.5, 0.0, static_cast<double>(frameWidth - 1));
            const double expectedY = std::clamp(v - 0.5, 0.0, static_cast<double>(frameHeight - 1));

            const double gotX = frameWidth > 1 ? pixels[offset + 2] * (frameWidth - 1) / 255.0 : 0.0;
            const double gotY = frameHeight > 1 ? pixels[offset + 1] * (frameHeight - 1) / 255.0 : 0.0;

            report.worstX = std::max(report.worstX, std::abs(gotX - expectedX));
            report.worstY = std::max(report.worstY, std::abs(gotY - expectedY));
            if (std::abs(gotX - expectedX) <= report.toleranceX) ++report.matchedX;
            if (std::abs(gotY - expectedY) <= report.toleranceY) ++report.matchedY;

            // A letterboxed or cleared destination pixel did not come from the frame at all,
            // and says so in the two channels the frame holds constant.
            if (std::abs(static_cast<int>(pixels[offset]) - kConstantBlue) <= 4 && pixels[offset + 3] >= 250) {
                ++report.carriedFrameConstants;
            }
        }
    }
    return report;
}

int g_coverCasesGraded = 0;

// One monitor, one frame, one verdict on where its pixels came from.
void gradeCover(Device& device, OffscreenTargetManager& targets, VideoRenderer& renderer, const RampFrame& frame,
                int monitorId, uint32_t targetWidth, uint32_t targetHeight, const char* what) {
    const OffscreenTarget& target = targets.create(monitorId, targetWidth, targetHeight);
    targets.render(renderer, target);
    const std::vector<unsigned char> pixels = wallpaperx::testing::reclaimAndReadback(device, targets, target);

    const CoverReport report = sampleCover(pixels, frame.width(), frame.height(), targetWidth, targetHeight);
    ++g_coverCasesGraded;

    check(report.compared == kSamplesPerCase, std::string("every sample point was read for: ") + what);
    const bool geometryHeld = report.matchedX == report.compared && report.matchedY == report.compared;
    check(geometryHeld, what);
    if (!geometryHeld) {
        const CropRect crop = expectedCover(frame.width(), frame.height(), targetWidth, targetHeight);
        std::fprintf(stderr,
                     "  %dx%d frame on a %ux%u monitor: expected the region x=[%.1f,%.1f) y=[%.1f,%.1f); %d of %d "
                     "sampled pixels came from the right column (worst miss %.1f px, tolerance %.1f) and %d of %d "
                     "from the right row (worst miss %.1f px, tolerance %.1f)\n",
                     frame.width(), frame.height(), targetWidth, targetHeight, crop.x0, crop.x0 + crop.width, crop.y0,
                     crop.y0 + crop.height, report.matchedX, report.compared, report.worstX, report.toleranceX,
                     report.matchedY, report.compared, report.worstY, report.toleranceY);
    }
    check(report.carriedFrameConstants == report.compared,
          std::string("every sampled pixel came from the frame rather than from a border, for: ") + what);
}

// ------------------------------------------------------------------------- //
// Phase 1 -- cover-crop geometry.
// ------------------------------------------------------------------------- //

void runGeometryChecks(Device& device, OffscreenTargetManager& targets) {
    // GOAL: S13.20, S13.22 -- a frame whose aspect differs from the monitor's is scaled to cover and cropped
    //       centrally -- never stretched to fit, never letterboxed, never cropped from a
    //       corner -- and a crop the arithmetic would round to nothing keeps one pixel
    //       instead of collapsing to an empty blit region.
    // WHY-THIS-SHAPE: the destination is compared against a *recovered source coordinate*,
    //       not against a stored reference image. A reference image would fix the driver's
    //       filter output as the contract, so it would break on the other ICD for a renderer
    //       that is perfectly correct, and it could not say which region was read -- the one
    //       thing being graded. Four shapes because the branches are distinct: wider than the
    //       monitor and taller than it take opposite arms, an exact aspect takes neither, and
    //       512x2 on an 8x64 monitor is the case whose computed crop width is zero before the
    //       clamp.
    // CONTROL: three, all reverted. (a) and (b) are in coverCrop, which
    //       renderer/renderers/cover_crop.h shares with the still-image renderer. (a) `if
    //       (sourceWide > targetWide)` -> `if (false)`, so a frame wider than the monitor is
    //       squeezed: "FAIL: a frame wider than the monitor is cropped left and right, not
    //       squeezed / 5 of 25 sampled pixels came from the right column (worst miss 63.5 px,
    //       tolerance 4.0)", the sliver case (also wider than its monitor) red too, and the
    //       taller-than-monitor case green -- the two arms are separable. (b) `const uint64_t x0
    //       = (sourceWidth - width) / 2;` and its y twin -> `= 0`, an anchored crop: four cases
    //       red, each naming the axis it was cropped on (worst miss 64.5 px). (c) the blit's
    //       `dstOffsets[1]` in VideoRenderer::record (renderer/renderers/video_renderer.cpp)
    //       halved, so the frame occupies a quarter of the monitor: all five cases red on both
    //       the geometry and the "came from the frame rather than from a border" check beside it.
    // COVERAGE-ASSERT: g_coverCasesGraded is asserted below, and each case asserts that all
    //       25 of its sample points were actually read, so a case that silently stopped
    //       comparing fails rather than passing on the samples it reached. Both were observed
    //       red by cutting the sample loop to 3 of 5 columns and by skipping one gradeCover
    //       call.
    {
        RampFrame wide(device, 256, 128, VK_IMAGE_LAYOUT_GENERAL);
        VideoRenderer renderer(device, wide.frame());
        gradeCover(device, targets, renderer, wide, /*monitorId=*/1, 64, 64,
                   "a frame wider than the monitor is cropped left and right, not squeezed");

        // GOAL: S13.21 -- the crop is decided per target, not once per frame or per renderer. The same
        //       renderer showing the same frame on a monitor of a different shape crops on the
        //       other axis.
        // WHY-THIS-SHAPE: reuses the renderer above deliberately. A version that built a second
        //       renderer would pass for an implementation that computed the crop in show() and
        //       cached it, which is the plausible refactor this guards. 128x32 is chosen so the
        //       2:1 frame is *taller* than the 4:1 monitor and the other arm of the branch runs
        //       -- the two calls together cover both arms through one renderer.
        // CONTROL: the anchored-crop mutation (b) above reddens this case with a *row* miss of
        //       32.2 px while the case above it goes red on columns, which is the measurement
        //       showing the two calls exercise opposite arms through one renderer. Mutation (a),
        //       which disables the wider-than-monitor arm, leaves this case green -- so it is
        //       not a second copy of the check above it.
        gradeCover(device, targets, renderer, wide, /*monitorId=*/2, 128, 32,
                   "the same frame on a wider monitor is cropped top and bottom instead");
    }
    {
        RampFrame tall(device, 128, 256, VK_IMAGE_LAYOUT_GENERAL);
        VideoRenderer renderer(device, tall.frame());
        gradeCover(device, targets, renderer, tall, /*monitorId=*/3, 64, 64,
                   "a frame taller than the monitor is cropped top and bottom, not squeezed");
    }
    {
        RampFrame square(device, 128, 128, VK_IMAGE_LAYOUT_GENERAL);
        VideoRenderer renderer(device, square.frame());
        gradeCover(device, targets, renderer, square, /*monitorId=*/4, 64, 64,
                   "a frame with the monitor's own aspect is shown whole");
    }
    {
        // GOAL: S13.22 -- at a ratio whose crop rounds to zero, the monitor still shows the middle column
        //       of the frame and the render is legal.
        // WHY-THIS-SHAPE: 512x2 on an 8x64 monitor makes the computed crop width 2*8/64 = 0
        //       before std::max clamps it to 1. The x half carries the verdict: the crop is one
        //       column wide, so every destination pixel must come from column 255 give or take
        //       the tolerance. The y half is weak here by construction -- the frame is two rows
        //       tall, less than the tolerance -- and is graded by the three cases above.
        // CONTROL: the wider-than-monitor arm's `std::max<uint64_t>(1, ...)` in coverCrop
        //       (renderer/renderers/cover_crop.h) changed to `std::max<uint64_t>(64, ...)`, a
        //       wrong clamp -> this case red alone, "0 of 25 sampled pixels came from the right
        //       column (worst miss 28.6 px, tolerance 2.6)". Reverted.
        //
        //       WHAT THIS DOES NOT GRADE, measured rather than assumed: deleting the clamp
        //       outright -- so the crop really is zero pixels wide -- leaves the whole probe
        //       GREEN on both the RTX 3080 Ti and Lavapipe. Both drivers accept a zero-extent
        //       blit region, emit no validation message (the spec states no VUID against it),
        //       and write the same pixels. So the clamp is defensive, and nothing here or
        //       elsewhere would catch its removal.
        RampFrame sliver(device, 512, 2, VK_IMAGE_LAYOUT_GENERAL);
        VideoRenderer renderer(device, sliver.frame());
        gradeCover(device, targets, renderer, sliver, /*monitorId=*/5, 8, 64,
                   "a crop the arithmetic would round to nothing keeps a column of the frame");
    }

    check(g_coverCasesGraded == 5, "all five geometry cases were rendered and read back");
}

// ------------------------------------------------------------------------- //
// Phase 2 -- the frame is left exactly as it was found.
// ------------------------------------------------------------------------- //

void runFrameRestoreChecks(Device& device, OffscreenTargetManager& targets) {
    int layoutsExamined = 0;

    // Two declared layouts, because a restore that names a fixed layout is right for one of
    // them and wrong for the other.
    const VkImageLayout declaredLayouts[] = {VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (const VkImageLayout declared : declaredLayouts) {
        RampFrame frame(device, 128, 128, declared);
        VideoRenderer renderer(device, frame.frame());

        const OffscreenTarget& first = targets.create(/*monitorId=*/10, 64, 64);
        startBarrierRecording();
        targets.render(renderer, first);
        stopBarrierRecording();

        const std::vector<RecordedBarrier> onFrame = barriersFor(frame.image());
        ++layoutsExamined;

        // GOAL: S13.23 -- a render transitions the borrowed frame to a blit source and back to the layout
        //       its owner declared, so the owner's own record of that image never drifts and the
        //       same frame can be shown again.
        // WHY-THIS-SHAPE: the barriers are read by interposing vkCmdPipelineBarrier, because
        //       there is nothing else to read: an image's layout is not queryable, the barriers
        //       are locals inside record(), and X.6 forbids adding a seam to production to
        //       expose them. The obvious alternative -- render twice and check the pixels -- is
        //       not an oracle: a missing restore leaves the image in TRANSFER_SRC_OPTIMAL, which
        //       is exactly the layout the next blit wants, so the pixels come back correct on
        //       both ICDs while every later owner of that image is misinformed. Two declared
        //       layouts for the reason above.
        // CONTROL: the `after` barrier's `m_frame.layout` in VideoRenderer::record
        //       (renderer/renderers/video_renderer.cpp) replaced by VK_IMAGE_LAYOUT_GENERAL, so
        //       the restore names a fixed layout -> the SHADER_READ_ONLY_OPTIMAL pass goes
        //       "FAIL: the render leaves the frame in the layout its owner declared" (and the
        //       validation layer's own layout-mismatch complaint alongside it), while the
        //       GENERAL pass stays green -- which is the measurement showing one declared layout
        //       cannot grade this. Not submitting the `after` barrier at all instead reddens "a
        //       render touches the borrowed frame with exactly two barriers / 1 barrier(s) named
        //       the frame image, not 2" for both layouts. Both reverted.
        // COVERAGE-ASSERT: the barrier count for the frame is asserted at exactly two, so a
        //       render that stopped touching the frame at all -- or that added a third,
        //       unpaired transition -- fails instead of leaving the pair checks unevaluated.
        const bool twoBarriers = onFrame.size() == 2;
        check(twoBarriers, "a render touches the borrowed frame with exactly two barriers");
        if (!twoBarriers) {
            std::fprintf(stderr, "  %zu barrier(s) named the frame image, not 2\n", onFrame.size());
        } else {
            const bool toSource = onFrame[0].oldLayout == declared &&
                                  onFrame[0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            const bool back = onFrame[1].oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
                              onFrame[1].newLayout == declared;
            check(toSource, "the render takes the frame from the layout its owner declared");
            check(back, "the render leaves the frame in the layout its owner declared");
            if (!toSource || !back) {
                std::fprintf(stderr, "  declared %s; recorded %s -> %s then %s -> %s\n", layoutName(declared).c_str(),
                             layoutName(onFrame[0].oldLayout).c_str(), layoutName(onFrame[0].newLayout).c_str(),
                             layoutName(onFrame[1].oldLayout).c_str(), layoutName(onFrame[1].newLayout).c_str());
            }
            check(!onFrame[0].isOwnershipTransfer() && !onFrame[1].isOwnershipTransfer(),
                  "the frame stays with the graphics queue -- the renderer transfers no ownership of it");
        }

        // GOAL: S13.24 -- showing one frame twice is legal and produces the same picture both times.
        // WHY-THIS-SHAPE: two monitors rather than one, because reading a target back reclaims
        //       it. Stated as a check even though the pair above is the stronger oracle: this is
        //       the property a caller depends on (a decoder holds a frame across several display
        //       frames), and it is graded by the validation layer as well -- a wrong restore
        //       makes the second render's barrier name a layout the image is not in.
        // CONTROL: a `mutable int m_renders` member added to VideoRenderer in
        //       renderer/renderers/video_renderer.h, and `if (m_renders++ > 0) crop.x0 += 8;`
        //       after the coverCrop call in renderer/renderers/video_renderer.cpp's record() --
        //       a renderer that carries state between renders -> "FAIL: the
        //       same frame rendered twice produces the same picture / 64 of 64 rows differ
        //       between the two renders of one frame", for both declared layouts. Reverted.
        //       Note what this check cannot do: deleting the restoring barrier does NOT redden
        //       it, because the layout it leaves behind is the one the next blit wants. That
        //       failure is caught by the barrier pair above and by the layer, not here.
        const OffscreenTarget& second = targets.create(/*monitorId=*/11, 64, 64);
        targets.render(renderer, second);
        const std::vector<unsigned char> firstPixels = wallpaperx::testing::reclaimAndReadback(device, targets, first);
        const std::vector<unsigned char> secondPixels =
            wallpaperx::testing::reclaimAndReadback(device, targets, second);
        const size_t differing = wallpaperx::testing::countDifferingRows(firstPixels, secondPixels, 64, 64);
        check(differing == 0, "the same frame rendered twice produces the same picture");
        if (differing != 0) {
            std::fprintf(stderr, "  %zu of 64 rows differ between the two renders of one frame\n", differing);
        }

        targets.destroy(10);
        targets.destroy(11);
    }

    // COVERAGE-ASSERT: both declared layouts were reached. Without this, a `continue` or an
    // early return in the loop would leave the fixed-layout restore ungraded and silent.
    check(layoutsExamined == 2, "the restore was graded for both declared layouts");
}

// ------------------------------------------------------------------------- //
// Phase 3 -- the target's layout is reported honestly.
// ------------------------------------------------------------------------- //

void runTargetLayoutChecks(Device& device, OffscreenTargetManager& targets) {
    RampFrame frame(device, 128, 128, VK_IMAGE_LAYOUT_GENERAL);
    VideoRenderer renderer(device, frame.frame());
    const OffscreenTarget& target = targets.create(/*monitorId=*/20, 64, 64);

    startBarrierRecording();
    targets.render(renderer, target);
    const VkImageLayout afterFirst = target.releasedFromLayout;
    targets.render(renderer, target);
    stopBarrierRecording();

    // GOAL: S13.25 -- a video render leaves the target in TRANSFER_DST_OPTIMAL and says so. What the
    //       target records as released is what the next acquire names, so a consumer's
    //       hand-back reverses the hand-over it was paired with.
    // WHY-THIS-SHAPE: this is why IRenderer::record() returns a layout at all. Asserting the
    //       returned value directly would need record() called outside
    //       OffscreenTargetManager::render(), which is the one caller that pairs it with an
    //       ownership release -- and the defect being guarded is precisely that the release
    //       names a layout the image was never in, which only shows up in the pair. So the
    //       recorded release and the *next* render's acquire are compared to each other and to
    //       TRANSFER_DST_OPTIMAL. kRenderedLayout (SHADER_READ_ONLY_OPTIMAL) is what a renderer
    //       drawing through the shared pass leaves behind, so a record() that returned that
    //       constant fails here.
    // CONTROL: two, both reverted. (a) the `return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;` at the
    //       end of VideoRenderer::record (renderer/renderers/video_renderer.cpp) changed to
    //       `return kRenderedLayout;` -> "FAIL: a video render leaves the target in the
    //       transfer-destination layout / the target recorded SHADER_READ_ONLY_OPTIMAL as the
    //       layout it was released from" and "FAIL: the release hands the target over from the
    //       layout the video render left it in", with ten validation warnings on top. (b) the
    //       acquire in recordAcquire (renderer/vulkan/offscreen.cpp) given a literal
    //       VK_IMAGE_LAYOUT_GENERAL as its oldLayout instead of target.releasedFromLayout ->
    //       "FAIL: the next acquire names the layout the release named / released from
    //       TRANSFER_DST_OPTIMAL but acquired from GENERAL" as the run's ONLY failure, and the
    //       validation layer said nothing at all -- ownership transfers are not tracked, which
    //       is why this pairing needs a check of its own.
    // COVERAGE-ASSERT: the number of ownership transfers found for the target is asserted at
    //       three (release, acquire, release), so a run in which the second render never
    //       happened cannot pass on the strength of the first one's release alone.
    std::vector<RecordedBarrier> transfers;
    for (const RecordedBarrier& barrier : barriersFor(target.image.image)) {
        if (barrier.isOwnershipTransfer()) transfers.push_back(barrier);
    }
    const bool threeTransfers = transfers.size() == 3;
    check(threeTransfers, "two renders of one target recorded a release, an acquire and a release");
    if (!threeTransfers) {
        std::fprintf(stderr, "  %zu ownership transfer(s) named the target image, not 3\n", transfers.size());
    }

    check(afterFirst == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          "a video render leaves the target in the transfer-destination layout");
    if (afterFirst != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        std::fprintf(stderr, "  the target recorded %s as the layout it was released from\n",
                     layoutName(afterFirst).c_str());
    }

    if (threeTransfers) {
        check(transfers[0].oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              "the release hands the target over from the layout the video render left it in");
        check(transfers[1].oldLayout == transfers[0].oldLayout,
              "the next acquire names the layout the release named");
        if (transfers[1].oldLayout != transfers[0].oldLayout) {
            std::fprintf(stderr, "  released from %s but acquired from %s\n",
                         layoutName(transfers[0].oldLayout).c_str(), layoutName(transfers[1].oldLayout).c_str());
        }
        check(transfers[0].seq < transfers[1].seq, "the acquire follows the release it reverses");
    }

    targets.destroy(20);
}

// ------------------------------------------------------------------------- //
// Phase 4 -- what is refused, and what a refusal leaves on the monitor.
// ------------------------------------------------------------------------- //

enum class Expected { ContentError, InvalidArgument };

// Runs `attempt` and grades which exception came out. A refusal of the wrong kind is a
// failure, not a pass: the two types are how the daemon tells "this file is the user's
// problem" from "this caller has a bug", and they reach different code paths there.
bool refuses(const char* what, Expected expected, const std::function<void()>& attempt) {
    try {
        attempt();
    } catch (const wallpaperx::WallpaperContentError& e) {
        const bool right = expected == Expected::ContentError;
        check(right, what);
        if (!right) {
            std::fprintf(stderr, "  refused with WallpaperContentError, expected std::invalid_argument: %s\n",
                         e.what());
        }
        return true;
    } catch (const std::invalid_argument& e) {
        const bool right = expected == Expected::InvalidArgument;
        check(right, what);
        if (!right) {
            std::fprintf(stderr, "  refused with std::invalid_argument, expected WallpaperContentError: %s\n",
                         e.what());
        }
        return true;
    } catch (const std::exception& e) {
        check(false, what);
        std::fprintf(stderr, "  refused with the wrong exception type: %s\n", e.what());
        return true;
    }
    check(false, what);
    std::fprintf(stderr, "  it was accepted\n");
    return false;
}

void runRefusalChecks(Device& device, OffscreenTargetManager& targets) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &properties);
    const uint32_t limit = properties.limits.maxImageDimension2D;

    // GOAL: S13.28 -- the constructor validates the frame it is handed. A wholly default-constructed
    //       VideoFrame -- the value a caller gets for free -- does not build a renderer.
    // CONTROL: `show(first);` in the VideoRenderer constructor
    //       (renderer/renderers/video_renderer.cpp) replaced by `m_frame = first;`, a
    //       constructor that adopts without checking -> "FAIL: a default-constructed frame
    //       cannot build a renderer / it was accepted", and the default-valued-declaration check
    //       below it, which an unchecking constructor also lets through; nothing else. Reverted.
    //       Note that removing any *single* guard inside show() leaves this green: a
    //       default-constructed frame is refused by the null image, by the undefined layout and
    //       by the zero extent independently, so only "the constructor does not check at all"
    //       can fail it.
    refuses("a default-constructed frame cannot build a renderer", Expected::InvalidArgument,
            [&] { VideoRenderer renderer(device, VideoFrame{}); });

    RampFrame good(device, 128, 128, VK_IMAGE_LAYOUT_GENERAL);
    VideoRenderer renderer(device, good.frame());

    // GOAL: S13.28 -- the first frame is checked even when its declaration is the default-constructed
    //       one -- the value a caller gets for free from `VideoSource{}`.
    // WHY-THIS-SHAPE: this is the case the marker has to get right, and the only one. A real
    //       image and a valid layout carry it past requireShowable, so what refuses it is the
    //       source check, and the source check runs only if "nothing shown yet" is decided by
    //       something other than the source itself. A default-valued frame handed to an
    //       already-running renderer (in the table below) does NOT test this: its source
    //       differs from the shown one, so it is checked either way -- measured, that version
    //       stayed green under the mutation below.
    // CONTROL: the `m_frame.image == VK_NULL_HANDLE ||` disjunct deleted from the condition in
    //       VideoRenderer::show (renderer/renderers/video_renderer.cpp), so "nothing shown yet"
    //       is decided by the source alone -> "FAIL: a first frame whose declaration is
    //       default-valued cannot build a renderer / it was accepted", with every other check
    //       in the file still green. Reverted.
    refuses("a first frame whose declaration is default-valued cannot build a renderer", Expected::InvalidArgument,
            [&] {
                VideoFrame frame = good.frame();
                frame.source = VideoSource{};
                VideoRenderer unchecked(device, frame);
            });

    const OffscreenTarget& target = targets.create(/*monitorId=*/30, 64, 64);
    targets.render(renderer, target);
    const std::vector<unsigned char> reference = wallpaperx::testing::reclaimAndReadback(device, targets, target);
    // Without this the comparisons below are between two readbacks of an image nothing wrote,
    // which agree with each other whatever happened.
    // CONTROL: the blit's dstOffsets[1] halved in VideoRenderer::record
    // (renderer/renderers/video_renderer.cpp), leaving three quarters of the monitor unwritten
    // -> this line red, so it really is reading the rendered frame and not a constant.
    check(wallpaperx::testing::variesInBothAxes(reference, 64, 64),
          "the frame on the monitor before the refusals varies in both axes, so a later render that showed "
          "something else would be told apart from it");

    struct Case {
        const char* what;
        Expected expected;
        // Whether a wrongly accepted frame can still be rendered without risking the run. See
        // the annotation below.
        bool renderAfter;
        VideoFrame frame;
    };

    // Every case starts from a frame that is real in every respect except the one field under
    // test: the image, its contents and its actual creation parameters are the good frame's.
    // That is the point -- Vulkan cannot be asked what an image was created as, so the
    // declaration is the only authority, and each case is a declaration that lies in one way.
    auto lying = [&](auto&& mutate) {
        VideoFrame frame = good.frame();
        mutate(frame);
        return frame;
    };

    // GOAL: S13.26 -- each of these declarations is refused rather than blitted, and refused with the type
    //       that names whose problem it is -- WallpaperContentError for content this device
    //       cannot take verbatim, std::invalid_argument for a declaration wrong on its face.
    // WHY-THIS-SHAPE: refuses() grades the exception type and not merely that something was
    //       thrown, because the daemon routes the two differently: one marks the wallpaper bad
    //       and leaves the daemon healthy, the other is a caller bug.
    // CONTROL: one production mutation per guard, each reverted, and each named with what it
    //       actually reddened -- several cases are refused by more than one guard, so removing
    //       one guard is not always enough:
    //         * `requireShowable(frame);` deleted from VideoRenderer::show -> the undefined
    //           layout, preinitialized and no-image cases, three red lines.
    //         * the extent guard in requireBlittable -> `if (false)` -> zero extent and
    //           over-limit. The default-valued case stayed green: the usage guard catches it.
    //         * the usage guard -> `if (false)` -> the missing TRANSFER_SRC case.
    //         * `if (!isUnormColour(source.format))` -> `if (false)` -> the two cases declaring
    //           sRGB, and the previously-shown-frame check, since the second of them also lies
    //           about its extent. NV12 stayed green because the format-feature query below it
    //           also refuses NV12; disabling that query as well reddens NV12 too.
    //         * the tiling guard -> `if (false)` -> the DRM-modifier case.
    //         * requireBlittable made to return immediately -> ten red lines: all eight cases in
    //           this table that are not a layout or a null handle, the default-valued first
    //           frame above, and the previously-shown-frame check.
    //       All in renderer/renderers/video_renderer.cpp.
    std::vector<Case> cases;
    cases.push_back({"a multi-planar Y'CbCr frame (NV12) is refused", Expected::ContentError, true,
                     lying([](VideoFrame& f) { f.source.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM; })});
    cases.push_back({"an sRGB frame is refused, since a blit would decode it and nothing re-encodes",
                     Expected::ContentError, true,
                     lying([](VideoFrame& f) { f.source.format = VK_FORMAT_B8G8R8A8_SRGB; })});
    cases.push_back({"a frame whose usage lacks TRANSFER_SRC is refused", Expected::InvalidArgument, true,
                     lying([](VideoFrame& f) { f.source.usage = VK_IMAGE_USAGE_SAMPLED_BIT; })});
    cases.push_back({"a frame in an undefined layout is refused", Expected::InvalidArgument, true,
                     lying([](VideoFrame& f) { f.layout = VK_IMAGE_LAYOUT_UNDEFINED; })});
    cases.push_back({"a frame declared preinitialized is refused, since no barrier transitions back into it",
                     Expected::InvalidArgument, true,
                     lying([](VideoFrame& f) { f.layout = VK_IMAGE_LAYOUT_PREINITIALIZED; })});
    cases.push_back({"a frame with a DRM-modifier tiling is refused, since its blit support is unknown",
                     Expected::ContentError, true,
                     lying([](VideoFrame& f) { f.source.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT; })});
    // The one refusal whose wrongful adoption is visible in pixels, and it is here for exactly
    // that reason. The six cases above lie about metadata the blit never reads -- format,
    // tiling, usage, layout -- so a build that adopted them before refusing them would still
    // draw the identical picture, and the "previously shown frame" check below could not tell.
    // This one lies about the extent as well, at half the real image, so an adopted declaration
    // crops to the frame's top-left quarter. Half rather than zero or over-limit: those two are
    // out of the image and undefined in the driver, which is why they are not rendered after.
    cases.push_back({"a refused frame is not adopted in part -- its extent does not reach the crop either",
                     Expected::ContentError, true, lying([](VideoFrame& f) {
                         f.source.format = VK_FORMAT_B8G8R8A8_SRGB;
                         f.source.width = 64;
                         f.source.height = 64;
                     })});
    cases.push_back({"a frame with no image is refused", Expected::InvalidArgument, false,
                     lying([](VideoFrame& f) { f.image = VK_NULL_HANDLE; })});
    cases.push_back({"a frame of zero extent is refused", Expected::InvalidArgument, false,
                     lying([](VideoFrame& f) { f.source.width = 0; })});
    cases.push_back({"a frame larger than this device can hold is refused", Expected::InvalidArgument, false,
                     lying([&](VideoFrame& f) { f.source.width = limit + 1; })});
    cases.push_back({"a frame whose declaration is entirely default-valued is refused rather than blitted",
                     Expected::InvalidArgument, false, lying([](VideoFrame& f) { f.source = VideoSource{}; })});

    int examined = 0;
    int previousFrameStillShown = 0;
    int renderedAfter = 0;
    for (const Case& testCase : cases) {
        ++examined;
        refuses(testCase.what, testCase.expected, [&] { renderer.show(testCase.frame); });

        // GOAL: S13.27 -- a refused frame leaves the previously shown one on the monitor -- the refusal
        //       costs the picture nothing.
        // WHY-THIS-SHAPE: rendered and compared after each refusal rather than once at the end,
        //       so a leak is attributed to the case that leaked. Four of the eleven cases are
        //       deliberately NOT rendered after: a wrongly accepted null image, zero extent,
        //       over-limit extent or default-valued declaration produces a blit whose source
        //       region is outside the image or whose handle is null. That is undefined in the
        //       driver rather than merely wrong -- a hang or a lost device, which would replace
        //       this probe's verdict with a ctest timeout. Their refusal is graded by the throw
        //       alone, which is not weaker for the property they carry.
        // CONTROL: VideoRenderer::show (renderer/renderers/video_renderer.cpp) rewritten to
        //       adopt first and check afterwards -- `m_frame = frame;` moved above the two
        //       checks, with the checks reading a saved copy of the previous frame so that they
        //       all still fire -> "FAIL: a refused frame leaves the previously shown frame on
        //       the monitor", with the diagnostic naming the extent-lying case and 64 of 64 rows
        //       differing. Reverted. The simpler mutation (move `m_frame = frame;` up and leave
        //       the condition reading m_frame) does NOT control this check: it stops the
        //       refusals happening at all, so the refusal lines go red instead and nothing is
        //       left to leak.
        // COVERAGE-ASSERT: both the number of cases examined and the number actually
        //       re-rendered are asserted, so a case list that shrank, or a `renderAfter` flag
        //       flipped off, fails instead of quietly grading less.
        if (testCase.renderAfter) {
            ++renderedAfter;
            targets.render(renderer, target);
            const std::vector<unsigned char> after = wallpaperx::testing::reclaimAndReadback(device, targets, target);
            const size_t differing = wallpaperx::testing::countDifferingRows(after, reference, 64, 64);
            if (differing == 0) {
                ++previousFrameStillShown;
            } else {
                std::fprintf(stderr, "  after refusing \"%s\", %zu of 64 rows differ from the frame that was shown\n",
                             testCase.what, differing);
            }
        }

        // Nothing under a correct build: the good frame is already the shown one and show()
        // re-adopts an identical declaration. It is here for the control runs, where a check
        // removed from the renderer means the lying frame really was adopted -- without this,
        // the next case would be graded against the previous case's leak and the red line would
        // name the wrong refusal.
        try {
            renderer.show(good.frame());
        } catch (const std::exception& e) {
            fail(std::string("the good frame could not be restored after a refusal: ") + e.what());
        }
    }

    check(examined == 11, "every refusal case was put to the renderer");
    check(renderedAfter == 7, "seven of the refusals were followed by a render");
    check(previousFrameStillShown == renderedAfter,
          "a refused frame leaves the previously shown frame on the monitor");

    targets.destroy(30);
}

// ------------------------------------------------------------------------- //
// The modifier filter's tiling-feature requirement.
// ------------------------------------------------------------------------- //

struct FeatureBit {
    VkFormatFeatureFlags bit;
    const char* name;
};

// The core format features a DRM modifier's tiling can advertise. Enumerated rather than
// looped over all 32 bit positions so a failure names something, and because the reserved
// positions would add nothing but noise.
constexpr FeatureBit kFeatureBits[] = {
    {VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "SAMPLED_IMAGE"},
    {VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "STORAGE_IMAGE"},
    {VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT, "STORAGE_IMAGE_ATOMIC"},
    {VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "COLOR_ATTACHMENT"},
    {VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, "COLOR_ATTACHMENT_BLEND"},
    {VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "DEPTH_STENCIL_ATTACHMENT"},
    {VK_FORMAT_FEATURE_BLIT_SRC_BIT, "BLIT_SRC"},
    {VK_FORMAT_FEATURE_BLIT_DST_BIT, "BLIT_DST"},
    {VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "SAMPLED_IMAGE_FILTER_LINEAR"},
    {VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, "TRANSFER_SRC"},
    {VK_FORMAT_FEATURE_TRANSFER_DST_BIT, "TRANSFER_DST"},
    {VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT, "MIDPOINT_CHROMA_SAMPLES"},
    {VK_FORMAT_FEATURE_DISJOINT_BIT, "DISJOINT"},
    {VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT, "COSITED_CHROMA_SAMPLES"},
};

constexpr VkExtent3D kModifierExtent{64, 64, 1};
constexpr VkExternalMemoryFeatureFlags kBothWays =
    VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;

std::vector<uint64_t> askUsable(VkPhysicalDevice physicalDevice, VkFormatFeatureFlags requiredTilingFeatures) {
    return wallpaperx::usableDrmModifiers(physicalDevice, OffscreenTargetManager::kFormat, VK_IMAGE_TYPE_2D,
                                          OffscreenTargetManager::kUsage, kModifierExtent, kBothWays,
                                          requiredTilingFeatures);
}

std::string hex(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

VkImageCreateInfo targetLikeImageInfo() {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = OffscreenTargetManager::kFormat;
    info.extent = kModifierExtent;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = OffscreenTargetManager::kUsage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

void runModifierFilterChecks(Device& device) {
    if (!device.info().hasImageDrmFormatModifier) {
        g_notVerified = true;
        std::printf("video_renderer_probe: NOT VERIFIED -- this driver has no DRM format modifiers, so there is no "
                    "modifier list to filter\n");
        return;
    }

    const std::vector<VkDrmFormatModifierPropertiesEXT> all =
        wallpaperx::queryDrmModifierList(device.physicalDevice(), OffscreenTargetManager::kFormat);
    const std::vector<uint64_t> base = askUsable(device.physicalDevice(), 0);
    std::printf("video_renderer_probe: %s reports %zu modifier(s) for the target format, %zu of them usable\n",
                device.info().gpuName.c_str(), all.size(), base.size());
    for (const VkDrmFormatModifierPropertiesEXT& properties : all) {
        std::printf("  %s tilingFeatures=0x%08x\n", hex(properties.drmFormatModifier).c_str(),
                    properties.drmFormatModifierTilingFeatures);
    }

    if (base.empty()) {
        g_notVerified = true;
        std::printf("video_renderer_probe: NOT VERIFIED -- no modifier is usable for a target-shaped image here, so "
                    "there is nothing for the tiling-feature filter to keep or drop\n");
        return;
    }

    auto featuresOf = [&](uint64_t modifier) -> VkFormatFeatureFlags {
        for (const VkDrmFormatModifierPropertiesEXT& properties : all) {
            if (properties.drmFormatModifier == modifier) return properties.drmFormatModifierTilingFeatures;
        }
        return 0;
    };
    bool everyUsableIsListed = true;
    for (uint64_t modifier : base) {
        bool found = false;
        for (const VkDrmFormatModifierPropertiesEXT& properties : all) {
            if (properties.drmFormatModifier == modifier) found = true;
        }
        everyUsableIsListed = everyUsableIsListed && found;
    }
    // The expectations below are built out of the driver's own feature report, so a usable
    // modifier missing from that report would silently be expected to be dropped.
    // CONTROL: the queryDrmModifierList() call above asked for VK_FORMAT_R4G4_UNORM_PACK8, a
    // format with no modifiers, so the report covers none of the usable ones -> "FAIL: every
    // usable modifier appears in the driver's modifier report" and the phase abandoned rather
    // than grading seven modifiers against an empty table. Reverted.
    check(everyUsableIsListed, "every usable modifier appears in the driver's modifier report");
    if (!everyUsableIsListed) return;

    // GOAL: S13.29 -- usableDrmModifiers() drops exactly those modifiers whose own tiling features lack
    //       the caller's required ones -- so requiring a feature no modifier advertises leaves
    //       none, and requiring one only some advertise drops precisely the others.
    // WHY-THIS-SHAPE: a check that asked for VK_FORMAT_FEATURE_BLIT_DST_BIT (what production
    //       asks for) and counted survivors would establish nothing: measured here, every
    //       modifier advertises it -- seven of seven on the RTX 3080 Ti, one of one on
    //       Lavapipe -- so that check passes identically with the filter deleted. What has to
    //       be shown is a *drop*, so the required feature is chosen from the driver's own
    //       report: one bit that no usable modifier has (nothing may survive) and, where the
    //       driver offers one, a bit that splits the set (exactly the advertisers survive).
    //       Every bit in the table is swept rather than only those two, because the two
    //       interesting bits differ per driver and a sweep needs no per-driver constant.
    // CONTROL: the `if ((candidate.drmFormatModifierTilingFeatures & requiredTilingFeatures) !=
    //       requiredTilingFeatures) continue;` block deleted from usableDrmModifiers
    //       (renderer/vulkan/memory.cpp) -> on the RTX 3080 Ti, six per-bit diagnostics ("
    //       requiring COLOR_ATTACHMENT: 6 modifier(s) advertise it but 7 survived the filter",
    //       and five bits nothing advertises where all 7 survived) followed by "FAIL: for every
    //       required tiling feature, the modifiers that survive are exactly those advertising
    //       it", "FAIL: at least one required tiling feature actually removed a modifier" and
    //       "FAIL: requiring a tiling feature no modifier advertises leaves no usable modifier
    //       at all". Reverted.
    // COVERAGE-ASSERT: the number of feature bits swept is asserted, and so is the number that
    //       actually caused a modifier to be dropped -- a sweep that compared nothing, or one
    //       run on a driver where the filter can never bite, fails instead of passing quietly.
    int bitsSwept = 0;
    int mismatches = 0;
    int bitsThatDropped = 0;
    bool sawEverythingDropped = false;
    bool sawPartialDrop = false;
    const char* partialBitName = nullptr;
    for (const FeatureBit& feature : kFeatureBits) {
        ++bitsSwept;
        std::vector<uint64_t> expected;
        for (uint64_t modifier : base) {
            if ((featuresOf(modifier) & feature.bit) == feature.bit) expected.push_back(modifier);
        }
        std::vector<uint64_t> got = askUsable(device.physicalDevice(), feature.bit);

        std::vector<uint64_t> sortedExpected = expected;
        std::sort(sortedExpected.begin(), sortedExpected.end());
        std::sort(got.begin(), got.end());
        if (sortedExpected != got) {
            ++mismatches;
            std::fprintf(stderr,
                         "  requiring %s: %zu modifier(s) advertise it but %zu survived the filter\n", feature.name,
                         expected.size(), got.size());
            continue;
        }
        if (expected.size() == base.size()) continue;
        ++bitsThatDropped;
        if (expected.empty()) {
            sawEverythingDropped = true;
        } else {
            sawPartialDrop = true;
            partialBitName = feature.name;
        }
    }

    check(bitsSwept == static_cast<int>(std::size(kFeatureBits)), "every format feature in the table was swept");
    check(mismatches == 0,
          "for every required tiling feature, the modifiers that survive are exactly those advertising it");
    check(bitsThatDropped > 0,
          "at least one required tiling feature actually removed a modifier, so the filter was observed doing "
          "something");
    check(sawEverythingDropped,
          "requiring a tiling feature no modifier advertises leaves no usable modifier at all");

    if (sawPartialDrop) {
        std::printf("video_renderer_probe: requiring %s kept some modifiers and dropped others\n", partialBitName);
    } else {
        // Scoped to this claim alone, and it is the claim the real GPU carries: Lavapipe offers
        // one modifier, so nothing there can distinguish "the filter drops what lacks the
        // feature" from "the filter drops everything". Reporting the whole entry as verified
        // would be claiming a measurement this driver cannot make.
        g_notVerified = true;
        std::printf("video_renderer_probe: NOT VERIFIED -- no format feature splits this driver's usable modifiers "
                    "(%zu of them), so a filter that kept the advertisers and dropped the rest cannot be told from "
                    "one that drops everything\n",
                    base.size());
    }

    if (!device.info().hasExternalMemoryDmaBuf) {
        g_notVerified = true;
        std::printf("video_renderer_probe: NOT VERIFIED -- this driver cannot export a dma_buf, so image creation "
                    "never reaches the modifier filter\n");
        return;
    }

    // GOAL: S13.30 -- the requirement reaches image creation. A dma_buf image whose required tiling
    //       features no modifier satisfies is refused, rather than created with a layout the
    //       blit it exists for cannot target.
    // WHY-THIS-SHAPE: graded through createExportableImage rather than only through the free
    //       function, because a caller passing the argument and a callee using it are separate
    //       failures -- the free-function sweep above stays green if createExportableImage
    //       hands the filter a zero. The impossible feature is taken from the sweep, so this
    //       cannot be a bit the driver happens to advertise. The opposite direction is asserted
    //       beside it: with what production actually asks for, the image is still created, so
    //       this cannot pass by refusing everything.
    // CONTROL: two, both in the usableDrmModifiers() call inside Memory::createExportableImage
    //       (renderer/vulkan/memory.cpp), both reverted. (a) `requiredTilingFeatures` replaced
    //       by `0`, a callee that ignores what it was given -> "FAIL: a dma_buf image is refused
    //       when no modifier has the required tiling features" as the run's only failure, with
    //       the sweep above still green -- which is the measurement showing this check is not
    //       redundant with it. (b) replaced by VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT, a
    //       callee that demands more than it was given -> "FAIL: a dma_buf image is still
    //       created for the tiling features an offscreen target asks for", so the pair cannot be
    //       satisfied by a build that refuses everything.
    VkFormatFeatureFlags impossible = 0;
    for (const FeatureBit& feature : kFeatureBits) {
        bool advertisedBySomeone = false;
        for (uint64_t modifier : base) {
            if ((featuresOf(modifier) & feature.bit) == feature.bit) advertisedBySomeone = true;
        }
        if (!advertisedBySomeone) {
            impossible = feature.bit;
            break;
        }
    }
    if (impossible == 0) {
        g_notVerified = true;
        std::printf("video_renderer_probe: NOT VERIFIED -- every format feature in the table is advertised by some "
                    "modifier here, so image creation cannot be asked for one that is impossible\n");
        return;
    }

    const VkImageCreateInfo info = targetLikeImageInfo();
    bool refusedImpossible = false;
    try {
        AllocatedImage image = device.memory().createExportableImage(
            info, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, impossible, "video-probe-impossible");
        device.memory().destroyImage(image);
    } catch (const std::exception&) {
        refusedImpossible = true;
    }
    check(refusedImpossible, "a dma_buf image is refused when no modifier has the required tiling features");

    bool createdWithProductionFeatures = false;
    try {
        AllocatedImage image =
            device.memory().createExportableImage(info, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                                  OffscreenTargetManager::kFormatFeatures, "video-probe-blittable");
        createdWithProductionFeatures = image.image != VK_NULL_HANDLE;
        device.memory().destroyImage(image);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  creating a dma_buf image with the target's own tiling features threw: %s\n",
                     e.what());
    }
    check(createdWithProductionFeatures,
          "a dma_buf image is still created for the tiling features an offscreen target asks for");

    // GOAL: S13.31 -- the opaque-fd path does not consult the requirement, as its documentation says --
    //       there is no modifier list there, and the caller can query the tiling it chose.
    // WHY-THIS-SHAPE: the same impossible feature that refuses the dma_buf path above. Without
    //       this, moving the filter above the handle-type branch -- the obvious "simplification"
    //       -- would break every opaque-fd export on the project's target GPU with nothing
    //       failing here.
    // CONTROL: the requirement applied above the handle-type branch, which is what "hoisting the
    //       filter" would amount to -- a vkGetPhysicalDeviceFormatProperties call at the top of
    //       Memory::createExportableImage (renderer/vulkan/memory.cpp) throwing when the
    //       format's optimal tiling features lack requiredTilingFeatures -> "creating an
    //       opaque-fd image threw: control: the required tiling features are unsupported" and
    //       "FAIL: an opaque-fd image ignores the tiling features the dma_buf path filters on",
    //       with the two dma_buf checks above still green. Reverted.
    bool opaqueIgnoredIt = false;
    try {
        AllocatedImage image = device.memory().createExportableImage(
            info, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, impossible, "video-probe-opaque");
        opaqueIgnoredIt = image.image != VK_NULL_HANDLE;
        device.memory().destroyImage(image);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  creating an opaque-fd image threw: %s\n", e.what());
    }
    check(opaqueIgnoredIt, "an opaque-fd image ignores the tiling features the dma_buf path filters on");
}

// ------------------------------------------------------------------------- //

void runRenderChecks(Device& device) {
    OffscreenTargetManager targets(device);
    runGeometryChecks(device, targets);
    runFrameRestoreChecks(device, targets);
    runTargetLayoutChecks(device, targets);
    runRefusalChecks(device, targets);
}

}  // namespace

// Reads the barriers the renderer submitted. The executable's definition preempts
// libvulkan's for every call site linked into it, including those inside
// wallpaperx-renderer.a, and the real entry point is reached through RTLD_NEXT. Forwarding is
// unconditional, so the renderer behaves identically whether or not recording is on.
//
// Interposition rather than a hook in the renderer because X.6 forbids a test-only seam in
// production, and the barriers are locals inside record() that no interface exposes.
extern "C" void vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask,
                                     VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags,
                                     uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
                                     uint32_t bufferMemoryBarrierCount,
                                     const VkBufferMemoryBarrier* pBufferMemoryBarriers,
                                     uint32_t imageMemoryBarrierCount,
                                     const VkImageMemoryBarrier* pImageMemoryBarriers) {
    {
        std::lock_guard lock(g_barrierMutex);
        if (g_recordingBarriers) {
            for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
                const VkImageMemoryBarrier& barrier = pImageMemoryBarriers[i];
                g_barriers.push_back(RecordedBarrier{barrier.image, barrier.oldLayout, barrier.newLayout,
                                                     barrier.srcQueueFamilyIndex, barrier.dstQueueFamilyIndex,
                                                     g_barrierSeq++});
            }
        }
    }

    using Fn = void (*)(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                        const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                        const VkImageMemoryBarrier*);
    // Fatal rather than silent: skipping the forward would drop every barrier in the process
    // and leave the probe grading a run whose GPU work was never synchronised.
    static const Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "vkCmdPipelineBarrier"));
    if (real == nullptr) {
        std::fprintf(stderr, "video_renderer_probe: cannot resolve the real vkCmdPipelineBarrier\n");
        std::abort();
    }
    real(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers,
         bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode != "render" && mode != "modifiers") {
        std::fprintf(stderr, "usage: %s render|modifiers\n", argv[0]);
        return 1;
    }

    const bool graded =
        wallpaperx::testing::gradedRun("video_renderer_probe (" + mode + ")", [&](auto& grade) {
            try {
                Device device(/*enableValidation=*/true);
                grade.watch(device);
                std::printf("video_renderer_probe (%s): on %s\n", mode.c_str(), device.info().gpuName.c_str());

                if (mode == "render") {
                    runRenderChecks(device);
                } else {
                    runModifierFilterChecks(device);
                }
            } catch (const std::exception& e) {
                // Reported, not rethrown: an escaping exception would skip the validation grade
                // in gradedRun's frame and print no verdict for the checks that did run.
                fail(std::string("video_renderer_probe threw: ") + e.what());
            }
            return wallpaperx::testing::failureCount() == 0;
        });

    // Counted before the verdict is combined, not inside a short-circuit: `!graded ||
    // reportFailures(...)` skips the summary line exactly when the validation grade failed,
    // which is a run whose diagnostics matter most.
    const int failures = wallpaperx::testing::reportFailures("video_renderer_probe");
    if (!graded || failures > 0) return 1;
    if (g_notVerified) {
        std::printf("video_renderer_probe (%s): NOT VERIFIED on this driver -- reported to ctest as a skip, not a "
                    "pass\n",
                    mode.c_str());
        return kNotVerifiedExitCode;
    }
    std::printf("video_renderer_probe (%s): all checks passed, zero validation warnings\n", mode.c_str());
    return 0;
}
