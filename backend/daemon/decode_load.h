// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video/picture_skip.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace wallpaperx {

// How long pictures take to decode against the time they are shown for, averaged over several so
// that one slow decode -- a key picture, a wait on the GPU queue, a stalled read -- is not taken
// for a file that cannot keep up. A sample counts once per picture interval it covers, so the
// average spans the same file time at every level: at the key level one picture can settle it.
// While the decoder, as it runs, still cannot keep pace over kSettled, or this level is predicted
// not to, what it leaves out climbs straight to the lowest higher level predicted to run under
// kKeepsUp; it comes down to the lowest level predicted to. The prediction is decoding every
// picture's cost over span(): the file time each picture that level returns spans. Entered at 1
// and left under kKeepsUp, so a file decoding at about one level's cost does not flip between two
// every few pictures. Past the last level is a file that even its key pictures cannot keep up
// with: they are shown as they are decoded, and its sound, never handed on, waits silent at the file's end
// for each pass's first picture to start it again (WallpaperManager::steerSoundLocked). A decoder that applies
// fewer levels goes from its deepest straight past the last, still skipping only what it applies.
//
// Holds no clock and no decoder: every input is a value from PictureTiming and the pipeline. One
// thread at a time.
class DecodeLoad {
public:
    static constexpr size_t kLevels = static_cast<size_t>(PictureSkip::NonKey) + 1;
    static constexpr size_t kUnpaced = kLevels;
    static constexpr double kKeepsUp = 0.9;
    // A level whose pictures span no more than this was not seen to leave anything out.
    static constexpr double kSkipped = 1.05;
    // One averaging window: the samples an average holds before it moves the level.
    static constexpr uint32_t kSettled = 8;

    // Each round, before its pictures are measured. A change of spacings also puts the level
    // back to the first, where the first level's span under them can be measured. Past
    // ShowGrids::kMaxSpacings different ones, the rest are not told apart.
    void thinUnder(std::span<const std::chrono::nanoseconds> spacings);
    // Given to the decoder with the spacings (VideoPipeline::leaveUnshown), so a picture thinned
    // under earlier ones is told apart. Moves on with each change; wraps.
    uint8_t thinningNumber() const { return m_thinning.number; }
    // Whether a picture reporting PictureTiming::thinnedUnder was thinned under the spacings now
    // measured; empty for one thinned under others, which measures nothing.
    std::optional<bool> thinnedHere(std::optional<uint8_t> under) const;
    // As seen at `level` once its pictures cover kSettled intervals, else as its pictures' types
    // predict. The first level is 1 unless `thinned`: the screens' rates leave out pictures,
    // which the first level then spans too, as measured. Empty when unknown: the first level
    // thinned and not yet measured, or a deeper one with under kSettled intervals counted to predict
    // it from, or thinned pictures counted that the first level's span is needed to weigh.
    std::optional<double> span(size_t level, bool thinned) const;
    // A PictureSkip up to the decoder's deepest, or kUnpaced.
    size_t level() const { return m_level; }
    // What the decoder is asked for; it applies no more than its deepest.
    PictureSkip skip() const { return static_cast<PictureSkip>(std::min(m_level, kLevels - 1)); }
    bool unpaced() const { return m_level == kUnpaced; }
    // `decodedAt` is the level the picture was decoded at, `previousAt` the picture's before it. Only
    // a picture decoded at the current level, after one that was too, measures it: a decoder's frames
    // in flight still return the last level's for a while, and `covered` reaches back to that one.
    // `deepest` is the decoder's own, as VideoPipeline::deepestSkip(); `read` as PictureTiming::readSince,
    // and the thinned pair as thinnedHere(). A level whose span is unknown is tried, not predicted:
    // taken to keep up, so a climb stops at it and a step down goes to it, and never seen behind
    // until measured. `shownFor` and `covered` must be positive.
    void add(std::chrono::nanoseconds took, std::chrono::nanoseconds shownFor, std::chrono::nanoseconds covered,
             PictureSkip previousAt, PictureSkip decodedAt, PictureSkip deepest, uint32_t read,
             bool previousThinned, bool thinned);
    // Every picture returned, sampled or not: a pass's first is often its only key picture.
    void saw(PictureSkip decodedAt, PictureSkip keptThrough, bool thinned);

private:
    struct Average {
        double value = 0;
        uint32_t samples = 0;  // up to kSettled
        // Counted as `weight` samples in a row, none past `cap`.
        void add(double sample, double cap = 2.0, uint32_t weight = 1);
    };
    // The pictures' intervals each returned picture at one level spans, kept across visits.
    struct Span {
        // One sample a picture, not weighted by its gap: a level's cost is per picture decoded,
        // so a long gap must not outweigh the short ones beside it.
        Average perPicture;
        // Intervals of the file those samples cover, at least one a picture, up to kSettled.
        uint32_t covered = 0;
        // `read` is the packets read for the picture: a gap a timestamp jump stretched covers no
        // more than those.
        void add(double intervals, uint32_t read);
    };
    // What the first level was seen to do while the screens' rates thinned it, under the playing
    // screens' spacings it was measured at, sorted: started again when those change.
    struct Thinning {
        std::array<std::chrono::nanoseconds, ShowGrids::kMaxSpacings> spacings{};
        size_t count = 0;
        uint8_t number = 0;
        Span first;
        // As `m_kept`, of the pictures returned thinned.
        std::array<uint64_t, kLevels> kept{};
    };

    // Against how long each picture is shown: decoding every picture, a skipped one taken to cost
    // what a decoded one does.
    Average m_everyPicture;
    // Against the time shown since the picture before: the decoder as it runs at the current
    // level, restarted at each change of it.
    Average m_asRun;
    // Levels past the first, at level - 1. They leave out every picture the screens' rates would,
    // so they span the same thinned or not.
    std::array<Span, kLevels - 1> m_spans;
    // Of the pictures decoded with nothing skipped and not thinned, how many each level would still
    // decode, by their type: what a level not yet visited predicts. Index 0 counts them all.
    std::array<uint64_t, kLevels> m_kept{};
    Thinning m_thinning;
    size_t m_level = 0;
};

}
