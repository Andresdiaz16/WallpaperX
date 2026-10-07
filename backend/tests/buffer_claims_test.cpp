// SPDX-License-Identifier: GPL-2.0-or-later

// The plugin's ledger of which daemon images this process still has a live import of, and of which
// daemon it is speaking to, graded on its own: no bus, no window, no GPU.
//
// It exists because the release message the plugin sends is only correct if this ledger is. The
// transitions it has to get right are all invisible from outside the plugin -- a release sent one
// import too early tells the daemon it may draw into memory this process is still sampling, a
// release never sent leaves an image out of the daemon's ring until the process exits, a release
// naming an id a NEW daemon has since handed out takes that image out from under its reader, and a
// release naming an older hand-over than the newest one read lets the daemon draw into an image
// this process is about to show. None of them shows up as a wrong pixel.
//
// Driven through BufferClaims::instance(), which is what the plugin uses, because a local instance
// would grade a copy of the type rather than the ledger the plugin keys its releases on. The ledger
// starts at lifetime 0 once per process and has no reset, so the startup phase runs first, and the
// departure-first run -- the other way out of lifetime 0 -- is its own ctest entry.

#include "kde/buffer_claims.h"
#include "tests/test_report.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace {

using wallpaperx::BufferClaims;
using wallpaperx::testing::check;

// A new lifetime owned by a name of its own, and its number. Not a reset(): the production type has
// none, and adding one would be test-only surface in a shipped header. Every report is a new
// lifetime, so this drops every entry, which is the state each phase below starts from.
uint64_t freshLifetime(const std::string& owner) {
    BufferClaims& claims = BufferClaims::instance();
    claims.daemonReplaced(owner);
    return claims.currentDaemon().lifetime;
}

std::string describe(const std::optional<BufferClaims::Release>& release) {
    if (!release) return "no release";
    return "release to '" + release->owner + "' as of hand-over " + std::to_string(release->handover);
}

bool releases(const std::optional<BufferClaims::Release>& release, const std::string& owner, uint64_t handover) {
    return release && release->owner == owner && release->handover == handover;
}

// The claim a reply grants, for the checks about claims; the release a take supersedes is graded by
// runSupersededChecks() and ignored here, as the ledger itself sends nothing.
BufferClaims::Claim claimOf(uint64_t daemon, int monitorId, uint64_t bufferId, uint64_t handover) {
    return BufferClaims::instance().take(daemon, monitorId, bufferId, handover).claim;
}

// Lifetime 0, the stretch before the bus has said who owns the daemon's name, and the answer that
// ends it. Must run first in its process.
void runStartupChecks() {
    BufferClaims& claims = BufferClaims::instance();
    const BufferClaims::Daemon start = claims.currentDaemon();
    if (start.lifetime != 0 || !start.owner.empty()) {
        wallpaperx::testing::harnessFailure("the ledger was not at lifetime 0 when the startup phase began");
        return;
    }

    // GOAL: S15.311's ledger half -- nothing is recorded while no owner is known, so no release can
    //       ever be addressed to an empty name (which the plugin would send to the well-known name,
    //       where whichever daemon owns it could free an image it handed to somebody else).
    // CONTROL: `&& !m_daemon.owner.empty()` deleted from BufferClaims::take
    //       (backend/kde/buffer_claims.cpp) -> "FAIL: a claim taken before the owner is known records
    //       nothing (release to '' as of hand-over 1)".
    const BufferClaims::Claim early = claimOf(0, 0, 9, 1);
    const std::optional<BufferClaims::Release> earlyRelease = claims.giveBack(early);
    check(early.held() && !earlyRelease,
          ("a claim taken before the owner is known records nothing (" + describe(earlyRelease) + ")").c_str());

    // GOAL: the bus's answer to who owns the name begins the first known lifetime, and a claim taken
    //       in it is released to that unique name -- the startup case, where the first picture is
    //       asked for once the answer is in.
    // CONTROL: `m_daemon.owner = std::move(owner);` deleted from BufferClaims::daemonNamed
    //       (backend/kde/buffer_claims.cpp) -> "FAIL: the answer begins lifetime 1 owned by the
    //       named daemon ..." and "FAIL: and a claim taken in it is released to that name (no
    //       release)".
    const bool named = claims.daemonNamed(":1.1");
    const BufferClaims::Daemon first = claims.currentDaemon();
    check(named && first.lifetime == 1 && first.owner == ":1.1",
          ("the answer begins lifetime 1 owned by the named daemon (lifetime " + std::to_string(first.lifetime) +
           ", owner '" + first.owner + "')")
              .c_str());
    const BufferClaims::Claim claim = claimOf(first.lifetime, 0, 9, 4);
    const std::optional<BufferClaims::Release> release = claims.giveBack(claim);
    check(releases(release, ":1.1", 4),
          ("and a claim taken in it is released to that name (" + describe(release) + ")").c_str());

    // GOAL: only the first answer counts: a second one, or one arriving after a change was
    //       reported, changes nothing and says so.
    // CONTROL: `if (m_daemon.lifetime != 0) return false;` deleted from BufferClaims::daemonNamed
    //       (backend/kde/buffer_claims.cpp) -> "FAIL: a second answer changes nothing (true,
    //       owner ':1.2')".
    const bool again = claims.daemonNamed(":1.2");
    const BufferClaims::Daemon after = claims.currentDaemon();
    check(!again && after.lifetime == 1 && after.owner == ":1.1",
          (std::string("a second answer changes nothing (") + (again ? "true" : "false") + ", owner '" + after.owner +
           "')")
              .c_str());
}

// The departure-first run: the startup daemon leaves before the bus has answered who owned the name.
// Its own process, because lifetime 0 is left only once per process.
void runDepartureFirstChecks() {
    BufferClaims& claims = BufferClaims::instance();
    if (claims.currentDaemon().lifetime != 0) {
        wallpaperx::testing::harnessFailure("the ledger was not at lifetime 0 when the departure-first run began");
        return;
    }

    // GOAL: S15.311 / edge-case issue 1 -- a departure reported while the startup lifetime is still
    //       unnamed is a change: it begins a new lifetime owned by nobody, in which nothing is
    //       recorded; and the late answer to who owned the name cannot bring the departed daemon
    //       back. False for the old rule that compared owners, which read "" == "" as no change.
    // CONTROL: in backend/kde/buffer_claims.cpp, (a) `if (owner == m_daemon.owner) return;` added at
    //       the top of BufferClaims::daemonReplaced (the old duplicate rule) -> "FAIL: a departure
    //       before the answer begins a new lifetime owned by nobody (lifetime 0, owner '')"; (b) the
    //       lifetime test in daemonNamed made `if (false)` -> "FAIL: and the late answer brings
    //       nothing back (true, lifetime 1, owner ':1.9')"; (c) `&& !m_daemon.owner.empty()` deleted
    //       from BufferClaims::take -> "FAIL: nothing is recorded while nobody owns the name".
    claims.daemonReplaced("");
    const BufferClaims::Daemon gone = claims.currentDaemon();
    check(gone.lifetime == 1 && gone.owner.empty(),
          ("a departure before the answer begins a new lifetime owned by nobody (lifetime " +
           std::to_string(gone.lifetime) + ", owner '" + gone.owner + "')")
              .c_str());
    const BufferClaims::Claim orphan = claimOf(gone.lifetime, 0, 3, 1);
    check(!claims.giveBack(orphan), "nothing is recorded while nobody owns the name");
    const bool late = claims.daemonNamed(":1.9");
    const BufferClaims::Daemon afterLate = claims.currentDaemon();
    check(!late && afterLate.lifetime == 1 && afterLate.owner.empty(),
          (std::string("and the late answer brings nothing back (") + (late ? "true" : "false") + ", lifetime " +
           std::to_string(afterLate.lifetime) + ", owner '" + afterLate.owner + "')")
              .c_str());

    // The arrival after it is an ordinary lifetime.
    claims.daemonReplaced(":1.10");
    const BufferClaims::Daemon arrived = claims.currentDaemon();
    const std::optional<BufferClaims::Release> release = claims.giveBack(claimOf(arrived.lifetime, 0, 3, 2));
    check(arrived.lifetime == 2 && releases(release, ":1.10", 2),
          ("the daemon that arrives next is a lifetime of its own and is released to (" + describe(release) + ")")
              .c_str());
}

void runCountingChecks() {
    BufferClaims& claims = BufferClaims::instance();
    const uint64_t lifetime = freshLifetime(":test.counting");

    // GOAL (S12.15, at the unit): a claim is a COUNT of live imports, not a flag. The plugin
    //       re-imports the image it is already showing on every scene change, so one image is
    //       genuinely imported twice at once; the daemon may only be told when the last of those
    //       imports is gone.
    // WHY-THIS-SHAPE: both answers are asserted, not just the second. "The last give-back
    //       releases" alone is satisfied by a flag, which releases on the FIRST give-back, while
    //       the older texture is still sampling the image.
    // CONTROL: `++entry->imports;` in BufferClaims::take (backend/kde/buffer_claims.cpp) replaced
    //       by a bare `;` -> "FAIL: the first of two live imports of one image does not release it".
    const BufferClaims::Claim first = claimOf(lifetime, 0, 7, 1);
    const BufferClaims::Claim second = claimOf(lifetime, 0, 7, 2);
    check(first.held() && second.held(), "both imports of one image record a claim");
    check(!claims.giveBack(second), "the first of two live imports of one image does not release it");

    // GOAL: S15.308 at the unit -- the release names the NEWEST hand-over taken of the image, even
    //       when the import given back last is the older one. The daemon ends the claim only when
    //       the release covers its newest hand-over; naming the older one leaves the image held for
    //       the session.
    // WHY-THIS-SHAPE: the newer import is given back first and the older last, so the release is
    //       produced by the claim that carries the OLD number. A ledger that released "this
    //       import's number" passes when the newest is given back last, which is the common order.
    // CONTROL: `entry->newest = std::max(entry->newest, handover);` deleted from BufferClaims::take
    //       (backend/kde/buffer_claims.cpp) -> "FAIL: the last one releases, as of the newest hand-over
    //       taken (release to ':test.counting' as of hand-over 1)".
    const std::optional<BufferClaims::Release> last = claims.giveBack(first);
    check(releases(last, ":test.counting", 2),
          ("the last one releases, as of the newest hand-over taken (" + describe(last) + ")").c_str());

    // GOAL (S12.18's precondition): a claim already given back is never given back again.
    // CONTROL: `m_entries.erase(entry);` in BufferClaims::giveBack (backend/kde/buffer_claims.cpp)
    //       deleted -> "FAIL: giving a claim back twice releases nothing the second time".
    check(!claims.giveBack(first), "giving a claim back twice releases nothing the second time");

    // GOAL: a reply numbered 0 -- no daemon issues one, but the plugin still takes it, so its
    //       import is counted -- does not lower the number a release names.
    // CONTROL: `entry->newest = std::max(entry->newest, handover);` made `entry->newest = handover;`
    //       (backend/kde/buffer_claims.cpp) -> "FAIL: a reply numbered 0 does not lower the number a
    //       release names (release to ':test.counting' as of hand-over 0)".
    const BufferClaims::Claim numbered = claimOf(lifetime, 0, 8, 5);
    const BufferClaims::Claim unnumbered = claimOf(lifetime, 0, 8, 0);
    (void)claims.giveBack(unnumbered);
    const std::optional<BufferClaims::Release> eight = claims.giveBack(numbered);
    check(releases(eight, ":test.counting", 5),
          ("a reply numbered 0 does not lower the number a release names (" + describe(eight) + ")").c_str());
}

// The release a take supersedes. The daemon charges every reply's descriptors until a release names
// it, and the ledger otherwise names a hand-over only when the last import of its image ends -- which
// for an image re-fetched while it stays on screen is never.
void runSupersededChecks() {
    BufferClaims& claims = BufferClaims::instance();
    const uint64_t lifetime = freshLifetime(":test.superseded");

    // GOAL: S15.345 at the unit -- the first take of an image supersedes nothing; each later take of
    //       the same image with a higher number supersedes the newest number taken before it, to the
    //       daemon that granted it -- never the number just taken, which would end the claim the new
    //       reply holds.
    // WHY-THIS-SHAPE: three takes, so "the previous newest" (2) is told apart from "the first" (1)
    //       and from "the one just taken" (3).
    // CONTROL: in BufferClaims::take (backend/kde/buffer_claims.cpp), (a) the `taken.superseded = ...`
    //       assignment deleted -> "FAIL: each later take supersedes the newest number taken before it
    //       (no release; no release)"; (b) the release made to name `handover` instead of `entry->newest`
    //       -> the same line, "(... hand-over 2; ... hand-over 3)".
    const BufferClaims::Taken one = claims.take(lifetime, 0, 21, 1);
    const BufferClaims::Taken two = claims.take(lifetime, 0, 21, 2);
    const BufferClaims::Taken three = claims.take(lifetime, 0, 21, 3);
    check(!one.superseded, ("the first take of an image supersedes nothing (" + describe(one.superseded) + ")").c_str());
    check(releases(two.superseded, ":test.superseded", 1) && releases(three.superseded, ":test.superseded", 2),
          ("each later take supersedes the newest number taken before it (" + describe(two.superseded) + "; " +
           describe(three.superseded) + ")")
              .c_str());

    // GOAL: S15.345 -- what a take supersedes leaves the image held: the last import going still
    //       releases it, naming the newest number.
    // CONTROL: the counting checks' `++entry->imports;` control reddens this too: "FAIL: and the image
    //       stays held until its last import goes, released as of the newest (no release)".
    (void)claims.giveBack(three.claim);
    (void)claims.giveBack(two.claim);
    const std::optional<BufferClaims::Release> lastOfThree = claims.giveBack(one.claim);
    check(releases(lastOfThree, ":test.superseded", 3),
          ("and the image stays held until its last import goes, released as of the newest (" +
           describe(lastOfThree) + ")")
              .c_str());

    // GOAL: S15.345 -- a number 0 on either side supersedes nothing: 0 names no hand-over a release
    //       could, and a release naming 0 is refused by the daemon.
    // CONTROL: in BufferClaims::take (backend/kde/buffer_claims.cpp), (a) `entry->newest != 0 &&`
    //       deleted -> "FAIL: an entry whose newest is 0 supersedes nothing (release to ':test.superseded'
    //       as of hand-over 0)"; (b) `&& entry->newest < handover` deleted -> "FAIL: a reply numbered 0
    //       supersedes nothing (... hand-over 4)" and "FAIL: a reply numbered below the newest supersedes
    //       nothing".
    const BufferClaims::Taken zeroFirst = claims.take(lifetime, 0, 22, 0);
    const BufferClaims::Taken afterZero = claims.take(lifetime, 0, 22, 4);
    check(!zeroFirst.superseded && !afterZero.superseded,
          ("an entry whose newest is 0 supersedes nothing (" + describe(afterZero.superseded) + ")").c_str());
    const BufferClaims::Taken zeroAfter = claims.take(lifetime, 0, 22, 0);
    check(!zeroAfter.superseded,
          ("a reply numbered 0 supersedes nothing (" + describe(zeroAfter.superseded) + ")").c_str());
    const BufferClaims::Taken lower = claims.take(lifetime, 0, 22, 2);
    check(!lower.superseded,
          ("a reply numbered below the newest supersedes nothing (" + describe(lower.superseded) + ")").c_str());
    (void)claims.giveBack(zeroFirst.claim);
    (void)claims.giveBack(afterZero.claim);
    (void)claims.giveBack(zeroAfter.claim);
    (void)claims.giveBack(lower.claim);

    // GOAL: S15.345 -- a take from a daemon lifetime that has ended supersedes nothing, even for an
    //       image the current daemon's entry holds: its number belongs to another daemon's sequence.
    // CONTROL: `daemon == m_daemon.lifetime &&` deleted from BufferClaims::take
    //       (backend/kde/buffer_claims.cpp) -> "FAIL: a take from an ended lifetime supersedes nothing
    //       (release to ':test.superseded' as of hand-over 5)".
    const BufferClaims::Taken current = claims.take(lifetime, 0, 23, 5);
    const BufferClaims::Taken stale = claims.take(lifetime - 1, 0, 23, 9);
    check(!stale.superseded,
          ("a take from an ended lifetime supersedes nothing (" + describe(stale.superseded) + ")").c_str());
    (void)claims.giveBack(current.claim);

    // GOAL: S15.345 -- once the last import has given the image back, the entry is gone, and the next
    //       take of that image is a first take: it supersedes nothing that was already released.
    // CONTROL: `m_entries.erase(entry);` deleted from BufferClaims::giveBack
    //       (backend/kde/buffer_claims.cpp) -> "FAIL: a take after the image was given back supersedes
    //       nothing (release to ':test.superseded' as of hand-over 6)".
    const BufferClaims::Taken before = claims.take(lifetime, 0, 24, 6);
    (void)claims.giveBack(before.claim);
    const BufferClaims::Taken afresh = claims.take(lifetime, 0, 24, 7);
    check(!afresh.superseded,
          ("a take after the image was given back supersedes nothing (" + describe(afresh.superseded) + ")").c_str());
    (void)claims.giveBack(afresh.claim);
}

void runKeyingChecks() {
    BufferClaims& claims = BufferClaims::instance();
    const uint64_t lifetime = freshLifetime(":test.keying");

    // GOAL: a claim is keyed on the monitor as well as the buffer id, so id 3 on monitor 0 and id 3
    //       on monitor 1 are different images and releasing one must not release the other.
    // WHY-THIS-SHAPE: the same buffer id deliberately; with different ids the check holds for a
    //       ledger that ignores the monitor entirely.
    // CONTROL: the `entry.monitorId == monitorId &&` term in findEntry (backend/kde/buffer_claims.cpp)
    //       deleted -> "FAIL: the same buffer id on another monitor is its own claim".
    const BufferClaims::Claim onZero = claimOf(lifetime, 0, 3, 1);
    const BufferClaims::Claim onOne = claimOf(lifetime, 1, 3, 2);
    check(claims.giveBack(onZero).has_value(), "the same buffer id on another monitor is its own claim");
    check(claims.giveBack(onOne).has_value(), "and releasing one leaves the other still held");

    // GOAL: a reply carrying no buffer id yields a claim that reads as unheld and releases nothing.
    // WHY-THIS-SHAPE: graded on what the caller can see; an entry keyed on id 0 could never be looked
    //       up again, so asserting it is stored would need test-only surface.
    // CONTROL: the `if (bufferId == 0) return {};` guard in BufferClaims::take
    //       (backend/kde/buffer_claims.cpp) changed to `return Taken{Claim{daemon, monitorId, 1},
    //       std::nullopt};` -> "FAIL: a reply with no buffer id yields no claim".
    const BufferClaims::Claim nothing = claimOf(lifetime, 0, 0, 3);
    check(!nothing.held() && !claims.giveBack(nothing), "a reply with no buffer id yields no claim");
}

void runDaemonLifetimeChecks() {
    BufferClaims& claims = BufferClaims::instance();
    const uint64_t oldLifetime = freshLifetime(":test.old");

    // GOAL (S12.17): a claim granted by one daemon is never released against its successor, whose
    //       ids restart at 1.
    // WHY-THIS-SHAPE: the successor hands out the SAME monitor and id, and the old claim is given
    //       back only after that; the successor's own claim must then still release, so a ledger
    //       that refused everything after a restart fails too.
    // CONTROL: `if (claim.daemon != m_daemon.lifetime) return std::nullopt;` deleted from
    //       BufferClaims::giveBack (backend/kde/buffer_claims.cpp) -> "FAIL: a claim from the
    //       previous daemon releases nothing against its successor (release to ':test.new' as of
    //       hand-over 1)".
    const BufferClaims::Claim fromOld = claimOf(oldLifetime, 2, 5, 10);
    const uint64_t newLifetime = freshLifetime(":test.new");
    const BufferClaims::Claim fromNew = claimOf(newLifetime, 2, 5, 1);
    const std::optional<BufferClaims::Release> stale = claims.giveBack(fromOld);
    check(!stale, ("a claim from the previous daemon releases nothing against its successor (" + describe(stale) + ")")
                      .c_str());
    const std::optional<BufferClaims::Release> current = claims.giveBack(fromNew);
    check(releases(current, ":test.new", 1),
          ("and the successor's own claim on that image is released to it (" + describe(current) + ")").c_str());

    // GOAL: S13.221's ledger half -- a reply asked of a daemon since replaced is taken under ITS
    //       lifetime: it records nothing, and so cannot share an entry with the successor's claim
    //       on the same (monitor, id) and hold back the successor's release.
    // WHY-THIS-SHAPE: the successor's claim is taken FIRST and the late one after, on the same image;
    //       the successor's single give-back must then release. Recorded under the current
    //       lifetime, the late take would add an import and that give-back would release nothing.
    // CONTROL: the `daemon == m_daemon.lifetime &&` term deleted from BufferClaims::take
    //       (backend/kde/buffer_claims.cpp) -> "FAIL: a late reply from a replaced daemon does not hold
    //       back the successor's release (no release)".
    const uint64_t successor = freshLifetime(":test.successor");
    const BufferClaims::Claim mine = claimOf(successor, 3, 4, 7);
    const BufferClaims::Claim late = claimOf(successor - 1, 3, 4, 3);
    const std::optional<BufferClaims::Release> mineReleased = claims.giveBack(mine);
    check(late.held() && late.daemon == successor - 1 && releases(mineReleased, ":test.successor", 7),
          ("a late reply from a replaced daemon does not hold back the successor's release (" +
           describe(mineReleased) + ")")
              .c_str());
    check(!claims.giveBack(late), "and the late claim itself releases nothing");

    // GOAL: every reported change is a new lifetime, the same owner reported twice included: the
    //       watch reports each change once, so a repeat is a daemon that left and came back.
    // CONTROL: `if (owner == m_daemon.owner) return;` added at the top of
    //       BufferClaims::daemonReplaced (backend/kde/buffer_claims.cpp) -> "FAIL: the same owner
    //       reported again is a new lifetime ...".
    const uint64_t once = freshLifetime(":test.same");
    const BufferClaims::Claim beforeRepeat = claimOf(once, 0, 6, 1);
    const uint64_t twice = freshLifetime(":test.same");
    check(twice == once + 1 && !claims.giveBack(beforeRepeat),
          ("the same owner reported again is a new lifetime, and drops the claims of the one before (lifetime " +
           std::to_string(once) + " then " + std::to_string(twice) + ")")
              .c_str());

    // GOAL: a departure (owner empty) is a lifetime with nobody to release to: nothing taken in it
    //       is recorded.
    // CONTROL: `&& !m_daemon.owner.empty()` deleted from BufferClaims::take
    //       (backend/kde/buffer_claims.cpp) -> "FAIL: after a departure nothing is recorded (release to ''
    //       as of hand-over 2)".
    const uint64_t departed = freshLifetime("");
    const std::optional<BufferClaims::Release> orphan = claims.giveBack(claimOf(departed, 0, 6, 2));
    check(!orphan, ("after a departure nothing is recorded (" + describe(orphan) + ")").c_str());
}

// ClaimSet: which claims ONE reader of the daemon's images is holding right now, so each is given
// back exactly once by whichever comes first -- the import ending, or the reader stopping showing
// the image at all.
void runClaimSetChecks() {
    const uint64_t lifetime = freshLifetime(":test.sets");
    BufferClaims& ledger = BufferClaims::instance();

    // GOAL: S13.168 -- a set gives back what it holds, once each, and reports whether it still held it.
    // CONTROL: `m_claims.erase(held);` deleted from ClaimSet::remove (backend/kde/buffer_claims.cpp)
    //       -> "FAIL: and a second attempt on the same claim reports nothing to give back".
    {
        wallpaperx::ClaimSet set;
        const BufferClaims::Claim one = claimOf(lifetime, 1, 11, 1);
        set.add(one);
        check(set.remove(one), "a claim the set holds is reported as still held when it is removed");
        check(!set.remove(one), "and a second attempt on the same claim reports nothing to give back");
        (void)ledger.giveBack(one);
    }

    // GOAL: S13.168 -- two live imports of one image are two claims, and the set hands both back.
    // CONTROL: `m_claims.push_back(claim);` in ClaimSet::add (backend/kde/buffer_claims.cpp) guarded
    //       by "only if not already present" -> "FAIL: two live imports of one image are two claims,
    //       and emptying the set returns both".
    {
        wallpaperx::ClaimSet set;
        const BufferClaims::Claim first = claimOf(lifetime, 1, 12, 2);
        const BufferClaims::Claim second = claimOf(lifetime, 1, 12, 3);
        set.add(first);
        set.add(second);
        check(set.removeAll().size() == 2, "two live imports of one image are two claims, and emptying the set "
                                           "returns both");
        check(set.removeAll().empty(), "and the set is empty afterwards");
        (void)ledger.giveBack(first);
        (void)ledger.giveBack(second);
    }

    // GOAL: S13.168's retirement -- a holder of a retired set cannot give back a claim the set that
    //       replaced it holds, even one equal in value (same daemon, monitor, image and hand-over).
    // WHY-THIS-SHAPE: equal values, because a claim names no owner: only the set it was added to says
    //       whose it is. The retired set is emptied first, as WallpaperXItem::releaseHeldImages empties it.
    //       That the item retires its set at all (the std::exchange there) is NOT graded: through a real
    //       item the exchange removed produces the same traffic (item_offscreen_test records the
    //       measurement), since the item's own bookkeeping has emptied the set by the time a stale reader
    //       could reach it.
    // CONTROL: in backend/kde/buffer_claims.h, ClaimSet's `m_mutex` and `m_claims` made `static inline`,
    //       so every set shares one store -> "FAIL: a retired set does not report as held a claim equal
    //       to one its replacement holds".
    {
        wallpaperx::ClaimSet retired;
        wallpaperx::ClaimSet replacement;
        const BufferClaims::Claim beforeHide = claimOf(lifetime, 1, 14, 4);
        retired.add(beforeHide);
        (void)retired.removeAll();
        const BufferClaims::Claim afterShow = claimOf(lifetime, 1, 14, 4);
        replacement.add(afterShow);
        check(!retired.remove(afterShow),
              "a retired set does not report as held a claim equal to one its replacement holds");
        check(replacement.remove(afterShow), "and the replacement still gives it back");
        (void)ledger.giveBack(beforeHide);
        (void)ledger.giveBack(afterShow);
    }
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "buffer_claims_test";

    if (argc > 1 && std::strcmp(argv[1], "departure-first") == 0) {
        runDepartureFirstChecks();
    } else if (argc > 1) {
        wallpaperx::testing::harnessFailure(std::string("unknown run '") + argv[1] + "'");
    } else {
        runStartupChecks();
        runCountingChecks();
        runSupersededChecks();
        runKeyingChecks();
        runDaemonLifetimeChecks();
        runClaimSetChecks();
    }

    const bool checksRed = wallpaperx::testing::reportCheckFailures();
    const bool harnessRed = wallpaperx::testing::reportHarnessFaults("in this test file, not in the ledger");
    if (checksRed || harnessRed) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
