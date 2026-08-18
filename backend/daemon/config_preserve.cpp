// SPDX-License-Identifier: GPL-2.0-or-later

#include "config_preserve.h"

#include "config_store.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace wallpaperx {
namespace {

// Bounds the .bad, .bad.1, ... search. One name meant the second unreadable config had
// nowhere to go, and nowhere to go meant saving was disabled for the run.
constexpr int kMaxPreservedCopies = 10;

// Where a preserved copy is preferably kept: $XDG_STATE_HOME/wallpaperx, else
// ~/.local/state/wallpaperx. Out of the config directory, so a hand edit or a tidy-up there
// cannot take the copy with the file it was made for. Separation from the directory the user
// edits, not protection from the user -- they own this one too. Empty when neither variable is
// usable, which preserveConfigTo() answers by keeping the copy beside the config instead; a
// relative value is ignored, as the basedir spec requires and defaultConfigPath() already does.
std::filesystem::path preservedCopyDir() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
        const std::filesystem::path base(xdg);
        if (base.is_absolute()) return base / "wallpaperx";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        const std::filesystem::path base(home);
        if (base.is_absolute()) return base / ".local" / "state" / "wallpaperx";
    }
    return {};
}

// The store's own limit, not a second copy of it: a file it refuses to read is not one to
// compare against a preserved copy.
constexpr std::uintmax_t kMaxComparedBytes = ConfigStore::kMaxBytes;

void appendReason(std::string& why, const std::string& reason) {
    if (!why.empty()) why += "; ";
    why += reason;
}

// Size first: it settles nearly every pair without a read.
bool sameContents(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    const std::uintmax_t sizeA = std::filesystem::file_size(a, ec);
    if (ec) return false;
    const std::uintmax_t sizeB = std::filesystem::file_size(b, ec);
    if (ec || sizeA != sizeB || sizeA > kMaxComparedBytes) return false;

    std::ifstream streamA(a, std::ios::binary);
    std::ifstream streamB(b, std::ios::binary);
    if (!streamA || !streamB) return false;
    return std::equal(std::istreambuf_iterator<char>(streamA), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(streamB));
}

// Where the bytes are and what went wrong, kept apart: a failed unlink after a good copy is a
// complaint about the original, not about the preservation, and one value for both let the
// second erase the first.
struct KeptCopy {
    std::string kept;     // empty: nothing was preserved
    std::string trouble;  // why, or what still needs saying about a copy that succeeded
};

// The name slot `n` would use: <file>.bad, then .bad.1, .bad.2 ...
std::filesystem::path preservedName(const std::filesystem::path& source,
                                    const std::filesystem::path& dir, int slot) {
    return dir / (source.filename().string() + ".bad" + (slot == 0 ? "" : "." + std::to_string(slot)));
}

enum class NameState {
    Free,        // nothing there
    SameBytes,   // an earlier run already preserved exactly this content here
    OtherBytes,  // an earlier broken config; never write over it
    Unknown,     // the filesystem would not say
};

NameState inspectName(const std::filesystem::path& source, const std::filesystem::path& kept,
                      std::string& why) {
    std::error_code ec;
    const bool taken = std::filesystem::exists(kept, ec);
    if (ec) {
        why = ec.message();
        return NameState::Unknown;
    }
    if (!taken) return NameState::Free;
    return sameContents(source, kept) ? NameState::SameBytes : NameState::OtherBytes;
}

// Whether the source survived matters to the caller: a rename has already taken it away, and
// asking again would report a file that is missing because this succeeded.
struct Placement {
    std::string error;
    bool sourceRemains = true;
};

Placement putBytes(const std::filesystem::path& source, const std::filesystem::path& kept,
                   bool move) {
    std::error_code ec;
    if (move) {
        // Atomic where it can be. The copy below exists only for the cross-filesystem case, and
        // unlike a rename it can leave a partial file behind.
        std::filesystem::rename(source, kept, ec);
        if (!ec) return {{}, false};
        if (ec != std::errc::cross_device_link) return {ec.message(), true};
        ec.clear();
    }

    std::filesystem::copy_file(source, kept, ec);
    if (!ec) return {{}, true};
    // A truncated remnant would hold a name every later attempt is denied, and offer the user a
    // silently shortened config as a preserved one. Left alone when something else is there.
    if (ec != std::errc::file_exists) {
        std::error_code cleanupEc;
        std::filesystem::remove(kept, cleanupEc);
    }
    return {ec.message(), true};
}

std::string removeSource(const std::filesystem::path& source) {
    std::error_code ec;
    std::filesystem::remove(source, ec);
    return ec ? ec.message() : std::string{};
}

// Keeps `source` in `dir` under the first usable <file>.bad[.N]. A copy already holding these
// bytes counts as preserved: a repair that cannot be written recurs every start, and a fresh
// name each time would exhaust kMaxPreservedCopies in as many restarts.
KeptCopy keepCopyIn(const std::filesystem::path& source, const std::filesystem::path& dir,
                    bool move) {
    std::string reason;
    for (int slot = 0; slot < kMaxPreservedCopies; ++slot) {
        const std::filesystem::path kept = preservedName(source, dir, slot);
        switch (inspectName(source, kept, reason)) {
            case NameState::OtherBytes:
            case NameState::Unknown:
                continue;
            case NameState::SameBytes:
                return {kept.string(), move ? removeSource(source) : std::string{}};
            case NameState::Free:
                break;
        }

        const Placement placed = putBytes(source, kept, move);
        if (!placed.error.empty()) {
            reason = placed.error;
            continue;
        }
        // The copy stays even when the original cannot go: the bytes are the point, and a start
        // that finds this same content reuses this name rather than spending another.
        return {kept.string(),
                move && placed.sourceRemains ? removeSource(source) : std::string{}};
    }
    return {{}, reason.empty() ? "no free name" : reason};
}

}

PreservedCopy preserveConfig(const std::filesystem::path& configFile, bool move) {
    PreservedCopy result;

    // The state directory is preferred but not required: the config's own directory must
    // already be writable for any save to work, so "nowhere preferred" must not become
    // "nowhere" -- which is what a daemon with a config path and no HOME would otherwise hit.
    std::vector<std::filesystem::path> dirs;
    if (std::filesystem::path preferred = preservedCopyDir(); !preferred.empty()) {
        dirs.push_back(std::move(preferred));
    }
    if (std::filesystem::path beside = configFile.parent_path();
        !beside.empty() && (dirs.empty() || dirs.front() != beside)) {
        dirs.push_back(std::move(beside));
    }
    if (dirs.empty()) {
        result.why = "config file has no directory to keep a copy in";
        return result;
    }

    for (const std::filesystem::path& dir : dirs) {
        std::error_code dirEc;
        std::filesystem::create_directories(dir, dirEc);
        if (dirEc) {
            appendReason(result.why, dir.string() + ": " + dirEc.message());
            continue;
        }
        const KeptCopy copy = keepCopyIn(configFile, dir, move);
        if (!copy.kept.empty()) {
            // Directories that failed before this one stop mattering once a copy exists; that
            // the original is still where the next save writes does not.
            result.kept = copy.kept;
            result.why = copy.trouble.empty() ? std::string{}
                                              : "original still in place: " + copy.trouble;
            return result;
        }
        // Every candidate named: a message blaming only the last one sends the user to the
        // wrong directory.
        appendReason(result.why, dir.string() + ": " + copy.trouble);
    }
    return result;
}

}
