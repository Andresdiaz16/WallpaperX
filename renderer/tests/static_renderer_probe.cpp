// SPDX-License-Identifier: GPL-2.0-or-later
//
// The still-image renderer: that it draws and fails cleanly on a missing file, and that it draws
// "scaled and cropped" -- uniformly scaled to cover the monitor, centred, overflow cropped -- on
// every descriptor tier, showing the same region the video renderer shows.
//
// HOW THE GEOMETRY IS READ BACK. The fixtures are ramps: red rises with x, green with y. A monitor
// pixel's red and green name the image position it was sampled from, so the region shown is
// recovered from the rendered frame and compared against the cover rule in real arithmetic
// (support/cover_expectation.h) -- never against coverCrop, which is what is under test.

#include "renderers/static_renderer.h"
#include "renderers/video_renderer.h"
#include "vulkan/device.h"
#include "vulkan/offscreen.h"

#include "support/check.h"
#include "support/cover_expectation.h"
#include "support/descriptor_probe_support.h"
#include "support/gpu_readback.h"
#include "support/lent_frame.h"
#include "support/test_image.h"
#include "support/validation_grade.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wallpaperx::DescriptorTier;
using wallpaperx::Device;
using wallpaperx::OffscreenTarget;
using wallpaperx::OffscreenTargetManager;
using wallpaperx::StaticRenderer;
using wallpaperx::VideoRenderer;
using wallpaperx::testing::check;
using wallpaperx::testing::CropRect;
using wallpaperx::testing::expectedCover;
using wallpaperx::testing::fail;

// ctest reads 77 as a skip (SKIP_RETURN_CODE on both entries in renderer/CMakeLists.txt). A tier
// this driver cannot put in the state the phase grades must not read as a graded pass.
constexpr int kNotVerifiedExitCode = 77;
bool g_notVerified = false;

std::string writeTestImage() {
    return wallpaperx::testing::writeSolidPng(
        std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/test_red.png", 8, 8, {255, 0, 0, 255});
}

bool testRenderValidImage() {

    const bool ok = wallpaperx::testing::gradedRun(
        "static_renderer_probe", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        const std::string imagePath = writeTestImage();

        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);
        const auto& target = targets.create(0, 64, 64);

        wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

        targets.render(renderer, target);

        // The render published the frame, so the graphics queue no longer owns this image.
        // reclaimAndReadback() submits the acquire that takes it back before it copies --
        // see gpu_readback.h for why copying without it is undefined and unreportable.
        const std::vector<unsigned char> pixels =
            wallpaperx::testing::reclaimAndReadback(device, targets, target);

        const unsigned char b = pixels[0];
        const unsigned char g = pixels[1];
        const unsigned char r = pixels[2];
        const unsigned char a = pixels[3];

        std::cout << "static_renderer_probe: readback pixel (B,G,R,A) = ("
                  << int(b) << "," << int(g) << "," << int(r) << "," << int(a) << ")" << std::endl;

        if (r < 250 || g > 5 || b > 5 || a < 250) {
            std::cerr << "static_renderer_probe: rendered pixel does not match expected solid red" << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "static_renderer_probe failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "static_renderer_probe: passed, zero validation warnings" << std::endl;
    return true;
}

// From TEST.md's QA pass (2026-07-18): confirms StaticRenderer's own error path
// (as opposed to stb_image's) throws a catchable, clear exception for a missing
// file, rather than crashing or leaving partially-constructed state.
bool testMissingImageThrows() {

    const bool ok = wallpaperx::testing::gradedRun(
        "static_renderer_probe (missing image)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        // renderPass() is valid immediately after construction (built in the
        // constructor, independent of create()) -- StaticRenderer fails at
        // stbi_load before it ever consumes the render pass, so no actual
        // offscreen target needs to exist for this test.
        wallpaperx::OffscreenTargetManager targets(device);

        bool threw = false;
        try {
            wallpaperx::StaticRenderer renderer(device, targets.renderPass(),
                                                 "/nonexistent/path/does-not-exist.png", WALLPAPERX_SHADER_DIR);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "static_renderer_probe (missing image): constructing with a nonexistent path "
                         "should have thrown"
                      << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "static_renderer_probe (missing image) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "static_renderer_probe (missing image): passed" << std::endl;
    return true;
}

// ------------------------------------------------------------------------- //
// Fixtures.
// ------------------------------------------------------------------------- //

constexpr unsigned char kConstantBlue = 128;

unsigned char ramp(uint32_t coordinate, uint32_t extent) {
    if (extent <= 1) return 0;
    return static_cast<unsigned char>(std::lround(255.0 * coordinate / (extent - 1)));
}

// RGBA, top row first.
struct Fixture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<unsigned char> rgba;
};

// Blue is `inside` within [x0, x1) x [y0, y1) and `outside` elsewhere.
Fixture makeFixture(uint32_t width, uint32_t height, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1,
                    unsigned char inside, unsigned char outside) {
    Fixture fixture{width, height, std::vector<unsigned char>(static_cast<size_t>(width) * height * 4)};
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            unsigned char* texel = &fixture.rgba[(static_cast<size_t>(y) * width + x) * 4];
            const bool in = x >= x0 && x < x1 && y >= y0 && y < y1;
            texel[0] = ramp(x, width);
            texel[1] = ramp(y, height);
            texel[2] = in ? inside : outside;
            texel[3] = 255;
        }
    }
    return fixture;
}

Fixture rampFixture(uint32_t width, uint32_t height) {
    return makeFixture(width, height, 0, width, 0, height, kConstantBlue, kConstantBlue);
}

std::vector<unsigned char> toBgra(const std::vector<unsigned char>& rgba) {
    std::vector<unsigned char> bgra = rgba;
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    return bgra;
}

std::string writeFixture(const Fixture& fixture, const std::string& name) {
    return wallpaperx::testing::writeRgbaPng(std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/cover_" + name + ".png",
                                             static_cast<int>(fixture.width), static_cast<int>(fixture.height),
                                             fixture.rgba);
}

// ------------------------------------------------------------------------- //
// Phase 1 -- cover geometry.
// ------------------------------------------------------------------------- //

struct CoverReport {
    size_t compared = 0;
    size_t matchedX = 0;
    size_t matchedY = 0;
    size_t fromImage = 0;
    double worstX = 0;
    double worstY = 0;
    double toleranceX = 0;
    double toleranceY = 0;
    // Mean image column at the monitor's right edge minus at its left, and row at bottom minus top.
    double spanX = 0;
    double spanY = 0;
    double expectedSpanX = 0;
    double expectedSpanY = 0;
};

// The renderer crops to whole texels: its origin is at most half a texel from the real one and its
// extent under one texel short, so a sample can land up to 1.5 texels away. Zero where the real
// crop is already whole, which keeps those cases tight.
double wholeTexelAllowance(double origin, double extent) {
    return origin == std::floor(origin) && extent == std::floor(extent) ? 0.0 : 1.5;
}

// The image coordinate a linear filter reads at a monitor pixel centre, for the real crop.
double expectedSample(double origin, double extent, uint32_t pixel, uint32_t targetExtent, uint32_t imageExtent) {
    const double continuous = origin + (pixel + 0.5) * extent / targetExtent;
    return std::clamp(continuous - 0.5, 0.0, static_cast<double>(imageExtent - 1));
}

CoverReport measureCover(const std::vector<unsigned char>& pixels, uint32_t imageWidth, uint32_t imageHeight,
                         uint32_t targetWidth, uint32_t targetHeight) {
    const CropRect crop = expectedCover(imageWidth, imageHeight, targetWidth, targetHeight);

    CoverReport report;
    // One ramp code of the fixture's rounding and one of the output's, a quarter texel of sampler
    // precision, and the whole-texel allowance.
    report.toleranceX = (imageWidth - 1) / 255.0 + 0.25 + wholeTexelAllowance(crop.x0, crop.width);
    report.toleranceY = (imageHeight - 1) / 255.0 + 0.25 + wholeTexelAllowance(crop.y0, crop.height);
    report.expectedSpanX = expectedSample(crop.x0, crop.width, targetWidth - 1, targetWidth, imageWidth) -
                           expectedSample(crop.x0, crop.width, 0, targetWidth, imageWidth);
    report.expectedSpanY = expectedSample(crop.y0, crop.height, targetHeight - 1, targetHeight, imageHeight) -
                           expectedSample(crop.y0, crop.height, 0, targetHeight, imageHeight);

    if (pixels.size() < static_cast<size_t>(targetWidth) * targetHeight * 4) return report;

    double left = 0, right = 0, top = 0, bottom = 0;
    for (uint32_t y = 0; y < targetHeight; ++y) {
        for (uint32_t x = 0; x < targetWidth; ++x) {
            const size_t offset = (static_cast<size_t>(y) * targetWidth + x) * 4;
            ++report.compared;

            const double gotX = pixels[offset + 2] * (imageWidth - 1) / 255.0;
            const double gotY = pixels[offset + 1] * (imageHeight - 1) / 255.0;
            const double missX = std::abs(gotX - expectedSample(crop.x0, crop.width, x, targetWidth, imageWidth));
            const double missY = std::abs(gotY - expectedSample(crop.y0, crop.height, y, targetHeight, imageHeight));
            report.worstX = std::max(report.worstX, missX);
            report.worstY = std::max(report.worstY, missY);
            if (missX <= report.toleranceX) ++report.matchedX;
            if (missY <= report.toleranceY) ++report.matchedY;

            // A cleared or letterboxed pixel did not come from the image, and its blue says so.
            if (std::abs(static_cast<int>(pixels[offset]) - kConstantBlue) <= 4 && pixels[offset + 3] >= 250) {
                ++report.fromImage;
            }

            if (x == 0) left += gotX;
            if (x == targetWidth - 1) right += gotX;
            if (y == 0) top += gotY;
            if (y == targetHeight - 1) bottom += gotY;
        }
    }
    report.spanX = (right - left) / targetHeight;
    report.spanY = (bottom - top) / targetWidth;
    return report;
}

int g_coverCases = 0;

void gradeCover(Device& device, OffscreenTargetManager& targets, StaticRenderer& renderer, int monitorId,
                uint32_t imageWidth, uint32_t imageHeight, uint32_t targetWidth, uint32_t targetHeight,
                const std::string& what) {
    const OffscreenTarget& target = targets.create(monitorId, targetWidth, targetHeight);
    targets.render(renderer, target);
    const std::vector<unsigned char> pixels = wallpaperx::testing::reclaimAndReadback(device, targets, target);
    const CoverReport report = measureCover(pixels, imageWidth, imageHeight, targetWidth, targetHeight);
    ++g_coverCases;

    const size_t expected = static_cast<size_t>(targetWidth) * targetHeight;
    const bool wholeScan = report.compared == expected;
    check(wholeScan, "every monitor pixel was compared, for: " + what);
    // Every verdict below reads those pixels, so a short readback would be reported again as a
    // geometry, border and orientation defect, with a geometry diagnostic for what is a readback
    // fault. One event, one failure, named for what it is.
    if (!wholeScan) return;

    const bool geometry = report.matchedX == expected && report.matchedY == expected;
    check(geometry, what);
    if (!geometry) {
        const CropRect crop = expectedCover(imageWidth, imageHeight, targetWidth, targetHeight);
        std::fprintf(stderr,
                     "  %ux%u image on a %ux%u monitor: expected the region x=[%.2f,%.2f) y=[%.2f,%.2f); %zu of %zu "
                     "pixels came from the right column (worst miss %.2f texels, tolerance %.2f) and %zu from the "
                     "right row (worst miss %.2f, tolerance %.2f)\n",
                     imageWidth, imageHeight, targetWidth, targetHeight, crop.x0, crop.x0 + crop.width, crop.y0,
                     crop.y0 + crop.height, report.matchedX, report.compared, report.worstX, report.toleranceX,
                     report.matchedY, report.worstY, report.toleranceY);
    }
    check(report.fromImage == expected, "every monitor pixel came from the image rather than a border, for: " + what);

    // GOAL: the image is not mirrored or flipped -- its left and top edges land on the monitor's
    //       left and top.
    // WHY-THIS-SHAPE: read off the same ramps rather than separate stripe markers: the ramp is a
    //       marker at every column and row, so a stripe fixture would add nothing the geometry
    //       check does not already read, while a named check says "mirrored" where the geometry
    //       check can only say "wrong column". Half the expected span, not its sign, so a crop
    //       collapsed to a sliver does not pass as "not mirrored".
    // CONTROL: renderer/shaders/fullscreen.vert with `inUV` replaced by `vec2(1.0 - inUV.x,
    //       inUV.y)` -> all five cases red on "not mirrored" (right minus left edge -126.00
    //       texels, expected 126.00) and on geometry, "not flipped" green; with `vec2(inUV.x,
    //       1.0 - inUV.y)` the reverse. Observed on all three tiers on both ICDs, both reverted.
    const bool leftToRight = report.spanX > report.expectedSpanX / 2;
    const bool topToBottom = report.spanY > report.expectedSpanY / 2;
    check(leftToRight, "the image's left edge is on the monitor's left, not mirrored, for: " + what);
    check(topToBottom, "the image's top edge is on the monitor's top, not flipped, for: " + what);
    if (!leftToRight || !topToBottom) {
        std::fprintf(stderr, "  right minus left edge %.2f texels (expected %.2f); bottom minus top %.2f (expected %.2f)\n",
                     report.spanX, report.expectedSpanX, report.spanY, report.expectedSpanY);
    }
}

void runCoverCases(Device& device, const std::string& tier) {
    OffscreenTargetManager targets(device);

    // GOAL: an image whose aspect differs from the monitor's is uniformly scaled to cover it,
    //       centred, the overflow cropped -- never stretched, letterboxed or anchored at a corner --
    //       and one whose aspect matches is shown whole.
    // WHY-THIS-SHAPE: every monitor pixel is compared against a recovered image coordinate, not a
    //       reference image: a reference would make one driver's filter output the contract. Both
    //       arms of the crop and the no-crop case are separate branches in the rule, and 200x150 on
    //       160x90 is the case whose crop is not whole texels, so rounding is exercised as well as
    //       the exact cases.
    // CONTROL: each reverted, each observed on all three tiers on both ICDs. (a)
    //       renderer/renderers/cover_crop.h `if (sourceWide > targetWide)` -> `if (false)`: the
    //       wider-than-monitor case alone red, "192 of 9216 pixels came from the right column
    //       (worst miss 31.83 texels, tolerance 0.75)". (b) cover_crop.h's `x0` and `y0` set to 0,
    //       an anchored crop: the four cropped cases red, each on its cropped axis, the matching
    //       case green. (c) renderer/renderers/static_renderer.cpp pushing origin {0,0} and extent
    //       {1,1}, the old stretch: the four cropped cases red, the matching case green, and both
    //       still-versus-video checks red.
    // COVERAGE-ASSERT: every case asserts all of its monitor's pixels were compared, and the tier
    //       asserts all eight of its cases ran. Both observed red: the first with measureCover's
    //       row loop stopping one row short, the second with one agreement case removed. The
    //       verdicts in gradeCover are gated on that first assert rather than repeating it:
    //       measured, the short readback reports 15 failures and no geometry diagnostic with the
    //       gate, and 60 failures with 15 geometry messages for a readback fault without it.
    const std::string widePath = writeFixture(rampFixture(128, 64), "wide");
    const std::string tallPath = writeFixture(rampFixture(64, 128), "tall");
    const std::string oddPath = writeFixture(rampFixture(200, 150), "odd");
    {
        StaticRenderer renderer(device, targets.renderPass(), widePath, WALLPAPERX_SHADER_DIR);
        gradeCover(device, targets, renderer, 1, 128, 64, 96, 96,
                   tier + ": an image wider than the monitor is cropped left and right, not squeezed");

        // GOAL: the crop is decided per target. One renderer drawing monitors of three shapes crops
        //       each for its own shape.
        // WHY-THIS-SHAPE: the same renderer as above on purpose -- a fresh renderer per shape would
        //       pass for one that computed the crop once and kept it. 192x36 makes the 2:1 image
        //       taller than the monitor, so the other arm runs through the same renderer.
        // CONTROL: a `bool m_haveCrop` and `Crop m_crop` added to renderer/renderers/static_renderer.h
        //       and record() in renderer/renderers/static_renderer.cpp computing the crop only on its
        //       first call -> these two red on all three tiers on both ICDs (worst miss 31.4
        //       texels), the first case and the other two renderers green. Reverted.
        gradeCover(device, targets, renderer, 2, 128, 64, 192, 36,
                   tier + ": the same renderer on a wider monitor crops top and bottom instead");
        gradeCover(device, targets, renderer, 3, 128, 64, 64, 32,
                   tier + ": the same renderer on a monitor of the image's own aspect shows it whole");
    }
    {
        StaticRenderer renderer(device, targets.renderPass(), tallPath, WALLPAPERX_SHADER_DIR);
        gradeCover(device, targets, renderer, 4, 64, 128, 96, 96,
                   tier + ": an image taller than the monitor is cropped top and bottom, not squeezed");
    }
    {
        StaticRenderer renderer(device, targets.renderPass(), oddPath, WALLPAPERX_SHADER_DIR);
        gradeCover(device, targets, renderer, 5, 200, 150, 160, 90,
                   tier + ": a crop that is not a whole number of texels is still centred and uniform");
    }
}

// ------------------------------------------------------------------------- //
// Phase 2 -- the still image and the video show the same region.
// ------------------------------------------------------------------------- //

// Largest per-channel difference allowed between the two renderers' pixels: one code of each
// one's rounding, and a sixteenth of a texel of filter position (the minimum sub-texel precision
// Vulkan allows) on the steepest ramp here, 23 codes per texel.
constexpr int kAgreementCodes = 4;
// A monitor edge pixel whose blue is at least this read the texel beyond the crop.
constexpr int kMarkerVisible = 32;

int g_agreementCases = 0;

void gradeAgreement(Device& device, OffscreenTargetManager& targets, int monitorId, const Fixture& fixture,
                    const std::string& path, uint32_t targetWidth, uint32_t targetHeight, const std::string& what) {
    StaticRenderer still(device, targets.renderPass(), path, WALLPAPERX_SHADER_DIR);
    wallpaperx::testing::LentFrame frame(device, fixture.width, fixture.height, VK_IMAGE_LAYOUT_GENERAL,
                                         toBgra(fixture.rgba), "cover-agreement-frame");
    VideoRenderer video(device, frame.frame());

    const std::vector<unsigned char> stillPixels =
        wallpaperx::testing::renderReference(device, targets, still, monitorId, targetWidth, targetHeight);
    const std::vector<unsigned char> videoPixels =
        wallpaperx::testing::renderReference(device, targets, video, monitorId + 1, targetWidth, targetHeight);
    ++g_agreementCases;

    const CropRect crop = expectedCover(fixture.width, fixture.height, targetWidth, targetHeight);
    size_t interior = 0, edge = 0;
    int worstInterior = 0, worstEdge = 0, videoEdgeBlue = 0;
    uint32_t worstInteriorX = 0, worstInteriorY = 0, worstEdgeX = 0, worstEdgeY = 0;
    const size_t bytes = static_cast<size_t>(targetWidth) * targetHeight * 4;
    if (stillPixels.size() >= bytes && videoPixels.size() >= bytes) {
        for (uint32_t y = 0; y < targetHeight; ++y) {
            for (uint32_t x = 0; x < targetWidth; ++x) {
                const size_t offset = (static_cast<size_t>(y) * targetWidth + x) * 4;
                const double u = crop.x0 + (x + 0.5) * crop.width / targetWidth;
                const double v = crop.y0 + (y + 0.5) * crop.height / targetHeight;
                // Within a texel of the crop's border, the filter may read beyond it.
                const bool atEdge = u - 0.5 < crop.x0 + 1 || u + 0.5 > crop.x0 + crop.width - 1 ||
                                    v - 0.5 < crop.y0 + 1 || v + 0.5 > crop.y0 + crop.height - 1;
                int difference = 0;
                for (int channel = 0; channel < 4; ++channel) {
                    difference = std::max(difference, std::abs(static_cast<int>(stillPixels[offset + channel]) -
                                                               static_cast<int>(videoPixels[offset + channel])));
                }
                if (atEdge) {
                    ++edge;
                    if (difference > worstEdge) worstEdge = difference, worstEdgeX = x, worstEdgeY = y;
                    videoEdgeBlue = std::max(videoEdgeBlue, static_cast<int>(videoPixels[offset]));
                } else {
                    ++interior;
                    if (difference > worstInterior) worstInterior = difference, worstInteriorX = x, worstInteriorY = y;
                }
            }
        }
    }

    check(interior > 0 && edge > 0 && interior + edge == static_cast<size_t>(targetWidth) * targetHeight,
          "every monitor pixel was compared, inside the crop and at its edge, for: " + what);

    // GOAL: the still-image and video renderers keep the same region of the same image on the same
    //       monitor.
    // WHY-THIS-SHAPE: compared pixel for pixel against the video renderer, not against the cover
    //       rule, because the property is agreement, and the video probe's own geometry check
    //       samples 25 points at a 4-pixel tolerance, which a one-texel drift in the blit passes.
    //       A 24-texel ramp makes a one-texel shift 11 codes, well over the limit. Interior only;
    //       the edge is graded separately below.
    // CONTROL: each reverted, all three tiers on both ICDs. (a) the blit's `srcOffsets[0]` x in
    //       renderer/renderers/video_renderer.cpp given `crop.x0 + 1` -> all three cases red (11
    //       to 20 codes), while video_renderer_probe and phase 1 stayed green. (b)
    //       renderer/renderers/static_renderer.cpp pushing `crop.x0 + 1` as the origin -> all
    //       three red (12 to 24 codes); phase 1 catches that one too. Mutating
    //       renderer/renderers/cover_crop.h leaves this green, as it should: both renderers move.
    check(worstInterior <= kAgreementCodes, "the still image and the video show the same region, for: " + what);
    if (worstInterior > kAgreementCodes) {
        std::fprintf(stderr, "  inside the crop the renderers differ by up to %d codes, first at (%u,%u)\n",
                     worstInterior, worstInteriorX, worstInteriorY);
    }

    // GOAL: at a cropped edge under magnification the still image blends in the texel beyond the
    //       crop exactly as the video renderer does. That blending is accepted behaviour (GOALS.md
    //       S13.113), so this grades agreement and never its absence.
    // WHY-THIS-SHAPE: blue is 255 beyond the crop and 0 inside it, so an edge pixel's blue is the
    //       weight its filter gave the outside texel -- about 96 at a 4x magnification. Asserting
    //       blue 0 would assert the accepted behaviour away; asserting only agreement would pass
    //       vacuously if neither renderer reached the marker, hence the check on the video's edge.
    // CONTROL: renderer/shaders/fullscreen_set.frag clamping its sample to half a texel inside the
    //       crop -> "blends in the same neighbouring texels" red for all three cases on the buffer
    //       and classic tiers on both ICDs (96 and 99 codes), interior agreement and phase 1
    //       green, heap tier green (it uses fullscreen_heap.frag). Reverted.
    // COVERAGE-ASSERT: the video's edge must show the marker, so a fixture or target change that
    //       stops the filter reaching past the crop fails instead of comparing two unmarked edges.
    //       Observed red with the fixture's outside blue set to 0, while the edge agreement beside
    //       it passed at 0 codes.
    check(videoEdgeBlue >= kMarkerVisible,
          "the edge fixture still discriminates: the video's cropped edge reads the texel beyond the crop, for: " + what);
    check(worstEdge <= kAgreementCodes,
          "at the cropped edge the still image blends in the same neighbouring texels as the video, for: " + what);
    if (worstEdge > kAgreementCodes || videoEdgeBlue < kMarkerVisible) {
        std::fprintf(stderr, "  at the edge the renderers differ by up to %d codes, first at (%u,%u); video edge blue %d\n",
                     worstEdge, worstEdgeX, worstEdgeY, videoEdgeBlue);
    }
}

void runAgreementCases(Device& device, const std::string& tier) {
    OffscreenTargetManager targets(device);
    // 64x48 and 70x50 both keep columns [4, 20) of the wide fixture; 48x64 keeps rows [4, 20) of
    // the tall one. Blue marks everything outside those.
    const Fixture wide = makeFixture(24, 12, 4, 20, 0, 12, 0, 255);
    const Fixture tall = makeFixture(12, 24, 0, 12, 4, 20, 0, 255);
    const std::string widePath = writeFixture(wide, "agree_wide");
    const std::string tallPath = writeFixture(tall, "agree_tall");
    gradeAgreement(device, targets, 1, wide, widePath, 64, 48, tier + ": a wide image, whole-texel crop");
    gradeAgreement(device, targets, 3, wide, widePath, 70, 50, tier + ": a wide image, crop not whole texels");
    gradeAgreement(device, targets, 5, tall, tallPath, 48, 64, tier + ": a tall image");
}

constexpr int kCasesPerTier = 8;

// GOAL: every tier draws the cropped image with zero validation messages, and each tier is really
//       the one named.
// WHY-THIS-SHAPE: one Device per tier, each graded on its own, because the tiers build different
//       pipeline layouts and a message on one must name that tier. Heap cannot be forced -- it is
//       chosen whenever the driver has it -- so it is taken from the default and a non-heap result
//       goes through heapTierGuard's policy. Forcing buffer is advisory too: a driver without
//       VK_EXT_descriptor_buffer falls back, a host fact rather than a defect, so that alone is a
//       skip. A skipped tier prints no pass line and the run exits 77, because a tier nothing was
//       drawn on must not read as a graded one -- this is the path CI takes wherever the heap tier
//       is absent. A driver that has the extension and ignored the override still fails.
// CONTROL: `pipelineLayoutInfo.pushConstantRangeCount = 1;` deleted from
//       renderer/vulkan/descriptor_classic.cpp -> "FAIL: static_renderer_probe (cover, classic
//       tier): 14 Vulkan validation warning(s)" (VUID-VkGraphicsPipelineCreateInfo-layout-07987,
//       VUID-vkCmdPushConstants-offset-01795), heap and buffer green; the same line deleted from
//       renderer/vulkan/descriptor_buffer.cpp -> the buffer tier alone. Both ICDs, both reverted.
//       NO CONTROL FOR THE HEAP TIER'S TEXTURE INDEX READS AS A REPORT: with
//       renderer/shaders/fullscreen_heap.frag's index at `offset = 0`, or
//       renderer/renderers/static_renderer.cpp pushing the index plus one, Lavapipe segfaults
//       with no validation message first -- red to ctest, but without diagnostics. Not run on
//       NVIDIA, where a wild heap index is a GPU fault.
//       THE SKIP PATHS CARRY CONTROLS OF THEIR OWN, because a tier nothing drew on must never read
//       as a graded one. heapTierGuard (support/descriptor_probe_support.h) forced to report no
//       heap tier, with WALLPAPERX_ALLOW_NO_HEAP_TIER=1 -> no "heap tier: passed" line, "NOT
//       VERIFIED ... reported to ctest as a skip", exit 77 on both ICDs; with this file's
//       g_notVerified return deleted, that same run printed "(cover, heap tier): passed" and exited
//       0, which is the state this guards against. The buffer case's capability test forced false,
//       its tier forced to classic -> "(cover, buffer tier): skipped, this driver has no
//       VK_EXT_descriptor_buffer", exit 77; with the real capability and the same forced classic ->
//       "FAIL: ... did not select the tier, it resolved to Classic", exit 1, so conditioning the
//       skip on the capability did not cost the check its teeth. All reverted.
// COVERAGE-ASSERT: the tiers that ran and the tiers skipped with a printed reason must add to
//       three; observed red with the classic tier dropped from the list.
bool runCoverOnEveryTier() {
    struct TierCase {
        const char* name;
        const char* force;
        DescriptorTier expected;
    };
    const TierCase tiers[] = {
        {"heap", nullptr, DescriptorTier::Heap},
        {"buffer", "buffer", DescriptorTier::Buffer},
        {"classic", "classic", DescriptorTier::Classic},
    };

    bool allGraded = true;
    int tiersRun = 0;
    int tiersSkipped = 0;
    for (const TierCase& tier : tiers) {
        if (tier.force != nullptr) {
            setenv("WALLPAPERX_FORCE_TIER", tier.force, 1);
        } else {
            unsetenv("WALLPAPERX_FORCE_TIER");
        }
        const std::string label = std::string("static_renderer_probe (cover, ") + tier.name + " tier)";
        const int failuresBefore = wallpaperx::testing::failureCount();
        bool skipped = false;

        const bool graded = wallpaperx::testing::gradedRun(label, [&](wallpaperx::testing::ValidationGrade& grade) {
            try {
                Device device(/*enableValidation=*/true);
                grade.watch(device);
                if (tier.expected == DescriptorTier::Heap) {
                    if (const auto early = wallpaperx::testing::heapTierGuard(device, "static_renderer_probe", "cover")) {
                        if (*early) {
                            ++tiersSkipped;
                            skipped = true;
                            g_notVerified = true;
                        } else {
                            fail(label + ": this driver resolved to another tier where the heap tier was expected");
                        }
                        return *early;
                    }
                } else if (device.descriptorAllocator().tier() != tier.expected) {
                    // Forcing a tier is a request, not a guarantee: selectTier falls back when the
                    // driver has no VK_EXT_descriptor_buffer, which is a fact about the host and
                    // not a renderer defect. Conditioned on the capability, not on the resolved
                    // tier, so a driver that HAS the extension and ignored the override still
                    // fails; classic cannot fall back at all.
                    if (tier.expected == DescriptorTier::Buffer && !device.info().hasDescriptorBuffer) {
                        std::printf("%s: skipped, this driver has no VK_EXT_descriptor_buffer\n", label.c_str());
                        ++tiersSkipped;
                        skipped = true;
                        g_notVerified = true;
                        return true;
                    }
                    fail(label + ": WALLPAPERX_FORCE_TIER did not select the tier, it resolved to " +
                         wallpaperx::toString(device.descriptorAllocator().tier()));
                    return false;
                }

                const int casesBefore = g_coverCases + g_agreementCases;
                runCoverCases(device, tier.name);
                runAgreementCases(device, tier.name);
                check(g_coverCases + g_agreementCases - casesBefore == kCasesPerTier,
                      label + ": every case was rendered and read back");
                ++tiersRun;
            } catch (const std::exception& e) {
                fail(label + " threw: " + e.what());
            }
            return wallpaperx::testing::failureCount() == failuresBefore;
        });
        if (graded && !skipped) std::printf("%s: passed, zero validation messages\n", label.c_str());
        allGraded = allGraded && graded;
    }
    unsetenv("WALLPAPERX_FORCE_TIER");

    check(tiersRun + tiersSkipped == 3, "the cover cases reached every descriptor tier or said why not");
    return allGraded;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= testRenderValidImage();
    ok &= testMissingImageThrows();
    ok &= runCoverOnEveryTier();
    const int failures = wallpaperx::testing::reportFailures("static_renderer_probe");
    // Failures first: a run that is both broken and inconclusive must report broken.
    if (!ok || failures != 0) return 1;
    if (g_notVerified) {
        std::printf("static_renderer_probe: NOT VERIFIED -- a descriptor tier could not be put in the state these "
                    "phases grade, reported to ctest as a skip, not a pass\n");
        return kNotVerifiedExitCode;
    }
    return 0;
}
