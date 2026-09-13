// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wallpaperx {

// Reads a compiled SPIR-V file. Throws std::runtime_error, prefixed with `context`, for a file
// that cannot be opened, is empty, or is not a whole number of words.
std::vector<uint32_t> readSpirv(const std::string& path, const char* context);

// A shader module read from a SPIR-V file, destroyed with this object. A module is only an input
// to pipeline creation, so no batch ever holds it and it may go as soon as the pipeline exists.
//
// Throws what readSpirv throws, and std::runtime_error when the driver rejects the module.
// Deliberately not classified against the GPU: readSpirv only size-checks the file, so a driver
// that rejects it is as likely to be reporting a bad shader bundle as a fault of its own.
class ShaderModule {
public:
    ShaderModule(VkDevice device, const std::string& path, const char* context);
    ~ShaderModule();

    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;

    VkShaderModule get() const { return m_module; }

private:
    VkDevice m_device;
    VkShaderModule m_module = VK_NULL_HANDLE;
};

}
