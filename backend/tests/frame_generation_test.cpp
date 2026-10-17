// SPDX-License-Identifier: GPL-2.0-or-later

// S12.5: a stale frame is never shown, and a frame already in hand is never fetched twice.
//
// Both decisions are pure functions on WallpaperXItem precisely so they can be graded here.
//
// Their ORDER is graded by the type system rather than by anything below: the two arguments mean
// opposite things, and with both as quint64 a call site could pass them the wrong way round and
// every check here would still pass. Arriving and Held make that a compile error -- verified,
// `cannot convert Held to Arriving`. A test cannot reach the call sites; the types can.
// Left inline in their call sites they were reachable only by staging two fetches that complete
// out of order against a live daemon, a window and a GPU -- which nothing in this suite can
// force, so the ordering rule would have shipped on reasoning alone.

#include "kde/wallpaperx_item.h"
#include "tests/test_report.h"

#include <cstdio>

namespace {

using wallpaperx::WallpaperXItem;
using wallpaperx::testing::check;

// GOAL: S12.5 -- a FrameReady for a generation already fetched costs nothing. False if it
//       refetches: that is a bus round trip, a fresh export by the daemon and a dup here,
//       per signal, and the daemon emits one per render.
// WHY-THIS-SHAPE: equality is tested as well as strict ordering, because the repeat case IS
//       equality -- the daemon re-announcing the frame already held. A `>` written as `>=`
//       passes an ordering-only check and refetches every repeat.
// CONTROL: `generation > newestFetched` changed to `>=` in WallpaperXItem::worthFetching ->
//       "FAIL: the generation already held is not worth fetching again". Observed 2026-08-21.
//       Reverted.
void checkWorthFetching() {
    check(WallpaperXItem::worthFetching({5}, {3}), "a newer generation is worth fetching");
    check(!WallpaperXItem::worthFetching({3}, {5}), "an older generation is not worth fetching");
    check(!WallpaperXItem::worthFetching({5}, {5}), "the generation already held is not worth fetching again");

    // Zero is "not known", which the callers that are not signals pass. It must fetch, or a
    // screen that has just learned its monitor id never asks for anything and stays blank.
    check(WallpaperXItem::worthFetching({0}, {9}), "an unknown generation is fetched even with a frame in hand");
}

// GOAL: S12.5 -- the newest frame wins regardless of the order imports complete in. False if
//       arrival decides: two fetches for one monitor can finish out of order, and the older one
//       landing second would put a stale wallpaper on screen and leave it there.
// WHY-THIS-SHAPE: equality must SUPERSEDE here, the opposite of the fetch rule above, and the
//       asymmetry is the point. The same frame can legitimately arrive twice -- a re-fetch
//       after a window change -- and refusing it there would leave the screen blank rather
//       than redraw what it already showed.
// CONTROL: `generation >= shown` changed to `>` in WallpaperXItem::supersedesShown ->
//       "FAIL: the same frame arriving again is still drawn". Observed 2026-08-21. Reverted.
void checkSupersedesShown() {
    check(WallpaperXItem::supersedesShown({7}, {4}), "a newer frame replaces what is shown");
    check(!WallpaperXItem::supersedesShown({4}, {7}), "a frame older than what is shown is dropped");
    check(WallpaperXItem::supersedesShown({7}, {7}), "the same frame arriving again is still drawn");
    check(WallpaperXItem::supersedesShown({0}, {9}), "a frame of unknown generation is drawn rather than discarded");
}

// GOAL: a reply with too few fields is refused rather than read past the end. False if it reads
//       anyway: the parse indexes fields 0 to 9, and indexing a shorter list is a crash inside
//       plasmashell -- the compositor, not this plugin.
// WHY-THIS-SHAPE: the boundary is tested from both sides and MORE fields must be accepted, not
//       just the exact count. A newer daemon adding a field is a reply this plugin can still
//       read, and an equality test would reject every one of them -- turning a compatible
//       upgrade into a blank desktop.
// COVERAGE: this grades the decision, not the parse it guards. Reaching the parse itself needs a
//       bus peer answering malformed replies, which this suite has no way to stand up.
// CONTROL: `fieldCount >= kFrameReplyFields` changed to `>` in WallpaperXItem::replyIsReadable
//       -> "FAIL: a reply with exactly the declared fields is read". Observed 2026-08-21; re-run
//       2026-10-04 via control_run.sh with the count at GetFrame's thirteen, and 2026-10-05 via
//       control_batch.sh at fourteen. Reverted.
void checkReplyBounds() {
    check(WallpaperXItem::replyIsReadable(WallpaperXItem::kFrameReplyFields),
          "a reply with exactly the declared fields is read");
    check(WallpaperXItem::replyIsReadable(WallpaperXItem::kFrameReplyFields + 3),
          "a reply from a newer daemon carrying extra fields is still read");
    check(!WallpaperXItem::replyIsReadable(WallpaperXItem::kFrameReplyFields - 1),
          "a reply one field short is refused rather than read past the end");
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "frame_generation_test";
    checkWorthFetching();
    checkSupersedesShown();
    checkReplyBounds();
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
