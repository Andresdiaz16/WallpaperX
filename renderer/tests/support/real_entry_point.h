// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The next definition of `name` after this executable's own, for a probe that interposes a Vulkan
// or libc entry point and forwards to the real one. A probe whose forward target cannot be found
// has nothing to grade, so it says which probe and which name, then aborts, rather than calling
// through a null pointer.

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>

namespace wallpaperx::testing {

template <class Fn>
Fn realEntryPoint(const char* probe, const char* name) {
    auto* fn = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
    if (fn == nullptr) {
        std::fprintf(stderr, "%s: cannot resolve the real %s\n", probe, name);
        std::abort();
    }
    return fn;
}

}  // namespace wallpaperx::testing
