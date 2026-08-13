// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A temporary directory a test owns: created on construction, removed on destruction.
//
// Extracted because three copies of the idiom existed and had already diverged --
// config_store_test's restored permissions before removing (a phase makes a directory
// read-only on purpose), daemon_system_test's did not, and SpawnedDaemon's cleanup lived
// in the enclosing destructor, so a throw between creating the directory and finishing the
// constructor leaked it. The surviving copy is the strictest of the three.
//
// Client-only, like its two siblings in this directory: nothing here includes a daemon
// header, so the out-of-process tier can use it too.

#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

namespace wallpaperx::testing {

class ScratchDir {
public:
    // `base` is a parameter because one phase needs a real disk: temp_directory_path() is
    // tmpfs on most hosts, and a measurement that compares a durable write against a plain
    // one cannot discriminate there.
    ScratchDir(const std::filesystem::path& base, const char* tag) {
        static int counter = 0;
        m_path = base / ("wpx_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + "_" +
                         std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
        m_ok = !ec;
    }

    explicit ScratchDir(const char* tag) : ScratchDir(std::filesystem::temp_directory_path(), tag) {}

    // Every call here has a non-throwing overload, `increment(ec)` included: this runs in a
    // destructor, so a directory error met while walking would otherwise leave through an
    // implicitly-noexcept function and take the process down with std::terminate, losing
    // exactly the diagnostics the test existed to print.
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::permissions(m_path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        std::error_code walkEc;
        const std::filesystem::recursive_directory_iterator last;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 m_path, std::filesystem::directory_options::skip_permission_denied, walkEc);
             !walkEc && it != last; it.increment(walkEc)) {
            std::error_code entryEc;
            if (it->is_directory(entryEc)) {
                std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::add, entryEc);
            }
        }
        // The chmod walk is best effort; the removal below is what must run whatever it did.
        ec.clear();
        std::filesystem::remove_all(m_path, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    bool ok() const { return m_ok; }
    const std::filesystem::path& path() const { return m_path; }

    // Where a daemon pointed at this directory as its XDG_CONFIG_HOME keeps its config.
    std::filesystem::path configFile() const { return m_path / "wallpaperx" / "config.json"; }
    std::string env() const { return "XDG_CONFIG_HOME=" + m_path.string(); }

private:
    std::filesystem::path m_path;
    bool m_ok = false;
};

}  // namespace wallpaperx::testing
