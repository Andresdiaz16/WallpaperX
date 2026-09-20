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

// ClaimSet, the per-reader half added for Stage E4: which claims ONE reader of the daemon's
// images is holding right now, so each is given back exactly once by whichever comes first -- the
// import ending, or the reader stopping showing the image at all.
//
// Graded here rather than through the plugin item, and that is a finding rather than a
// convenience. The state the retirement exists for -- a texture dying AFTER its item has been
// handed the same image again -- could not be reached through a real item under
// QQuickRenderControl: by the time the old texture is destroyed the item's own bookkeeping has
// emptied the set again, so the two shapes are indistinguishable from outside the plugin.
// Measured, with the retirement removed: item_offscreen_hidden stayed green, and the claim set
// was empty at the teardown in both builds. What is left is the decision itself, which is a
// pure, Qt-free property of this class and of the one line in the item that uses it.
void runClaimSetChecks() {
    BufferClaims& ledger = freshLedger();

    // GOAL: a set gives back what it holds, once each, and reports whether it still held it. The
    //       answer is what the two release paths branch on: an import ending and an item leaving
    //       the screen can both reach the same claim, and whichever is second must send nothing.
    //       False for a set that answers true twice -- which is a second ReleaseBuffer for an
    //       image the first one already freed, and on a ring that reuses ids that is an
    //       invitation to draw into memory still being read.
    // CONTROL: `m_claims.erase(held);` deleted from ClaimSet::remove (backend/kde/
    //       buffer_claims.cpp) -> "FAIL: and a second attempt on the same claim reports nothing
    //       to give back". Observed 2026-09-20. Reverted.
    {
        wallpaperx::ClaimSet set;
        const BufferClaims::Claim one = ledger.take(1, 11);
        set.add(one);
        check(set.remove(one), "a claim the set holds is reported as still held when it is removed");
        check(!set.remove(one), "and a second attempt on the same claim reports nothing to give back");
    }

    // GOAL: two live imports of one image are two claims, and the set hands both back. The plugin
    //       re-imports the image it is already showing whenever the scene changes, so this is the
    //       ordinary steady state -- and the ledger above counts imports, so a set that collapsed
    //       duplicates would leave that count one too high for ever and the image would never be
    //       released at all.
    // CONTROL: `m_claims.push_back(claim);` in ClaimSet::add (backend/kde/buffer_claims.cpp)
    //       guarded by "only if not already present" -> "FAIL: two live imports of one image are
    //       two claims, and emptying the set returns both". Observed 2026-09-20. Reverted.
    {
        wallpaperx::ClaimSet set;
        const BufferClaims::Claim first = ledger.take(1, 12);
        const BufferClaims::Claim second = ledger.take(1, 12);
        set.add(first);
        set.add(second);
        check(set.removeAll().size() == 2, "two live imports of one image are two claims, and emptying the set "
                                           "returns both");
        check(set.removeAll().empty(), "and the set is empty afterwards");
        (void)ledger.giveBack(first);
        (void)ledger.giveBack(second);
    }

    // GOAL: the owner is not part of what identifies a claim. The same image can be claimed once
    //       before this process has learned the daemon's unique bus name and again after, and a
    //       set that matched on the owner would refuse to give the first one back -- leaving that
    //       image out of the monitor's ring for the life of the session.
    // CONTROL: `mine.daemon == claim.daemon && mine.monitorId == ...` in ClaimSet::remove
    //       (backend/kde/buffer_claims.cpp) given `&& mine.owner == claim.owner` -> "FAIL: a
    //       claim is matched by the image it names, not by which bus name granted it". Observed
    //       2026-09-20. Reverted.
    {
        wallpaperx::ClaimSet set;
        BufferClaims::Claim unnamed = ledger.take(1, 13);
        BufferClaims::Claim named = unnamed;
        named.owner = ":1.77";
        set.add(unnamed);
        check(set.remove(named), "a claim is matched by the image it names, not by which bus name granted it");
        (void)ledger.giveBack(unnamed);
    }

    // GOAL: a holder that kept a reference to a RETIRED set cannot give back a claim registered
    //       in the set that replaced it. This is the whole reason the item exchanges its set for
    //       a fresh one when it leaves the screen instead of just emptying the one it has: a ring
    //       slot hands the same buffer id out again, so a stale holder's claim is value-identical
    //       to the one the item is showing now, and a set that had merely been emptied would
    //       match it and free an image being displayed.
    // WHY-THIS-SHAPE: the two claims are deliberately made value-identical -- same daemon, same
    //       monitor, same buffer id -- because that is the only case in which the two shapes
    //       differ. With different ids a stale holder finds nothing in either shape, and the
    //       check would be green for a set that was never retired at all.
    // CONTROL: the retirement is one line in the ITEM, not in this class, so the control is the
    //       harness's own: `wallpaperx::ClaimSet replacement;` below replaced by a reference to
    //       `retired`, which is what an item that emptied its set instead of exchanging it would
    //       produce -> "FAIL: a holder of the retired set cannot give back a claim the new set
    //       holds" and "and the new set still holds it". Observed 2026-09-20. Reverted.
    //       NOT COVERED, and stated because it is the part a reviewer will look for: that
    //       WallpaperXItem::releaseHeldImages actually performs the exchange is not graded
    //       anywhere. Removing it leaves item_offscreen_hidden green, for the reason stated at
    //       the top of this function.
    {
        wallpaperx::ClaimSet retired;
        wallpaperx::ClaimSet replacement;
        const BufferClaims::Claim beforeHide = ledger.take(1, 14);
        retired.add(beforeHide);
        check(retired.removeAll().size() == 1, "the set the item leaves behind gives back what it was holding");

        // The same image handed out again, after the hide: a fresh claim with identical contents.
        const BufferClaims::Claim afterShow = ledger.take(1, 14);
        replacement.add(afterShow);
        check(!retired.remove(beforeHide), "a holder of the retired set cannot give back a claim the new set holds");
        check(replacement.remove(afterShow), "and the new set still holds it");
        (void)ledger.giveBack(beforeHide);
        (void)ledger.giveBack(afterShow);
    }
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "buffer_claims_test";

    runCountingChecks();
    runKeyingChecks();
    runDaemonLifetimeChecks();
    runClaimSetChecks();

    // Left empty for anything else in this process, though nothing else runs here. The ledger
    // is a never-destroyed singleton by design, so a phase that walked away from its entries
    // would leave them for the next one.
    BufferClaims::instance().daemonReplaced();

    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
