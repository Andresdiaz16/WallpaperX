// SPDX-License-Identifier: GPL-2.0-or-later

#include "decode_ahead.h"

#include "../vulkan/device.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <system_error>
#include <utility>

namespace wallpaperx {

namespace {

// Pictures decoded ahead and not yet taken. With the one VideoPipeline holds converted until it is
// due, three are ready, which covers the longest single decode call measured: an AV1 packet of
// five pictures, about 21 ms at the card's low-power clocks against 8.3 ms a picture at 120.
constexpr size_t kDepth = 2;

// Far above any one decode call; a decoder still inside one is waiting on a GPU that has stopped.
constexpr std::chrono::seconds kStopBound{10};

}  // namespace

struct DecodeAhead::Shared {
    // First, so it is destroyed last, after the pictures it decoded.
    std::unique_ptr<PictureSource> source;
    Around around;
    Ready ready;

    std::mutex mutex;
    // Signalled for every change below, whichever side waits.
    std::condition_variable changed;
    // At most kDepth pictures and then one item that ends decoding; reserved, so a push never
    // allocates.
    std::vector<Item> items;
    PictureSkip skip = PictureSkip::Nothing;
    bool stopping = false;
    bool exited = false;
    // A take() found nothing, so the next item calls the ready hook.
    bool awaited = false;
    enum class Question { None, Asked, Healthy, Unhealthy };
    Question question = Question::None;

    size_t pictures() const {
        return static_cast<size_t>(std::count_if(items.begin(), items.end(), [](const Item& item) {
            return std::holds_alternative<PictureSource::Picture>(item);
        }));
    }
};

DecodeAhead::DecodeAhead(Device& device) : m_device(device), m_shared(std::make_unique<Shared>()) {
    m_shared->items.reserve(kDepth + 1);
}

std::unique_ptr<DecodeAhead> DecodeAhead::start(Device& device, std::unique_ptr<PictureSource>& source, Around around,
                                                Ready ready) {
    std::unique_ptr<DecodeAhead> ahead(new DecodeAhead(device));
    Shared& shared = *ahead->m_shared;
    shared.around = std::move(around);
    shared.ready = std::move(ready);
    shared.source = std::move(source);
    // Everything that can throw from here on hands the source back below.
    try {
        shared.source->askDeviceThrough([&shared] { return ask(shared); });
        ahead->m_thread = std::thread([&shared] { run(shared); });
    } catch (...) {
        shared.source->askDeviceThrough({});
        source = std::move(shared.source);
        try {
            throw;
        } catch (const std::system_error& e) {
            std::fprintf(stderr, "DecodeAhead: no thread to decode on, so the video decodes on the GPU thread: %s\n",
                         e.what());
            return nullptr;
        }
    }
    return ahead;
}

DecodeAhead::~DecodeAhead() {
    if (!m_thread.joinable()) return;
    bool exited = false;
    {
        std::unique_lock<std::mutex> lock(m_shared->mutex);
        m_shared->stopping = true;
        m_shared->changed.notify_all();
        exited = m_shared->changed.wait_for(lock, kStopBound, [this] { return m_shared->exited; });
    }
    if (exited) {
        m_thread.join();
        return;
    }
    m_thread.detach();
    static_cast<void>(m_shared.release());
    m_device.recordThreadLeftRunning(
        "a video decoder still inside ffmpeg after its thread was told to stop, with its pictures,");
}

void DecodeAhead::notifyReady(Shared& shared) noexcept {
    if (!shared.ready) return;
    try {
        shared.ready();
    } catch (...) {
        // Only a lock failing gets here; the next round finds the item anyway.
    }
}

void DecodeAhead::run(Shared& shared) noexcept {
    // One step's input and output in one place, so the step function holds a single pointer and
    // never allocates.
    struct Call {
        Shared* shared;
        PictureSkip skip = PictureSkip::Nothing;
        PictureSource::Step step = PictureSource::Dropping{};
    } call{&shared};
    const auto decode = [&call] { call.step = call.shared->source->next(1, call.skip); };

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(shared.mutex);
            shared.changed.wait(lock, [&] { return shared.stopping || shared.pictures() < kDepth; });
            if (shared.stopping) break;
            call.skip = shared.skip;
        }
        // One picture dropped at a time, so a stop is seen between drops.
        std::optional<Item> item;
        try {
            if (shared.around) {
                shared.around(decode);
            } else {
                decode();
            }
            if (auto* picture = std::get_if<PictureSource::Picture>(&call.step)) {
                item.emplace(std::move(*picture));
            } else if (std::holds_alternative<PictureSource::End>(call.step)) {
                item.emplace(PictureSource::End{});
            }
        } catch (...) {
            item.emplace(std::current_exception());
        }
        call.step = PictureSource::Dropping{};
        if (!item) continue;

        const bool last = !std::holds_alternative<PictureSource::Picture>(*item);
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.items.push_back(std::move(*item));
            wake = std::exchange(shared.awaited, false) && !shared.stopping;
        }
        shared.changed.notify_all();
        if (wake) notifyReady(shared);
        if (last) break;
    }
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.exited = true;
    }
    shared.changed.notify_all();
}

bool DecodeAhead::ask(Shared& shared) {
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (shared.stopping) return false;
        shared.question = Shared::Question::Asked;
    }
    shared.changed.notify_all();
    notifyReady(shared);
    std::unique_lock<std::mutex> lock(shared.mutex);
    shared.changed.wait(lock, [&] { return shared.stopping || shared.question != Shared::Question::Asked; });
    const bool healthy = shared.question == Shared::Question::Healthy;
    shared.question = Shared::Question::None;
    return healthy;
}

void DecodeAhead::answer(std::unique_lock<std::mutex>& lock) {
    if (m_shared->question != Shared::Question::Asked) return;
    lock.unlock();
    const bool healthy = VideoDecoder::deviceAnswers(m_device);
    lock.lock();
    m_shared->question = healthy ? Shared::Question::Healthy : Shared::Question::Unhealthy;
    m_shared->changed.notify_all();
}

// Caller holds the lock and has seen an item.
DecodeAhead::Item DecodeAhead::pop() {
    Item& head = m_shared->items.front();
    if (auto* picture = std::get_if<PictureSource::Picture>(&head)) {
        Item taken(std::move(*picture));
        m_shared->items.erase(m_shared->items.begin());
        m_shared->changed.notify_all();
        return taken;
    }
    if (std::holds_alternative<PictureSource::End>(head)) return PictureSource::End{};
    return std::get<std::exception_ptr>(head);
}

std::optional<DecodeAhead::Item> DecodeAhead::take(PictureSkip skip) {
    std::unique_lock<std::mutex> lock(m_shared->mutex);
    m_shared->skip = skip;
    answer(lock);
    if (m_shared->items.empty()) {
        m_shared->awaited = true;
        return std::nullopt;
    }
    return pop();
}

DecodeAhead::Item DecodeAhead::wait(std::optional<PictureSkip> skip) {
    std::unique_lock<std::mutex> lock(m_shared->mutex);
    if (skip) m_shared->skip = *skip;
    for (;;) {
        answer(lock);
        if (!m_shared->items.empty()) return pop();
        m_shared->changed.wait(lock, [this] {
            return !m_shared->items.empty() || m_shared->question == Shared::Question::Asked;
        });
    }
}

}
