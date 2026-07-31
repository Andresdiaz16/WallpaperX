#include "vulkan/device.h"
#include "vulkan/offscreen.h"
#include "vulkan/frame_export.h"
#include "renderers/static_renderer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

// A distinct filename from static_renderer_probe's test_red.png so the two
// probes never touch the same file, even under `ctest -jN` -- no RESOURCE_LOCK
// needed here.
std::string writeTestImage() {
    std::filesystem::create_directories(WALLPAPERX_TEST_IMAGE_DIR);
    const std::string path = std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/frame_export_red.png";

    const int w = 8;
    const int h = 8;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 1] = 0;
        pixels[i * 4 + 2] = 0;
        pixels[i * 4 + 3] = 255;
    }

    if (!stbi_write_png(path.c_str(), w, h, 4, pixels.data(), w * 4)) {
        throw std::runtime_error("frame_export_probe: failed to write test image");
    }
    return path;
}

bool testHappyPathExport() {
    wallpaperx::resetValidationWarningCount();

    try {
        const std::string imagePath = writeTestImage();

        wallpaperx::Device device(/*enableValidation=*/true);
        wallpaperx::OffscreenTargetManager targets(device);

        const uint32_t width = 64;
        const uint32_t height = 64;
        const auto& target = targets.create(0, width, height);

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
        if (frame1.width != width || frame1.height != height) {
            std::cerr << "frame_export_probe: exported dimensions " << frame1.width << "x" << frame1.height
                      << " do not match target " << width << "x" << height << std::endl;
            ok = false;
        }

        // The backing allocation must be large enough to hold the image. It can
        // legitimately be larger than width*height*bpp due to alignment/padding
        // (dedicated allocation), so assert a lower bound, not equality.
        const VkDeviceSize minBytes = static_cast<VkDeviceSize>(width) * height * 4;  // B8G8R8A8_UNORM
        if (frame1.memorySize < minBytes) {
            std::cerr << "frame_export_probe: exported memorySize " << frame1.memorySize
                      << " is smaller than the minimum " << minBytes << " for a " << width << "x" << height
                      << " BGRA image" << std::endl;
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
        // Gated on the image actually being modifier-tiled, not just on the frame being
        // DmaBuf: a driver with dma_buf but no usable DRM modifier is a configuration
        // the renderer deliberately supports in degraded form, and it correctly reports
        // the unset sentinels. Asserting unconditionally would fail that config.
        // An opaque fd is imported by another context on the same device, which needs
        // no layout metadata -- publishing a real-looking modifier here would send the
        // KDE plugin down its dmabuf-import branch for a handle that cannot support it.
        if (frame1.type == wallpaperx::ExportedFrame::HandleType::OpaqueFd) {
            if (frame1.modifier != wallpaperx::kDrmModifierInvalid || frame1.drmFormat != 0 ||
                !frame1.planes.empty()) {
                std::cerr << "frame_export_probe: an OpaqueFd frame must not carry DMA-BUF layout metadata"
                          << std::endl;
                ok = false;
            }
        }
        if (frame1.type == wallpaperx::ExportedFrame::HandleType::DmaBuf && !target.image.drmModifierTiled) {
            if (frame1.modifier != wallpaperx::kDrmModifierInvalid || frame1.drmFormat != 0 ||
                !frame1.planes.empty()) {
                std::cerr << "frame_export_probe: a DmaBuf frame without modifier tiling must report unset layout "
                             "metadata, not a partially-filled one"
                          << std::endl;
                ok = false;
            }
        }
        if (frame1.type == wallpaperx::ExportedFrame::HandleType::DmaBuf && target.image.drmModifierTiled) {
            if (frame1.modifier == wallpaperx::kDrmModifierInvalid) {
                std::cerr << "frame_export_probe: DmaBuf frame reports DRM_FORMAT_MOD_INVALID -- "
                             "the image was not created with an explicit DRM format modifier"
                          << std::endl;
                ok = false;
            }
            // Derived from kFormat through the same mapping the exporter uses, so this
            // tracks a format change instead of pinning a literal. The real guard
            // against an unmapped format is drmFourccForVkFormat returning 0, which
            // makes exportFrame throw -- asserted separately below.
            const uint32_t expectedFourcc =
                wallpaperx::drmFourccForVkFormat(wallpaperx::OffscreenTargetManager::kFormat);
            if (expectedFourcc == 0) {
                std::cerr << "frame_export_probe: no DRM fourcc mapping for the offscreen target's format -- "
                             "drmFourccForVkFormat needs a case for it"
                          << std::endl;
                ok = false;
            }
            if (frame1.drmFormat != expectedFourcc) {
                std::cerr << "frame_export_probe: DmaBuf frame reports drmFormat 0x" << std::hex << frame1.drmFormat
                          << ", expected 0x" << expectedFourcc << std::dec << " for the target's Vulkan format"
                          << std::endl;
                ok = false;
            }
            if (frame1.planes.empty()) {
                std::cerr << "frame_export_probe: DmaBuf frame carries no plane layout" << std::endl;
                ok = false;
            }
            // Every plane must sit inside the memory actually exported -- catches a
            // layout that was defaulted rather than queried, and holds for any modifier.
            for (size_t i = 0; i < frame1.planes.size(); ++i) {
                if (frame1.planes[i].offset >= frame1.memorySize) {
                    std::cerr << "frame_export_probe: plane " << i << " offset " << frame1.planes[i].offset
                              << " lies outside the exported allocation (" << frame1.memorySize << " bytes)"
                              << std::endl;
                    ok = false;
                }
            }
            // rowPitch is a real byte stride only under LINEAR. For tiled/compressed
            // modifiers (AMD DCC, Intel Y-tiled+CCS) vkGetImageSubresourceLayout may
            // report a pitch that is not the linear width of a row, and 0 is legal for
            // planes whose contents are not row-addressable -- so bounding it there
            // would fail a correct export on exactly the hardware this code is for.
            // Lavapipe only offers LINEAR, so CI still exercises this branch.
            constexpr uint64_t kDrmModifierLinear = 0;
            if (frame1.modifier == kDrmModifierLinear && !frame1.planes.empty() &&
                frame1.planes[0].rowPitch < uint64_t{width} * 4) {
                std::cerr << "frame_export_probe: LINEAR plane 0 rowPitch " << frame1.planes[0].rowPitch
                          << " is below the " << (width * 4) << " bytes a " << width << "px BGRA row needs"
                          << std::endl;
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

    const int warnings = wallpaperx::validationWarningCount();
    if (warnings > 0) {
        std::cerr << "frame_export_probe: " << warnings << " Vulkan validation warning(s)" << std::endl;
        return false;
    }

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
    wallpaperx::resetValidationWarningCount();
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        wallpaperx::OffscreenTargetManager targets(device);

        const wallpaperx::OffscreenTarget stale = targets.create(0, 64, 64);
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

    const int warnings = wallpaperx::validationWarningCount();
    if (warnings > 0) {
        std::cerr << "frame_export_probe (destroyed target): " << warnings << " Vulkan validation warning(s)"
                  << std::endl;
        return false;
    }

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
    wallpaperx::resetValidationWarningCount();
    try {
        wallpaperx::Device device(/*enableValidation=*/true);
        wallpaperx::OffscreenTargetManager targets(device);
        const auto& target = targets.create(0, 64, 64);

        rlimit original{};
        getrlimit(RLIMIT_NOFILE, &original);
        rlimit lowered = original;
        lowered.rlim_cur = 40;  // this process only; system untouched
        if (setrlimit(RLIMIT_NOFILE, &lowered) != 0) {
            const int skipWarnings = wallpaperx::validationWarningCount();
            if (skipWarnings > 0) {
                std::cerr << "frame_export_probe (exhaustion): " << skipWarnings
                          << " Vulkan validation warning(s) before the skip" << std::endl;
                return false;
            }
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

    const int warnings = wallpaperx::validationWarningCount();
    if (warnings > 0) {
        std::cerr << "frame_export_probe (exhaustion): " << warnings << " Vulkan validation warning(s)" << std::endl;
        return false;
    }

    std::cout << "frame_export_probe (exhaustion): passed, zero validation warnings" << std::endl;
    return true;
}

}

int main() {
    bool ok = true;
    ok &= testHappyPathExport();
    ok &= testExportFromDestroyedTargetThrows();
    ok &= testNeverReturnsInvalidFdUnderExhaustion();
    return ok ? 0 : 1;
}
