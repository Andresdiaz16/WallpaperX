// SPDX-License-Identifier: GPL-2.0-or-later

#include "descriptor_slots.h"

#include <stdexcept>
#include <utility>

namespace wallpaperx {

DescriptorSlotPool::DescriptorSlotPool(Device& device, uint32_t capacity, std::string what)
    : m_device(device), m_what(std::move(what)), m_state(capacity, SlotState::Fresh) {
    // A released slot is one that was issued, so at most capacity - 1 are ever waiting at
    // once and this reserve makes release()'s push_back unable to reallocate -- it must not
    // throw after the caller has already given the slot up.
    m_released.reserve(capacity);
}

uint32_t DescriptorSlotPool::takeOldestReleased() {
    const uint32_t slot = m_released.front().slot;
    m_released.erase(m_released.begin());
    m_state[slot] = SlotState::Issued;
    return slot;
}

uint32_t DescriptorSlotPool::acquire() {
    // Taken before a fresh slot when its reader has finished: it costs nothing, and it keeps
    // a pool that is never under pressure using the same slot instead of walking the whole
    // range. The oldest is checked because it carries the lowest ticket.
    if (!m_released.empty() && m_device.hasCompleted(m_released.front().lastReader)) {
        return takeOldestReleased();
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

    // Nothing spare, and the one slot that will come free first is still being read. Waiting
    // is what a pool this size costs; the alternative is overwriting a descriptor mid-draw.
    m_device.wait(m_released.front().lastReader);
    return takeOldestReleased();
}

void DescriptorSlotPool::release(uint32_t slot) {
    if (slot >= m_state.size() || m_state[slot] != SlotState::Issued) {
        throw std::runtime_error(m_what + ": handle already freed or never allocated");
    }
    // The last batch that could read this slot: no later one can, because the handle naming
    // it is gone.
    m_released.push_back(Released{slot, m_device.lastSubmission()});
    m_state[slot] = SlotState::Released;
}

}
