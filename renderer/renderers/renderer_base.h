// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vulkan/vulkan.h>

namespace wallpaperx {

struct OffscreenTarget;

class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Records a render of `target` into `cmd` and returns the image layout it leaves
    // the target in.
    //
    // Returning the layout rather than assuming one keeps the export path honest for
    // renderers that do not use OffscreenTargetManager's render pass -- a compute
    // write leaving the image in GENERAL, or a blit leaving TRANSFER_DST_OPTIMAL
    // (Step 13's video/shader tiers). A renderer that hard-coded the shared render
    // pass's finalLayout would make exportFrame declare a layout the image was never
    // in, which no validation layer catches on an ownership release.
    //
    // Callers should go through OffscreenTargetManager::render(), which submits the
    // work and records the returned layout on the target only once the submit has
    // succeeded.
    [[nodiscard]] virtual VkImageLayout record(VkCommandBuffer cmd, const OffscreenTarget& target) = 0;
};

}
