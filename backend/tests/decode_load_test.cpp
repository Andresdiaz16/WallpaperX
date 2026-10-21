// SPDX-License-Identifier: GPL-2.0-or-later

// S15.905-S15.907: the decode-load controller (backend/daemon/decode_load.h) under the screens'
// thinning, driven with plain values: no decoder, no GPU, no manager. A picture's cost is given in
// intervals of a 60-a-second file, so `cost` 2 is a decode taking two of its pictures' time.

#include "daemon/decode_load.h"
#include "tests/test_report.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>

using wallpaperx::DecodeLoad;
using wallpaperx::PictureSkip;
using wallpaperx::testing::g_failures;

namespace {

constexpr std::chrono::nanoseconds kFrame{16'666'667};  // one picture of a 60-a-second file
constexpr std::array<std::chrono::nanoseconds, 1> k30Hz{2 * kFrame};
constexpr std::array<std::chrono::nanoseconds, 2> k30And60Hz{2 * kFrame, kFrame};

void check(bool cond, const std::string& what) { wallpaperx::testing::check(cond, what.c_str()); }

std::string levelName(const DecodeLoad& load) {
    static constexpr const char* kNames[] = {"Nothing", "Unreferenced", "NonIntra", "NonKey", "unpaced"};
    return kNames[load.level()];
}

std::string spanText(std::optional<double> span) { return span ? std::to_string(*span) : std::string("unknown"); }

// One returned picture: `intervals` of the file since the one before it, decoded at `at` like the one
// before it, and `deepest` the decoder's own.
void add(DecodeLoad& load, double cost, PictureSkip at, bool previousThinned, bool thinned, int intervals = 1,
         PictureSkip deepest = PictureSkip::NonKey) {
    const auto took = std::chrono::nanoseconds(std::llround(cost * static_cast<double>(kFrame.count())));
    load.add(took, kFrame, kFrame * intervals, at, at, deepest, static_cast<uint32_t>(intervals), previousThinned,
             thinned);
}

// A run of 24 pictures as decoded with nothing skipped: one key picture, one other intra picture, six
// referenced and sixteen not, so a level, by type, keeps 24, 8, 2 and 1 of them: spans 1, 3, 12, 24.
// With `noB` every picture but the key one is referenced: spans 1, 1, 24, 24.
void countRuns(DecodeLoad& load, int runs, bool thinned, bool noB = false) {
    for (int i = 0; i < 24 * runs; ++i) {
        const int at = i % 24;
        PictureSkip kept = PictureSkip::Nothing;
        if (at == 0) {
            kept = PictureSkip::NonKey;
        } else if (at == 12 && !noB) {
            kept = PictureSkip::NonIntra;
        } else if (noB || at % 3 == 0) {
            kept = PictureSkip::Unreferenced;
        }
        load.saw(PictureSkip::Nothing, kept, thinned);
    }
}

// Decoding costing `cost` throughout, the pictures counted by type as countRuns() does. Returned
// still from the level before skipping (frames in flight), so no level's own span gets measured and
// every level past the first is as its pictures' types predict.
void climbBy(DecodeLoad& load, double cost, int pictures = 200) {
    countRuns(load, 2, false);
    for (int i = 0; i < pictures; ++i) add(load, cost, PictureSkip::Nothing, false, false);
}

// S15.905
void runSpacingChangeChecks() {
    // GOAL: S15.905 -- a change of the playing screens' spacing set puts the level back to 0 from every level,
    //       Unreferenced, NonIntra, NonKey and unpaced alike; the same set again changes nothing.
    // WHY-THIS-SHAPE: each level is reached by its own cost against the predicted spans 3, 12 and 24, and the
    //       level is asserted before the change, so a climb that never happened cannot pass as a reset. The
    //       same set given again must leave the level alone, so a thinUnder() resetting on every call fails too.
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, backend/daemon/decode_load.cpp: `m_level = 0;` deleted
    //       from DecodeLoad::thinUnder() -> "FAIL: a change of spacings puts the level back to 0 from Unreferenced
    //       (now Unreferenced)" and the same for NonIntra, NonKey and unpaced (each observed).
    struct Case {
        const char* name;
        double cost;
        size_t level;
    };
    constexpr std::array<Case, 4> kCases{{{"Unreferenced", 2, 1}, {"NonIntra", 6, 2}, {"NonKey", 20, 3},
                                          {"unpaced", 100, DecodeLoad::kUnpaced}}};
    int reached = 0;
    for (const Case& c : kCases) {
        DecodeLoad load;
        load.thinUnder(k30Hz);
        climbBy(load, c.cost);
        if (load.level() != c.level) {
            check(false, std::string("the climb to ") + c.name + " reaches it (at " + levelName(load) + ")");
            continue;
        }
        ++reached;
        load.thinUnder(k30Hz);
        check(load.level() == c.level, std::string("the same spacings again leave the level at ") + c.name +
                                           " (now " + levelName(load) + ")");
        load.thinUnder(k30And60Hz);
        check(load.level() == 0, std::string("a change of spacings puts the level back to 0 from ") + c.name +
                                     " (now " + levelName(load) + ")");
    }
    check(reached == 4, "every level, unpaced included, was reached before a change (" + std::to_string(reached) +
                            " of 4)");

    // GOAL: S15.905 -- after a change the first level's thinned span is measured again, only from pictures
    //       thinned under the new set: unknown until they cover kSettled intervals, then exactly their mean.
    // WHY-THIS-SHAPE: the old set's pictures span 2 and the new set's 3, and the new set is given seven
    //       intervals, then nine: a record carried over is either already measured at seven or, at nine, a
    //       mean between 2 and 3 -- neither is "unknown, then exactly 3".
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, backend/daemon/decode_load.cpp: DecodeLoad::thinUnder()
    //       carrying the old first-level record over (`under.first = m_thinning.first;`) -> "FAIL: ... is unknown
    //       before the new set's pictures cover 8 intervals (it is 2.333333)" and "... exactly the new set's 3
    //       (it is 2.428571)".
    DecodeLoad load;
    load.thinUnder(k30Hz);
    for (int i = 0; i < 4; ++i) add(load, 0.1, PictureSkip::Nothing, true, true, 2);
    const std::optional<double> before = load.span(0, true);
    check(before && std::abs(*before - 2.0) < 1e-9, "the first set's thinned span is measured at 2 (it is " +
                                                         spanText(before) + ")");
    load.thinUnder(k30And60Hz);
    check(!load.span(0, true), "a change of spacings leaves the first level's thinned span unknown (it is " +
                                   spanText(load.span(0, true)) + ")");
    for (int i = 0; i < 2; ++i) add(load, 0.1, PictureSkip::Nothing, true, true, 3);
    add(load, 0.1, PictureSkip::Nothing, false, false, 1);  // a picture not thinned measures nothing
    check(!load.span(0, true),
          "the first level's thinned span is unknown before the new set's pictures cover 8 intervals (it is " +
              spanText(load.span(0, true)) + ")");
    add(load, 0.1, PictureSkip::Nothing, true, true, 3);
    const std::optional<double> after = load.span(0, true);
    check(after && std::abs(*after - 3.0) < 1e-9,
          "the first level's thinned span is measured again from the new set's pictures alone, exactly the new "
          "set's 3 (it is " + spanText(after) + ")");
}

// S15.906
void runUnmeasuredFirstLevelChecks() {
    // GOAL: S15.906 -- at level 0 with the thinned first level unmeasured there is no climb however costly
    //       decoding is; the climb comes once its span is measured.
    // WHY-THIS-SHAPE: 64 thinned pictures, each costing 100 of its interval, not paired (each follows one not
    //       thinned), so the span stays unknown; then four pairs measure it and the level must leave 0. That
    //       second half shows the cost was enough to climb, so "no climb" is not just a cheap file.
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, backend/daemon/decode_load.cpp: DecodeLoad::add()'s
    //       every-picture test reading an unknown span as 1 (`here.value_or(1.0)` for `here &&`) -> "FAIL: at
    //       level 0 with the thinned first level unmeasured, no climb (at Unreferenced)".
    {
        DecodeLoad load;
        load.thinUnder(k30Hz);
        countRuns(load, 2, true);
        for (int i = 0; i < 64; ++i) add(load, 100, PictureSkip::Nothing, false, true, 2);
        check(!load.span(0, true), "the thinned first level is still unmeasured (premise)");
        check(load.level() == 0,
              "at level 0 with the thinned first level unmeasured, no climb (at " + levelName(load) + ")");
        for (int i = 0; i < 4; ++i) add(load, 100, PictureSkip::Nothing, true, true, 2);
        check(load.span(0, true).has_value() && load.level() > 0,
              "once the thinned first level is measured the same cost climbs (at " + levelName(load) + ", span " +
                  spanText(load.span(0, true)) + ")");
    }

    // GOAL: S15.906 -- at a level above 0, a thinned picture arriving while the first level's thinned span is
    //       unmeasured moves the level to 0.
    // WHY-THIS-SHAPE: the climb is made by pictures not thinned (a pass's first ones after a change), and is
    //       asserted, so the step down is from a level actually held.
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, backend/daemon/decode_load.cpp: DecodeLoad::add()'s
    //       keepsUpAt trying an unknown level only past the first (`known ? ... : at != 0`) -> "FAIL: a thinned
    //       picture while the first level is unmeasured moves the level to 0 (at Unreferenced)".
    DecodeLoad load;
    load.thinUnder(k30Hz);
    for (int i = 0; i < 16; ++i) add(load, 4, PictureSkip::Nothing, false, false);
    check(load.level() == 1, "unthinned pictures over load climb to Unreferenced (premise; at " + levelName(load) + ")");
    add(load, 4, PictureSkip::Unreferenced, false, true, 2);
    check(load.level() == 0,
          "a thinned picture while the first level is unmeasured moves the level to 0 (at " + levelName(load) + ")");
}

// S15.907
void runClimbChecks() {
    // GOAL: S15.907 -- with nothing counted to predict from, a climb stops at Unreferenced, the level it
    //       cannot predict, rather than passing it.
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, backend/daemon/decode_load.cpp: DecodeLoad::add()'s
    //       spansOver taking an unknown level to leave nothing out (`known && *known > than`) -> "FAIL: with
    //       nothing counted the climb stops at Unreferenced (at unpaced)", and the three climbs below with it.
    {
        DecodeLoad load;
        for (int i = 0; i < 16; ++i) add(load, 20, PictureSkip::Nothing, false, false);
        check(load.level() == 1, "with nothing counted the climb stops at Unreferenced (at " + levelName(load) + ")");
    }

    // GOAL: S15.907 -- with thinned pictures counted and the first level's thinned span unmeasured, a climb
    //       stops at Unreferenced: those counts cannot be weighed, so they predict nothing.
    // WHY-THIS-SHAPE: the thinned counts alone, read with any span, would predict NonIntra keeps up at this
    //       cost (spans 3, 12, 24 at a cost of 8), so a climb past Unreferenced is what weighing them shows.
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, backend/daemon/decode_load.cpp: DecodeLoad::span()
    //       weighing thinned counts by 1 while the first level is unmeasured (`thinnedFirst.value_or(1.0)`, the
    //       early return gone) -> "FAIL: with thinned pictures counted and the first level unmeasured the climb
    //       stops at Unreferenced (at NonIntra)".
    {
        DecodeLoad load;
        load.thinUnder(k30Hz);
        countRuns(load, 5, true);
        for (int i = 0; i < 64; ++i) add(load, 8, PictureSkip::Nothing, false, false);
        check(load.level() == 1, "with thinned pictures counted and the first level unmeasured the climb stops at "
                                 "Unreferenced (at " + levelName(load) + ")");
    }

    // GOAL: S15.907 -- a 60 fps file on a 30 Hz screen, thinned and measured there, then a 60 Hz screen joining,
    //       over load: the first picture after the join climbs to Unreferenced, not to key pictures only.
    // WHY-THIS-SHAPE: what the 30 Hz screen measured (first level 2, thinned counts) would predict NonIntra; the
    //       join must forget it. The pictures after the join are still the old level's (frames in flight), so
    //       none is counted, as in the daemon.
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, backend/daemon/decode_load.cpp: DecodeLoad::thinUnder()
    //       keeping the old thinned record (`under.first`/`under.kept` copied over) -> "FAIL: after a 60 Hz screen
    //       joins, the climb over load stops at Unreferenced (at NonIntra)". Carrying `first` alone reddens only the
    //       re-measurement lines above: kept counts with no first span predict nothing.
    {
        DecodeLoad load;
        load.thinUnder(k30Hz);
        countRuns(load, 5, true);
        for (int i = 0; i < 64; ++i) add(load, 8, PictureSkip::Nothing, true, true, 2);
        check(load.span(0, true).has_value() && load.level() > 0,
              "at 30 Hz the thinned first level is measured and the file climbs (premise; at " + levelName(load) + ")");
        load.thinUnder(k30And60Hz);
        add(load, 8, PictureSkip::Unreferenced, false, false);
        check(load.level() == 1,
              "after a 60 Hz screen joins, the climb over load stops at Unreferenced (at " + levelName(load) + ")");
    }

    // GOAL: S15.907 / E10d item 5 -- once the thinned first level is measured, a deeper level is predicted from
    //       the unthinned and the thinned pictures pooled: (unthinned + thinned x first span) over what each kept.
    // WHY-THIS-SHAPE: exact values against hand-computed counts, so leaving either group out, or not weighing the
    //       thinned by the first span, moves the number.
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, backend/daemon/decode_load.cpp: DecodeLoad::span() adding
    //       thinned counts unweighed (`* *thinnedFirst` dropped) -> "FAIL: NonIntra predicted from both kinds pooled
    //       is 18 (it is 12.000000)", and the Unreferenced (3) and NonKey (24) lines.
    {
        DecodeLoad load;
        load.thinUnder(k30Hz);
        countRuns(load, 1, false);  // 24 pictures: kept 24, 8, 2, 1
        countRuns(load, 1, true);   // 24 thinned pictures, the same
        for (int i = 0; i < 4; ++i) add(load, 0.1, PictureSkip::Nothing, true, true, 2);  // first span 2
        // NonIntra: (24 + 24 x 2) / (2 + 2) = 18; Unreferenced: 72 / 16 = 4.5; NonKey: 72 / 2 = 36.
        const std::optional<double> unref = load.span(1, true), intra = load.span(2, true), key = load.span(3, true);
        check(unref && std::abs(*unref - 4.5) < 1e-9, "Unreferenced predicted from both kinds pooled is 4.5 (it is " +
                                                         spanText(unref) + ")");
        check(intra && std::abs(*intra - 18.0) < 1e-9, "NonIntra predicted from both kinds pooled is 18 (it is " +
                                                           spanText(intra) + ")");
        check(key && std::abs(*key - 36.0) < 1e-9, "NonKey predicted from both kinds pooled is 36 (it is " +
                                                       spanText(key) + ")");
    }

    // GOAL: S15.907 -- once the thinned first level is measured, a file with no B pictures climbs straight past
    //       Unreferenced (which, predicted, leaves nothing out) to NonIntra.
    // WHY-THIS-SHAPE: all its pictures were seen thinned, so the prediction exists only by pooling the thinned
    //       counts; at level 0 the climb comes once the first level's span (2) is measured.
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, backend/daemon/decode_load.cpp: DecodeLoad::span() never
    //       pooling thinned counts (`if (false)` for `if (m_thinning.kept[0] > 0)`) -> "FAIL: a file with no B
    //       pictures, its first level measured, climbs straight past Unreferenced (at Unreferenced)".
    {
        DecodeLoad load;
        load.thinUnder(k30Hz);
        countRuns(load, 5, true, /*noB=*/true);
        for (int i = 0; i < 64; ++i) add(load, 8, PictureSkip::Nothing, true, true, 2);
        check(load.level() == 2, "a file with no B pictures, its first level measured, climbs straight past "
                                 "Unreferenced (at " + levelName(load) + ")");
    }
}

// S15.907 / E13: a deeper level's prediction needs kSettled (8) intervals counted, as a measured span does.
void runTooFewCountedChecks() {
    // GOAL: one key picture counted predicts nothing: every deeper span is unknown, and a climb under load from
    //       level 0 stops at Unreferenced rather than passing every level to unpaced.
    // WHY-THIS-SHAPE: the existing "nothing counted" climb has no count at all, so it cannot tell a threshold of
    //       one picture from one of eight; a single key picture (kept through NonKey) is the case that, read as
    //       a prediction, says every level spans 1 and leaves nothing out.
    // CONTROL: control_batch.sh 2026-10-20 (T9a), reverted, backend/daemon/decode_load.cpp: DecodeLoad::span()'s
    //       `if (all < kSettled)` put back to `if (all == 0)` -> "FAIL: with one key picture counted, Unreferenced
    //       is unknown (it is 1.000000)", the NonIntra and NonKey lines, and "... the climb stops at Unreferenced
    //       (at unpaced)".
    {
        DecodeLoad load;
        load.saw(PictureSkip::Nothing, PictureSkip::NonKey, false);
        static constexpr const char* kNames[] = {"", "Unreferenced", "NonIntra", "NonKey"};
        for (size_t at = 1; at < DecodeLoad::kLevels; ++at) {
            check(!load.span(at, false), std::string("with one key picture counted, ") + kNames[at] + " is unknown (it is " +
                                             spanText(load.span(at, false)) + ")");
        }
        for (int i = 0; i < 16; ++i) add(load, 20, PictureSkip::Nothing, false, false);
        check(load.level() == 1, "with one key picture counted the climb stops at Unreferenced (at " + levelName(load) + ")");
    }

    // GOAL: the bar is exactly kSettled intervals: 7 pictures counted (not thinned, one interval each) leave
    //       Unreferenced unknown, 8 make it known, at what their types predict.
    // WHY-THIS-SHAPE: a pair either side of the bar, so both `== 0` (7 known) and `<= kSettled` (8 unknown) fail;
    //       the 8 are 1 key and 3 referenced pictures and 4 not, so the value is exactly 8 / 4 = 2.
    // CONTROL: control_batch.sh 2026-10-20 (T9a), reverted, backend/daemon/decode_load.cpp: `if (all == 0)` ->
    //       "FAIL: 7 counted intervals leave Unreferenced unknown (it is 1.750000)"; `if (all <= kSettled)` ->
    //       "FAIL: 8 counted intervals make Unreferenced known, at 2 (it is unknown)".
    {
        const auto counted = [](int pictures) {
            DecodeLoad load;
            for (int i = 0; i < pictures; ++i) {
                const PictureSkip kept = i == 0 ? PictureSkip::NonKey : i < 4 ? PictureSkip::Unreferenced : PictureSkip::Nothing;
                load.saw(PictureSkip::Nothing, kept, false);
            }
            return load.span(1, false);
        };
        const std::optional<double> seven = counted(7), eight = counted(8);
        check(!seven, "7 counted intervals leave Unreferenced unknown (it is " + spanText(seven) + ")");
        check(eight && std::abs(*eight - 2.0) < 1e-9,
              "8 counted intervals make Unreferenced known, at 2 (it is " + spanText(eight) + ")");
    }

    // GOAL: thinned pictures count toward the bar at the first level's measured span, not one each: 4 thinned
    //       pictures at a first-level span of 2 are 8 intervals and make the deeper levels known (Unreferenced
    //       exactly 4 x 2 / 1 = 8); 3 are 6 and leave them unknown.
    // WHY-THIS-SHAPE: counted one a picture, 4 would stay under the bar; with no bar at all, 3 would be known.
    //       The first span is measured by four paired pictures two intervals apart, as runSpacingChangeChecks does.
    // CONTROL: control_batch.sh 2026-10-20 (T9a), reverted, backend/daemon/decode_load.cpp: `if (all == 0)` ->
    //       "FAIL: 3 thinned pictures at a first span of 2 leave Unreferenced unknown (it is 6.000000)";
    //       `if (all <= kSettled)` and, separately, the thinned counts added unweighed (`* *thinnedFirst` dropped)
    //       -> "FAIL: 4 thinned pictures at a first span of 2 make Unreferenced known, at 8 (it is unknown)".
    {
        const auto thinned = [](int pictures) {
            DecodeLoad load;
            load.thinUnder(k30Hz);
            for (int i = 0; i < pictures; ++i) {
                load.saw(PictureSkip::Nothing, i == 0 ? PictureSkip::NonKey : PictureSkip::Nothing, true);
            }
            for (int i = 0; i < 4; ++i) add(load, 0.1, PictureSkip::Nothing, true, true, 2);
            const std::optional<double> first = load.span(0, true);
            check(first && std::abs(*first - 2.0) < 1e-9,
                  "the thinned first level is measured at 2 (premise; it is " + spanText(first) + ")");
            return load.span(1, true);
        };
        const std::optional<double> three = thinned(3), four = thinned(4);
        check(!three, "3 thinned pictures at a first span of 2 leave Unreferenced unknown (it is " + spanText(three) + ")");
        check(four && std::abs(*four - 8.0) < 1e-9,
              "4 thinned pictures at a first span of 2 make Unreferenced known, at 8 (it is " + spanText(four) + ")");
    }
}

// E10d item 6, as far as DecodeLoad decides it: a picture thinned under earlier spacings is told apart.
void runEarlierSpacingsChecks() {
    // GOAL: a picture thinned under earlier spacings is told apart from one thinned under these and from one not
    //       thinned (thinnedHere empty), so the caller counts it nowhere and pairs nothing with it.
    // WHY-THIS-SHAPE: what the daemon does with the empty answer (backend/daemon/wallpaper_manager.cpp, measureDecode:
    //       no saw(), no add(), value_or(false) for the next picture's pair) is not reachable from this binary;
    //       this grades the answer it acts on, for the number before and the one two changes back.
    // CONTROL: control_batch.sh 2026-10-20 (F5), reverted, backend/daemon/decode_load.cpp: DecodeLoad::thinnedHere()
    //       answering true for another number -> "FAIL: a picture thinned under the spacings before is told apart
    //       (answered thinned here)".
    DecodeLoad load;
    load.thinUnder(k30Hz);
    const uint8_t first = load.thinningNumber();
    load.thinUnder(k30And60Hz);
    const uint8_t second = load.thinningNumber();
    load.thinUnder(k30Hz);
    const uint8_t third = load.thinningNumber();
    check(first != second && second != third, "each change of spacings moves the number on (premise)");
    const std::optional<bool> old = load.thinnedHere(second);
    const std::optional<bool> older = load.thinnedHere(first);
    check(!old.has_value(), std::string("a picture thinned under the spacings before is told apart (answered ") +
                                (old ? (*old ? "thinned here" : "not thinned") : "empty") + ")");
    check(!older.has_value(), "a picture thinned two changes back is told apart");
    check(load.thinnedHere(third) == std::optional<bool>(true), "a picture thinned under these spacings is thinned here");
    check(load.thinnedHere(std::nullopt) == std::optional<bool>(false), "a picture not thinned is not thinned here");
}

}  // namespace

int main() {
    wallpaperx::testing::g_testName = "decode_load_test";
    try {
        runSpacingChangeChecks();
        runUnmeasuredFirstLevelChecks();
        runClimbChecks();
        runTooFewCountedChecks();
        runEarlierSpacingsChecks();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", e.what());
        ++g_failures;
    }
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    std::printf("decode_load_test: all checks passed\n");
    return 0;
}
