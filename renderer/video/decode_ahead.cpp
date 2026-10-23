// SPDX-License-Identifier: GPL-2.0-or-later

#include "decode_ahead.h"

#include "../vulkan/device.h"

#include <algorithm>
#include <atomic>
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
    // Set as the stop begins, for the source's start to read once a packet without the lock; before the
    // source, which holds its address.
    std::atomic<bool> stopStart{false};
    // Destroyed after everything but the flag, so after the pictures it decoded.
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
    std::optional<NumberedGrids> unshown;
    // Stopping still answers questions, so a decode call that needs one ends as it would have; once
    // Abandoned, none is answered.
    enum class Phase { Running, Stopping, Abandoned };
    Phase phase = Phase::Running;
    bool exited = false;
    // A take() found nothing, so the next item calls the ready hook.
    bool awaited = false;
    enum class Question { None, Asked, Healthy, Unhealthy };
    Question question = Question::None;
    // The decoder's probe: none posted, posted and ProbeUnsent until the GPU thread submits it, then its
    // outcome until the decoder collects it. Never waited for, so a stop has nothing of it to answer.
    std::optional<VideoDecoder::Probe> probe;

    bool probeUnsent() const { return probe && std::holds_alternative<VideoDecoder::ProbeUnsent>(*probe); }

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
    shared.unshown = shared.source->decoder().unshown();
    // Everything that can throw from here on hands the source back below.
    try {
        shared.source->askDeviceThrough([&shared] { return ask(shared); });
        shared.source->decoder().probeDeviceThrough([&shared] { return probe(shared); });
        shared.source->decoder().stopStartOn(&shared.stopStart);
        ahead->m_thread = std::thread([&shared] { run(shared); });
    } catch (...) {
        shared.source->askDeviceThrough({});
        shared.source->decoder().probeDeviceThrough({});
        shared.source->decoder().stopStartOn(nullptr);
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
    const auto deadline = std::chrono::steady_clock::now() + kStopBound;
    bool exited = false;
    {
        std::unique_lock<std::mutex> lock(m_shared->mutex);
        m_shared->phase = Shared::Phase::Stopping;
        m_shared->stopStart.store(true, std::memory_order_relaxed);
        m_shared->changed.notify_all();
        // An answer begun before the deadline can run past it by the Device's own submit bound.
        while (m_shared->changed.wait_until(lock, deadline, [this] {
            return m_shared->exited || m_shared->question == Shared::Question::Asked;
        })) {
            if (m_shared->exited) {
                exited = true;
                break;
            }
            lock.unlock();
            bool healthy = false;
            try {
                healthy = VideoDecoder::deviceAnswers(m_device);
            } catch (...) {
                // Anything but a clean yes is a no: the decoder then leaves its device codec unfreed.
            }
            lock.lock();
            m_shared->question = healthy ? Shared::Question::Healthy : Shared::Question::Unhealthy;
            m_shared->changed.notify_all();
        }
        if (!exited) {
            m_shared->phase = Shared::Phase::Abandoned;
            m_shared->changed.notify_all();
        }
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
        std::optional<NumberedGrids> unshown;
        PictureSource::Step step = PictureSource::Dropping{};
    } call{&shared};
    const auto decode = [&call] {
        call.shared->source->decoder().leaveUnshown(call.unshown);
        call.step = call.shared->source->next(1, call.skip);
    };

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(shared.mutex);
            shared.changed.wait(
                lock, [&] { return shared.phase != Shared::Phase::Running || shared.pictures() < kDepth; });
            if (shared.phase != Shared::Phase::Running) break;
            call.skip = shared.skip;
            call.unshown = shared.unshown;
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
            wake = std::exchange(shared.awaited, false) && shared.phase == Shared::Phase::Running;
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
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (shared.phase == Shared::Phase::Abandoned) return false;
        shared.question = Shared::Question::Asked;
        running = shared.phase == Shared::Phase::Running;
    }
    shared.changed.notify_all();
    // A stopping destructor is woken by the notify; the hook is for a GPU thread still taking pictures.
    if (running) notifyReady(shared);
    std::unique_lock<std::mutex> lock(shared.mutex);
    shared.changed.wait(lock, [&] {
        return shared.phase == Shared::Phase::Abandoned || shared.question != Shared::Question::Asked;
    });
    const bool healthy = shared.question == Shared::Question::Healthy;
    shared.question = Shared::Question::None;
    return healthy;
}

// Posting wakes the GPU thread as a question does, so the probe is not left until the next picture.
VideoDecoder::Probe DecodeAhead::probe(Shared& shared) {
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (shared.probe) {
            if (std::holds_alternative<VideoDecoder::ProbeUnsent>(*shared.probe)) return VideoDecoder::ProbeUnsent{};
            return *std::exchange(shared.probe, std::nullopt);
        }
        shared.probe = VideoDecoder::ProbeUnsent{};
        running = shared.phase == Shared::Phase::Running;
    }
    shared.changed.notify_all();
    if (running) notifyReady(shared);
    return VideoDecoder::ProbeUnsent{};
}

// Only this thread moves a posted probe on from ProbeUnsent, so it is still posted when the lock is retaken.
void DecodeAhead::sendProbe(std::unique_lock<std::mutex>& lock) {
    if (!m_shared->probeUnsent()) return;
    lock.unlock();
    VideoDecoder::Probe sent = VideoDecoder::ProbeRefused{};
    try {
        // A marker, so the decoding thread sees it complete without this thread looking again.
        sent = m_device.submitMarker();
    } catch (...) {
        // Anything but a batch submitted is a no.
    }
    lock.lock();
    m_shared->probe = sent;
}

void DecodeAhead::answer(std::unique_lock<std::mutex>& lock) {
    sendProbe(lock);
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

void DecodeAhead::leaveUnshown(std::optional<NumberedGrids> grids) {
    std::lock_guard<std::mutex> lock(m_shared->mutex);
    m_shared->unshown = grids;
}

DecodeAhead::Item DecodeAhead::wait(std::optional<PictureSkip> skip) {
    std::unique_lock<std::mutex> lock(m_shared->mutex);
    if (skip) m_shared->skip = *skip;
    for (;;) {
        answer(lock);
        if (!m_shared->items.empty()) return pop();
        m_shared->changed.wait(lock, [this] {
            return !m_shared->items.empty() || m_shared->question == Shared::Question::Asked || m_shared->probeUnsent();
        });
    }
}

}
