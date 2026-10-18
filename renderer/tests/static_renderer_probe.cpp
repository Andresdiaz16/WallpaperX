// SPDX-License-Identifier: GPL-2.0-or-later
//
// The still-image renderer: that it draws
// "scaled and cropped" -- uniformly scaled to cover the monitor, centred, overflow cropped -- on
// every descriptor tier, showing the same region the video renderer shows; and that every position
// mode lays the picture where its rule says, identically in both renderers.
//
// HOW THE GEOMETRY IS READ BACK. The fixtures are ramps: red rises with x, green with y. A monitor
// pixel's red and green name the image position it was sampled from, so the region shown is
// recovered from the rendered frame and compared against the cover rule in real arithmetic
// (support/cover_expectation.h) -- never against placedRegion(), which is what is under test.

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
#include <iterator>
#include <memory>
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

// The image coordinate a linear filter reads at a monitor pixel centre, for the real crop.
double expectedSample(double origin, double extent, uint32_t pixel, uint32_t targetExtent, uint32_t imageExtent) {
    const double continuous = origin + (pixel + 0.5) * extent / targetExtent;
    return std::clamp(continuous - 0.5, 0.0, static_cast<double>(imageExtent - 1));
}

CoverReport measureCover(const std::vector<unsigned char>& pixels, uint32_t imageWidth, uint32_t imageHeight,
                         uint32_t targetWidth, uint32_t targetHeight) {
    const CropRect crop = expectedCover(imageWidth, imageHeight, targetWidth, targetHeight);

    CoverReport report;
    // GOAL: S15.760 -- a still's cut edge is exactly where cover cuts, not rounded to a whole texel.
    // WHY-THIS-SHAPE: one ramp code of the fixture's rounding and one of the output's and a quarter
    //       texel of sampler precision, with no rounding allowance: cover's uncut edges are the
    //       monitor's own and its cut edges are sampled exactly. A half-texel allowance, which the
    //       video's blit is owed, would pass a still cropped to whole texels.
    // CONTROL: renderer/renderers/static_renderer.cpp's record() sampling region->source in place of
    //       region->exact -> "a still's crop that cuts mid-texel is sampled exactly there" red on
    //       every tier, both ICDs (worst miss 0.51 texels, tolerance 0.29); the 200x150 case stays
    //       green, its quarter-texel rounding being under its ramp's 0.58-texel code. Reverted.
    report.toleranceX = (imageWidth - 1) / 255.0 + 0.25;
    report.toleranceY = (imageHeight - 1) / 255.0 + 0.25;
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

    // GOAL: S13.110 -- the image is not mirrored or flipped -- its left and top edges land on the monitor's
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

    // GOAL: S13.108 -- an image whose aspect differs from the monitor's is uniformly scaled to cover it,
    //       centred, the overflow cropped -- never stretched, letterboxed or anchored at a corner --
    //       and one whose aspect matches is shown whole.
    // WHY-THIS-SHAPE: every monitor pixel is compared against a recovered image coordinate, not a
    //       reference image: a reference would make one driver's filter output the contract. Both
    //       arms of the crop and the no-crop case are separate branches in the rule, and 200x150 on
    //       160x90 is the case whose crop is not whole texels, so rounding is exercised as well as
    //       the exact cases.
    // CONTROL: each via control_batch, reverted, on all three tiers on both ICDs. (a)
    //       renderer/renderers/cover_crop.h placedRegion's Cover scale `std::max(widthRatio,
    //       heightRatio)` -> `widthRatio`: the wider-than-monitor case alone red in this phase,
    //       "96 of 9216 pixels came from the right column (worst miss 95.17 texels, tolerance
    //       0.75)". (b) its Cover `left` and `top` set to 0, an anchored crop: the five cropped
    //       cases red, each on its cropped axis, the matching case green. (c)
    //       renderer/renderers/static_renderer.cpp's record() sampling the whole texture: the five
    //       cropped cases red, the matching case green, and phase 2's agreement red.
    // COVERAGE-ASSERT: every case asserts all of its monitor's pixels were compared, and the tier
    //       asserts all eight of its cases ran. Both observed red: the first with measureCover's
    //       row loop stopping one row short, the second with one agreement case removed. The
    //       verdicts in gradeCover are gated on that first assert rather than repeating it:
    //       measured, the short readback reports one failure per case with the gate, and a
    //       geometry message for every case without it.
    const std::string widePath = writeFixture(rampFixture(128, 64), "wide");
    const std::string tallPath = writeFixture(rampFixture(64, 128), "tall");
    const std::string oddPath = writeFixture(rampFixture(200, 150), "odd");
    const std::string halfPath = writeFixture(rampFixture(10, 10), "half");
    {
        StaticRenderer renderer(device, targets.renderPass(), widePath, WALLPAPERX_SHADER_DIR);
        gradeCover(device, targets, renderer, 1, 128, 64, 96, 96,
                   tier + ": an image wider than the monitor is cropped left and right, not squeezed");

        // GOAL: S13.109 -- the crop is decided per target. One renderer drawing monitors of three shapes crops
        //       each for its own shape.
        // WHY-THIS-SHAPE: the same renderer as above on purpose -- a fresh renderer per shape would
        //       pass for one that computed the crop once and kept it. 192x36 makes the 2:1 image
        //       taller than the monitor, so the other arm runs through the same renderer.
        // CONTROL: record() in renderer/renderers/static_renderer.cpp keeping the first
        //       placedRegion() it computes for as long as the same renderer address draws -> these
        //       two red on all three tiers on both ICDs ("0 of 6912 pixels came from the right
        //       column (worst miss 127.00 texels)"), and the later renderers too, since a renderer
        //       built at a reused address inherits the region; the first case green. Reverted.
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
    {
        // Rows [2.5, 7.5) at 4x: half a texel from whole, and a ramp step of 28 codes, so a still
        // cropped to whole texels misses by 0.5 against a tolerance of 0.29.
        StaticRenderer renderer(device, targets.renderPass(), halfPath, WALLPAPERX_SHADER_DIR);
        gradeCover(device, targets, renderer, 6, 10, 10, 40, 20,
                   tier + ": a still's crop that cuts mid-texel is sampled exactly there");
    }
}

// ------------------------------------------------------------------------- //
// Phase 2 -- the still image and the video show the same region.
// ------------------------------------------------------------------------- //

// Largest per-channel difference between two draws of one sample position: one code of each one's
// rounding, and a sixteenth of a texel of filter position (the minimum sub-texel precision Vulkan
// allows) on the steepest ramp here, 23 codes per texel.
constexpr int kAgreementCodes = 4;
// A monitor edge pixel whose blue is at least this read the texel beyond the crop.
constexpr int kMarkerVisible = 32;
// Blue a linear filter may be off by: half a weight step at the minimum 4 sub-texel bits, plus
// one code of output rounding.
constexpr double kFilterCodes = 255.0 / 32 + 1;

// How far the video's whole-texel crop may sit from the exact one on an axis: the larger distance
// from a cut edge to its nearest whole texel. The still samples the exact edges and the video the
// rounded ones, and both map linearly between them, so no sample on the axis moves further.
double cutEdgeDrift(double exact0, double exact1) {
    return std::max(std::abs(exact0 - std::round(exact0)), std::abs(exact1 - std::round(exact1)));
}

// Codes a ramp of `extent` texels changes over `texels`.
double rampCodes(double texels, uint32_t extent) { return texels * 255.0 / (extent - 1); }

// The weight a linear filter at texel coordinate `u` gives texels inside [in0, in1), clamped to edge.
double weightInside(double u, int in0, int in1, uint32_t extent) {
    const double t = u - 0.5;
    const double base = std::floor(t);
    const double f = t - base;
    auto inside = [&](double i) {
        const int texel = static_cast<int>(std::clamp(i, 0.0, static_cast<double>(extent - 1)));
        return texel >= in0 && texel < in1 ? 1.0 : 0.0;
    };
    return (1 - f) * inside(base) + f * inside(base + 1);
}

int g_agreementCases = 0;

struct EdgeMarker {
    double worstMiss = 0;
    double mostExpected = 0;
    uint32_t x = 0, y = 0;
};

// Blue against what a linear filter reads across [x0, x1) x [y0, y1) of the fixture, whose marked
// texels lie outside [mx0, mx1) x [my0, my1).
EdgeMarker gradeMarker(const std::vector<unsigned char>& pixels, const Fixture& f, double x0, double y0, double x1,
                       double y1, int mx0, int mx1, int my0, int my1, uint32_t tw, uint32_t th) {
    EdgeMarker m;
    for (uint32_t y = 0; y < th; ++y) {
        for (uint32_t x = 0; x < tw; ++x) {
            const double u = x0 + (x + 0.5) * (x1 - x0) / tw;
            const double v = y0 + (y + 0.5) * (y1 - y0) / th;
            const double expected =
                255 * (1 - weightInside(u, mx0, mx1, f.width) * weightInside(v, my0, my1, f.height));
            const double miss = std::abs(pixels[(static_cast<size_t>(y) * tw + x) * 4] - expected);
            m.mostExpected = std::max(m.mostExpected, expected);
            if (miss > m.worstMiss) m.worstMiss = miss, m.x = x, m.y = y;
        }
    }
    return m;
}

void gradeAgreement(Device& device, OffscreenTargetManager& targets, int monitorId, const Fixture& fixture,
                    const std::string& path, int mx0, int mx1, int my0, int my1, uint32_t targetWidth,
                    uint32_t targetHeight, const std::string& what) {
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
    const double cx1 = crop.x0 + crop.width, cy1 = crop.y0 + crop.height;
    // Red (byte 2) follows x and green (byte 1) y; blue carries the marker and is graded per renderer.
    const double allowance[4] = {0, kAgreementCodes + rampCodes(cutEdgeDrift(crop.y0, cy1), fixture.height),
                                 kAgreementCodes + rampCodes(cutEdgeDrift(crop.x0, cx1), fixture.width),
                                 kAgreementCodes};
    size_t compared = 0;
    double worst = 0;
    uint32_t worstX = 0, worstY = 0;
    EdgeMarker stillMarker, videoMarker;
    const size_t bytes = static_cast<size_t>(targetWidth) * targetHeight * 4;
    if (stillPixels.size() >= bytes && videoPixels.size() >= bytes) {
        for (uint32_t y = 0; y < targetHeight; ++y) {
            for (uint32_t x = 0; x < targetWidth; ++x, ++compared) {
                const size_t offset = (static_cast<size_t>(y) * targetWidth + x) * 4;
                for (int channel = 1; channel < 4; ++channel) {
                    const double over = std::abs(static_cast<int>(stillPixels[offset + channel]) -
                                                 static_cast<int>(videoPixels[offset + channel])) -
                                        allowance[channel];
                    if (over > worst) worst = over, worstX = x, worstY = y;
                }
            }
        }
        stillMarker = gradeMarker(stillPixels, fixture, crop.x0, crop.y0, cx1, cy1, mx0, mx1, my0, my1, targetWidth,
                                  targetHeight);
        videoMarker = gradeMarker(videoPixels, fixture, std::round(crop.x0), std::round(crop.y0), std::round(cx1),
                                  std::round(cy1), mx0, mx1, my0, my1, targetWidth, targetHeight);
    }

    check(compared == static_cast<size_t>(targetWidth) * targetHeight,
          "every monitor pixel was compared, for: " + what);

    // GOAL: S13.111 -- the still-image and video renderers keep the same region of the same image on the same
    //       monitor, apart from the video's rounding of a cut edge to a whole texel.
    // WHY-THIS-SHAPE: compared pixel for pixel against the video renderer, not against the cover
    //       rule, because the video probe's own geometry check samples 25 points at a 4-pixel
    //       tolerance, which a one-texel drift passes. The allowance per channel is the codes its
    //       ramp moves over the derived cut-edge drift (cutEdgeDrift), so the whole-texel cases keep
    //       4 codes and a one-texel shift, 11 codes on the 24-texel ramp, stays red everywhere.
    // CONTROL: via control_batch, every tier, both ICDs, reverted: (a) the blit's `srcOffsets[0]`
    //       x in renderer/renderers/video_renderer.cpp given `region->source.x0 + 1` -> both wide
    //       cases red (7.0 and 7.6 codes over the allowance); (b)
    //       renderer/renderers/static_renderer.cpp pushing `shown.x0 + 1` as the origin -> the
    //       same (8.0 and 7.6). Changing placedRegion's scale leaves this green: both renderers move.
    check(worst <= 0, "the still image and the video show the same region, for: " + what);
    if (worst > 0) {
        std::fprintf(stderr, "  the renderers differ by %.1f codes more than allowed, first at (%u,%u)\n", worst,
                     worstX, worstY);
    }

    // GOAL: S15.761 -- under magnification each renderer blends in the texel beyond a cropped edge by
    //       exactly its own crop's share: the still by the exact cover crop, the video by that crop
    //       rounded to whole texels. That blending is accepted behaviour (S13.113), so this grades
    //       its amount and never its absence.
    // WHY-THIS-SHAPE: blue is 255 on the marked texels and 0 elsewhere, so a pixel's blue is the
    //       weight its filter gave the marked texels, worked out here for each crop. Agreement
    //       between the two cannot be graded any more: at a cut edge not on a whole texel the still
    //       reads a part of the marker texel that the video's rounded crop leaves out.
    // CONTROL: via control_batch, every tier, both ICDs, reverted. renderer/renderers/static_renderer.cpp
    //       sampling region->source -> the still's check red on "crop not whole texels" (99.9
    //       codes off), the video's green; renderer/renderers/cover_crop.h placeSpan rounding
    //       source0 with std::floor -> the video's red (228 codes off), the still's green.
    // COVERAGE-ASSERT: both expectations must reach the marker somewhere, so a fixture or target that
    //       stops the filter reaching past the crop fails instead of comparing unmarked edges.
    check(stillMarker.mostExpected >= kMarkerVisible && videoMarker.mostExpected >= kMarkerVisible,
          "the edge fixture still discriminates: the cropped edge reads the texel beyond the crop, for: " + what);
    check(stillMarker.worstMiss <= kFilterCodes,
          "at the cropped edge the still image blends in the marker by the exact crop's share, for: " + what);
    check(videoMarker.worstMiss <= kFilterCodes,
          "at the cropped edge the video blends in the marker by the whole-texel crop's share, for: " + what);
    if (stillMarker.worstMiss > kFilterCodes || videoMarker.worstMiss > kFilterCodes) {
        std::fprintf(stderr, "  marker blue off by up to %.1f codes on the still at (%u,%u), %.1f on the video at (%u,%u)\n",
                     stillMarker.worstMiss, stillMarker.x, stillMarker.y, videoMarker.worstMiss, videoMarker.x,
                     videoMarker.y);
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
    gradeAgreement(device, targets, 1, wide, widePath, 4, 20, 0, 12, 64, 48, tier + ": a wide image, whole-texel crop");
    gradeAgreement(device, targets, 3, wide, widePath, 4, 20, 0, 12, 70, 50,
                   tier + ": a wide image, crop not whole texels");
    gradeAgreement(device, targets, 5, tall, tallPath, 0, 12, 4, 20, 48, 64, tier + ": a tall image");
}

// ------------------------------------------------------------------------- //
// Phase 3 -- every position mode, both renderers.
// ------------------------------------------------------------------------- //

using wallpaperx::Placement;
using wallpaperx::PositionMode;

// Where the mode puts the picture, worked out here in real arithmetic from the rules in
// shared/wallpaper_position.h -- never through placedRegion(), which is what is under test.
struct Placed {
    double left, top, scaleX, scaleY;
};

Placed expectedPlacement(const Placement& p, double sw, double sh, double tw, double th) {
    const double cover = std::max(tw / sw, th / sh);
    switch (p.mode) {
        case PositionMode::Cover: return {(tw - sw * cover) / 2, (th - sh * cover) / 2, cover, cover};
        case PositionMode::Fit: {
            const double fit = std::min(tw / sw, th / sh);
            return {(tw - sw * fit) / 2, (th - sh * fit) / 2, fit, fit};
        }
        case PositionMode::Centre:
        case PositionMode::Tile: return {std::floor((tw - sw) / 2), std::floor((th - sh) / 2), 1, 1};
        case PositionMode::Stretch: return {0, 0, tw / sw, th / sh};
        case PositionMode::Free: {
            const double s = p.zoom * cover;
            return {tw * (0.5 + p.x) - sw * s / 2, th * (0.5 + p.y) - sh * s / 2, s, s};
        }
    }
    return {};
}

bool whole(double v) { return std::abs(v - std::round(v)) < 1e-9; }

struct AxisRule {
    double start, end, scale;
    bool exact;  // visible edges on whole pixels and whole texels: no rounding anywhere
};

AxisRule axisRule(double start, double scale, uint32_t source, uint32_t target) {
    const double end = start + source * scale;
    const double t0 = std::clamp(start, 0.0, double(target)), t1 = std::clamp(end, 0.0, double(target));
    return {start, end, scale, whole(t0) && whole(t1) && whole((t0 - start) / scale) && whole((t1 - start) / scale)};
}

enum class PixelClass { Outside, Inside, Edge };

// A pixel centre within half a pixel of an edge that is not on a whole pixel may land either side.
PixelClass classify(double centre, const AxisRule& a, bool tile) {
    if (tile) return PixelClass::Inside;
    const double band = a.exact ? 0.0 : 0.5;
    if (centre < a.start - band || centre > a.end + band) return PixelClass::Outside;
    if (centre > a.start + band && centre < a.end - band) return PixelClass::Inside;
    return PixelClass::Edge;
}

// The texel a pixel centre should read, and how far off whole-texel rounding may put it: half a
// texel or half a pixel's worth of texels at each end, whichever is more, where the axis is not exact.
double expectedTexel(double centre, const AxisRule& a, bool tile, uint32_t source) {
    if (tile) {
        const double local = std::fmod(std::fmod(centre - a.start, source) + source, source);
        return local - 0.5;
    }
    return std::clamp((centre - a.start) / a.scale - 0.5, 0.0, double(source - 1));
}

// The video's blit rounds every edge: a cut edge to the nearest whole texel, an uncut one to the
// nearest pixel. A still samples its cut edges exactly, so it is owed only the uncut edges' half
// pixel, 0.5/scale texels, and only where such an edge is not on a whole pixel.
double texelTolerance(const AxisRule& a, uint32_t source, uint32_t target, bool still) {
    const double base = (source - 1) / 255.0 + 0.25;
    if (a.exact) return base;
    if (!still) return base + 0.5 * std::max(1.0, 1.0 / a.scale);
    const bool offPixel = (a.start >= 0 && !whole(a.start)) || (a.end <= target && !whole(a.end));
    return base + (offPixel ? 0.5 / a.scale : 0.0);
}

struct PlacedReport {
    size_t inside = 0, outside = 0, edge = 0;
    size_t notBlack = 0, notPicture = 0, wrongTexel = 0;
    uint32_t firstX = 0, firstY = 0;
    double worstMiss = 0;
};

PlacedReport measurePlaced(const std::vector<unsigned char>& pixels, const Placement& placement, const Fixture& f,
                           uint32_t tw, uint32_t th, bool still) {
    PlacedReport r;
    if (pixels.size() < static_cast<size_t>(tw) * th * 4) return r;
    const Placed e = expectedPlacement(placement, f.width, f.height, tw, th);
    const bool tile = placement.mode == PositionMode::Tile;
    const AxisRule ax = axisRule(e.left, e.scaleX, f.width, tw), ay = axisRule(e.top, e.scaleY, f.height, th);
    const double tolX = texelTolerance(ax, f.width, tw, still), tolY = texelTolerance(ay, f.height, th, still);
    auto note = [&](size_t& counter, uint32_t x, uint32_t y) {
        if (r.notBlack + r.notPicture + r.wrongTexel == 0) r.firstX = x, r.firstY = y;
        ++counter;
    };
    for (uint32_t y = 0; y < th; ++y) {
        for (uint32_t x = 0; x < tw; ++x) {
            const unsigned char* q = &pixels[(static_cast<size_t>(y) * tw + x) * 4];
            const PixelClass cx = classify(x + 0.5, ax, tile), cy = classify(y + 0.5, ay, tile);
            if (cx == PixelClass::Outside || cy == PixelClass::Outside) {
                ++r.outside;
                if (q[0] > 1 || q[1] > 1 || q[2] > 1) note(r.notBlack, x, y);
            } else if (cx == PixelClass::Inside && cy == PixelClass::Inside) {
                ++r.inside;
                if (std::abs(q[0] - kConstantBlue) > 3) {
                    note(r.notPicture, x, y);
                    continue;
                }
                const double missX = std::abs(q[2] * (f.width - 1) / 255.0 - expectedTexel(x + 0.5, ax, tile, f.width));
                const double missY =
                    std::abs(q[1] * (f.height - 1) / 255.0 - expectedTexel(y + 0.5, ay, tile, f.height));
                r.worstMiss = std::max({r.worstMiss, missX, missY});
                if (missX > tolX || missY > tolY) note(r.wrongTexel, x, y);
            } else {
                ++r.edge;
            }
        }
    }
    return r;
}

int g_placedCases = 0;
int g_placedAgreements = 0;

// Grades one read-back; true when the picture landed at all, for the caller's coverage count.
bool gradePlaced(const std::vector<unsigned char>& pixels, const Placement& placement, const Fixture& f, uint32_t tw,
                 uint32_t th, const std::string& what, bool fullyOff, bool still) {
    const PlacedReport r = measurePlaced(pixels, placement, f, tw, th, still);
    ++g_placedCases;
    const size_t total = static_cast<size_t>(tw) * th;
    // COVERAGE-ASSERT: every pixel is classified, and a placement that shows the picture must
    // have graded some of it -- an edge band that swallowed the whole picture would grade nothing.
    // Observed red with the non-exact band widened to 500 pixels; the per-tier count of graded
    // cases observed red with the stretch cases skipped.
    check(r.inside + r.outside + r.edge == total && (fullyOff ? r.outside == total : r.inside > 0),
          "every pixel was classified and the picture was graded where it lands, for: " + what);
    // GOAL: S15.700, S15.701 -- black exactly where the mode leaves the screen uncovered, the
    //       picture everywhere it is placed, and each placed pixel read from the source texel the
    //       mode's scale and offset put there (uniform scale on both axes except in stretch).
    // WHY-THIS-SHAPE: graded against geometry derived here from the stated rules, not against
    //       placedRegion(): calling it would grade the function against itself. Tolerance is zero
    //       rounding where every visible edge falls on whole pixels and texels; otherwise the video
    //       is owed half a texel for its blit and a still only half a pixel at an uncut edge
    //       (texelTolerance), so a still cropped to whole texels like the video fails.
    // CONTROL: each via control_batch, red on all tiers and both ICDs, reverted. In
    //       renderer/renderers/cover_crop.h: Fit's min -> max, "not black ... fit"; Fit's y axis
    //       scaled to the screen's height, the portrait squash, the same; centredStart + 1, "the
    //       picture wherever it is placed ... center"; Free's `0.5 + x` -> `0.5 - x`, free cases
    //       red; placeSpan's source0 + 1 at a cut start edge, "reads the expected source texel ...
    //       free 2,-0.25,0.25 (video)" (a still samples the exact edge, so only the video moves);
    //       Stretch falling into Cover's case, stretch red; a fully-off placement drawn as cover,
    //       "not black ... free 1,5,0"; static_renderer.cpp sampling region->source, "reads the
    //       expected source texel ... cover (default) (still)" and free stills. Grey clears in
    //       static_renderer.cpp and video_renderer.cpp, "not black" for that renderer; the tile
    //       push flag forced 0, fullscreen_set.frag without fract() (buffer and classic only) and
    //       fullscreen_heap.frag without it (heap only), "reads the expected source texel ... tile".
    check(r.notBlack == 0, "black wherever the picture does not reach, for: " + what);
    check(r.notPicture == 0, "the picture wherever it is placed, for: " + what);
    check(r.wrongTexel == 0, "each placed pixel reads the expected source texel, for: " + what);
    if (r.notBlack + r.notPicture + r.wrongTexel > 0) {
        std::fprintf(stderr,
                     "  %ux%u picture on %ux%u: %zu not black, %zu not picture, %zu wrong texel (worst %.2f), first at "
                     "(%u,%u)\n",
                     f.width, f.height, tw, th, r.notBlack, r.notPicture, r.wrongTexel, r.worstMiss, r.firstX, r.firstY);
    }
    return r.inside > 0;
}

struct ModeCase {
    Placement placement;
    const char* name;
    bool fullyOff;
};

const ModeCase kModeCases[] = {
    {{PositionMode::Cover}, "cover (default)", false},
    {{PositionMode::Fit}, "fit", false},
    {{PositionMode::Centre}, "center", false},
    {{PositionMode::Tile}, "tile", false},
    {{PositionMode::Stretch}, "stretch", false},
    {{PositionMode::Free, 1.0, 0.0, 0.0}, "free 1,0,0", false},
    {{PositionMode::Free, 0.5, 0.25, -0.125}, "free 0.5,0.25,-0.125", false},
    {{PositionMode::Free, 2.0, -0.25, 0.25}, "free 2,-0.25,0.25", false},
    {{PositionMode::Free, 0.5, 0.6, 0.0}, "free 0.5,0.6,0 (partly off screen)", false},
    {{PositionMode::Free, 1.0, 5.0, 0.0}, "free 1,5,0 (fully off screen)", true},
};
constexpr int kModeCaseCount = static_cast<int>(std::size(kModeCases));

struct Screen {
    uint32_t width, height;
};
// 36x64 is a portrait screen under a landscape picture; 128x128 makes the 64x32 fixture's fit,
// centre and free edges fall on whole pixels, so those cases carry no rounding allowance.
const Screen kScreens[] = {{36, 64}, {128, 128}, {96, 54}, {40, 20}};
constexpr int kScreenCount = static_cast<int>(std::size(kScreens));
constexpr int kPositionFixtures = 2;

// The cut-edge drift (cutEdgeDrift) of one axis of a placement: zero where neither edge is cut.
double placedDrift(double start, double scale, uint32_t source, uint32_t target) {
    const double exact0 = start < 0 ? -start / scale : 0.0;
    const double exact1 = start + source * scale > target ? (target - start) / scale : source;
    return cutEdgeDrift(exact0, exact1);
}

int g_coverFreeComparisons = 0;

void runPositionCases(Device& device, const std::string& tier) {
    OffscreenTargetManager targets(device);
    const Fixture fixtures[kPositionFixtures] = {rampFixture(64, 32), rampFixture(50, 30)};
    const std::string paths[kPositionFixtures] = {writeFixture(fixtures[0], "pos_wide"),
                                                  writeFixture(fixtures[1], "pos_odd")};
    int monitorId = 100;
    for (int which = 0; which < kPositionFixtures; ++which) {
        const Fixture& f = fixtures[which];
        StaticRenderer still(device, targets.renderPass(), paths[which], WALLPAPERX_SHADER_DIR);
        wallpaperx::testing::LentFrame frame(device, f.width, f.height, VK_IMAGE_LAYOUT_GENERAL, toBgra(f.rgba),
                                             "position-frame");
        VideoRenderer video(device, frame.frame());
        std::vector<unsigned char> coverStill[kScreenCount], coverVideo[kScreenCount];
        for (const ModeCase& c : kModeCases) {
            for (int screen = 0; screen < kScreenCount; ++screen) {
                const Screen& s = kScreens[screen];
                const std::string what = tier + ": " + std::to_string(f.width) + "x" + std::to_string(f.height) +
                                         " on " + std::to_string(s.width) + "x" + std::to_string(s.height) + ", " +
                                         c.name;
                still.place(c.placement);
                const auto stillPixels =
                    wallpaperx::testing::renderReference(device, targets, still, monitorId++, s.width, s.height);
                gradePlaced(stillPixels, c.placement, f, s.width, s.height, what + " (still)", c.fullyOff, true);
                if (c.placement.mode == PositionMode::Tile) continue;  // refused by video: video_renderer_probe

                video.place(c.placement);
                const auto videoPixels =
                    wallpaperx::testing::renderReference(device, targets, video, monitorId++, s.width, s.height);
                gradePlaced(videoPixels, c.placement, f, s.width, s.height, what + " (video)", c.fullyOff, false);

                // GOAL: S15.762 -- free mode at zoom 1, offset 0,0 draws exactly what cover draws, for
                //       a still and for a video, on every screen.
                // WHY-THIS-SHAPE: byte for byte, no tolerance: the two are one calculation, so any
                //       difference at all is a second calculation drifting from the first, which
                //       the placement grading above would pass within its rounding allowance.
                // CONTROL: renderer/renderers/cover_crop.h placedRegion adding 0.3 pixel to `left`
                //       in Free only -> both checks red for every screen and fixture, every tier,
                //       both ICDs, via control_batch. Reverted.
                if (c.placement.mode == PositionMode::Cover) {
                    coverStill[screen] = stillPixels;
                    coverVideo[screen] = videoPixels;
                } else if (c.placement.mode == PositionMode::Free && c.placement.zoom == 1.0 &&
                           c.placement.x == 0.0 && c.placement.y == 0.0) {
                    ++g_coverFreeComparisons;
                    check(!stillPixels.empty() && stillPixels == coverStill[screen],
                          "a still in free mode at zoom 1, offset 0,0 is drawn exactly as cover, for: " + what);
                    check(!videoPixels.empty() && videoPixels == coverVideo[screen],
                          "a video in free mode at zoom 1, offset 0,0 is drawn exactly as cover, for: " + what);
                }

                // GOAL: S15.702 -- the still and video renderers draw the same frame in every mode but
                //       tile, edges and bars included, apart from the video's rounding of a cut edge to
                //       a whole texel.
                // WHY-THIS-SHAPE: every byte of the frame, not the interior only: an edge that
                //       differs is a real divergence (a draw and a blit placed on different pixels),
                //       which an interior-only compare would pass. Red and green are allowed the
                //       codes their ramp moves over the axis's cut-edge drift (placedDrift), worked
                //       out from the mode's rule here, so an axis with no cut edge stays at 4 codes.
                // CONTROL: via control_batch, every tier, both ICDs, reverted:
                //       renderer/renderers/video_renderer.cpp's blit dstOffsets[0].x + 1 -> red
                //       across modes (up to 251 codes over); renderer/renderers/cover_crop.h placeSpan
                //       rounding source0 down, a video crop half a texel further than rounding
                //       allows -> red on cut cases of the 50x30 fixture (0.8 and 3.5 codes over).
                const Placed e = expectedPlacement(c.placement, f.width, f.height, s.width, s.height);
                const double allowance[4] = {
                    kAgreementCodes,
                    kAgreementCodes + rampCodes(placedDrift(e.top, e.scaleY, f.height, s.height), f.height),
                    kAgreementCodes + rampCodes(placedDrift(e.left, e.scaleX, f.width, s.width), f.width),
                    kAgreementCodes};
                size_t compared = 0;
                double worst = 0;
                if (stillPixels.size() == videoPixels.size() &&
                    stillPixels.size() >= static_cast<size_t>(s.width) * s.height * 4) {
                    for (size_t i = 0; i < static_cast<size_t>(s.width) * s.height * 4; ++i, ++compared) {
                        worst = std::max(worst, std::abs(int(stillPixels[i]) - int(videoPixels[i])) - allowance[i % 4]);
                    }
                }
                ++g_placedAgreements;
                check(compared == static_cast<size_t>(s.width) * s.height * 4,
                      "every byte of both frames was compared, for: " + what);
                check(worst <= 0, "the still and the video draw the same frame, for: " + what);
                if (worst > 0) std::fprintf(stderr, "  they differ by %.1f codes more than allowed\n", worst);
            }
        }
    }
}

// ------------------------------------------------------------------------- //
// Phase 3a' -- a placement thinner than a pixel.
// ------------------------------------------------------------------------- //

int g_thinCases = 0;

struct ThinCase {
    uint32_t sourceWidth, sourceHeight;
    Placement placement;
    const char* name;
    // The one column or row the picture must land on; -1 where that axis is covered whole.
    int column, row;
};

// On 96x54. Fit of 1000x2 is 0.192 pixels tall; the Free cases leave 0.3 pixel of a 64x32 image
// on the screen's left or right edge (cover scale 1.6875, so x = -+1.059375).
constexpr uint32_t kThinWidth = 96, kThinHeight = 54;
const ThinCase kThinCases[] = {
    {1000, 2, {PositionMode::Fit}, "fit 1000x2 (0.192 pixel tall)", -1, 27},
    {2, 1000, {PositionMode::Fit}, "fit 2x1000 (0.108 pixel wide)", 48, -1},
    {64, 32, {PositionMode::Free, 1.0, -1.059375, 0.0}, "free, 0.3 pixel left on the left edge", 0, -1},
    {64, 32, {PositionMode::Free, 1.0, 1.059375, 0.0}, "free, 0.3 pixel left on the right edge", 95, -1},
};
constexpr int kThinCaseCount = static_cast<int>(std::size(kThinCases));

// GOAL: S15.800 -- Fit draws at least one pixel on each axis however elongated the image (exactly one
//       row or column where its span rounds to nothing), and a Free image overlapping the screen by under
//       half a pixel draws a one-pixel line on the edge it overlaps; still and video alike.
// WHY-THIS-SHAPE: graded on the drawn pixels, not on placedRegion(), so a renderer dropping a thin region
//       is caught too. Exactly one line, not "at least one": a widening to more would also pass that.
// CONTROL: control_batch.sh 2026-10-18, each alone, reverted, in renderer/renderers/cover_crop.h placeSpan:
//       the `if (target0 == target1)` widening deleted -> "...draws its one line..." red for all four cases,
//       every tier, NVIDIA and Lavapipe; its far-edge `target0 -= 1` made `target0 -= 0` (NVIDIA) or
//       `target1 += 1` (Lavapipe; on NVIDIA that draw loses the device first) -> the right-edge case alone;
//       the `!(end > 0 && start < limit)` guard deleted -> gradePlaced's "black wherever ... free 1,5,0
//       (fully off screen)", both ICDs.
void runThinCases(Device& device, const std::string& tier) {
    OffscreenTargetManager targets(device);
    int monitorId = 400;
    for (const ThinCase& c : kThinCases) {
        const Fixture f = rampFixture(c.sourceWidth, c.sourceHeight);
        const std::string path = writeFixture(f, "thin_" + std::to_string(c.sourceWidth) + "x" + std::to_string(c.sourceHeight));
        StaticRenderer still(device, targets.renderPass(), path, WALLPAPERX_SHADER_DIR);
        wallpaperx::testing::LentFrame frame(device, f.width, f.height, VK_IMAGE_LAYOUT_GENERAL, toBgra(f.rgba),
                                             "thin-frame");
        VideoRenderer video(device, frame.frame());
        still.place(c.placement);
        video.place(c.placement);
        const std::pair<const char*, std::vector<unsigned char>> drawn[] = {
            {"still", wallpaperx::testing::renderReference(device, targets, still, monitorId++, kThinWidth, kThinHeight)},
            {"video", wallpaperx::testing::renderReference(device, targets, video, monitorId++, kThinWidth, kThinHeight)},
        };
        for (const auto& [renderer, pixels] : drawn) {
            const std::string what = tier + ": " + c.name + " (" + renderer + ")";
            ++g_thinCases;
            if (pixels.size() < static_cast<size_t>(kThinWidth) * kThinHeight * 4) {
                check(false, "the frame was read back, for: " + what);
                continue;
            }
            size_t misplaced = 0, missing = 0;
            for (uint32_t y = 0; y < kThinHeight; ++y) {
                for (uint32_t x = 0; x < kThinWidth; ++x) {
                    const unsigned char* q = &pixels[(static_cast<size_t>(y) * kThinWidth + x) * 4];
                    const bool picture = std::abs(q[0] - kConstantBlue) <= 3;
                    const bool owed = (c.column < 0 || int(x) == c.column) && (c.row < 0 || int(y) == c.row);
                    if (owed && !picture) ++missing;
                    if (!owed && (q[0] > 1 || q[1] > 1 || q[2] > 1)) ++misplaced;
                }
            }
            check(missing == 0 && misplaced == 0,
                  "a placement thinner than a pixel draws its one line and nothing else, for: " + what);
            if (missing + misplaced > 0) {
                std::fprintf(stderr, "  %zu pixel(s) of the line not the picture, %zu outside it not black\n", missing,
                             misplaced);
            }
        }
    }
}

// ------------------------------------------------------------------------- //
// Phase 3b -- panning at high zoom.
// ------------------------------------------------------------------------- //

int g_panSteps = 0;

// The mean image column a frame shows, read off its red ramp.
double meanColumn(const std::vector<unsigned char>& pixels, uint32_t imageWidth) {
    double sum = 0;
    size_t count = 0;
    for (size_t i = 2; i < pixels.size(); i += 4, ++count) sum += pixels[i] * (imageWidth - 1) / 255.0;
    return count ? sum / count : 0;
}

void runPanCases(Device& device, const std::string& tier) {
    OffscreenTargetManager targets(device);
    // 17 codes per texel, and at zoom 4 on 128x128 one texel is 64 pixels, so a quarter texel is
    // 16 pixels: big enough to read, far from a whole texel.
    const Fixture f = rampFixture(16, 8);
    const std::string path = writeFixture(f, "pan");
    constexpr uint32_t kSize = 128;
    constexpr double kZoom = 4.0;
    const double scale = kZoom * std::max(double(kSize) / f.width, double(kSize) / f.height);
    StaticRenderer still(device, targets.renderPass(), path, WALLPAPERX_SHADER_DIR);
    wallpaperx::testing::LentFrame frame(device, f.width, f.height, VK_IMAGE_LAYOUT_GENERAL, toBgra(f.rgba), "pan-frame");
    VideoRenderer video(device, frame.frame());

    // GOAL: S15.763 -- at high zoom a small change of position.x moves a still by the matching
    //       fraction of a texel, and a video by whole texels, the nearest whole number to it.
    // WHY-THIS-SHAPE: the mean column over the whole frame, against the move worked out from Free's
    //       rule (a pixel of x offset is 1/scale texels), in quarter-texel steps: a still stepping
    //       in whole texels misses a quarter-texel step by 0.25, four times the 0.06 allowed. The
    //       video's step must be within half a texel of the exact move (a blit only takes whole
    //       texels, so that is its own bound, not graded).
    // CONTROL: via control_batch, every tier, both ICDs, reverted. renderer/renderers/static_renderer.cpp
    //       sampling region->source -> the still's check red on steps 1-3 ("expected a move of
    //       -0.25 texels: the still moved 0.000"); renderer/renderers/cover_crop.h placeSpan
    //       rounding source0 with std::floor -> the video's red on steps 1 and 2.
    // COVERAGE-ASSERT: all five steps rendered for both renderers, and the exact moves span a full
    //       texel, so the steps cannot all fall on whole texels.
    std::vector<unsigned char> firstStill, firstVideo;
    int graded = 0;
    for (int step = 0; step <= 4; ++step) {
        const Placement p{PositionMode::Free, kZoom, step * 0.25 * scale / kSize, 0.0};
        still.place(p);
        video.place(p);
        const auto stillPixels = wallpaperx::testing::renderReference(device, targets, still, 300 + 2 * step, kSize, kSize);
        const auto videoPixels =
            wallpaperx::testing::renderReference(device, targets, video, 301 + 2 * step, kSize, kSize);
        if (stillPixels.size() < size_t(kSize) * kSize * 4 || videoPixels.size() < size_t(kSize) * kSize * 4) continue;
        if (step == 0) firstStill = stillPixels, firstVideo = videoPixels;
        if (firstStill.empty()) continue;
        const double exact = -step * 0.25;
        const double stillMove = meanColumn(stillPixels, f.width) - meanColumn(firstStill, f.width);
        const double videoMove = meanColumn(videoPixels, f.width) - meanColumn(firstVideo, f.width);
        const std::string what = tier + ": a quarter-texel pan step " + std::to_string(step) + " at zoom 4";
        check(std::abs(stillMove - exact) <= 0.06, "a still pans by the fraction of a texel the offset asks, for: " + what);
        check(std::abs(videoMove - exact) <= 0.5 + 0.06,
              "a video pans by the whole texel nearest the offset's move, for: " + what);
        if (std::abs(stillMove - exact) > 0.06 || std::abs(videoMove - exact) > 0.56) {
            std::fprintf(stderr, "  expected a move of %.2f texels: the still moved %.3f, the video %.3f\n", exact,
                         stillMove, videoMove);
        }
        ++graded;
    }
    ++g_panSteps;
    check(graded == 5, tier + ": every pan step was rendered and graded");
}

// ------------------------------------------------------------------------- //
// Phase 4 -- two layouts from one decoded still.
// ------------------------------------------------------------------------- //

int g_sharedCases = 0;

void runSharedStillCases(Device& device, const std::string& tier) {
    using wallpaperx::StillImage;
    OffscreenTargetManager targets(device);
    const Fixture first = rampFixture(64, 32);
    const std::string path = writeFixture(first, "pos_shared");
    const std::shared_ptr<const StillImage> image = StillImage::load(device, path);

    // GOAL: S15.703 -- two StaticRenderers drawing one StillImage each lay it out by their own
    //       placement, and loading the unchanged file again with that image kept returns the same
    //       image rather than a second decode.
    // WHY-THIS-SHAPE: the pointer identity is load()'s own report of "nothing decoded"; a timing
    //       or allocation count would assert on the driver. One renderer at a time, because the
    //       buffer and classic tiers have a single image slot.
    // CONTROL: renderer/renderers/static_renderer.cpp's `if (kept && kept->m_file == identity)
    //       return kept;` deleted -> "loading an unchanged still again with it kept decodes
    //       nothing" red on every tier, both ICDs, alone. Reverted.
    std::vector<unsigned char> fitPixels, centrePixels;
    {
        StaticRenderer fit(device, targets.renderPass(), image, WALLPAPERX_SHADER_DIR);
        fit.place({PositionMode::Fit});
        fitPixels = wallpaperx::testing::renderReference(device, targets, fit, 200, 128, 128);
    }
    {
        StaticRenderer centre(device, targets.renderPass(), image, WALLPAPERX_SHADER_DIR);
        centre.place({PositionMode::Centre});
        centrePixels = wallpaperx::testing::renderReference(device, targets, centre, 201, 128, 128);
    }
    gradePlaced(fitPixels, {PositionMode::Fit}, first, 128, 128, tier + ": a shared still drawn fit", false, true);
    gradePlaced(centrePixels, {PositionMode::Centre}, first, 128, 128, tier + ": the same still drawn centred", false, true);
    check(StillImage::load(device, path, image) == image,
          tier + ": loading an unchanged still again with it kept decodes nothing");

    // GOAL: S15.704 -- a still rewritten on disk is decoded afresh even when a kept image is passed.
    // CONTROL: FileIdentity::operator== made to return true in
    //       renderer/renderers/static_renderer.cpp -> "a still rewritten on disk is decoded
    //       again" red on every tier, both ICDs, alone. Reverted.
    const Fixture second = rampFixture(50, 30);
    writeFixture(second, "pos_shared");
    const auto reloaded = StillImage::load(device, path, image);
    check(reloaded != image && reloaded->extent().width == 50 && reloaded->extent().height == 30,
          tier + ": a still rewritten on disk is decoded again, not the kept image returned");
    ++g_sharedCases;
}

constexpr int kCasesPerTier = 9;
// Per tier: still graded per mode and screen and fixture, the video for all but tile, and two shared.
constexpr int kPlacedGradesPerTier = kPositionFixtures * kScreenCount * (2 * kModeCaseCount - 1) + 2;

// GOAL: S13.112 -- every tier draws the cropped image with zero validation messages, and each tier is really
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
                const int placedBefore = g_placedCases, sharedBefore = g_sharedCases;
                const int coverFreeBefore = g_coverFreeComparisons, panBefore = g_panSteps;
                runPositionCases(device, tier.name);
                runPanCases(device, tier.name);
                runSharedStillCases(device, tier.name);
                const int thinBefore = g_thinCases;
                runThinCases(device, tier.name);
                check(g_thinCases - thinBefore == 2 * kThinCaseCount, label + ": every thin placement was drawn and graded");
                check(g_placedCases - placedBefore == kPlacedGradesPerTier && g_sharedCases - sharedBefore == 1 &&
                          g_coverFreeComparisons - coverFreeBefore == kPositionFixtures * kScreenCount &&
                          g_panSteps - panBefore == 1,
                      label + ": every position case was rendered and graded");
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
