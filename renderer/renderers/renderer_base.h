// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vulkan/vulkan.h>

#include <functional>

namespace wallpaperx {

class Device;
struct OffscreenTarget;

class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Records a render of `target` into `cmd` and returns the image layout it leaves
    // the target in.
    //
    // Returning the layout rather than assuming one keeps the export path honest for
    // renderers that do not end in OffscreenTargetManager's render pass, should one ever
    // add work after it. A renderer that hard-coded the shared render
    // pass's finalLayout would make exportFrame declare a layout the image was never
    // in, which no validation layer catches on an ownership release.
    //
    // Callers should go through OffscreenTargetManager::render(), which submits the
    // work and records the returned layout on the target only once the submit has
    // succeeded.
    [[nodiscard]] virtual VkImageLayout record(VkCommandBuffer cmd, const OffscreenTarget& target) = 0;

    // Submits the batch render() records -- which calls record() -- exactly once, or throws
    // having submitted nothing. A renderer reading something another queue writes overrides it
    // to join that ordering; the default is a plain Device::submit. Defined in offscreen.cpp,
    // beside its one caller.
    virtual void submit(Device& device, const std::function<void(VkCommandBuffer)>& recordBatch);
};

}
