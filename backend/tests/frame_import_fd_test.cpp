// SPDX-License-Identifier: GPL-2.0-or-later

// The plugin's GL importer holds no descriptor per frame (S15.421, S15.422): several hundred imports
// in the item's own shape -- import() without the acquire descriptors, then acquire() with them --
// leave this process holding exactly what it held after a warm-up, and so do repeated refusals at
// the driver. Who owns each descriptor on return is checked per call.
//
// Counted against a warm-up, never as an absolute: NVIDIA opens one sync_file of its own the first
// time a context is torn down or a fence is asked for, and keeps it for the process.
//
// The refused semaphore import runs last: on NVIDIA no semaphore import succeeds in a context after
// one is refused.

#include "frame_import.h"

#include "support/test_image.h"
#include "support/validation_grade.h"
// Must precede any GL use: it supplies the entry-point setup and the GL/EGL headers.
#include "tests/gl_import_harness.h"
#include "tests/test_report.h"
#include "renderers/static_renderer.h"
#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

using wallpaperx::FrameImport;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

constexpr const char* kGoals = "S15.421 and S15.422 (the importer holds no descriptor per frame, and hands each back "
                               "as its contract says)";
constexpr uint32_t kWidth = 68;
constexpr uint32_t kHeight = 36;
constexpr int kWarmUp = 8;
constexpr int kRounds = 300;

int g_ungradedValidation = 0;

// 0 when /proc/self/fd could not be walked, which no live process can hold as a real count.
size_t openFdCount() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) return 0;
    size_t count = 0;
    errno = 0;
    while (::readdir(dir) != nullptr) ++count;
    const bool failed = errno != 0;
    ::closedir(dir);
    return failed ? 0 : count;
}

// The open file a descriptor names, so "still ours" is a statement about the file and not about a
// number the driver may have freed and had reissued.
struct FileId {
    dev_t dev = 0;
    ino_t ino = 0;
    bool valid = false;
};

FileId identify(int fd) {
    struct ::stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0) return {};
    return {st.st_dev, st.st_ino, true};
}

bool stillNames(int fd, const FileId& id) {
    const FileId now = identify(fd);
    return id.valid && now.valid && ::fcntl(fd, F_GETFD) != -1 && now.dev == id.dev && now.ino == id.ino;
}

// Closes `fd` only if it still names `id`: a number the importer closed may already belong to
// something else, and closing that would damage the run that is meant to report the fault.
void closeIfStill(int fd, const FileId& id) {
    if (fd >= 0 && stillNames(fd, id)) ::close(fd);
}

// After everything queued has finished on both sides, so a descriptor held until a queued command
// runs is not mistaken for one held for good.
size_t settledCount(wallpaperx::Device& device) {
    glFinish();
    device.waitForSubmissions();
    return openFdCount();
}

FrameImport::Request requestFor(const wallpaperx::ExportedFrame& frame) {
    FrameImport::Request request;
    request.fd = frame.fd;
    request.width = kWidth;
    request.height = kHeight;
    request.memorySize = frame.memorySize;
    request.handleType = frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf ? FrameImport::HandleType::DmaBuf
                                                                                     : FrameImport::HandleType::OpaqueFd;
    request.drmFormat = frame.drmFormat;
    request.drmModifier = frame.modifier;
    request.deviceUuid.assign(frame.deviceUuid.begin(), frame.deviceUuid.end());
    for (const auto& plane : frame.planes) request.planes.emplace_back(plane.offset, plane.rowPitch);
    return request;
}

std::string growth(size_t before, size_t after) {
    return std::to_string(static_cast<long long>(after) - static_cast<long long>(before)) + " (" +
           std::to_string(before) + " held after the warm-up, " + std::to_string(after) + " after " +
           std::to_string(kRounds) + " more)";
}

bool runChecks() {
    return wallpaperx::testing::gradedRun("frame_import_fd_test", [&](wallpaperx::testing::ValidationGrade& grade) {
        grade.reportUngradedTo(g_ungradedValidation);
        wallpaperx::Device device(/*enableValidation=*/true);
        grade.watch(device);

        wallpaperx::testing::HeadlessGl gl;
        std::string why;
        if (!gl.createMatching(wallpaperx::testing::vulkanDeviceUuid(device.physicalDevice()), why)) {
            notVerified(kGoals, "no GL context on the same device as the Vulkan one -- " + why);
            return true;
        }
        wallpaperx::OffscreenTargetManager targets(device);
        const std::string imagePath = wallpaperx::testing::writeGradientPng(
            std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/frame_import_fd_gradient.png", 16, 16);
        wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);
        const wallpaperx::OffscreenTarget& target = targets.create(0, kWidth, kHeight);
        targets.render(renderer, target);

        const wallpaperx::ExportedFrame probe = wallpaperx::exportFrame(device, target);
        FrameImport::Request shape = requestFor(probe);
        ::close(probe.fd);
        shape.fd = -1;
        const bool dmaBuf = shape.handleType == FrameImport::HandleType::DmaBuf;
        std::printf("frame_import_fd_test: %s path, on %s\n", dmaBuf ? "dma_buf" : "opaque fd", gl.renderer().c_str());
        if (!FrameImport::supports(shape.handleType, &why)) {
            notVerified(kGoals, std::string("this context cannot import the ") + (dmaBuf ? "dma_buf" : "opaque fd") +
                                    " this device exports -- " + why);
            return true;
        }
        std::string noSync;
        const bool canWait = FrameImport::supportsSync(&noSync);
        // Printed, not a skip: Mesa's GL has no semaphores, and the entry it runs on exists for the
        // dma_buf memory branch, which it grades in full. The acquire half is the NVIDIA entry's.
        if (!canWait) {
            std::printf("frame_import_fd_test: the acquire descriptors are NOT graded on this context, which cannot "
                        "wait on them (%s); the memory half is graded alone\n",
                        noSync.c_str());
        }

        // One frame in the item's shape. False, with the failure reported, when the importer refused
        // what it was given; `misowned` counts a descriptor the call should have consumed and did not.
        int misowned = 0;
        const auto importOnce = [&](int round) {
            FrameImport::Request request = shape;
            const wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, target);
            request.fd = frame.fd;
            const FileId memoryId = identify(request.fd);
            int syncFd = -1;
            FileId syncId;
            if (canWait) {
                syncFd = wallpaperx::exportFrameAcquireFd(device, target);
                syncId = identify(syncFd);
            }
            const FrameImport::Result imported = FrameImport::import(request);
            if (imported.texture == 0) {
                check(false, ("a frame in the item's shape imports (round " + std::to_string(round) +
                              "): " + imported.error).c_str());
                closeIfStill(request.fd, memoryId);
                closeIfStill(syncFd, syncId);
                return false;
            }
            if (request.fd != -1) {
                ++misowned;
                closeIfStill(request.fd, memoryId);
            }
            bool acquired = true;
            if (canWait) {
                request.syncFds = {syncFd};
                bool refused = false;
                acquired = FrameImport::acquire(request, imported.texture, &why, &refused);
                if (!acquired) {
                    check(false, ("the frame's acquire is waited on (round " + std::to_string(round) + "): " + why)
                                     .c_str());
                    closeIfStill(request.syncFds[0], syncId);
                } else if (request.syncFds[0] != -1) {
                    ++misowned;
                    closeIfStill(request.syncFds[0], syncId);
                }
            }
            FrameImport::release(imported);
            return acquired;
        };

        // GOAL: S15.421 -- several hundred imports in the item's shape, each carrying an acquire
        //       descriptor, leave the process holding no more descriptors than after the warm-up;
        //       and every successful call sets what it consumed to -1 (the memory descriptor after
        //       import(), the acquire descriptor after acquire()).
        // WHY-THIS-SHAPE: growth of zero, not "a few", measured against a warm-up and after both
        //       queues drained: the driver's own descriptors are fixed by then, and one leaked per
        //       frame is the defect. An absolute sync_file count would charge the driver's own one.
        // CONTROL: in backend/kde/frame_import.cpp, via control_batch.sh, 2026-10-08, each reverted:
        //       (a) `::close(syncFd);` deleted from FrameImport::waitOnSyncFds -> on frame_import_fds
        //       (NVIDIA) "FAIL: importing frames in the item's shape holds no descriptor per frame:
        //       growth 300 (56 held after the warm-up, 356 after 300 more)"; (b) `::close(request.fd);`
        //       after the successful import in FrameImport::importOpaqueFd deleted -> the same line
        //       there; (c) the same in FrameImport::importDmaBuf -> the same line on
        //       frame_import_fds_lavapipe, "51 ... 351"; (d) `syncFd = -1;` deleted from waitOnSyncFds
        //       -> "FAIL: each successful call hands back what it consumed as -1 (308 descriptor(s) left
        //       set)", the warm-up's eight included.
        // COVERAGE-ASSERT: every one of the rounds must have imported and waited, and the counts must
        //       have been read.
        for (int i = 0; i < kWarmUp; ++i) {
            if (!importOnce(-1 - i)) return true;
        }
        const size_t before = settledCount(device);
        int done = 0;
        for (int i = 0; i < kRounds; ++i) {
            if (!importOnce(i)) break;
            ++done;
        }
        const size_t after = settledCount(device);
        if (before == 0 || after == 0) {
            notVerified(kGoals, "/proc/self/fd could not be read, so no descriptor was counted");
            return true;
        }
        check(done == kRounds, ("every round imported and waited (" + std::to_string(done) + " of " +
                                std::to_string(kRounds) + ")")
                                   .c_str());
        check(after == before,
              ("importing frames in the item's shape holds no descriptor per frame: growth " + growth(before, after))
                  .c_str());
        check(misowned == 0, ("each successful call hands back what it consumed as -1 (" + std::to_string(misowned) +
                              " descriptor(s) left set)")
                                 .c_str());
        std::printf("frame_import_fd_test: %d frame(s) imported and %s, %zu descriptor(s) before and %zu after\n",
                    done, canWait ? "waited on" : "not waited on (no sync)", before, after);

        // GOAL: S15.422 -- a memory import the driver itself refuses, repeated, holds no descriptor,
        //       and leaves the caller's descriptor open and naming the same file every time.
        // WHY-THIS-SHAPE: refused at the driver, not by the importer's own checks: the opaque fd
        //       declares 64 times its real size, which glImportMemoryFdEXT rejects after it has been
        //       handed a duplicate (the error must be kImportRejectedError, or the refusal came
        //       earlier and graded nothing); a dma_buf names a modifier no driver implements, which
        //       eglCreateImage rejects. Identity by fstat, because a freed number reissued by the
        //       driver would pass an openness test.
        // CONTROL: in FrameImport::importOpaqueFd (backend/kde/frame_import.cpp), via
        //       control_batch.sh, 2026-10-08, NVIDIA, each reverted: (a) `if (sameOpenFile(gift,
        //       request.fd)) ::close(gift);` deleted from the refused-import branch -> "FAIL: a memory
        //       import the driver refuses holds no descriptor: growth 300 (55 held after the warm-up,
        //       355 after 300 more)"; (b) `::close(request.fd);` added to that branch -> "FAIL: and
        //       leaves the caller's descriptor open and its own (300 of 300 not)". The dma_buf branch's
        //       refusal has no control here.
        // COVERAGE-ASSERT: every round must have been refused at the driver.
        {
            int notRefusedThere = 0;
            int lost = 0;
            const auto refuseOnce = [&] {
                FrameImport::Request request = shape;
                const wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, target);
                request.fd = frame.fd;
                if (dmaBuf) {
                    request.drmModifier = 0x0123456789abcdefULL;
                } else {
                    request.memorySize *= 64;
                }
                const FileId id = identify(request.fd);
                const FrameImport::Result result = FrameImport::import(request);
                if (result.texture != 0) {
                    FrameImport::release(result);
                    ++notRefusedThere;
                    return;
                }
                const bool atDriver = dmaBuf ? result.error.find("eglCreateImage") != std::string::npos
                                             : result.error == FrameImport::kImportRejectedError;
                if (!atDriver) ++notRefusedThere;
                if (!stillNames(request.fd, id)) ++lost;
                closeIfStill(request.fd, id);
            };
            for (int i = 0; i < kWarmUp; ++i) refuseOnce();
            const size_t refusedBefore = settledCount(device);
            notRefusedThere = 0;
            lost = 0;
            for (int i = 0; i < kRounds; ++i) refuseOnce();
            const size_t refusedAfter = settledCount(device);
            if (notRefusedThere != 0) {
                harnessFailure(std::to_string(notRefusedThere) + " of " + std::to_string(kRounds) +
                               " staged memory refusals were not refused at the driver, so that branch was not "
                               "exercised");
            } else if (refusedBefore == 0 || refusedAfter == 0) {
                notVerified(kGoals, "/proc/self/fd could not be read around the refused memory imports");
            } else {
                std::printf("frame_import_fd_test: %d memory import(s) refused at the driver, %zu descriptor(s) "
                            "before and %zu after\n",
                            kRounds, refusedBefore, refusedAfter);
                check(refusedAfter == refusedBefore,
                      ("a memory import the driver refuses holds no descriptor: growth " +
                       growth(refusedBefore, refusedAfter))
                          .c_str());
                check(lost == 0, ("and leaves the caller's descriptor open and its own (" + std::to_string(lost) +
                                  " of " + std::to_string(kRounds) + " not)")
                                     .c_str());
            }
        }

        // GOAL: S15.422 -- a semaphore import the driver refuses (a pipe handed over as the acquire
        //       descriptor), repeated, holds no descriptor, and acquire() leaves the caller's
        //       descriptor open, its own, and reports the refusal as the driver's.
        // WHY-THIS-SHAPE: a pipe is a descriptor NVIDIA rejects at glImportSemaphoreFdEXT (a memory
        //       descriptor it accepts), so the branch that duplicates, is refused and must close only
        //       its own duplicate is the one reached. Last, because on NVIDIA the refusal poisons every
        //       later semaphore import in this context.
        // CONTROL: in FrameImport::waitOnSyncFds (backend/kde/frame_import.cpp), via
        //       control_batch.sh, 2026-10-08, NVIDIA, each reverted: (a) `if (sameOpenFile(gift,
        //       syncFd)) ::close(gift);` deleted -> "FAIL: a semaphore import the driver refuses holds
        //       no descriptor: growth 300 (56 held after the warm-up, 356 after 300 more)"; (b) that
        //       line's `::close(gift)` made `::close(syncFd)` -> "FAIL: and acquire() leaves the
        //       caller's descriptor open and its own (300 of 300 not)", with (a)'s line beside it.
        // COVERAGE-ASSERT: every round must have been reported as refused by the driver.
        if (canWait) {
            FrameImport::Request held = shape;
            const wallpaperx::ExportedFrame frame = wallpaperx::exportFrame(device, target);
            held.fd = frame.fd;
            const FileId heldId = identify(held.fd);
            const FrameImport::Result texture = FrameImport::import(held);
            if (texture.texture == 0) {
                closeIfStill(held.fd, heldId);
                check(false, ("a frame imports to wait against: " + texture.error).c_str());
                return true;
            }
            int notRefused = 0;
            int lost = 0;
            const auto refuseOnce = [&] {
                int ends[2] = {-1, -1};
                if (::pipe2(ends, O_CLOEXEC) != 0) {
                    ++notRefused;
                    return;
                }
                const FileId id = identify(ends[0]);
                FrameImport::Request request = shape;
                request.syncFds = {ends[0]};
                bool refused = false;
                const bool ok = FrameImport::acquire(request, texture.texture, &why, &refused);
                if (ok || !refused) ++notRefused;
                if (!stillNames(request.syncFds[0], id)) ++lost;
                closeIfStill(request.syncFds[0], id);
                ::close(ends[1]);
            };
            for (int i = 0; i < kWarmUp; ++i) refuseOnce();
            const size_t refusedBefore = settledCount(device);
            notRefused = 0;
            lost = 0;
            for (int i = 0; i < kRounds; ++i) refuseOnce();
            const size_t refusedAfter = settledCount(device);
            FrameImport::release(texture);
            if (notRefused != 0) {
                notVerified(kGoals, std::to_string(notRefused) + " of " + std::to_string(kRounds) +
                                        " pipes handed over as acquire descriptors were not refused by this driver's "
                                        "glImportSemaphoreFdEXT, so the refused branch was not reached");
            } else if (refusedBefore == 0 || refusedAfter == 0) {
                notVerified(kGoals, "/proc/self/fd could not be read around the refused semaphore imports");
            } else {
                std::printf("frame_import_fd_test: %d semaphore import(s) refused at the driver, %zu descriptor(s) "
                            "before and %zu after\n",
                            kRounds, refusedBefore, refusedAfter);
                check(refusedAfter == refusedBefore,
                      ("a semaphore import the driver refuses holds no descriptor: growth " +
                       growth(refusedBefore, refusedAfter))
                          .c_str());
                check(lost == 0, ("and acquire() leaves the caller's descriptor open and its own (" +
                                  std::to_string(lost) + " of " + std::to_string(kRounds) + " not)")
                                     .c_str());
            }
        }
        return true;
    });
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "frame_import_fd_test";
    bool ok = false;
    try {
        ok = runChecks();
    } catch (const std::exception& e) {
        harnessFailure(std::string("the test could not build its preconditions: ") + e.what());
    }
    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file, not in the importer");
    if (!ok || failed || faulted) return 1;
    if (g_ungradedValidation > 0) {
        std::fprintf(stderr, "frame_import_fd_test: the validation grade could not be made -- "
                             "VK_LAYER_KHRONOS_validation is not loaded.\n");
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
