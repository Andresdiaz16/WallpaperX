#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace wallpaperx::testing {

// The folder a test writes its generated fixtures into: under TMPDIR, which isolated_run.sh makes a
// fresh private directory for every ctest entry and removes afterwards. So entries running in
// parallel -- including twins of one binary -- never write a file another is reading. Empty when
// the folder cannot be made; callers then fail their write, which they report as a harness fault.
inline const std::string& testImageDir() {
    static const std::string dir = [] {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) return std::string();
        const std::filesystem::path images = base / "wallpaperx-test-images";
        std::filesystem::create_directories(images, ec);
        return ec ? std::string() : images.string();
    }();
    return dir;
}

}  // namespace wallpaperx::testing
