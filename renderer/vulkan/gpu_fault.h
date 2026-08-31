// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

namespace wallpaperx {

// A failure of the GPU itself -- submission, fences, command buffers -- as opposed to a
// failure of the thing being rendered. Lets a caller attribute the fault without matching
// on message text; a bad image and a lost device are both runtime_error otherwise.
struct GpuFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A rejection of the thing being rendered -- unreadable, undecodable, or too large an image.
// The user can fix it and the daemon cannot, so it must not mark the daemon unhealthy. It is
// named rather than inferred because the alternative default -- treat anything unrecognised
// as the wallpaper's fault -- reports a healthy daemon showing nothing.
struct WallpaperContentError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// "VK_ERROR_DEVICE_LOST", or the raw number for a code this build does not name. Only for
// diagnostics: an operator reads the daemon's last_error, and an integer there names nothing.
std::string vkResultName(VkResult result);

// Raises the fault `result` describes, for a driver call that has already failed. Running out
// of a host or device resource is a limit that says nothing about the GPU, so it stays
// unclassified and leaves the recorded GPU health alone; everything else -- a lost device, an
// initialization failure, a code the call may not return at all -- is the driver going wrong.
//
// Classified on the result rather than on a per-call list of permitted results: such a list
// is a second copy of the specification that nothing re-checks, and its only effect would be
// to promote a resource limit to a driver fault at calls not documented to return it.
//
// `context` should name the entry point, which is otherwise absent from the message.
[[noreturn]] void throwVkFailure(const std::string& context, VkResult result);

}  // namespace wallpaperx
