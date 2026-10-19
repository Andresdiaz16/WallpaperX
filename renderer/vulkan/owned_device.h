// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

namespace wallpaperx {

class Device;

// Deletes a Device, unless Device::threadLeftRunning(): then it is leaked, since a thread left
// running may still read it.
struct DeviceDeleter {
    void operator()(Device* device) const noexcept;
};

// What an owner holds a Device by when a thread it cannot join may use it.
using OwnedDevice = std::unique_ptr<Device, DeviceDeleter>;

}
