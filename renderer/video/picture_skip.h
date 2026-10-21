// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace wallpaperx {

// Which pictures a VideoDecoder call may leave undecoded, each level leaving out more: those no other
// picture references; every picture but intra ones; every picture but those that reset the decoder
// (H.264 IDR or a recovery point, HEVC IRAP). A picture returned decodes as it would in full, and its
// timestamp still places it. No level drops B pictures as such: in a B pyramid P pictures can reference
// them. VideoDecoder::deepestSkip() says how deep each codec, in software or on the device, is asked
// to go.
// Apart from video_decoder.h so a caller can choose one without ffmpeg's headers.
enum class PictureSkip { Nothing, Unreferenced, NonIntra, NonKey };

// The pictures a display taking one picture every `spacing` of a stream's own time shows: each whose
// own interval, centred on its timestamp, holds a multiple of `spacing` after `origin`. Centred, so a
// timestamp rounded to its stream's time base stays on its side of a refresh; and a picture lasting
// `spacing` or longer always holds one, so a spacing no longer than the stream's pictures shows them all.
struct ShowGrid {
    std::chrono::nanoseconds origin{};
    std::chrono::nanoseconds spacing{};

    bool shows(std::chrono::nanoseconds at, std::chrono::nanoseconds interval) const {
        if (interval >= spacing || interval <= std::chrono::nanoseconds::zero()) return true;
        const int64_t from = (at - origin - interval / 2).count();
        const int64_t step = spacing.count();
        int64_t past = from % step;
        if (past < 0) past += step;
        return (past == 0 ? 0 : step - past) < interval.count();
    }
};

// The pictures any of several displays sharing one stream shows, each a ShowGrid laid from one origin.
// Grids of spacings that are not whole multiples of each other do not nest, so the fastest display's
// grid alone would leave out pictures a slower one shows.
class ShowGrids {
public:
    static constexpr size_t kMaxSpacings = 32;

    // Throws std::invalid_argument for no spacing or one not positive, and std::length_error for more
    // than kMaxSpacings different ones.
    ShowGrids(std::chrono::nanoseconds origin, std::span<const std::chrono::nanoseconds> spacings) : m_origin(origin) {
        if (spacings.empty()) throw std::invalid_argument("ShowGrids: no spacing");
        for (const std::chrono::nanoseconds spacing : spacings) {
            if (spacing <= std::chrono::nanoseconds::zero()) throw std::invalid_argument("ShowGrids: spacing not positive");
            if (std::find(m_spacings.begin(), m_spacings.begin() + m_count, spacing) != m_spacings.begin() + m_count) continue;
            if (m_count == kMaxSpacings) throw std::length_error("ShowGrids: too many spacings");
            m_spacings[m_count++] = spacing;
        }
    }

    bool shows(std::chrono::nanoseconds at, std::chrono::nanoseconds interval) const {
        return std::any_of(m_spacings.begin(), m_spacings.begin() + m_count,
                           [&](std::chrono::nanoseconds spacing) { return ShowGrid{m_origin, spacing}.shows(at, interval); });
    }

    // Whether any picture lasting `interval` can be left out: it is shorter than every spacing.
    bool mayLeaveOut(std::chrono::nanoseconds interval) const {
        return interval > std::chrono::nanoseconds::zero() &&
               std::all_of(m_spacings.begin(), m_spacings.begin() + m_count,
                           [&](std::chrono::nanoseconds spacing) { return interval < spacing; });
    }

private:
    std::chrono::nanoseconds m_origin;
    std::array<std::chrono::nanoseconds, kMaxSpacings> m_spacings{};
    size_t m_count = 0;
};

// ShowGrids and the number a caller gave them, which each picture judged under them reports: pictures
// judged under the last ones are still on their way when they change.
struct NumberedGrids {
    ShowGrids grids;
    uint8_t number = 0;
};

}
