// SPDX-License-Identifier: GPL-2.0-or-later

#include "picture_source.h"

#include <utility>

namespace wallpaperx {

PictureSource::PictureSource(Device& device, const std::string& file, Playback playback,
                             std::optional<StreamPosition> startAt)
    : m_decoder(device, file, startAt), m_playback(playback) {}

std::variant<std::optional<DecodedFrame>, VideoDecoder::Dropping> PictureSource::decode(
    std::optional<uint32_t> dropLimit, PictureSkip skip) {
    if (dropLimit) return m_decoder.next(*dropLimit, skip);
    return m_decoder.next(skip);
}

void PictureSource::askDeviceThrough(std::function<bool()> ask) {
    if (!ask) {
        m_decoder.askDeviceThrough({});
        return;
    }
    m_decoder.askDeviceThrough([this, ask = std::move(ask)] {
        const auto asked = std::chrono::steady_clock::now();
        const bool healthy = ask();
        m_asking += std::chrono::steady_clock::now() - asked;
        return healthy;
    });
}

PictureSource::Step PictureSource::next(std::optional<uint32_t> dropLimit, PictureSkip skip) {
    const auto started = std::chrono::steady_clock::now();
    m_asking = {};
    const auto spend = [&] { m_spent += std::chrono::steady_clock::now() - started - m_asking; };
    auto step = decode(dropLimit, skip);
    if (std::holds_alternative<VideoDecoder::Dropping>(step)) {
        spend();
        return Dropping{};
    }
    std::optional<DecodedFrame> frame = std::move(std::get<std::optional<DecodedFrame>>(step));
    bool startsPass = !m_returnedAny && !m_decoder.startedMidStream();
    std::optional<std::chrono::nanoseconds> passEnded;
    // Nothing returned yet means a stream with nothing to show, which must not spin here.
    if (!frame && m_playback == Playback::Loop && m_returnedAny) {
        passEnded = m_decoder.readThrough();
        m_decoder.restart();
        // Unbounded: a restarted pass has returned nothing to resume after, so it drops nothing.
        frame = m_decoder.next(skip);
        startsPass = true;
    }
    spend();
    if (!frame) return End{};

    m_returnedAny = true;
    PictureTiming timing{frame->presentationTime(), frame->duration(), startsPass, frame->skip(),
                         passEnded, frame->keptThrough(), frame->readSince(), std::exchange(m_spent, {})};
    return Picture{std::move(*frame), timing, m_decoder.deepestSkip()};
}

}
