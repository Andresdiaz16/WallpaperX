// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A Wallpaper Engine scene package, scene.pkg or gifscene.pkg (docs/phase4/spec_pkg.md). The file is
// untrusted: its table is checked against the file's real size before anything is kept, and entries
// are read from the file on demand, never the whole package at once.

#include "unique_fd.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wallpaperx::we {

// Far above any real package (the bases are in PLAN.md); a package past one of them is refused.
inline constexpr std::uint32_t kMaxPackageEntries = 65536;
inline constexpr std::uint32_t kMaxPackageNameBytes = 4096;
inline constexpr std::size_t kMaxPackageTableBytes = 1u << 20;

class Package {
public:
    struct Entry {
        // As stored, case-sensitive, and a plain relative path (plain_path.h).
        std::string name;
        // From the start of the file.
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
    };

    // `fd` must be open for reading; the package keeps it until destroyed. Refuses anything but a
    // regular file. Which file, and whether it may be outside its item's folder, is the opener's
    // call. Errors are sentences that do not name the file; the caller adds it.
    static std::expected<Package, std::string> open(UniqueFd fd);

    // "PKGV" and four digits.
    const std::string& version() const { return m_version; }
    // Sorted by name; names are unique.
    std::span<const Entry> entries() const { return m_entries; }
    const Entry* find(std::string_view name) const;

    // The entry's bytes, or why not: no such entry, larger than `limit`, or the file no longer holds
    // them (it shrank since it was opened). Safe to call from several threads at once.
    std::expected<std::string, std::string> read(std::string_view name, std::size_t limit) const;

private:
    Package(UniqueFd fd, std::string version, std::vector<Entry> entries)
        : m_fd(std::move(fd)), m_version(std::move(version)), m_entries(std::move(entries))
    {
    }

    UniqueFd m_fd;
    std::string m_version;
    std::vector<Entry> m_entries;
};

} // namespace wallpaperx::we
