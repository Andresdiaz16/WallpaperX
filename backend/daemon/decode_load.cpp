// SPDX-License-Identifier: GPL-2.0-or-later

#include "decode_load.h"

#include <cmath>

namespace wallpaperx {

void DecodeLoad::Average::add(double sample, double cap, uint32_t weight) {
    // The mean of the first kSettled, then an eighth of each one after; the cap keeps one decode, however
    // slow, from making a file that keeps up look slow for long.
    const double counted = std::min(sample, cap);
    for (; weight > 0 && samples < kSettled; --weight) value += (counted - value) / ++samples;
    value = counted + (value - counted) * std::pow(1.0 - 1.0 / kSettled, weight);
}

void DecodeLoad::Span::add(double intervals, uint32_t read) {
    perPicture.add(intervals, 1e9);
    const double counted = std::clamp(std::min(intervals, static_cast<double>(read)), 1.0, double{kSettled});
    covered = std::min(kSettled, covered + static_cast<uint32_t>(std::lround(counted)));
}

void DecodeLoad::thinUnder(std::span<const std::chrono::nanoseconds> spacings) {
    // None thins nothing, and what the last ones measured still holds when they come back.
    if (spacings.empty()) return;
    Thinning under;
    under.count = std::min(spacings.size(), under.spacings.size());
    std::copy_n(spacings.begin(), under.count, under.spacings.begin());
    std::sort(under.spacings.begin(), under.spacings.begin() + under.count);
    under.count = static_cast<size_t>(std::unique(under.spacings.begin(), under.spacings.begin() + under.count) -
                                      under.spacings.begin());
    if (under.count != m_thinning.count ||
        !std::equal(under.spacings.begin(), under.spacings.begin() + under.count, m_thinning.spacings.begin())) {
        under.number = static_cast<uint8_t>(m_thinning.number + 1);
        m_thinning = under;
        m_level = 0;
        m_asRun = {};
    }
}

std::optional<bool> DecodeLoad::thinnedHere(std::optional<uint8_t> under) const {
    if (!under) return false;
    if (*under != m_thinning.number) return std::nullopt;
    return true;
}

std::optional<double> DecodeLoad::span(size_t at, bool thinned) const {
    at = std::min(at, kLevels - 1);
    // Measured once its pictures cover kSettled intervals: one long gap at a sparse-key file's key level
    // does, a lone short one -- a P picture just before a key one -- does not.
    const auto measured = [](const Span& span) {
        return span.covered >= kSettled ? std::optional(span.perPicture.value) : std::nullopt;
    };
    const std::optional<double> thinnedFirst = measured(m_thinning.first);
    if (at == 0) return thinned ? thinnedFirst : std::optional(1.0);
    if (const std::optional<double> value = measured(m_spans[at - 1])) return value;
    // As the pictures' types predict, from what was decoded before any skipping: too few key pictures
    // seen there, or a B pyramid, can put it either side of what the level spans. A picture returned
    // thinned stands for the first level's span of the file's, and leaves out none a deeper level keeps.
    double all = static_cast<double>(m_kept[0]);
    double still = static_cast<double>(m_kept[at]);
    if (m_thinning.kept[0] > 0) {
        if (!thinnedFirst) return std::nullopt;
        all += static_cast<double>(m_thinning.kept[0]) * *thinnedFirst;
        still += static_cast<double>(m_thinning.kept[at]);
    }
    // Under kSettled intervals counted, as for a measured span: one key picture alone predicts every
    // level leaves nothing out, and a climb would pass them all.
    if (all < kSettled) return std::nullopt;
    return all / std::max(still, 1.0);
}

void DecodeLoad::saw(PictureSkip decodedAt, PictureSkip keptThrough, bool thinned) {
    if (decodedAt != PictureSkip::Nothing) return;
    std::array<uint64_t, kLevels>& counts = thinned ? m_thinning.kept : m_kept;
    for (size_t at = 0; at <= static_cast<size_t>(keptThrough); ++at) ++counts[at];
}

void DecodeLoad::add(std::chrono::nanoseconds took, std::chrono::nanoseconds shownFor,
                     std::chrono::nanoseconds covered, PictureSkip previousAt, PictureSkip decodedAt,
                     PictureSkip deepest, uint32_t read, bool previousThinned, bool thinned) {
    const auto span = [&](size_t level) { return this->span(level, thinned); };
    const auto against = [&](std::chrono::nanoseconds span) {
        return static_cast<double>(took.count()) / static_cast<double>(span.count());
    };
    const double intervals = static_cast<double>(covered.count()) / static_cast<double>(shownFor.count());
    // The pictures this one stands for: each one skipped was read, so a gap a timestamp jump stretched
    // counts no more than the packets behind it.
    const auto weight =
        static_cast<uint32_t>(std::lround(std::clamp(std::min(intervals, static_cast<double>(read)), 1.0, 1e6)));
    // Capped at twice the average, or what a picture of this level spans if more: a file far over load
    // must be seen to be, or a climb stops short at a level predicted to keep up that does not, and a
    // level that skips would predict a lower one keeps up that was already seen not to.
    const auto deepestAt = static_cast<size_t>(deepest);
    m_everyPicture.add(against(shownFor),
                       2.0 * std::max({m_everyPicture.value, span(std::min(m_level, deepestAt)).value_or(1.0), 1.0}),
                       weight);
    if (decodedAt == std::min(skip(), deepest) && previousAt == decodedAt) {
        // One a picture, not weighted: whether this level keeps pace is judged on kSettled of its own
        // pictures, not on the late ones it inherited from the last.
        m_asRun.add(against(covered));
        if (decodedAt != PictureSkip::Nothing) {
            m_spans[static_cast<size_t>(decodedAt) - 1].add(intervals, read);
        } else if (previousThinned && thinned) {
            m_thinning.first.add(intervals, read);
        }
    }
    const auto moveTo = [this](size_t to) {
        m_level = to;
        m_asRun = {};
    };
    // A level unknown is tried: taken to keep up, and to leave out more than any other.
    const auto keepsUpAt = [&](size_t at) {
        const std::optional<double> known = span(at);
        return !known || m_everyPicture.value / *known < kKeepsUp;
    };
    const auto spansOver = [&](size_t at, double than) {
        const std::optional<double> known = span(at);
        return !known || *known > than;
    };
    // A level seen or predicted to leave nothing out cannot help, nor one the decoder does not apply.
    for (size_t lower = 0; lower < std::min(m_level, deepestAt + 1); ++lower) {
        if ((lower == 0 || spansOver(lower, kSkipped)) && keepsUpAt(lower)) {
            moveTo(lower);
            return;
        }
    }
    // Behind as this level's own pictures measure it, or as the cost of every picture predicts for it:
    // that needs none of this level's pictures, which a decoder's frames in flight hold back for a while.
    const std::optional<double> here = span(m_level);
    const bool behind = (m_level > 0 && m_asRun.samples >= kSettled && m_asRun.value >= 1.0) ||
                        (m_everyPicture.samples >= kSettled && here && m_everyPicture.value >= *here);
    if (!behind || unpaced()) return;
    size_t to = m_level + 1;
    while (to <= deepestAt && !(spansOver(to, kSkipped * here.value_or(1.0)) && keepsUpAt(to))) ++to;
    moveTo(to > deepestAt ? kUnpaced : to);
}

}
