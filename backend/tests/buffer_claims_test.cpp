// SPDX-License-Identifier: GPL-2.0-or-later

// The plugin's ledger of which daemon images this process still has a live import of, graded
// on its own: no bus, no window, no GPU.
//
// It exists because the release message the plugin sends is only correct if this ledger is. The
// three transitions it has to get right are all invisible from outside the plugin -- a release
// sent one import too early tells the daemon it may draw into memory this process is still
// sampling, a release never sent leaves an image out of the daemon's ring until the process
// exits, and a release naming an id a NEW daemon has since handed to somebody else takes that
// image out from under its reader. None of the three shows up as a wrong pixel.
//
// Driven through BufferClaims::instance(), which is what the plugin uses, because a local
// instance would grade a copy of the type rather than the ledger the plugin actually keys its
// releases on. Every phase below therefore starts by calling daemonReplaced(), which is the
// only way to clear it.

#include "kde/buffer_claims.h"
#include "tests/test_report.h"

#include <cstdio>

namespace {

using wallpaperx::BufferClaims;
using wallpaperx::testing::check;

// The ledger, emptied. Not a reset(): the production type has none, and adding one would be
// test-only surface in a shipped header. daemonReplaced() drops every entry, which is exactly
// the state each phase below wants to start from -- and using it here is not circular, because
// the phase that GRADES it compares claims taken on either side of a call to it.
BufferClaims& freshLedger() {
    BufferClaims& claims = BufferClaims::instance();
    claims.daemonReplaced();
    return claims;
}

void runCountingChecks() {
    BufferClaims& claims = freshLedger();

    // GOAL (S12.15, at the unit): a claim is a COUNT of live imports, not a flag. The plugin
    //       re-imports the image it is already showing on every scene change, so one image is
    //       genuinely imported twice at once; the daemon may only be told when the last of those
    //       imports is gone.
    // WHY-THIS-SHAPE: both answers are asserted, not just the second. "The last give-back
    //       returns true" alone is satisfied by a flag, which is the implementation this goal
    //       exists to exclude -- and a flag releases on the FIRST give-back, while the older
    //       texture is still sampling the image.
    // CONTROL: `++entry->imports;` in BufferClaims::take (backend/kde/buffer_claims.cpp)
    //       replaced by a bare `;`, so a repeat import records nothing -> "FAIL: the first of
    //       two live imports of one image does not release it" and "FAIL: the last one does".
    //       Reverted.
    const BufferClaims::Claim first = claims.take(0, 7);
    const BufferClaims::Claim second = claims.take(0, 7);
    check(first.held() && second.held(), "both imports of one image record a claim");
    check(!claims.giveBack(first), "the first of two live imports of one image does not release it");
    check(claims.giveBack(second), "the last one does");

    // GOAL (S12.18's precondition): a claim already given back is never given back again. False
    //       for a ledger that leaves the entry behind, which sends the daemon a second release
    //       naming an image it may already have handed to somebody else.
    // CONTROL: `m_entries.erase(entry);` in BufferClaims::giveBack (backend/kde/
    //       buffer_claims.cpp) deleted -> "FAIL: giving a claim back twice releases nothing the
    //       second time", alone. Reverted.
    check(!claims.giveBack(second), "giving a claim back twice releases nothing the second time");
}

void runKeyingChecks() {
    BufferClaims& claims = freshLedger();

    // GOAL: a claim is keyed on the monitor as well as the buffer id. The daemon numbers images
    //       per monitor, so id 3 on monitor 0 and id 3 on monitor 1 are different images and
    //       releasing one must not release the other.
    // WHY-THIS-SHAPE: the two takes are given the SAME buffer id deliberately. With different
    //       ids the check holds for a ledger that ignores the monitor entirely, which is the
    //       implementation it exists to exclude.
    // CONTROL: the `entry.monitorId == monitorId &&` term in findEntry (backend/kde/
    //       buffer_claims.cpp) deleted -> "FAIL: the same buffer id on another monitor is its
    //       own claim". The second line stays green, and that is the shape of the defect rather
    //       than a gap: with the monitor ignored, the two takes collapse into one entry with
    //       two imports, so the first give-back is refused (red) and the second succeeds --
    //       which is exactly one release too few, sent for the wrong image. Reverted.
    const BufferClaims::Claim onZero = claims.take(0, 3);
    const BufferClaims::Claim onOne = claims.take(1, 3);
    check(claims.giveBack(onZero), "the same buffer id on another monitor is its own claim");
    check(claims.giveBack(onOne), "and releasing one leaves the other still held");

    // GOAL: a reply carrying no buffer id -- which no daemon issues -- yields a claim that
    //       reads as unheld and releases nothing, so a caller parsing a reply need not branch
    //       before asking. False if it hands back a claim: the plugin would then send the
    //       daemon a release for an image it was never given.
    // WHY-THIS-SHAPE: it grades what the caller can SEE, and the goal is worded to match. The
    //       header also says a zero id records no entry, and that half is not gradeable from
    //       out here at all: an entry keyed on buffer id 0 can never be looked up again,
    //       because every lookup goes through a claim whose held() is already false. Asserting
    //       it would mean a size accessor on a shipped header for a test's benefit.
    // CONTROL: the `if (bufferId == 0) return {};` guard at the top of BufferClaims::take
    //       (backend/kde/buffer_claims.cpp) changed to `return Claim{m_daemon, monitorId, 1};`
    //       -> "FAIL: a reply with no buffer id yields no claim". Deleting the guard outright
    //       is NOT a control here: the entry it then records is invisible for the reason above,
    //       and the run stayed green -- measured, which is why the goal above is stated as what
    //       the caller sees rather than as what the ledger stores. Reverted.
    const BufferClaims::Claim nothing = claims.take(0, 0);
    check(!nothing.held() && !claims.giveBack(nothing), "a reply with no buffer id yields no claim");
}

void runDaemonLifetimeChecks() {
    BufferClaims& claims = freshLedger();

    // GOAL (S12.17): a claim granted by one daemon is never released against its successor.
    //       Buffer ids restart at 1 when the daemon does, so a release crossing a restart would
    //       name an id the NEW daemon has already handed out -- to this process or another --
    //       and take that image out from under whoever is reading it.
    // WHY-THIS-SHAPE: the successor is made to hand out the SAME monitor and buffer id, and the
    //       old claim is given back only AFTER that. With different ids the check passes for a
    //       ledger that has no notion of daemon lifetime at all, because the stale claim would
    //       find no entry to match and return false for that reason instead -- which is how an
    //       earlier attempt at this control came back green against a guard that was doing
    //       nothing. The successor's own claim is then given back and required to still work,
    //       so a ledger that "fixed" this by refusing everything after a restart fails too.
    // CONTROL: `if (claim.daemon != m_daemon) return false;` in BufferClaims::giveBack
    //       (backend/kde/buffer_claims.cpp) deleted -> "FAIL: a claim from the previous daemon
    //       releases nothing against its successor" and "FAIL: and the successor's own claim on
    //       that image is still there to release" -- the second because the stale give-back has
    //       by then consumed the successor's entry, which is the damage stated in the goal.
    //       Reverted.
    const BufferClaims::Claim fromOldDaemon = claims.take(2, 5);
    claims.daemonReplaced();
    const BufferClaims::Claim fromNewDaemon = claims.take(2, 5);
    check(fromNewDaemon.held(), "the successor's hand-over of the same id records a claim of its own");
    check(!claims.giveBack(fromOldDaemon), "a claim from the previous daemon releases nothing against its successor");
    check(claims.giveBack(fromNewDaemon), "and the successor's own claim on that image is still there to release");
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "buffer_claims_test";

    runCountingChecks();
    runKeyingChecks();
    runDaemonLifetimeChecks();

    // Left empty for anything else in this process, though nothing else runs here. The ledger
    // is a never-destroyed singleton by design, so a phase that walked away from its entries
    // would leave them for the next one.
    BufferClaims::instance().daemonReplaced();

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
