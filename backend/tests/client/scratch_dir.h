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

#include <cstdlib>
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
    ScratchDir(const std::filesystem::path& base, const char* tag) { create(base, tag); }

    // temp_directory_path() through its error_code overload rather than the throwing one.
    // A host whose $TMPDIR names a directory that does not exist makes the throwing one
    // throw out of the very first line of a phase -- past the !ok() arm that phase already
    // wrote -- and into whatever catch-all its main() has, which then reports "this machine
    // has nowhere to put a scratch directory" as a defect of the code under test. Failing
    // into ok() == false instead routes it to the reason the caller already prints.
    explicit ScratchDir(const char* tag) {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        // m_path stays empty and m_ok stays false: an empty base would make the name below
        // relative, so a failure here would create the scratch directory in the working
        // directory and then delete it from there.
        if (ec) return;
        create(base, tag);
    }

    // Every call here has a non-throwing overload, `increment(ec)` included: this runs in a
    // destructor, so a directory error met while walking would otherwise leave through an
    // implicitly-noexcept function and take the process down with std::terminate, losing
    // exactly the diagnostics the test existed to print.
    ~ScratchDir() {
        // Never constructed a name at all, so there is nothing of ours to chmod or remove.
        if (m_path.empty()) return;
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

    // The same for the state directory, which is where a copy of a rejected config goes. A
    // spawn that is given neither gets one of its own: a daemon started with the real
    // XDG_STATE_HOME would leave copies in the developer's own state directory.
    std::filesystem::path preservedDir() const { return m_path / "state" / "wallpaperx"; }
    std::string stateEnv() const { return "XDG_STATE_HOME=" + (m_path / "state").string(); }

private:
    void create(const std::filesystem::path& base, const char* tag) {
        static int counter = 0;
        m_path = base / ("wpx_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + "_" +
                         std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
        m_ok = !ec;
    }

    std::filesystem::path m_path;
    bool m_ok = false;
};

// Points this whole process's state directory at a scratch directory, for a test that builds
// a WallpaperManager in its own process.
//
// WallpaperManager::Config leaves stateDir empty like configFile, so a manager told nothing
// about state writes no record anywhere. This is still worth having for two reasons: not every
// path takes the directory from the Config -- preserveConfig() resolves defaultStateDir()
// itself -- and it is what lets dbus_service_test assert that an empty state directory stayed
// empty, in a directory it owns rather than the developer's.
//
// Construct it first in main(), before any Config: a manager already built has resolved
// whatever it was going to resolve, and an override set afterwards changes nothing for it.
class ScratchStateHome {
public:
    ScratchStateHome() : m_dir("statehome") {
        if (!m_dir.ok()) return;
        m_ok = ::setenv("XDG_STATE_HOME", m_dir.path().c_str(), 1) == 0;
    }

    bool ok() const { return m_ok; }
    const std::filesystem::path& path() const { return m_dir.path(); }

private:
    ScratchDir m_dir;
    bool m_ok = false;
};

}  // namespace wallpaperx::testing
