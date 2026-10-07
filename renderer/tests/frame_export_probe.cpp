// SPDX-License-Identifier: GPL-2.0-or-later

#include "vulkan/device.h"
#include "support/validation_grade.h"

#include "vulkan/offscreen.h"
#include "vulkan/frame_export.h"
#include "renderers/static_renderer.h"

#include "support/exported_frame_map.h"
#include "support/gpu_readback.h"

#include "support/test_image.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::kBytesPerPixel;
using wallpaperx::testing::kDrmModifierLinear;
using wallpaperx::testing::renderReference;
using wallpaperx::testing::variesInBothAxes;

// Deliberately not square, and deliberately not a round number of pixels wide.
//
// A square target passes just as happily with width and height transposed, and a
// power-of-two width makes a rowPitch that ignores padding indistinguishable from one
// that accounts for it -- the two mistakes this probe most needs to catch. 68x36 is
// large enough for a driver to pad the stride and small enough to stay fast under
// software rasterisation.
constexpr uint32_t kTargetWidth = 68;
constexpr uint32_t kTargetHeight = 36;

// A distinct filename from static_renderer_probe's test_red.png so the two probes
// never touch the same file. That is not sufficient on its own: this probe and its
// _lavapipe twin are the same binary and write this same path, so they are locked to
// a shared ctest RESOURCE_LOCK -- without it `ctest -jN` can have one process reading
// a half-written PNG the other is still emitting.
//
// A gradient, not the flat red static_renderer_probe uses: flat colour is worthless to
// this probe specifically -- see writeGradientPng in support/test_image.h, which carries
// the reasoning next to the pattern it produces.
std::string writeTestImage() {
    return wallpaperx::testing::writeGradientPng(
        std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/frame_export_gradient.png", 16, 16);
}

// The layout assertions that hold for any DmaBuf frame carrying a real modifier.
// Split out of testHappyPathExport so the content test can apply them too without
// duplicating the reasoning about which of them are modifier-dependent.
bool checkDmaBufLayout(const wallpaperx::ExportedFrame& frame, uint32_t width, uint32_t height) {
    bool ok = true;

    if (frame.modifier == wallpaperx::kDrmModifierInvalid) {
        std::cerr << "frame_export_probe: DmaBuf frame reports DRM_FORMAT_MOD_INVALID -- "
                     "the image was not created with an explicit DRM format modifier"
                  << std::endl;
        ok = false;
    }
    // Derived from kFormat through the same mapping the exporter uses, so this
    // tracks a format change instead of pinning a literal. The real guard
    // against an unmapped format is drmFourccForVkFormat returning 0, which
    // makes exportFrame throw -- asserted separately here.
    const uint32_t expectedFourcc = wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
    if (expectedFourcc == 0) {
        std::cerr << "frame_export_probe: no DRM fourcc mapping for the offscreen target's format -- "
                     "drmFourccForVkFormat needs a case for it"
                  << std::endl;
        ok = false;
    }
    if (frame.drmFormat != expectedFourcc) {
        std::cerr << "frame_export_probe: DmaBuf frame reports drmFormat 0x" << std::hex << frame.drmFormat
                  << ", expected 0x" << expectedFourcc << std::dec << " for the target's Vulkan format" << std::endl;
        ok = false;
    }
    if (frame.planes.empty()) {
        std::cerr << "frame_export_probe: DmaBuf frame carries no plane layout" << std::endl;
        ok = false;
    }
    // Every plane must sit inside the memory actually exported -- catches a
    // layout that was defaulted rather than queried, and holds for any modifier.
    for (size_t i = 0; i < frame.planes.size(); ++i) {
        if (frame.planes[i].offset >= frame.memorySize) {
            std::cerr << "frame_export_probe: plane " << i << " offset " << frame.planes[i].offset
                      << " lies outside the exported allocation (" << frame.memorySize << " bytes)" << std::endl;
            ok = false;
        }
    }
    // rowPitch is a real byte stride only under LINEAR. For tiled/compressed
    // modifiers (AMD DCC, Intel Y-tiled+CCS) vkGetImageSubresourceLayout may
    // report a pitch that is not the linear width of a row, and 0 is legal for
    // planes whose contents are not row-addressable -- so bounding it there
    // would fail a correct export on exactly the hardware this code is for.
    // Lavapipe only offers LINEAR, so CI still exercises this branch.
    if (frame.modifier == kDrmModifierLinear && !frame.planes.empty()) {
        const uint64_t rowBytes = uint64_t{width} * kBytesPerPixel;
        if (frame.planes[0].rowPitch < rowBytes) {
            std::cerr << "frame_export_probe: LINEAR plane 0 rowPitch " << frame.planes[0].rowPitch << " is below the "
                      << rowBytes << " bytes a " << width << "px BGRA row needs" << std::endl;
            ok = false;
        }
        // Under LINEAR the whole image is addressable, so the *extent* is checkable
        // and not just the starting offset. An allocation that stops short of the
        // last row is one an importer walks off the end of -- and the offset-only
        // check above passes for it, because the offset is still in range.
        const uint64_t lastRowEnd =
            frame.planes[0].offset + uint64_t{height - 1} * frame.planes[0].rowPitch + rowBytes;
        if (lastRowEnd > frame.memorySize) {
            std::cerr << "frame_export_probe: LINEAR plane 0 needs " << lastRowEnd << " bytes for " << width << "x"
                      << height << " at pitch " << frame.planes[0].rowPitch << ", but only " << frame.memorySize
                      << " were exported" << std::endl;
            ok = false;
        }
    }
    return ok;
}

// The check this probe exists for, and the only one here that does not take the
// exporter's word for anything: map the exported memory through nothing but the fd,
// walk it using nothing but the offset/rowPitch/dimensions the frame published, and
// compare the result against a Vulkan readback of the same image.
//
// Every previous assertion in this file is the export path describing itself --
// internally consistent whether or not the numbers mean anything to a consumer. This
// one fails if plane[0].offset points at the wrong place, if rowPitch is off by a
// padding row, if width and height are transposed, or if memorySize describes a
// different allocation than the fd does.
//
// Only runs where the bytes are CPU-addressable: a DmaBuf frame with the LINEAR
// modifier. That is Lavapipe, which runs as a ctest twin on every build, so this is
// not a branch that first executes on a stranger's machine. A tiled modifier
// (NVIDIA's block-linear, AMD DCC) has no CPU-walkable row layout and is skipped
// loudly -- verifying those needs a real importer, which is a separate piece of work.
bool testExportedBytesMatchReadback() {

    // Set by the paths that cannot run the comparison on this device. They are not
    // failures, but they must not re-encode the pass criterion either: each used to
    // `return validationWarningCount() == 0` inline, which silently dropped the
    // "N Vulkan validation warning(s)" diagnostic the tail prints. On the OPAQUE_FD
    // path -- the default on this project's own hardware, so the one that runs most
    // often -- that meant a validation warning failed the probe with no stated reason.
    bool skipped = false;

    const bool ok = wallpaperx::testing::gradedRun(
        "frame_export_probe (contents)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        const std::string imagePath = writeTestImage();

        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);
        wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

        // The reference comes from its own target, which is never exported. See
        // renderReference() for why reading back the exported target first would move
        // exportFrame() onto a release scope the daemon never uses.
        const std::vector<unsigned char> expected =
            renderReference(device, targets, renderer, /*monitorId=*/1, kTargetWidth, kTargetHeight);
        if (!variesInBothAxes(expected, kTargetWidth, kTargetHeight)) {
            std::cerr << "frame_export_probe (contents): the rendered frame is constant along an axis, so comparing "
                         "it against the exported memory cannot detect a wrong stride or a transposed read -- the "
                         "test image or the renderer changed and this check has lost its power"
                      << std::endl;
            return false;
        }

        // Exported straight from the layout the render left behind, exactly as
        // GetFrame does.
        const auto& target = targets.create(0, kTargetWidth, kTargetHeight);
        targets.render(renderer, target);

        wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, target);
        struct FdGuard {
            int fd;
            ~FdGuard() {
                if (fd >= 0) close(fd);
            }
        } fdGuard{frame.fd};

        std::cout << "frame_export_probe (contents): fd " << frame.fd << " -> "
                  << wallpaperx::testing::fdDescription(frame.fd) << std::endl;

        if (frame.type != wallpaperx::ExportedFrame::HandleType::DmaBuf) {
            std::cout << "frame_export_probe (contents): skipped, this device exports OPAQUE_FD -- its bytes are "
                         "importable only by another Vulkan/GL context on the same device, not by the CPU"
                      << std::endl;
            skipped = true;
        } else if (!checkDmaBufLayout(frame, kTargetWidth, kTargetHeight)) {
            return false;
        } else if (frame.modifier != kDrmModifierLinear) {
            std::cout << "frame_export_probe (contents): skipped, modifier 0x" << std::hex << frame.modifier << std::dec
                      << " is tiled -- its rows are not CPU-addressable, so a byte-for-byte comparison would fail a "
                         "correct export"
                      << std::endl;
            skipped = true;
        }

        if (!skipped) {
            // An independent statement of the buffer's size: for a real dma_buf the
            // kernel answers this from the object itself, with no involvement from
            // Vulkan. Not every fd supports it (a memfd does, a pipe does not), so a
            // failure is a skip rather than a verdict -- but when it answers, it must
            // not contradict the memorySize the frame carries.
            const off_t seekSize = lseek(frame.fd, 0, SEEK_END);
            if (seekSize >= 0 && static_cast<uint64_t>(seekSize) < frame.memorySize) {
                std::cerr << "frame_export_probe (contents): the fd is " << seekSize
                          << " bytes but the frame claims memorySize " << frame.memorySize
                          << " -- the exported handle is smaller than the allocation it describes" << std::endl;
                return false;
            }

            const wallpaperx::testing::MappedFrame mapping(frame.fd, frame.memorySize);
            if (!mapping.valid()) {
                // A LINEAR dma_buf that cannot be mapped is not a configuration this
                // project can verify, but it is also not one it can call broken:
                // whether an exporter permits CPU mapping is driver policy, and a GPU
                // importer never needs it. Skip loudly rather than fail, and rather
                // than pass in silence.
                std::cout << "frame_export_probe (contents): skipped, mmap of the exported fd failed ("
                          << mapping.error() << ") -- this driver does not permit CPU mapping of its dma_buf"
                          << std::endl;
                skipped = true;
            } else {
                const uint64_t offset = frame.planes[0].offset;
                const uint64_t pitch = frame.planes[0].rowPitch;
                const size_t rowBytes = static_cast<size_t>(kTargetWidth) * kBytesPerPixel;

                size_t mismatchedRows = 0;
                uint32_t firstBadRow = 0;
                for (uint32_t y = 0; y < kTargetHeight; ++y) {
                    const unsigned char* exportedRow = mapping.row(offset, pitch, y, rowBytes);
                    const unsigned char* expectedRow = expected.data() + static_cast<size_t>(y) * rowBytes;
                    if (exportedRow == nullptr || std::memcmp(exportedRow, expectedRow, rowBytes) != 0) {
                        if (mismatchedRows == 0) firstBadRow = y;
                        ++mismatchedRows;
                    }
                }

                if (mismatchedRows != 0) {
                    std::cerr << "frame_export_probe (contents): " << mismatchedRows << " of " << kTargetHeight
                              << " rows read back through the exported fd differ from the Vulkan readback (first at "
                                 "row "
                              << firstBadRow << ", plane offset " << offset << ", rowPitch " << pitch
                              << ") -- the published layout does not describe the exported bytes" << std::endl;
                    return false;
                }

                std::cout << "frame_export_probe (contents): all " << kTargetHeight << " rows of the " << kTargetWidth
                          << "x" << kTargetHeight << " frame read correctly through the exported fd (offset " << offset
                          << ", rowPitch " << pitch << ")" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "frame_export_probe (contents) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "frame_export_probe (contents): " << (skipped ? "skipped" : "passed")
              << ", zero validation warnings" << std::endl;
    return true;
}

bool testHappyPathExport() {

    const bool ok = wallpaperx::testing::gradedRun(
        "frame_export_probe (export)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        const std::string imagePath = writeTestImage();

        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);

        const auto& target = targets.create(0, kTargetWidth, kTargetHeight);

        // Render a real frame into the target first -- exercises the realistic
        // "export something that has actually been drawn into" flow, not just an
        // untouched allocation. Same StaticRenderer pattern as static_renderer_probe.
        wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);
        targets.render(renderer, target);

        wallpaperx::ExportedFrame frame1 = wallpaperx::exportFrame(device, target);
        wallpaperx::ExportedFrame frame2 = wallpaperx::exportFrame(device, target);

        bool ok = true;

        // Re-render and export again, the way the daemon will on a second SetWallpaper.
        // IRenderer::render() puts the target back in the rendered layout, so the export
        // must re-issue the ownership release for the new contents rather than skip it.
        // A stale oldLayout here trips validation, which the zero-warnings assertion
        // below turns into a failure.
        targets.render(renderer, target);
        wallpaperx::ExportedFrame frame3 = wallpaperx::exportFrame(device, target);
        if (fcntl(frame3.fd, F_GETFD) == -1) {
            std::cerr << "frame_export_probe: re-rendered export did not produce a valid fd" << std::endl;
            ok = false;
        }
        // Re-exporting the same target must describe the same buffer. The image was not
        // reallocated between the two exports, so a differing modifier, fourcc, plane
        // count or size means the metadata is being derived from transient state rather
        // than queried from the image -- which an importer caching the first frame's
        // layout (as a compositor does) would then misread on every later frame.
        if (frame3.type != frame1.type || frame3.modifier != frame1.modifier ||
            frame3.drmFormat != frame1.drmFormat || frame3.planes.size() != frame1.planes.size() ||
            frame3.memorySize != frame1.memorySize) {
            std::cerr << "frame_export_probe: re-exporting an unchanged target produced different layout metadata"
                      << std::endl;
            ok = false;
        }
        close(frame3.fd);

        // The fd must be a real, currently-open OS file descriptor.
        if (fcntl(frame1.fd, F_GETFD) == -1) {
            std::cerr << "frame_export_probe: first exported fd is not a valid open fd" << std::endl;
            ok = false;
        }
        if (fcntl(frame2.fd, F_GETFD) == -1) {
            std::cerr << "frame_export_probe: second exported fd is not a valid open fd" << std::endl;
            ok = false;
        }

        // width/height are copied straight from the target and must match exactly.
        if (frame1.width != kTargetWidth || frame1.height != kTargetHeight) {
            std::cerr << "frame_export_probe: exported dimensions " << frame1.width << "x" << frame1.height
                      << " do not match target " << kTargetWidth << "x" << kTargetHeight << std::endl;
            ok = false;
        }

        // The backing allocation must be large enough to hold the image. It can
        // legitimately be larger than width*height*bpp due to alignment/padding
        // (dedicated allocation), so assert a lower bound, not equality.
        const VkDeviceSize minBytes = static_cast<VkDeviceSize>(kTargetWidth) * kTargetHeight * kBytesPerPixel;
        if (frame1.memorySize < minBytes) {
            std::cerr << "frame_export_probe: exported memorySize " << frame1.memorySize
                      << " is smaller than the minimum " << minBytes << " for a " << kTargetWidth << "x"
                      << kTargetHeight << " BGRA image" << std::endl;
            ok = false;
        }

        // Each vkGetMemoryFdKHR call is contractually a fresh fd -- while both are
        // still open they must be distinct (see qa_scratch/frame_export_fd_lifecycle.cpp
        // for the exhaustive version of this check).
        if (frame1.fd == frame2.fd) {
            std::cerr << "frame_export_probe: two exports returned the same fd (" << frame1.fd << ")" << std::endl;
            ok = false;
        }

        // DMA-BUF layout contract (exercised under Lavapipe, which takes this path).
        // An importer gets nothing but an fd unless all of this is real, so assert it
        // rather than assert the absence of it: a frame carrying MOD_INVALID or a zero
        // pitch is one no compositor can interpret.
        //
        // Applied to every DmaBuf frame, with no extra gate on the image being
        // modifier-tiled. That gate dated from when a dma_buf export without
        // VK_EXT_image_drm_format_modifier was degraded rather than refused;
        // createExportableImage() now throws for it, so drmModifierTiled is necessarily
        // true for any frame that reaches here as DmaBuf and the condition was
        // unreachable-false. Worse, the two call sites of checkDmaBufLayout() disagreed
        // about it, so the same frame was held to different standards depending on which
        // test looked at it.
        // An opaque fd is imported by another context on the same device, which needs
        // no layout metadata -- publishing a real-looking modifier here would send the
        // KDE plugin down its dmabuf-import branch for a handle that cannot support it.
        // The fourcc is graded for both handle types: it is channel order, not layout, and an
        // opaque fd carries one too. The plugin picks its import path from handle_type.
        const uint32_t expectedFourcc =
            wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
        if (frame1.drmFormat != expectedFourcc) {
            std::cerr << "frame_export_probe: frame reports drmFormat 0x" << std::hex << frame1.drmFormat
                      << ", expected 0x" << expectedFourcc << std::dec << " for the target's Vulkan format"
                      << std::endl;
            ok = false;
        }
        if (frame1.type == wallpaperx::ExportedFrame::HandleType::OpaqueFd) {
            if (frame1.modifier != wallpaperx::kDrmModifierInvalid || !frame1.planes.empty()) {
                std::cerr << "frame_export_probe: an OpaqueFd frame must not carry DMA-BUF layout metadata"
                          << std::endl;
                ok = false;
            }
        }
        if (frame1.type == wallpaperx::ExportedFrame::HandleType::DmaBuf) {
            if (!checkDmaBufLayout(frame1, kTargetWidth, kTargetHeight)) {
                ok = false;
            }
        }

        std::cout << "frame_export_probe: fd1=" << frame1.fd << " fd2=" << frame2.fd << " " << frame1.width << "x"
                  << frame1.height << " memorySize=" << frame1.memorySize << " type="
                  << (frame1.type == wallpaperx::ExportedFrame::HandleType::OpaqueFd ? "OpaqueFd" : "DmaBuf")
                  << std::endl;
        if (frame1.type == wallpaperx::ExportedFrame::HandleType::DmaBuf) {
            std::cout << "frame_export_probe: modifier=0x" << std::hex << frame1.modifier << std::dec << " planes="
                      << frame1.planes.size();
            for (const auto& plane : frame1.planes) {
                std::cout << " [offset=" << plane.offset << " rowPitch=" << plane.rowPitch << "]";
            }
            std::cout << std::endl;
        }

        close(frame1.fd);
        close(frame2.fd);

        if (!ok) {
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "frame_export_probe failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "frame_export_probe: passed, zero validation warnings" << std::endl;
    return true;
}

// Regression test for the QA finding (2026-07-18, qa_scratch/frame_export_stale_target.cpp):
// exporting from a target whose backing allocation has been destroyed used to
// segfault inside the driver with no exception and no validation warning. The
// Memory-layer live-allocation guard now turns that into a clean, catchable
// std::runtime_error. Copy the target struct by value so our own C++ object stays
// valid -- only the Vulkan/VMA handles inside it are dead after destroy().
bool testExportFromDestroyedTargetThrows() {
    // Reset here too, not just in testHappyPathExport: without it any validation error
    // this test provokes is printed and discarded, and the probe reports "passed" on a
    // spec violation. The zero-warnings assertion is the backstop for the export path's
    // barriers, so it has to cover every test that exercises them.
    const bool ok = wallpaperx::testing::gradedRun(
        "frame_export_probe (destroyed target)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);

        const wallpaperx::OffscreenTarget stale = targets.create(0, kTargetWidth, kTargetHeight);
        targets.destroy(0);

        bool threw = false;
        try {
            wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, stale);
            if (frame.fd >= 0) {
                close(frame.fd);
            }
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "frame_export_probe (destroyed target): exporting a destroyed target should have thrown"
                      << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "frame_export_probe (destroyed target) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "frame_export_probe (destroyed target): passed, zero validation warnings" << std::endl;
    return true;
}

// Regression test (QA pass 2026-07-18): under fd exhaustion, Lavapipe's DMA_BUF
// vkGetMemoryFdKHR returns VK_SUCCESS while writing fd = -1, and exportFrame() used
// to hand that back as a valid-looking frame with a dead fd. The contract is "a
// fresh valid fd, or throw" -- exportFrame() must NEVER return an fd < 0. Runs on
// both backends (the OpaqueFd path fails via the return code, the DmaBuf path via
// the fd check). RLIMIT_NOFILE is lowered for this process only.
bool testNeverReturnsInvalidFdUnderExhaustion() {
    // This test exports from a target that was never rendered into, so it is the one
    // that catches an export-time barrier declaring a layout the image was never in.
    // It only catches it if the warning count is actually checked.
    const bool ok = wallpaperx::testing::gradedRun(
        "frame_export_probe (exhaustion)", [&](wallpaperx::testing::ValidationGrade& grade) {
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);
        wallpaperx::OffscreenTargetManager targets(device);
        const auto& target = targets.create(0, kTargetWidth, kTargetHeight);

        rlimit original{};
        getrlimit(RLIMIT_NOFILE, &original);
        rlimit lowered = original;
        lowered.rlim_cur = 40;  // this process only; system untouched
        if (setrlimit(RLIMIT_NOFILE, &lowered) != 0) {
            // No hand-rolled warning check here: returning destroys the Device and then
            // `grade`, which grades the full count including teardown. Reading the
            // counter mid-life would report a smaller number and call it clean.
            std::cout << "frame_export_probe (exhaustion): skipped, could not lower RLIMIT_NOFILE" << std::endl;
            return true;
        }

        std::vector<int> held;
        bool sawInvalidFd = false;
        for (int i = 0; i < 200; ++i) {  // deliberately never close -- force exhaustion
            try {
                wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, target);
                if (frame.fd < 0) {
                    sawInvalidFd = true;  // the bug: returned without throwing but fd is dead
                    break;
                }
                held.push_back(frame.fd);
            } catch (const std::runtime_error&) {
                break;  // clean failure once the OS runs out of fds -- expected
            }
        }

        for (int fd : held) close(fd);
        setrlimit(RLIMIT_NOFILE, &original);

        if (sawInvalidFd) {
            std::cerr << "frame_export_probe (exhaustion): exportFrame() returned an ExportedFrame with fd < 0 "
                         "instead of throwing"
                      << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "frame_export_probe (exhaustion) failed: " << e.what() << std::endl;
        return false;
    }

    return true;
    });
    if (!ok) return false;
    std::cout << "frame_export_probe (exhaustion): passed, zero validation warnings" << std::endl;
    return true;
}

}

int main() {
    bool ok = true;
    ok &= testHappyPathExport();
    ok &= testExportedBytesMatchReadback();
    ok &= testExportFromDestroyedTargetThrows();
    ok &= testNeverReturnsInvalidFdUnderExhaustion();
    return ok ? 0 : 1;
}
