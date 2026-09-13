// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_module.h"

#include "gpu_fault.h"

#include <fstream>
#include <stdexcept>

namespace wallpaperx {

std::vector<uint32_t> readSpirv(const std::string& path, const char* context) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error(std::string(context) + ": failed to open shader file " + path);
    }
    const std::streamoff rawSize = file.tellg();
    // tellg() returns -1 on a stat failure even for an open stream; casting that to size_t
    // would request a ~16 EB allocation. A zero-byte file would build a 0-word module, which
    // is invalid. Reject both, plus non-word sizes, before allocating.
    if (rawSize <= 0 || static_cast<size_t>(rawSize) % sizeof(uint32_t) != 0) {
        throw std::runtime_error(std::string(context) + ": shader file is not a valid SPIR-V module: " + path);
    }
    const size_t size = static_cast<size_t>(rawSize);
    std::vector<uint32_t> buffer(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    if (!file || file.gcount() != static_cast<std::streamsize>(size)) {
        throw std::runtime_error(std::string(context) + ": shader file could not be read in full: " + path);
    }
    return buffer;
}

ShaderModule::ShaderModule(VkDevice device, const std::string& path, const char* context) : m_device(device) {
    const std::vector<uint32_t> spirv = readSpirv(path, context);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    if (const VkResult r = vkCreateShaderModule(m_device, &info, nullptr, &m_module); r != VK_SUCCESS) {
        throw std::runtime_error(std::string(context) + ": vkCreateShaderModule failed with " + vkResultName(r));
    }
}

ShaderModule::~ShaderModule() {
    if (m_module != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, m_module, nullptr);
}

}
