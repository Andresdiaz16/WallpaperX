// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <system_error>

namespace wallpaperx::manifest {

// `errno` as the sentence fragment the system gives it. Safe from any thread, unlike std::strerror.
inline std::string systemError(int error) { return std::error_code(error, std::generic_category()).message(); }

} // namespace wallpaperx::manifest
