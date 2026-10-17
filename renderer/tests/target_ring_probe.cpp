// SPDX-License-Identifier: GPL-2.0-or-later
//
// Which of a monitor's images the next render draws into, and what makes an image "the
// published frame".
//
// The daemon now keeps three images per monitor and asks OffscreenTargetManager for one no
// consumer is reading. Two claims underpin everything the daemon does with that answer, and
// neither is visible from the daemon tier:
//
//   * the answer is a QUERY -- asking does not rotate the ring, so a render that is refused,
//     superseded or throws leaves the published frame naming the image that was last drawn;
//   * an image named in the held list is never returned, and that really does protect the
//     bytes -- established here by reading the held image back off the GPU after three more
//     renders, which is a device-side copy and therefore answerable on both ICDs. The
//     backend tier cannot do this: a CPU mapping of an exported frame needs DMA_BUF with a
//     LINEAR layout, and the project's target GPU exports opaque file descriptors.
//
// Depth 3 here is this probe's own fixture, not the daemon's wallpaperx::kTargetDepth
// (backend/daemon/ring_depth.h) -- a renderer probe may not include a backend header. That the
// *daemon* keeps its ring deep enough is graded at the backend tier, in dbus_service_test's
// buffer-hold phase.

#include "vulkan/device.h"
#include "vulkan/offscreen.h"
#include "renderers/static_renderer.h"

#include "support/check.h"
#include "support/gpu_readback.h"
#include "support/test_image.h"
#include "support/validation_grade.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wallpaperx::Device;
using wallpaperx::OffscreenTarget;
using wallpaperx::OffscreenTargetManager;
using wallpaperx::StaticRenderer;
using wallpaperx::testing::check;

// Separate monitors so the two halves cannot disturb each other: the pixel half renders
// four times and reads images back (which unpublishes them), and the query half asserts on
// exactly where the cursor sits.
constexpr int kRingMonitor = 0;
constexpr int kContentMonitor = 1;
// Depth 1, rebuilt per reference frame: renderReference() creates its own target.
constexpr int kReferenceMonitor = 2;

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 48;
constexpr uint32_t kDepth = 3;

std::string imagePath(const char* name) {
    return std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/ring_" + name + ".png";
}

// Solid fills, not gradients. What is being told apart here is "which of four renders left
// its pixels in this image", so four flat colours discriminate perfectly and the two-axis
// variation gpu_readback's variesInBothAxes() demands is irrelevant -- that guard is about
// row-stride and plane-offset mistakes in an *import*, and nothing here imports anything.
std::string writeFill(const char* name, std::array<unsigned char, 4> rgba) {
    return wallpaperx::testing::writeSolidPng(imagePath(name), 8, 8, rgba);
}

struct Slot {
    uint64_t id = 0;
    VkImage image = VK_NULL_HANDLE;
};

// Every slot of a monitor's ring, walked with advance() -- one full lap, which leaves the
// cursor exactly where it started.
//
// Deliberately NOT walked with nextUnheld() and a growing held set, which is the shorter way
// to write this. That version compares nextUnheld against itself: an implementation that
// returned one slot forever would yield a one-element list, every later comparison would be
// about that one slot, and the checks would report the ring as consistent. The lap is
// independent of the thing under test.
std::vector<Slot> lapRing(OffscreenTargetManager& targets, int monitorId, uint32_t depth) {
    std::vector<Slot> slots;
    for (uint32_t i = 0; i < depth; ++i) {
        const OffscreenTarget& target = targets.advance(monitorId);
        slots.push_back({target.bufferId, target.image.image});
    }
    return slots;
}

// Every subset of `ids`, as index bitmasks. Small and exhaustive on purpose: with three
// slots there are eight cases and no reason to sample.
std::vector<std::vector<uint64_t>> allSubsets(const std::vector<uint64_t>& ids) {
    std::vector<std::vector<uint64_t>> subsets;
    for (uint32_t mask = 0; mask < (1u << ids.size()); ++mask) {
        std::vector<uint64_t> subset;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (mask & (1u << i)) subset.push_back(ids[i]);
        }
        subsets.push_back(std::move(subset));
    }
    return subsets;
}

// ------------------------------------------------------------------ //
// Half one: what nextUnheld() answers, and what moves the ring cursor.
// ------------------------------------------------------------------ //
void runSelectionChecks(Device& device, OffscreenTargetManager& targets, const std::string& redPath) {
    targets.create(kRingMonitor, kWidth, kHeight, kDepth);

    const OffscreenTarget* const lapStart = &targets.get(kRingMonitor);
    const std::vector<Slot> slots = lapRing(targets, kRingMonitor, kDepth);
    check(&targets.get(kRingMonitor) == lapStart,
          "a full lap of advance() returns the ring to the slot it started on, so the walk below saw each image "
          "exactly once");

    std::vector<uint64_t> ids;
    std::set<uint64_t> uniqueIds;
    for (const Slot& slot : slots) {
        ids.push_back(slot.id);
        uniqueIds.insert(slot.id);
    }

    // GOAL: the slots carry three distinct, non-zero buffer ids.
    // WHY-THIS-SHAPE: every other check here says "a different BUFFER ID was chosen", and
    //       that phrasing only means "the consumer's pixels were left alone" if distinct ids
    //       really are distinct memory. Aliased slots would satisfy all of them while the
    //       producer overwrote the frame in use -- the exact defect this ring exists to close.
    //       Checked once, here, so the rest may speak in ids. That distinct ids are distinct
    //       memory is graded in pixels by the held-content phase.
    // CONTROL: `target.bufferId = g_nextBufferId.fetch_add(1, std::memory_order_relaxed);` in
    //       OffscreenTargetManager::makeTarget (renderer/vulkan/offscreen.cpp) changed to
    //       `target.bufferId = 1;` (re-run 2026-10-04 via control_batch.sh) -> "FAIL: every
    //       image has a distinct, non-zero buffer id" as the first line, then both phases
    //       abandoning themselves ("could not name a second image", "reported nowhere to
    //       draw"). Reverted.
    check(uniqueIds.size() == kDepth && uniqueIds.count(0) == 0, "every image has a distinct, non-zero buffer id");

    // GOAL: with nothing published yet, the next render draws into the slot the ring is
    //       already on -- image zero is not skipped past and left blank.
    // CONTROL: the `ring.images[ring.current].published() ? 1 : 0` in
    //       OffscreenTargetManager::nextUnheld (renderer/vulkan/offscreen.cpp) replaced by a
    //       bare `1` -> "FAIL: with nothing published, the next render draws into the slot
    //       the ring is on". Reverted.
    check(targets.nextUnheld(kRingMonitor, {}) == &targets.get(kRingMonitor),
          "with nothing published, the next render draws into the slot the ring is on");

    StaticRenderer red(device, targets.renderPass(), redPath, WALLPAPERX_SHADER_DIR);

    // GOAL: a completed render is what makes its image the published frame -- get() and "the
    //       image that was just drawn" cannot name different slots, whichever slot that was.
    // WHY-THIS-SHAPE: draws into the slot TWO past the cursor, named by putting the other two
    //       ids in `held`, rather than into whatever nextUnheld() offers first. The obvious
    //       version renders into nextUnheld(m, {}) -- which before anything is published is
    //       the slot the cursor is already on, so `get() == drawn` holds for a render() that
    //       does not touch the cursor at all. Measured: deleting makeCurrent() left that
    //       version green. Only a render that lands somewhere the cursor would not have gone
    //       by itself can tell "the cursor follows the render" from "the cursor never moved"
    //       or "the cursor advanced by one".
    // CONTROL: two, both in OffscreenTargetManager::render (renderer/vulkan/offscreen.cpp),
    //       both reverted. (a) `makeCurrent(target);` deleted and (b) `makeCurrent(target);`
    //       replaced by an advance-by-one of the owning ring: each produces the same four red
    //       lines, led by "FAIL: the image a render drew into is the one get() then names".
    //       The other three are the later cursor checks in this file, which the same broken
    //       cursor also fails -- see the last block for the mutation that separates them.
    // Each answer is checked for null before the next question is built from it. Not
    // defensive noise: a ring whose slots share a buffer id -- which is what the distinctness
    // check above exists to catch -- answers "nowhere" to the second question, and
    // dereferencing that answer kills the process by SIGSEGV before reportFailures() prints
    // anything. Observed: the first check went red and the run then segfaulted, so the verdict
    // for every other check in the file was lost.
    const OffscreenTarget* const firstSlot = targets.nextUnheld(kRingMonitor, {});
    if (firstSlot == nullptr) {
        wallpaperx::testing::fail("the ring could not name a first image, so the cursor checks were abandoned");
        return;
    }
    const OffscreenTarget* const secondSlot = targets.nextUnheld(kRingMonitor, {firstSlot->bufferId});
    if (secondSlot == nullptr) {
        wallpaperx::testing::fail("the ring could not name a second image, so the cursor checks were abandoned");
        return;
    }
    const OffscreenTarget* const drawn =
        targets.nextUnheld(kRingMonitor, {firstSlot->bufferId, secondSlot->bufferId});
    if (drawn == nullptr) {
        wallpaperx::testing::fail("the ring could not name a third image, so the cursor checks were abandoned");
        return;
    }
    targets.render(red, *drawn);
    check(&targets.get(kRingMonitor) == drawn, "the image a render drew into is the one get() then names");

    // GOAL: nextUnheld() is a query. Asking N times without rendering returns the same image
    //       every time, and leaves the published frame where it was.
    // WHY-THIS-SHAPE: two placement decisions, and both were measured rather than assumed.
    //       (1) AFTER the first render. With nothing published the search starts at the
    //       current slot, so a cursor moved to the answer moves it onto the slot it was
    //       already on, and a rotation is indistinguishable from a query -- the mutation
    //       below left an earlier version of this block, placed before the render, green.
    //       (2) BEFORE any other call that queries this ring. A rotation corrupts the cursor
    //       on its first use, and from then on the ring is self-consistent again (the cursor
    //       sits on an unpublished slot, which every later ask returns), so a block placed
    //       after even one other nextUnheld() call sees nothing wrong. `publishedBefore` is
    //       therefore read before the first ask, not after it.
    //       Asking eight times rather than twice: a two-call comparison is satisfied by a
    //       rotation of period 2, and at depth 3 by a period-3 one on the wrap. Requiring all
    //       eight answers to agree cannot be satisfied by any rotation. The daemon reaches
    //       exactly this state when a render is refused for want of a free image and the
    //       monitor is re-posted, which is a loop and not a one-off.
    // CONTROL: two, both in OffscreenTargetManager::nextUnheld
    //       (renderer/vulkan/offscreen.cpp), both reverted. (a) `const_cast<Ring&>(ring).current
    //       = (ring.current + step) % count;` before the `return &candidate;` -- rotate at
    //       selection time -> "FAIL: asking which image to draw into does not move the
    //       published frame". Its agreement half stays green under (a), for the reason in (2)
    //       above. (b) an unconditional `const_cast<Ring&>(ring).current = (ring.current + 1)
    //       % count;` as the first statement of the function -> "FAIL: asking which image to
    //       draw into does not itself change the answer / 5 of 8 answers agreed". Under (b)
    //       the cursor half stays green: nine asks at depth 3 land the cursor back where it
    //       started, which is why the agreement count and not the cursor is what catches a
    //       rotation that never stops.
    // COVERAGE-ASSERT: the number of answers compared is asserted, not just their agreement,
    //       so an edit that reduced the loop to one iteration fails instead of going quiet.
    {
        constexpr int kAsks = 8;
        const OffscreenTarget* const publishedBefore = &targets.get(kRingMonitor);
        const OffscreenTarget* const firstAnswer = targets.nextUnheld(kRingMonitor, {});
        int agreed = 0;
        for (int i = 0; i < kAsks; ++i) {
            if (targets.nextUnheld(kRingMonitor, {}) == firstAnswer) ++agreed;
        }
        check(agreed == kAsks, "asking which image to draw into does not itself change the answer");
        if (agreed != kAsks) {
            std::fprintf(stderr, "  %d of %d answers agreed; a query cannot disagree with itself\n", agreed, kAsks);
        }
        check(&targets.get(kRingMonitor) == publishedBefore,
              "asking which image to draw into does not move the published frame");
    }

    // GOAL: once a frame is published, the next render goes somewhere else -- a consumer that
    //       has been told about a frame is not redrawn over merely because it holds nothing
    //       yet. The window between the reply being built and the consumer's import is
    //       exactly where a repaint would be invisible.
    // CONTROL: the same `? 1 : 0` -> `0` mutation as above, in
    //       OffscreenTargetManager::nextUnheld (renderer/vulkan/offscreen.cpp) -> "FAIL: once
    //       a frame is published, the next render is aimed at a different image". Reverted.
    check(targets.nextUnheld(kRingMonitor, {}) != &targets.get(kRingMonitor),
          "once a frame is published, the next render is aimed at a different image");

    // GOAL: an image named in `held` is never handed back, and when every image is named the
    //       answer is "nowhere to draw" rather than an image somebody is reading.
    // WHY-THIS-SHAPE: exhaustive over all eight subsets rather than the one case the daemon
    //       happens to produce. Which subset is held depends on how many consumers there are
    //       and in what order they fetched -- a single case would pass for an implementation
    //       that only checks the first entry of the list, or only the last.
    // CONTROL: `std::find(held.begin(), held.end(), candidate.bufferId) == held.end()` in
    //       OffscreenTargetManager::nextUnheld (renderer/vulkan/offscreen.cpp) replaced by
    //       `true` -> "FAIL: no held image is ever offered as somewhere to draw / 4 of 8
    //       subsets answered correctly" and "FAIL: with every image held there is nowhere to
    //       draw". Four and not seven: with the held list ignored every subset gets the same
    //       slot back, and exactly the four subsets that do not name that slot pass. Reverted.
    // COVERAGE-ASSERT: the number of subsets examined is asserted, so a sweep that stopped
    //       early fails rather than passing on the subsets it did reach.
    {
        const std::vector<std::vector<uint64_t>> subsets = allSubsets(ids);
        int examined = 0;
        int correct = 0;
        bool fullSetRefused = false;
        for (const std::vector<uint64_t>& held : subsets) {
            ++examined;
            const OffscreenTarget* next = targets.nextUnheld(kRingMonitor, held);
            if (held.size() == ids.size()) {
                fullSetRefused = next == nullptr;
                if (fullSetRefused) ++correct;
                continue;
            }
            if (next == nullptr) continue;
            if (std::find(held.begin(), held.end(), next->bufferId) == held.end()) ++correct;
        }
        check(examined == static_cast<int>(1u << kDepth), "every combination of held images was put to nextUnheld");
        check(correct == examined, "no held image is ever offered as somewhere to draw");
        if (correct != examined) {
            std::fprintf(stderr, "  %d of %d subsets answered correctly (the full set must answer \"nowhere\")\n",
                         correct, examined);
        }
        check(fullSetRefused, "with every image held there is nowhere to draw");
    }

    // GOAL: a selection the caller then abandons -- which is exactly what a deferred render
    //       is -- does not move the published frame. Stated separately from the
    //       eight-answers check above because there the ring had published nothing, and the
    //       state that matters in the daemon is a ring that HAS a published frame and is
    //       being asked, refused and asked again.
    // CONTROL: the same `ring.current = ...` insertion into
    //       OffscreenTargetManager::nextUnheld (renderer/vulkan/offscreen.cpp) -> "FAIL: a
    //       render that is asked for and then abandoned leaves the published frame alone".
    //       Reverted.
    check(&targets.get(kRingMonitor) == drawn,
          "a render that is asked for and then abandoned leaves the published frame alone");

    // GOAL: the cursor follows the image that was actually drawn, not the next one round the
    //       ring. False for a render() that advances the cursor by one instead of pointing it
    //       at its argument -- which agrees with the check above on every render that happens
    //       to take the adjacent slot, and is wrong the moment a held image is skipped.
    // WHY-THIS-SHAPE: renders into the slot TWO past the current one, which is the only
    //       distance at depth 3 that a plus-one rotation cannot fake. Deliberately reaches
    //       for that slot by naming it in `held` rather than by advance(), which would move
    //       the cursor and make the question meaningless.
    // CONTROL: the body of OffscreenTargetManager::makeCurrent (renderer/vulkan/offscreen.cpp)
    //       guarded by `if (!ring.images[ring.current].published())`, so the cursor follows
    //       the FIRST render on a ring and no later one -> "FAIL: the published frame names
    //       the image the render drew into, not the next one round the ring" as the run's
    //       ONLY failure. That single red line is the measurement showing this check is not
    //       redundant with the two cursor checks above, which stay green under it. A plain
    //       advance-by-one instead reddens all four together and separates nothing. Reverted.
    {
        const uint64_t currentId = targets.get(kRingMonitor).bufferId;
        const OffscreenTarget* const adjacent = targets.nextUnheld(kRingMonitor, {currentId});
        if (adjacent == nullptr) {
            wallpaperx::testing::fail("the ring could not name a second image, so the skip-a-slot check was "
                                      "abandoned");
            return;
        }
        const OffscreenTarget* const twoAway = targets.nextUnheld(kRingMonitor, {currentId, adjacent->bufferId});
        if (twoAway == nullptr) {
            wallpaperx::testing::fail("the ring could not name a third image, so the skip-a-slot check was "
                                      "abandoned");
            return;
        }
        targets.render(red, *twoAway);
        check(&targets.get(kRingMonitor) == twoAway,
              "the published frame names the image the render drew into, not the next one round the ring");
    }
}

// ------------------------------------------------------------------ //
// Half two: the bytes of a held image survive the renders that follow.
// ------------------------------------------------------------------ //
void runHeldContentChecks(Device& device, OffscreenTargetManager& targets, const std::string& redPath,
                          const std::string& bluePath, const std::string& greenPath,
                          const std::string& yellowPath) {
    // The two frames the comparison is made against. Rendered on a monitor of their own and
    // read straight back, so they are what this device and this shader produce -- not a
    // colour this file believes the PNG contains.
    std::vector<unsigned char> redReference;
    std::vector<unsigned char> yellowReference;
    {
        StaticRenderer red(device, targets.renderPass(), redPath, WALLPAPERX_SHADER_DIR);
        redReference = wallpaperx::testing::renderReference(device, targets, red, kReferenceMonitor, kWidth, kHeight);
    }
    {
        StaticRenderer yellow(device, targets.renderPass(), yellowPath, WALLPAPERX_SHADER_DIR);
        yellowReference =
            wallpaperx::testing::renderReference(device, targets, yellow, kReferenceMonitor, kWidth, kHeight);
    }
    targets.destroy(kReferenceMonitor);

    // COVERAGE-ASSERT: if the two reference frames were equal, every comparison below would
    // hold for a producer that had overwritten the held image, and the phase would be
    // vacuous. Asserted rather than assumed because it depends on two PNG fixtures and a
    // shader, none of which this file controls.
    // CONTROL: yellowReference assigned from redReference -> "FAIL: the red and yellow
    //       reference frames differ, so a comparison against them can tell them apart"
    //       followed by "FAIL: the fourth render actually drew"; the held-image check stayed
    //       green, which is exactly the vacuity this guards. Reverted.
    const size_t referenceRowsDiffering =
        wallpaperx::testing::countDifferingRows(redReference, yellowReference, kWidth, kHeight);
    check(referenceRowsDiffering == kHeight,
          "the red and yellow reference frames differ, so a comparison against them can tell them apart");

    targets.create(kContentMonitor, kWidth, kHeight, kDepth);

    // GOAL: the producer does not write over an image a consumer is reading -- stated in
    //       pixels, not in buffer ids. With one image held, three further renders land
    //       somewhere else and the held image still reads back as the frame it was handed.
    // WHY-THIS-SHAPE: FOUR renders, not two. With depth 3 the ring hands out three distinct
    //       slots in a row whether or not it honours `held` at all, so a two- or three-render
    //       version passes for an implementation that ignores its held list entirely -- the
    //       trap this ring's depth sets. The fourth render is the first one that has to wrap,
    //       and it is the only one whose destination is decided by the held list.
    //       Read back through Vulkan rather than through a CPU mapping of the exported fd:
    //       a mapping needs DMA_BUF with a LINEAR layout, which the target GPU does not
    //       produce, so that version would skip on the hardware that matters.
    // CONTROL: `std::find(...) == held.end()` in OffscreenTargetManager::nextUnheld
    //       (renderer/vulkan/offscreen.cpp) replaced by `true` -> "FAIL: an image a consumer
    //       holds still reads back as the frame it was handed, after three more renders (48
    //       of 48 rows differ from it)". Reverted.
    const OffscreenTarget* const heldTarget = targets.nextUnheld(kContentMonitor, {});
    // Guarded for the reason the selection phase guards its answers: a null here is
    // dereferenced two lines down and kills the process by SIGSEGV before reportFailures()
    // prints anything, so the verdict for every check in this phase would be lost.
    // CONTROL: `if (held.empty()) return nullptr;` added at the top of
    //       OffscreenTargetManager::nextUnheld (renderer/vulkan/offscreen.cpp). Without this
    //       guard the run ended "***Exception: SegFault" with no verdict; with it, the phase
    //       prints "FAIL: a monitor with nothing held reported nowhere to draw ..." and the
    //       run reports its failures normally. Reverted.
    if (heldTarget == nullptr) {
        wallpaperx::testing::fail("a monitor with nothing held reported nowhere to draw, so the held-content check "
                                  "was abandoned");
        return;
    }
    {
        StaticRenderer red(device, targets.renderPass(), redPath, WALLPAPERX_SHADER_DIR);
        targets.render(red, *heldTarget);
    }
    const uint64_t heldId = heldTarget->bufferId;

    const OffscreenTarget* lastDrawn = nullptr;
    const std::array<const std::string*, 3> rest{&bluePath, &greenPath, &yellowPath};
    for (const std::string* path : rest) {
        const OffscreenTarget* next = targets.nextUnheld(kContentMonitor, {heldId});
        if (next == nullptr) {
            wallpaperx::testing::fail("a monitor with one of three images held reported nowhere to draw, so the "
                                      "held-content check was abandoned");
            return;
        }
        StaticRenderer renderer(device, targets.renderPass(), *path, WALLPAPERX_SHADER_DIR);
        targets.render(renderer, *next);
        lastDrawn = next;
    }

    // The held image is read back FIRST, and the order is load-bearing. reclaimAndReadback()
    // unpublishes what it reads and throws for an image that is not published, so on the very
    // failure this phase exists to catch -- a later render landing on the held image, leaving
    // the two names pointing at one slot -- reading the other one first would throw out of the
    // phase before the pixel comparison ever ran. Reading the held image first makes that
    // failure a row count instead of an exception.
    const std::vector<unsigned char> heldPixels = wallpaperx::testing::reclaimAndReadback(device, targets, *heldTarget);
    const size_t heldRowsDiffering =
        wallpaperx::testing::countDifferingRows(heldPixels, redReference, kWidth, kHeight);
    check(heldRowsDiffering == 0,
          "an image a consumer holds still reads back as the frame it was handed, after three more renders");
    if (heldRowsDiffering != 0) {
        std::fprintf(stderr, "  %zu of %u rows differ from it\n", heldRowsDiffering, kHeight);
    }

    // Only reachable when the producer really did draw elsewhere; the check above has already
    // gone red otherwise, and this one has nothing left to read.
    if (lastDrawn == nullptr || lastDrawn == heldTarget) {
        std::fprintf(stderr, "  the later renders landed on the held image, so \"the fourth render actually drew\" "
                             "had no second image to read and was not graded\n");
        return;
    }
    const std::vector<unsigned char> lastPixels = wallpaperx::testing::reclaimAndReadback(device, targets, *lastDrawn);
    const size_t lastRowsDiffering =
        wallpaperx::testing::countDifferingRows(lastPixels, yellowReference, kWidth, kHeight);
    check(lastRowsDiffering == 0, "the fourth render actually drew");
    if (lastRowsDiffering != 0) {
        std::fprintf(stderr, "  %zu of %u rows differ from the yellow reference, so the held-image check above had "
                             "no producer under it\n",
                     lastRowsDiffering, kHeight);
    }
}

// GOAL: S15.290 -- a buffer id names one image in the whole process: two target managers on one
//       device hand out disjoint, non-zero ids. False for a counter per manager, under which a
//       consumer declaring an id it holds of one manager would be sent no memory for another's image.
// WHY-THIS-SHAPE: two managers alive at once, a full ring in each, so a per-manager counter starting
//       at 1 in both collides on every slot; ids within one manager are graded above.
// CONTROL: in renderer/vulkan/offscreen.cpp, the process-wide g_nextBufferId replaced by a member
//       counter of OffscreenTargetManager starting at 1 -> "FAIL: two target managers in one process
//       hand out disjoint, non-zero buffer ids (3 distinct of 6, 3 shared)". Via control_batch.sh,
//       2026-10-04. Reverted.
void runCrossManagerIdChecks(Device& device) {
    std::set<uint64_t> first;
    std::set<uint64_t> both;
    size_t shared = 0;
    {
        OffscreenTargetManager one(device);
        OffscreenTargetManager two(device);
        one.create(kRingMonitor, kWidth, kHeight, kDepth);
        two.create(kRingMonitor, kWidth, kHeight, kDepth);
        for (const Slot& slot : lapRing(one, kRingMonitor, kDepth)) {
            first.insert(slot.id);
            both.insert(slot.id);
        }
        for (const Slot& slot : lapRing(two, kRingMonitor, kDepth)) {
            if (first.count(slot.id) != 0) ++shared;
            both.insert(slot.id);
        }
    }
    check(both.size() == 2 * kDepth && shared == 0 && both.count(0) == 0,
          ("two target managers in one process hand out disjoint, non-zero buffer ids (" + std::to_string(both.size()) +
           " distinct of " + std::to_string(2 * kDepth) + ", " + std::to_string(shared) + " shared)")
              .c_str());
}

}  // namespace

int main() {
    const bool graded =
        wallpaperx::testing::gradedRun("target_ring_probe", [](wallpaperx::testing::ValidationGrade& grade) {
            try {
                const std::string redPath = writeFill("red", {255, 0, 0, 255});
                const std::string bluePath = writeFill("blue", {0, 0, 255, 255});
                const std::string greenPath = writeFill("green", {0, 255, 0, 255});
                const std::string yellowPath = writeFill("yellow", {255, 255, 0, 255});

                Device device(/*enableValidation=*/true);
                grade.watch(device);
                std::printf("target_ring_probe: on %s\n", device.info().gpuName.c_str());
                OffscreenTargetManager targets(device);

                runSelectionChecks(device, targets, redPath);
                runHeldContentChecks(device, targets, redPath, bluePath, greenPath, yellowPath);
                runCrossManagerIdChecks(device);
            } catch (const std::exception& e) {
                // Reported, not rethrown: an escaping exception would skip the validation
                // grade in gradedRun's frame and print no verdict for the checks that did run.
                wallpaperx::testing::fail(std::string("target_ring_probe threw: ") + e.what());
            }
            return wallpaperx::testing::failureCount() == 0;
        });

    if (!graded || wallpaperx::testing::reportFailures("target_ring_probe") > 0) return 1;
    std::printf("target_ring_probe: all checks passed, zero validation warnings\n");
    return 0;
}
