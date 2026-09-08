// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "device.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wallpaperx {

// Hands out descriptor slots, and will not hand one back while a submitted batch could still
// be reading it.
//
// Writing a descriptor is not a queue operation: on the heap and descriptor-buffer tiers it
// is a host write into mapped memory the GPU reads at draw time, and on the classic tier a
// vkUpdateDescriptorSets on a set a pending command buffer has bound. Nothing recorded in a
// command buffer orders any of that against work already submitted, so the only thing that
// can is when the write happens. Reuse is what puts a write on bytes something is still
// reading, so reuse is what this delays.
//
// A slot is in exactly one of three states -- never issued, issued, or released and waiting
// for the last batch that could read it -- so the same slot backing two live allocations at
// once is not expressible.
class DescriptorSlotPool {
public:
    // `what` names the owner in this pool's errors, e.g. "DescriptorHeapAllocator: image".
    // `device` must outlive the pool, and every call must be on its GPU thread.
    DescriptorSlotPool(Device& device, uint32_t capacity, std::string what);

    DescriptorSlotPool(const DescriptorSlotPool&) = delete;
    DescriptorSlotPool& operator=(const DescriptorSlotPool&) = delete;

    // Blocks only when nothing is spare and the longest-released slot is still being read.
    // Throws std::runtime_error when every slot is issued.
    uint32_t acquire();

    // Throws std::runtime_error unless `slot` is currently issued, which is what makes a
    // double free a reported error rather than two allocations aliasing one slot.
    void release(uint32_t slot);

private:
    enum class SlotState : uint8_t { Fresh, Issued, Released };

    struct Released {
        uint32_t slot = 0;
        Device::SubmitTicket lastReader;
    };

    // Removes m_released.front(), marks it issued and returns it.
    uint32_t takeOldestReleased();

    Device& m_device;
    std::string m_what;
    std::vector<SlotState> m_state;
    uint32_t m_nextFresh = 0;
    // Oldest first, which is also lowest ticket first: release order follows submission order.
    std::vector<Released> m_released;
};

}
