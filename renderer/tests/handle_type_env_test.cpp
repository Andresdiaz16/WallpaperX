// SPDX-License-Identifier: GPL-2.0-or-later

// Covers the two-argument externalMemoryHandleType() -- the overload the shipping code
// actually calls, and the one memory_export_test deliberately never touches.
//
// memory_export_test asserts the policy through the pure three-argument form, passing
// the override explicitly, so that its verdict cannot depend on the tester's shell. That
// is correct for testing the policy, but it left the wrapper -- the getenv and the
// function-local static that caches it -- with no coverage at all. Breaking either would
// have kept the whole suite green: memory_export_test never calls the wrapper, and
// modifier_roundtrip_probe only ever forces "dmabuf", a value that changes the outcome on
// NVIDIA alone.
//
// What this deliberately does NOT claim to cover is how an *empty* override reaches the
// three-argument form. It cannot: "" is an unrecognised override that falls through to
// the vendor policy, exactly as nullptr does, so no observation here can tell the two
// apart. The wrapper passes forced.c_str() unconditionally for that reason.
//
// The wrapper resolves the environment ONCE into a function-local static, so a single
// process can only ever observe one value of WALLPAPERX_FORCE_HANDLE_TYPE. That is why
// this is its own executable, registered three times with different ENVIRONMENT rather
// than looping over setenv internally: a loop would test the first value three times.
//
// argv[1] is the override value ctest put in the environment ("", "opaque" or
// "dmabuf"), and every expectation is derived from it rather than passed in separately.
// Passing the expected *answer* instead was the first attempt and was wrong: "opaque" is
// the right answer for NVIDIA both when opaque is forced and when nothing is forced, but
// those two cases differ elsewhere -- a device without dma_buf support must still throw
// when nothing was forced, and must not when opaque was. An argument naming the input
// keeps the two apart; one naming an output cannot.

#include "vulkan/memory_export.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    using wallpaperx::GpuVendor;

    if (argc < 2) {
        std::cerr << "handle_type_env_test: expected the handle type this environment should produce" << std::endl;
        return 1;
    }
    const std::string forced = argv[1];
    const bool forcedDmaBuf = forced == "dmabuf";
    const bool forcedOpaque = forced == "opaque";
    if (!forced.empty() && !forcedDmaBuf && !forcedOpaque) {
        std::cerr << "handle_type_env_test: unknown override '" << forced << "'" << std::endl;
        return 1;
    }

    int failures = 0;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            std::cerr << "FAIL: " << what << std::endl;
            ++failures;
        }
    };

    // NVIDIA is the discriminating vendor: its default policy is OPAQUE_FD, so a forced
    // "dmabuf" changes the answer and anything else does not. Asking about a vendor whose
    // default is already DMA_BUF would let the dmabuf case pass without the override
    // being read at all.
    // GOAL: S3.2 -- the two-argument overload the shipping code calls reads WALLPAPERX_FORCE_HANDLE_TYPE,
    //       and an unset variable means no override.
    // CONTROL: renderer/vulkan/memory_export.cpp, the wrapper passing nullptr instead of the cached
    //       value -> handle_type_env_dmabuf "FAIL: the two-argument overload honours
    //       WALLPAPERX_FORCE_HANDLE_TYPE for NVIDIA", handle_type_env_opaque "FAIL: forcing opaque must
    //       not throw for a device without dma_buf support". Separately, an empty value passed as
    //       "opaque" -> handle_type_env_default "FAIL: a device without dma_buf support yields a handle
    //       type only when opaque was forced". Both reverted.
    const VkExternalMemoryHandleTypeFlagBits expected =
        forcedDmaBuf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    const VkExternalMemoryHandleTypeFlagBits actual =
        wallpaperx::externalMemoryHandleType(GpuVendor::NVIDIA, /*hasExternalMemoryDmaBuf=*/true);
    check(actual == expected, "the two-argument overload honours WALLPAPERX_FORCE_HANDLE_TYPE for NVIDIA");
    if (actual != expected) {
        std::cerr << "  override=\"" << forced << "\" expected 0x" << std::hex << expected << " but got 0x" << actual
                  << std::dec << std::endl;
    }

    // A device that cannot do dma_buf. Forcing opaque makes this answerable; anything
    // else -- including an empty override, which must fall through to the vendor policy
    // rather than count as "set" -- has to throw rather than hand back a handle type the
    // driver cannot produce.
    try {
        const VkExternalMemoryHandleTypeFlagBits noDmaBuf =
            wallpaperx::externalMemoryHandleType(GpuVendor::AMD, /*hasExternalMemoryDmaBuf=*/false);
        check(forcedOpaque && noDmaBuf == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
              "a device without dma_buf support yields a handle type only when opaque was forced");
    } catch (const std::runtime_error&) {
        check(!forcedOpaque, "forcing opaque must not throw for a device without dma_buf support");
    }

    if (failures == 0) {
        std::cout << "handle_type_env_test (override=\"" << forced << "\"): all checks passed" << std::endl;
        return 0;
    }
    std::cerr << "handle_type_env_test (override=\"" << forced << "\"): " << failures << " check(s) failed" << std::endl;
    return 1;
}
