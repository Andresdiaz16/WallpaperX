// SPDX-License-Identifier: GPL-2.0-or-later

// Each caller's share of the render worker's queue, graded on WallpaperManager's own record: no bus,
// no device, no worker. The bus-level half -- the refusal on the wire, its log line, a second caller
// served, a refused caller's departure -- is dbus_service_test's requestShare phase; what is here is
// what the bus cannot show: when a run of refusals ends, and what a departure does to a caller's
// record when nothing of it is queued.
//
// The quiet period that ends a run of refusals is ten seconds and is not configurable, so the phase
// that grades it waits for it in real time.

#include "daemon/wallpaper_manager.h"

#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <vector>

namespace {

using wallpaperx::WallpaperManager;
using Admission = WallpaperManager::Admission;
using wallpaperx::testing::check;

// Mirrored from WallpaperManager::kMaxQueuedRequestsPerClient and the manager's ten-second quiet
// period rather than read from them, so a change to either in production alone reddens this file.
constexpr int kExpectedShare = 16;
constexpr auto kExpectedQuiet = std::chrono::seconds(10);

const char* admissionName(Admission admission) {
    switch (admission) {
        case Admission::Admitted: return "Admitted";
        case Admission::Refused: return "Refused";
        case Admission::FirstRefusal: return "FirstRefusal";
    }
    return "?";
}

// Fills `client`'s share and asks once more. Returns the answer to that one; `admitted` is how many
// of the share were admitted, which every caller checks before reading the answer.
Admission fillAndAskOnce(WallpaperManager& manager, const std::string& client, int& admitted) {
    admitted = 0;
    for (int i = 0; i < kExpectedShare; ++i) {
        if (manager.admitRequest(client) == Admission::Admitted) ++admitted;
    }
    return manager.admitRequest(client);
}

// Answers `count` admitted requests; true when the last one reported the client's departure.
bool answer(WallpaperManager& manager, const std::string& client, int count) {
    bool departed = false;
    for (int i = 0; i < count; ++i) departed = manager.noteRequestDone(client);
    return departed;
}

// GOAL: S15.279 -- a run of refusals ends after ten seconds without one, and not before: a second
//       burst from the same caller inside the quiet period is not a first refusal, and one after more
//       than ten seconds of quiet is. (FirstRefusal is what DbusService::postCall logs on; the line
//       itself is graded on the bus by dbus_service_test's requestShare phase.)
// WHY-THIS-SHAPE: driven through admitRequest and noteRequestDone directly, so the queue is exactly
//       full when the extra request is asked, with no worker to drain it. The quiet period is not
//       configurable, so the second half sleeps it out in real time -- half a second past it, which
//       a correct manager cannot answer any other way. The inside half asks again within
//       microseconds, so no scheduling delay could carry it past ten seconds.
// CONTROL: two, observed 2026-10-04, both reverted. (a) QueuedRequests::refusing
//       (backend/daemon/wallpaper_manager.cpp) made to return `lastRefusal.has_value()` -> "FAIL: a
//       burst after more than ten seconds without a refusal starts a new run (16 admitted, then
//       Refused)". (b) kRequestRefusalQuiet (backend/daemon/wallpaper_manager.h) made 0 s -> "FAIL: a
//       second burst inside the quiet period, the queue emptied between, is not a new run (16
//       admitted, then FirstRefusal)".
// COVERAGE-ASSERT: every burst must have had its whole share admitted, so each extra request is
//       provably the one past the share.
void runQuietPeriodChecks(WallpaperManager& manager) {
    const std::string client = ":1.quiet";
    int admitted = 0;
    const Admission first = fillAndAskOnce(manager, client, admitted);
    check(admitted == kExpectedShare && first == Admission::FirstRefusal,
          ("the request past a full share is the first refusal of a run (" + std::to_string(admitted) +
           " admitted, then " + admissionName(first) + ")")
              .c_str());
    (void)answer(manager, client, admitted);

    const Admission inside = fillAndAskOnce(manager, client, admitted);
    const auto lastRefusal = std::chrono::steady_clock::now();
    check(admitted == kExpectedShare && inside == Admission::Refused,
          ("a second burst inside the quiet period, the queue emptied between, is not a new run (" +
           std::to_string(admitted) + " admitted, then " + admissionName(inside) + ")")
              .c_str());
    (void)answer(manager, client, admitted);

    std::this_thread::sleep_until(lastRefusal + kExpectedQuiet + std::chrono::milliseconds(500));
    const Admission after = fillAndAskOnce(manager, client, admitted);
    check(admitted == kExpectedShare && after == Admission::FirstRefusal,
          ("a burst after more than ten seconds without a refusal starts a new run (" + std::to_string(admitted) +
           " admitted, then " + admissionName(after) + ")")
              .c_str());
    (void)answer(manager, client, admitted);
}

// GOAL: S15.280 -- a caller that leaves the bus with nothing queued and a run of refusals open has
//       its record erased: a later caller under the same name starts afresh, so its first refusal
//       opens a new run and its answered requests report no departure.
// WHY-THIS-SHAPE: bus names are never reused, so the erase is invisible on the bus; reusing the name
//       here is the only way to ask the record whether it is still there. Both of its fields are
//       read: the open run (FirstRefusal, not Refused) and the departure mark (noteRequestDone false).
// CONTROL: WallpaperManager::noteDeparted (backend/daemon/wallpaper_manager.cpp) made to only set
//       `departed`, never erase -> "FAIL: after its departure, the same name's first refusal opens a
//       new run (16 admitted, then Refused)" and "FAIL: and its answered requests report no
//       departure".
//       Observed 2026-10-04. Reverted.
void runDepartureEraseChecks(WallpaperManager& manager) {
    const std::string client = ":1.departs";
    int admitted = 0;
    const Admission first = fillAndAskOnce(manager, client, admitted);
    (void)answer(manager, client, admitted);
    if (admitted != kExpectedShare || first != Admission::FirstRefusal) {
        check(false, ("a fresh caller's request past its share is a first refusal (" + std::to_string(admitted) +
                      " admitted, then " + admissionName(first) + ")")
                         .c_str());
        return;
    }
    manager.noteDeparted(client);

    const Admission again = fillAndAskOnce(manager, client, admitted);
    check(admitted == kExpectedShare && again == Admission::FirstRefusal,
          ("after its departure, the same name's first refusal opens a new run (" + std::to_string(admitted) +
           " admitted, then " + admissionName(again) + ")")
              .c_str());
    const bool reportedDeparture = answer(manager, client, admitted);
    check(!reportedDeparture, "and its answered requests report no departure");
}

// GOAL: S15.281 -- a refused request is not counted: a caller refused past its share that then
//       leaves with its share still queued has its departure reported when the last of the queued
//       requests is answered, and not before.
// WHY-THIS-SHAPE: the manager-level twin of dbus_service_test's S15.278, which grades the same
//       defect through the hold ledger but needs a GPU; this one runs on every host.
// CONTROL: `++queued.count;` moved above the cap test in WallpaperManager::admitRequest
//       (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a refused caller that left is reported
//       departed when its last queued request is answered (reported at none of 16)". Observed
//       2026-10-04. Reverted.
void runRefusalUncountedChecks(WallpaperManager& manager) {
    const std::string client = ":1.refused-then-left";
    int admitted = 0;
    const Admission extra = fillAndAskOnce(manager, client, admitted);
    if (admitted != kExpectedShare || extra == Admission::Admitted) {
        check(false, ("a fresh caller is refused past its share (" + std::to_string(admitted) + " admitted, then " +
                      admissionName(extra) + ")")
                         .c_str());
        return;
    }
    manager.noteDeparted(client);
    int reportedAt = 0;
    for (int i = 1; i <= admitted; ++i) {
        if (manager.noteRequestDone(client)) reportedAt = reportedAt == 0 ? i : -1;
    }
    check(reportedAt == admitted, ("a refused caller that left is reported departed when its last queued request is "
                                   "answered (reported at " +
                                   (reportedAt == 0 ? std::string("none") : std::to_string(reportedAt)) + " of " +
                                   std::to_string(admitted) + ")")
                                      .c_str());
}

// GOAL: S15.282 -- a caller with no bus name has no share: it is never refused by this record, only
//       by the worker's own cap.
// CONTROL: the `if (client.empty()) return Admission::Admitted;` line deleted from
//       WallpaperManager::admitRequest (backend/daemon/wallpaper_manager.cpp) -> "FAIL: a caller with
//       no bus name is never refused a share (16 of 64 admitted)". Observed 2026-10-04. Reverted.
void runUnnamedChecks(WallpaperManager& manager) {
    int admitted = 0;
    for (int i = 0; i < 4 * kExpectedShare; ++i) {
        if (manager.admitRequest("") == Admission::Admitted) ++admitted;
    }
    check(admitted == 4 * kExpectedShare, ("a caller with no bus name is never refused a share (" +
                                           std::to_string(admitted) + " of " + std::to_string(4 * kExpectedShare) +
                                           " admitted)")
                                              .c_str());
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "request_share_test";
    using wallpaperx::testing::phaseSelected;
    try {
        // Nothing here builds a device: admitRequest and its neighbours touch only the manager's
        // state, and the GPU is made lazily by a render this file never asks for.
        WallpaperManager::Config config;
        if (phaseSelected("departureErase")) {
            WallpaperManager manager(config);
            runDepartureEraseChecks(manager);
        }
        if (phaseSelected("refusalUncounted")) {
            WallpaperManager manager(config);
            runRefusalUncountedChecks(manager);
        }
        if (phaseSelected("unnamed")) {
            WallpaperManager manager(config);
            runUnnamedChecks(manager);
        }
        // Last: it sleeps out the quiet period.
        if (phaseSelected("quietPeriod")) {
            WallpaperManager manager(config);
            runQuietPeriodChecks(manager);
        }
    } catch (const std::exception& e) {
        wallpaperx::testing::harnessFailure(std::string("the manager could not be set up: ") + e.what());
    }
    const bool red = wallpaperx::testing::reportCheckFailures();
    const bool harnessRed = wallpaperx::testing::reportHarnessFaults("in this test file, not in the daemon");
    if (red || harnessRed) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
