#pragma once

// The probe suite's on-disk PNG fixtures, in one place.
//
// Four probes had each written their own copy of "make the directory, fill a w*h*4
// buffer, stbi_write_png, throw on 0", and a fourth `#define STB_IMAGE_WRITE_IMPLEMENTATION`
// recompiled the writer into another TU each time. The two gradient copies had already
// converged to character-identical source, which is what an idiom looks like once it
// should have been extracted.
//
// Width and height stay separate parameters even though two callers pass a square: the
// non-square callers are the interesting ones (see writeGradientPng), and a `side`-only
// helper is what makes the next caller fork its own copy again.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace wallpaperx::testing {

namespace detail {

inline void writePngOrThrow(const std::string& path, int width, int height,
                            const std::vector<unsigned char>& pixels) {
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    if (stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) == 0) {
        throw std::runtime_error("test_image: could not write " + path);
    }
}

}  // namespace detail

// A flat RGBA image. Returns the path it wrote, so a caller can name the file and use it
// in one expression.
inline std::string writeSolidPng(const std::string& path, int width, int height,
                                 std::array<unsigned char, 4> rgba) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = rgba[0];
        pixels[i + 1] = rgba[1];
        pixels[i + 2] = rgba[2];
        pixels[i + 3] = rgba[3];
    }
    detail::writePngOrThrow(path, width, height, pixels);
    return path;
}

// A per-pixel gradient that varies in *both* axes, and the reason that matters lives with
// the code that produces it: the export probes compare a CPU mapping of exported memory
// against a Vulkan readback of the same image, and against a uniform frame that comparison
// succeeds for any rowPitch, any plane offset and either orientation. Only content that
// varies in both axes can fail it. R varies along x only and G along y only, so a
// transposed reading is distinguishable from a correct one.
inline std::string writeGradientPng(const std::string& path, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char* px = &pixels[(static_cast<size_t>(y) * width + x) * 4];
            px[0] = static_cast<unsigned char>(16 + x * 15);
            px[1] = static_cast<unsigned char>(16 + y * 15);
            px[2] = static_cast<unsigned char>((x * 16 + y * 3) & 0xff);
            px[3] = 255;
        }
    }
    detail::writePngOrThrow(path, width, height, pixels);
    return path;
}

}  // namespace wallpaperx::testing
