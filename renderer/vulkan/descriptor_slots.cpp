// SPDX-License-Identifier: GPL-2.0-or-later

#include "descriptor_slots.h"

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace wallpaperx {

DescriptorSlotPool::DescriptorSlotPool(Device& device, uint32_t capacity, std::string what)
    : m_device(device), m_what(std::move(what)), m_state(capacity, SlotState::Fresh) {
    // Every slot can be waiting on its reader at once, so this is the capacity the list needs.
    // The reserve is what makes release()'s push_back unable to reallocate -- it must not throw
    // after the caller has already given the slot up.
    m_released.reserve(capacity);
}

uint32_t DescriptorSlotPool::takeReleased(size_t index) {
    const uint32_t slot = m_released[index].slot;
    m_released.erase(m_released.begin() + static_cast<std::ptrdiff_t>(index));
    m_state[slot] = SlotState::Issued;
    return slot;
}

uint32_t DescriptorSlotPool::acquire() {
    // Taken before a fresh slot when its reader has finished: it costs nothing, and it keeps
    // a pool that is never under pressure using the same slot instead of walking the whole
    // range. Every released slot is considered, not just the oldest: one slow reader would
    // otherwise hold back slots whose own readers finished long ago.
    for (size_t i = 0; i < m_released.size(); ++i) {
        if (m_device.hasCompleted(m_released[i].lastReader)) return takeReleased(i);
    }

    if (m_nextFresh < m_state.size()) {
        const uint32_t slot = m_nextFresh++;
        m_state[slot] = SlotState::Issued;
        return slot;
    }

    if (m_released.empty()) {
        throw std::runtime_error(m_what + ": all " + std::to_string(m_state.size()) +
                                 " descriptor slots are in use; free a handle first");
    }

    // Nothing spare, and no released slot's reader has finished. The front is the one that will
    // come free first: release order follows submission order, and the loop above has already
    // ruled out every slot released without a reader. Waiting is what a pool this size costs;
    // the alternative is overwriting a descriptor mid-draw.
    m_device.wait(m_released.front().lastReader);
    return takeReleased(0);
}

void DescriptorSlotPool::release(uint32_t slot) {
    // The last batch that could read this slot: no later one can, because the handle naming
    // it is gone.
    releaseTo(slot, m_device.lastSubmission());
}

void DescriptorSlotPool::releaseUnwritten(uint32_t slot) {
    releaseTo(slot, Device::SubmitTicket{});
}

void DescriptorSlotPool::releaseTo(uint32_t slot, Device::SubmitTicket lastReader) {
    if (slot >= m_state.size() || m_state[slot] != SlotState::Issued) {
        throw std::runtime_error(m_what + ": handle already freed or never allocated");
    }
    m_released.push_back(Released{slot, lastReader});
    m_state[slot] = SlotState::Released;
}

}
