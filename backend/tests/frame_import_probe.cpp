// SPDX-License-Identifier: GPL-2.0-or-later

// S12.1's import half: a frame the renderer exported becomes a GL texture whose pixels are the
// ones that were rendered. The export half already has frame_export_probe, which maps the fd on
// the CPU; this drives the same frame through the path the compositor actually uses.
//
// The oracle is the same frame read two ways -- Vulkan reads the target back, GL reads the
// imported texture back, and the two must agree byte for byte. That is stronger than comparing
// against a constant: a stride mistake, a transposed read or a wrong format all survive "is it
// roughly the right colour" and none of them survive this.
//
// WHY IT CAN SKIP: an opaque fd is importable only by the driver that produced it. This binary
// pairs a Vulkan device with a GL context, and there is no guarantee the two resolve to the same
// driver -- a headless EGL context on this machine comes up on llvmpipe even where Vulkan
// resolves to NVIDIA. When they do not match, or when the context lacks the memory-object
// extensions, this reports NOT VERIFIED rather than failing: an unimportable pairing says
// nothing about whether the importer is correct.

#include "frame_import.h"

#include "support/gpu_readback.h"
#include "support/test_image.h"
#include "support/validation_grade.h"
// Must precede any GL use: it supplies the entry-point setup and the GL/EGL headers.
#include "tests/gl_import_harness.h"
#include "tests/test_report.h"
#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::HeadlessGl;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::readSampled;
using wallpaperx::testing::renderReference;
using wallpaperx::testing::variesInBothAxes;

constexpr const char* kGoals = "S12.1, S12.2 (a frame the daemon exported reaches the screen as the pixels that "
                               "were rendered, imported by the path the driver actually supports)";

// Not square, and not a round number of pixels wide, for the reason frame_export_probe gives:
// a square target passes with width and height transposed, and a power-of-two width hides a
// stride that ignores padding.
constexpr uint32_t kWidth = 68;
constexpr uint32_t kHeight = 36;


// How many descriptors this process holds. Its own copy rather than the one in
// tests/client/daemon_client.h, which this binary cannot include -- that header pulls in sdbus.
// The SENTINEL must stay identical to it: 0 for a directory that cannot be read. It answered -1
// here, so a guard written against one is silently wrong against the other.
size_t openFdCount() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) return 0;
    size_t count = 0;
    while (::readdir(dir) != nullptr) ++count;
    ::closedir(dir);
    return count;
}

// Reads an imported texture back through a framebuffer object. glGetTexImage would be shorter
// but is not available on GLES-shaped contexts, and the plugin's own context may be one.
std::vector<unsigned char> readTexture(GLuint texture, uint32_t width, uint32_t height, std::string& why) {
    // Drained first: GL queues errors until read, so one left by the caller would be attributed
    // to this read and reported as a texture that could not be read back.
    while (glGetError() != GL_NO_ERROR) {
    }
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    std::vector<unsigned char> pixels;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        why = "the imported texture could not be attached to a framebuffer to read back";
    } else {
        pixels.resize(static_cast<size_t>(width) * height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels.data());
        if (glGetError() != GL_NO_ERROR) {
            why = "glReadPixels failed on the imported texture";
            pixels.clear();
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return pixels;
}

using wallpaperx::testing::vulkanDeviceUuid;

// Nonzero when the validation layer was absent. Its own counter, because charging that to
// failures turns every deliberate skip in this file into exit 1 -- a host without the layer
// package reports NOT VERIFIED in its output and FAILS to ctest, blaming the importer.
int g_ungradedValidation = 0;

bool runImportChecks() {
    return wallpaperx::testing::gradedRun("frame_import_probe", [&](wallpaperx::testing::ValidationGrade& grade) {
        grade.reportUngradedTo(g_ungradedValidation);
        const std::string imagePath = wallpaperx::testing::writeGradientPng(
            std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/frame_import_gradient.png", 16, 16);

        // Created BEFORE the GL context, because its UUID is what decides which GL device to
        // ask for. The other order forces a guess, and the guess is wrong on this machine.
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);

        HeadlessGl gl;
        std::string why;
        if (!gl.createMatching(vulkanDeviceUuid(device.physicalDevice()), why)) {
            notVerified(kGoals, "no GL context on the same device as the Vulkan one -- " + why);
            return true;
        }
        wallpaperx::OffscreenTargetManager targets(device);
        wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);

        // From its own target, never exported: reading back the exported one would move the
        // export onto a release scope the daemon never uses.
        const std::vector<unsigned char> expected =
            renderReference(device, targets, renderer, /*monitorId=*/1, kWidth, kHeight);

        // COVERAGE-ASSERT: an image that is constant along an axis cannot distinguish a correct
        // import from a transposed or wrongly-strided one, so this check would keep passing for
        // exactly the builds it exists to catch.
        if (!variesInBothAxes(expected, kWidth, kHeight)) {
            notVerified(kGoals, "the rendered frame is constant along an axis, so comparing an imported copy of it "
                                "cannot detect a wrong stride or a transposed read");
            return true;
        }

        const auto& target = targets.create(0, kWidth, kHeight);
        targets.render(renderer, target);
        wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, target);

        const bool isDmaBuf = frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf;

        wallpaperx::FrameImport::Request request;
        request.fd = frame.fd;
        request.width = kWidth;
        request.height = kHeight;
        request.memorySize = frame.memorySize;
        request.handleType = isDmaBuf ? wallpaperx::FrameImport::HandleType::DmaBuf
                                      : wallpaperx::FrameImport::HandleType::OpaqueFd;
        request.drmFormat = frame.drmFormat;
        request.drmModifier = frame.modifier;
        for (const auto& plane : frame.planes) {
            request.planes.emplace_back(plane.offset, plane.rowPitch);
        }

        // Which path runs is PINNED per ctest entry through WALLPAPERX_FORCE_HANDLE_TYPE, not
        // left to the device. One run therefore covers one path, and this line says which -- a
        // green log for one entry is not evidence about the other.
        std::printf("frame_import_probe: %s path, on %s\n", isDmaBuf ? "dma_buf" : "opaque fd",
                    gl.renderer().c_str());
        if (!wallpaperx::FrameImport::supports(request.handleType, &why)) {
            if (frame.fd >= 0) ::close(frame.fd);
            notVerified(kGoals, std::string("this context cannot import the handle type this device exports (") +
                                    (isDmaBuf ? "dma_buf" : "opaque fd") + ") -- " + why);
            return true;
        }

        // GOAL: S12.2 -- the import succeeds once the pairing is known good. Graded as a
        //       FAILURE, not a skip: every reason an import could legitimately be impossible
        //       here has been ruled out above, since the GL device's UUID equals the Vulkan
        //       device's and the context advertises the extensions the path needs. Treating a
        //       failure past this point as "could not be exercised" is how a broken importer
        //       reports itself as a skip -- measured, not imagined: transposing the requested
        //       dimensions made glTexStorageMem2DEXT fail and the probe went green-as-skipped
        //       with its own control applied.
        // CONTROL: `request.width` and `request.height` swapped at the import site -> "FAIL: an
        //       exported frame imports into a GL context on the same device", printing "on
        //       NVIDIA GeForce RTX 3080 Ti/PCIe/SSE2: glTexStorageMem2DEXT could not back a
        //       texture with the imported memory". Observed 2026-08-20 -- and observed BEFORE
        //       the fix producing a skip instead, which is what the fix was for. Reverted.
        // CONTROL, for the "chosen from what the driver advertises" half specifically: the
        //       handle-type branch in FrameImport::import forced each way round. `if (false)`,
        //       sending a dma_buf through the memory-object importer, reds the lavapipe entry
        //       alone; `if (true)`, sending an opaque fd through EGLImage, reds the NVIDIA entry
        //       alone. Neither wrong path is quietly importable, and it takes both entries to
        //       show it. Observed 2026-08-21. Both reverted.
        const size_t beforeImport = openFdCount();
        const wallpaperx::FrameImport::Result imported = wallpaperx::FrameImport::import(request);
        const bool importedOk = imported.texture != 0;
        check(importedOk, "an exported frame imports into a GL context on the same device");
        if (!importedOk) {
            // request, not frame: import() clears the descriptor only when it consumed it, which
            // a failed import never does -- so this one is still ours to close.
            if (request.fd >= 0) ::close(request.fd);
            std::fprintf(stderr, "  on %s: %s\n", gl.renderer().c_str(), imported.error.c_str());
            return true;
        }

        std::string readWhy;
        const std::vector<unsigned char> got = readTexture(imported.texture, kWidth, kHeight, readWhy);
        if (got.empty()) {
            wallpaperx::FrameImport::release(imported);
            notVerified(kGoals, "the imported texture could not be read back -- " + readWhy);
            return true;
        }

        // GOAL: S12.1 -- the imported texture holds the pixels that were rendered, in the layout
        //       they were rendered in: same geometry, same stride, same row order.
        // WHY-THIS-SHAPE: deliberately accepts EITHER channel order, and therefore grades
        //       LAYOUT ONLY -- it is not the check that establishes the colours are right, and
        //       must not be read as one. The two paths report channels differently for correct
        //       reasons: the memory-object path lays GL_RGBA8 over the raw bytes so a framebuffer
        //       read returns memory order (the order the Vulkan readback is in), while the
        //       EGLImage path is told the fourcc and reads back as RGBA against a B8G8R8A8
        //       render. Requiring one order here would fail one path for being correct. Colour is
        //       graded by the sampled check below, which is where the swizzle becomes visible.
        // CONTROL: `request.width` and `request.height` swapped at the import site
        // CONTROL, dma_buf path: the plane pitch replaced by an assumed `width * 4` in
        //       FrameImport::importDmaBuf -> "FAIL: the imported texture is laid out as the frame
        //       that was rendered", 7101 of 9792 bytes differing on llvmpipe -- and the sampled
        //       check below red with it. Re-run against this wording 2026-08-21 on the lavapipe
        //       entry. Reverted.
        // NOT COVERED, measured rather than assumed: forcing the DRM modifier to 0 changes
        //       nothing here, because lavapipe renders LINEAR and 0 IS linear. The modifier
        //       plumbing is therefore exercised but not graded, and would need a device that
        //       picks a tiled or compressed modifier -- which this machine has none of, since
        //       its only other Vulkan device is NVIDIA and that exports an opaque fd.
        const auto swappedRedBlue = [](const std::vector<unsigned char>& in) {
            std::vector<unsigned char> out = in;
            for (size_t i = 0; i + 3 < out.size(); i += 4) std::swap(out[i], out[i + 2]);
            return out;
        };
        const std::vector<unsigned char> asColours = swappedRedBlue(expected);
        // Alpha is normalised out of the LAYOUT comparison on both sides. The fourcc declares it
        // undefined, and the two paths disagree about what a framebuffer read of it returns --
        // GL_RGBA8 storage hands back the raw bytes, an XRGB EGLImage hands back 1, and a
        // texture swizzle is a sampling rule that a framebuffer read does not apply. This check
        // is about where bytes land; the sampled check below is what grades alpha.
        const auto opaque = [](std::vector<unsigned char> in) {
            for (size_t i = 3; i < in.size(); i += 4) in[i] = 255;
            return in;
        };
        const std::vector<unsigned char> read = opaque(got);
        const std::vector<unsigned char> asLaidOut = opaque(expected);
        const std::vector<unsigned char> asLaidOutSwapped = opaque(asColours);
        const bool same = got.size() == expected.size() && (read == asLaidOut || read == asLaidOutSwapped);
        check(same, "the imported texture is laid out as the frame that was rendered");
        if (!same) {
            size_t differing = 0;
            for (size_t i = 0; i < read.size() && i < asLaidOut.size(); ++i) {
                if (read[i] != asLaidOut[i]) ++differing;
            }
            std::fprintf(stderr, "  %zu of %zu bytes differ (read %zu, expected %zu) on %s\n", differing,
                         asLaidOut.size(), got.size(), asLaidOut.size(), gl.renderer().c_str());
        }

        // GOAL: S12.1 -- what a compositor SAMPLES from the imported texture is the colour that
        //       was rendered, channel for channel. False if red and blue arrive transposed, which
        //       is a wallpaper with the wrong colours on every screen -- the defect the importer's
        //       swizzle exists to fix, and one that was hit for real on the first cross-process
        //       frame.
        // WHY-THIS-SHAPE: sampled through a shader rather than read out of the texture's own
        //       memory, and asserted against ONE order rather than either. The obvious simpler
        //       form -- reuse the framebuffer read above -- cannot answer this at all: a texture
        //       swizzle applies to lookups and not to a framebuffer read, so the raw read returns
        //       the same bytes whether the swizzle is set or not. Measured: with the swizzle
        //       deleted the whole 40-test suite stayed green.
        //       The expected order is the same on BOTH paths and that is not a coincidence -- one
        //       gets there by swizzle and the other by telling EGL the fourcc, and both are ways
        //       of saying the same thing. So one expectation grades both, and neither path can
        //       reach it by accident.
        // CONTROL: the swizzle in FrameImport::import in backend/kde/frame_import.cpp disabled
        //       (`if (false)`) -> "FAIL: sampling
        //       the imported texture gives the colours that were rendered", 4788 of 9792 bytes
        //       differing, "first at pixel (0,0): sampled 0,16,16,255; rendered 16,16,0,255", on
        //       the NVIDIA opaque entry and correctly green on the dma_buf one, which does not
        //       use the swizzle. Observed 2026-08-21. Reverted.
        // CONTROL: B8G8R8A8_UNORM mapped to XBGR8888 instead of XRGB8888 in
        //       drmFourccForVkFormat in renderer/vulkan/frame_export.cpp -- an exporter that lies
        //       about its channel order -> the same
        //       FAIL on BOTH entries. Before this check, that mutation left all 40 tests green.
        //       Observed 2026-08-21. Reverted.
        // CONTROL, the alpha half: the `GL_TEXTURE_SWIZZLE_A` block in FrameImport::import in
        //       backend/kde/frame_import.cpp disabled (`if (false && ...)`) -> "FAIL: sampling the
        //       imported texture gives the colours that were rendered, with the undefined alpha
        //       forced opaque", 2448 of 9792 bytes differing -- exactly the alpha channel --
        //       "sampled 16,16,0,32; rendered 16,16,0,255" on the NVIDIA opaque entry. This is
        //       gradable only because the source frame's alpha varies and is never 255.
        //       Observed 2026-08-22. Reverted.
        std::string sampledWhy;
        const std::vector<unsigned char> sampled = readSampled(imported.texture, kWidth, kHeight, sampledWhy);
        if (sampled.empty()) {
            // The probe's own machinery, not the importer's: attributed as a skip so a broken
            // harness cannot print as a failure of the code under test.
            notVerified(kGoals, "the imported texture could not be sampled -- " + sampledWhy);
        } else {
            // Alpha is expected OPAQUE whatever was rendered: the daemon exports an X fourcc, so
            // those bytes are undefined and the importer must force them to 1. The source frame
            // carries a varying alpha precisely so this is a claim that can fail.
            std::vector<unsigned char> expectedSample = asColours;
            for (size_t i = 3; i < expectedSample.size(); i += 4) expectedSample[i] = 255;

            const bool coloursMatch = sampled == expectedSample;
            check(coloursMatch, "sampling the imported texture gives the colours that were rendered, with the "
                                "undefined alpha forced opaque");
            if (!coloursMatch) {
                size_t differing = 0;
                size_t firstAt = sampled.size();
                for (size_t i = 0; i < sampled.size() && i < expectedSample.size(); ++i) {
                    if (sampled[i] == expectedSample[i]) continue;
                    ++differing;
                    if (firstAt == sampled.size()) firstAt = i;
                }
                if (firstAt < sampled.size()) {
                    const size_t pixel = (firstAt / 4) * 4;
                    std::fprintf(stderr,
                                 "  %zu of %zu bytes differ, first at pixel (%zu,%zu): sampled %u,%u,%u,%u; rendered "
                                 "%u,%u,%u,%u -- on %s\n",
                                 differing, expectedSample.size(), (pixel / 4) % kWidth, (pixel / 4) / kWidth,
                                 sampled[pixel], sampled[pixel + 1], sampled[pixel + 2], sampled[pixel + 3],
                                 expectedSample[pixel], expectedSample[pixel + 1], expectedSample[pixel + 2],
                                 expectedSample[pixel + 3], gl.renderer().c_str());
                }
            }
        }

        wallpaperx::FrameImport::release(imported);

        // GOAL: S12.3 -- a successful import consumes the fd exactly once, and a failed one
        //       leaves it with the caller. Both halves matter and they fail in opposite
        //       directions: consuming twice closes a descriptor this process may have reused,
        //       and consuming neither leaks one per frame on a process that runs for a login
        //       session.
        // WHY-THIS-SHAPE: counted from /proc/self/fd rather than by inspecting the importer,
        //       because the two paths dispose of the fd differently -- GL takes ownership,
        //       EGL dups and the importer closes its own -- and the property that must hold is
        //       about the process, not about which call was made.
        // CONTROL: the `::close(request.fd)` deleted from FrameImport::importDmaBuf in
        //       backend/kde/frame_import.cpp -> "FAIL:
        //       importing and releasing repeatedly does not accumulate descriptors", printing
        //       "45 open fds before, 47 after one cycle, 54 after eight" -- one per frame, which
        //       is what the loop exists to show. Observed 2026-08-21 on the lavapipe entry.
        //       Reverted.
        // CONTROL, the other half: `::close(request.fd)` added to the width==0 refusal in
        //       FrameImport::import -> "FAIL: a failed import leaves its file descriptor with the
        //       caller, neither closed nor leaked", printing "55 open fds before the failed
        //       import, 54 after". Observed 2026-08-21 on both entries. Reverted.
        // COVERAGE-ASSERT: every count below comes from /proc/self/fd, which returns -1 when it
        //       cannot be read. Left ungraded, every comparison here is -1 against -1 and the
        //       whole phase passes having counted nothing; asserted so that becomes a skip.

        // A failure that never reaches the driver: the fd must come back untouched.
        wallpaperx::FrameImport::Request rejected;
        rejected.fd = ::dup(STDIN_FILENO);
        rejected.width = 0;  // refused before anything is imported
        rejected.height = kHeight;
        rejected.memorySize = 4096;
        rejected.handleType = request.handleType;
        const size_t beforeRejected = openFdCount();
        const wallpaperx::FrameImport::Result none = wallpaperx::FrameImport::import(rejected);
        const size_t afterRejected = openFdCount();
        check(none.texture == 0, "an import that cannot proceed produces no texture");
        if (beforeRejected == 0 || afterRejected == 0) {
            notVerified(kGoals, "/proc/self/fd could not be read, so no descriptor was counted -- reported as "
                                "uncounted rather than as a passing comparison of 0 against 0");
            if (rejected.fd >= 0) ::close(rejected.fd);
            return true;
        }
        check(afterRejected == beforeRejected,
              "a failed import leaves its file descriptor with the caller, neither closed nor leaked");
        if (afterRejected != beforeRejected) {
            std::fprintf(stderr, "  %zu open fds before the failed import, %zu after\n", beforeRejected,
                         afterRejected);
        }
        // Closed here, or this descriptor stays open for the run and the counts printed by the
        // growth check below are one high -- sending whoever reads them after a leak the harness
        // caused. NOT graded, and deliberately so: that check compares growth BETWEEN cycles, so
        // a constant one-off offset cancels. Measured -- deleting this close leaves all three
        // probe entries green.
        if (rejected.fd >= 0) ::close(rejected.fd);

        // GOAL: a failure AFTER the driver has taken the descriptor still tells the caller the
        //       descriptor is gone. The case above is refused before any driver call, so it
        //       grades the early half only: on the opaque path the importer closes the caller's
        //       descriptor once glImportMemoryFdEXT accepts the duplicate it was handed, and two
        //       later steps can still fail after that. A caller told the fd is
        //       still its own closes a number the process has since reused -- inside a
        //       compositor, its own socket.
        // WHY-THIS-SHAPE: asserted by asking whether the descriptor is STILL OPEN, not by
        //       counting. Counting cannot see this: the caller's second close simply fails with
        //       EBADF and the total is unchanged, which is exactly what a count-based version of
        //       this check did -- it passed with the defect present, measured.
        // CONTROL: `::close(request.fd); request.fd = -1;` -- the pair that runs once
        //       glImportMemoryFdEXT has succeeded, in FrameImport::import in
        //       backend/kde/frame_import.cpp -- commented out ->
        //       "FAIL: a failed import that consumed the descriptor says so...", printing
        //       "import failed and returned fd 52, which still names the exported frame".
        //       Observed 2026-08-22. Reverted.
        // NOT COVERED HERE: the refusal at the hand-over itself -- glImportMemoryFdEXT declining
        //       the descriptor. This check grades the step AFTER it. That branch is graded by
        //       cross_process_import_test's refusal sweep, which stages the refusal inside
        //       glImportMemoryFdEXT and asserts no descriptor leaks across 32 of them.
        // CONTROL, the dma_buf half: `::close(request.fd); request.fd = -1;` added to the
        //       eglCreateImage failure return in FrameImport::importDmaBuf in
        //       backend/kde/frame_import.cpp -> "FAIL: a failed dma_buf import leaves the
        //       caller's descriptor open and unchanged" on BOTH dma_buf entries and correctly
        //       green on the opaque one. Observed 2026-08-22. Reverted.
        // COVERAGE-ASSERT: a staged failure that does not fail is a harness fault, not a skip.
        //       Reporting it as notVerified() would downgrade the whole binary to 77, and an
        //       earlier version of this block simply printed and fell through -- which left the
        //       dma_buf claim below unreachable while appearing to grade it.
        {
            const auto& lateTarget = targets.create(0, kWidth, kHeight);
            targets.render(renderer, lateTarget);
            wallpaperx::ExportedFrame lateFrame = wallpaperx::exportFrame(device, lateTarget);

            // Every value describes THIS export. Copying the first frame's size, fourcc or
            // modifier makes the import fail on the mismatch instead of where it is staged to,
            // which grades the pre-driver refusal already covered above and reports it as this.
            wallpaperx::FrameImport::Request late = request;
            late.fd = lateFrame.fd;
            late.memorySize = lateFrame.memorySize;
            late.drmFormat = lateFrame.drmFormat;
            late.drmModifier = lateFrame.modifier;
            late.planes.clear();
            for (const auto& plane : lateFrame.planes) late.planes.emplace_back(plane.offset, plane.rowPitch);
            // Each path is failed at the point its driver has actually been handed the fd, which
            // is the only place the two can disagree about who owns it.
            const bool opaquePath = late.handleType == wallpaperx::FrameImport::HandleType::OpaqueFd;
            if (opaquePath) {
                // Transposed: the memory import takes the fd, then the texture allocation fails.
                // eglCreateImage does not validate the declared size, so this would not fail there.
                late.width = kHeight;
                late.height = kWidth;
            } else {
                // A modifier no driver implements, so eglCreateImage refuses the buffer it has
                // just been given the fd for.
                late.drmModifier = 0x0123456789abcdefULL;
            }
            const int lateFd = late.fd;
            // Identity, not openness: a number the driver freed can be reopened by the driver
            // before this is read, and fcntl would call that "still ours". The dup keeps the
            // original file alive so a replacement is distinguishable by inode.
            const int guard = ::dup(lateFd);
            struct ::stat before {};
            const bool guarded = guard >= 0 && ::fstat(guard, &before) == 0;

            const wallpaperx::FrameImport::Result lateResult = wallpaperx::FrameImport::import(late);
            struct ::stat after {};
            const bool sameFile = guarded && late.fd >= 0 && ::fstat(late.fd, &after) == 0 &&
                                  after.st_dev == before.st_dev && after.st_ino == before.st_ino;

            if (!guarded) {
                wallpaperx::testing::harnessFailure("the descriptor could not be duplicated, so what the import did "
                                                   "with it could not be established");
            } else if (lateResult.texture != 0) {
                // Reported, not skipped: notVerified() here would downgrade the whole binary to
                // 77 and hide everything else the run graded.
                wallpaperx::FrameImport::release(lateResult);
                if (late.fd >= 0 && sameFile) ::close(late.fd);
                if (guard >= 0) ::close(guard);
                wallpaperx::testing::harnessFailure(
                    "the staged failure did not fail on this driver, so who owns the descriptor "
                    "afterwards was not established");
                return true;
            } else if (opaquePath) {
                // The fd was handed to GL, so the request must say it is gone.
                const bool honest = late.fd < 0;
                check(honest, "a failed import that consumed the descriptor says so, rather than handing back a "
                              "number the caller would close twice");
                if (!honest) {
                    std::fprintf(stderr, "  import failed and returned fd %d, which %s\n", lateFd,
                                 sameFile ? "still names the exported frame" : "now names a different file");
                }
            } else {
                // The opposite claim, and a real one: EGL duplicates what it needs, so a dma_buf
                // failure must leave the caller's descriptor untouched rather than consumed.
                check(sameFile, "a failed dma_buf import leaves the caller's descriptor open and unchanged");
            }
            if (late.fd >= 0 && sameFile) ::close(late.fd);
            if (guard >= 0) ::close(guard);
        }

        // The property that matters is that the count does not grow PER FRAME. A driver may hold
        // one descriptor of its own for an imported allocation and release it only with the
        // context -- measured here, NVIDIA does exactly that -- and a single import cannot tell
        // that apart from a leak. A wallpaper that changes every few seconds can.
        size_t afterFirstCycle = 0;
        for (int cycle = 0; cycle < 8; ++cycle) {
            const auto& spare = targets.create(0, kWidth, kHeight);
            targets.render(renderer, spare);
            wallpaperx::ExportedFrame extra = wallpaperx::exportFrame(device, spare);

            // Metadata from THIS export: exportFrame recomputes it per image.
            wallpaperx::FrameImport::Request again = request;
            again.fd = extra.fd;
            again.memorySize = extra.memorySize;
            again.drmFormat = extra.drmFormat;
            again.drmModifier = extra.modifier;
            again.planes.clear();
            for (const auto& plane : extra.planes) again.planes.emplace_back(plane.offset, plane.rowPitch);

            const wallpaperx::FrameImport::Result round = wallpaperx::FrameImport::import(again);
            if (round.texture == 0) {
                if (again.fd >= 0) ::close(again.fd);
                notVerified(kGoals, "a repeat import failed, so descriptor growth could not be measured: " +
                                        round.error);
                return true;
            }
            wallpaperx::FrameImport::release(round);
            if (cycle == 0) afterFirstCycle = openFdCount();
        }

        const size_t atEnd = openFdCount();
        if (beforeImport == 0 || afterFirstCycle == 0 || atEnd == 0) {
            notVerified(kGoals, "/proc/self/fd stopped answering partway through the import cycles, so growth could "
                                "not be measured");
            return true;
        }
        const bool steady = atEnd <= afterFirstCycle;
        check(steady, "importing and releasing repeatedly does not accumulate descriptors");
        if (!steady) {
            std::fprintf(stderr, "  %zu open fds before, %zu after one cycle, %zu after eight\n", beforeImport,
                         afterFirstCycle, atEnd);
        }
        return true;
    });
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "frame_import_probe";
    // Everything this drives reports failure by throwing, and without a boundary a host with no
    // Vulkan ends in std::terminate -- a failure carrying no reason.
    bool ok = false;
    try {
        ok = runImportChecks();
    } catch (const std::exception& e) {
        wallpaperx::testing::harnessFailure(std::string("the probe could not build its preconditions: ") +
                                            e.what());
    }
    // Both reports run before the verdict, or a fault's reason line is never printed.
    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("while importing an exported frame");
    if (!ok || failed || faulted) return 1;
    // NOT a skip, and not folded into the goals above: those checks ran and passed, so saying they
    // could not be exercised is false, and marking the entry Skipped erases the only real-GPU
    // import coverage there is. Only the GPU-side cleanliness of this run went unjudged. Same
    // shape as device_affinity_probe; a host that legitimately has no layer package sets
    // WALLPAPERX_ALLOW_NO_VALIDATION=1, which announces the waiver and never reaches here.
    if (g_ungradedValidation > 0) {
        std::fprintf(stderr,
                     "frame_import_probe: the validation grade could not be made -- VK_LAYER_KHRONOS_validation is "
                     "not loaded. The checks that ran are still judged; the GPU-side cleanliness of this run is "
                     "not.\n");
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
