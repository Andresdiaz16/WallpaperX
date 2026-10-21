// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "picture_skip.h"
#include "picture_source.h"

#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace wallpaperx {

class Device;

// A PictureSource decoding on a thread of its own, a few pictures ahead of whoever takes them, so
// a decode call that blocks holds up no picture already decoded: ffmpeg 9.0.2 waits for the GPU
// after each picture of a packet, and an AV1 packet can hold five.
//
// Made, used and destroyed on the GPU thread, and only for a device whose queues are internally
// synchronised; the thread it starts is the only other one to touch the source. The source's
// decoder asks whether the device answers through the GPU thread, so take() and wait() answer a
// pending question before anything else, and the ready hook is called when one is asked.
//
// Destruction stops the thread once the decode call it is in returns. One still inside after
// kStopBound -- ffmpeg's own waits on the GPU are unbounded -- is left running, rather than the GPU
// thread held behind it: the source, the pictures queued and this object's state are leaked, and
// Device::recordThreadLeftRunning() keeps the Device, which the thread still reads, from being freed.
class DecodeAhead {
public:
    // Runs each decode the thread makes; must call the step it is given once.
    using Around = std::function<void(const std::function<void()>&)>;
    // Called from the thread, with nothing locked, once there is an item a take() found missing
    // or a question for the GPU thread.
    using Ready = std::function<void()>;

    // Starts the thread on `source`. Null, with `source` as it was, when the system has no thread
    // to give; throws std::bad_alloc, with `source` as it was, when out of memory.
    static std::unique_ptr<DecodeAhead> start(Device& device, std::unique_ptr<PictureSource>& source, Around around,
                                              Ready ready);
    ~DecodeAhead();

    DecodeAhead(const DecodeAhead&) = delete;
    DecodeAhead& operator=(const DecodeAhead&) = delete;

    // What the thread produced, in order: a picture, the stream's end, or what the source threw,
    // which ends decoding. Either of the last two stays at the head once reached.
    using Item = std::variant<PictureSource::Picture, PictureSource::End, std::exception_ptr>;

    // The next item, or nothing yet, never waiting for the thread; with nothing, the ready hook
    // is called once there is. `skip` is what the thread's next decodes leave out. Throws what
    // the device's answer to a pending question throws.
    std::optional<Item> take(PictureSkip skip);
    // The next item, waiting for it. `skip`, when given, is as take()'s; without one the thread's
    // next decodes leave out what they did. Throws as take() does.
    Item wait(std::optional<PictureSkip> skip);

    // What the thread's next decodes leave out beside `skip`, as VideoDecoder::leaveUnshown(). Starts as
    // the source's own.
    void leaveUnshown(std::optional<NumberedGrids> grids);

private:
    struct Shared;
    DecodeAhead(Device& device);

    static void run(Shared& shared) noexcept;
    // The decoder's question, put to the GPU thread from the decoding one.
    static bool ask(Shared& shared);
    static void notifyReady(Shared& shared) noexcept;
    void answer(std::unique_lock<std::mutex>& lock);
    Item pop();

    Device& m_device;
    std::unique_ptr<Shared> m_shared;
    std::thread m_thread;
};

}
