// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "log_escape.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wallpaperx {

// A screen's size in device pixels as its desktop draws it, rotation and scaling applied.
struct OutputSize {
    uint32_t width = 0;
    uint32_t height = 0;

    bool operator==(const OutputSize&) const = default;
};

// One monitor's wallpaper as it survives a restart; nothing about the current run is kept.
// `output` is the connector name -- "DP-3" -- and may ride on a row with no type or path: a
// screen named but not yet chosen for is the ordinary first-run state, and losing it leaves
// the plugin anonymous again.
struct PersistedWallpaper {
    int monitorId = 0;
    std::string type;
    std::string path;
    std::string output;

    // What `output`'s screen last reported. A fact about that name, so it is written only
    // beside it. The `output_size` field as JSON text, empty when the row has none; text for the
    // reason propertiesJson gives: a row kept for a newer build carries it as read.
    std::string outputSizeJson;

    // The wallpaper's edited values, which belong to `type` and `path` and go wherever they go: the
    // `properties` field as JSON text, empty when the row has none. Text rather than
    // WallpaperProperties because a row kept for a newer build carries values this build cannot
    // type, and only the manager knows which rows those are; it types the rest itself.
    std::string propertiesJson;
};

// Not a fit -- real names run under a dozen bytes -- but a finite bound on a string a
// client chooses, which is then stored, written to the config and echoed back.
constexpr std::size_t kMaxOutputNameBytes = 256;

// Why a connector name is unusable, or empty when it is fine. One definition because three
// callers reject it: WallpaperManager::setMonitorOutput, GET /config?output=, and
// WallpaperManager::restoreSaved on what it read out of this file. ConfigStore does not --
// it validates structure, and these are the manager's rules.
inline std::string outputNameError(std::string_view name) {
    if (name.empty()) return "output name must not be empty";
    if (name.size() > kMaxOutputNameBytes) {
        return "output name must be at most " + std::to_string(kMaxOutputNameBytes) +
               " bytes, got " + std::to_string(name.size());
    }
    // dump() throws on invalid UTF-8, so a name that reached the file and could not be
    // written would cost the user every other entry in it.
    if (!isValidUtf8(name)) return "output name is not valid UTF-8";
    return {};
}

// Per axis. Above every shipping panel, and the Vulkan Roadmap 2022 floor for a 2D image, so a
// size accepted here is one the render device can create rather than a GPU failure recorded
// against the daemon on a peer's say-so.
constexpr uint32_t kMaxOutputDimension = 8192;

// Why a reported screen size is unusable, or empty when it is fine. Shared by
// WallpaperManager::setOutputSize and WallpaperManager::restoreSaved, like outputNameError.
inline std::string outputSizeError(const OutputSize& size) {
    if (size.width == 0 || size.height == 0 || size.width > kMaxOutputDimension ||
        size.height > kMaxOutputDimension) {
        return "output size must be 1 to " + std::to_string(kMaxOutputDimension) +
               " pixels on each side, got " + std::to_string(size.width) + "x" +
               std::to_string(size.height);
    }
    return {};
}

// The form a config row carries a size in.
std::string toJsonText(const OutputSize& size);

// The reverse. Anything unreadable -- not an object, a missing or non-integer side, a negative
// one, text that is not JSON -- comes back 0x0, which outputSizeError() refuses.
OutputSize outputSizeFromJson(const std::string& text);

struct LoadResult {
    std::vector<PersistedWallpaper> entries;

    // Empty when nothing was wrong, including the ordinary case of no file at all.
    std::string error;

    // Structurally wrong entries, counted rather than fatal: one broken entry should not
    // cost the user their other monitors.
    int malformedEntries = 0;

    bool fileExisted = false;

    // Whether an ordinary file was read and rejected on its contents, as opposed to never
    // being inspected. Only then is moving it aside safe: a transient stat failure on a
    // good config must not rename it away.
    bool preservable = false;
};

struct SaveResult {
    std::string error;

    // Wallpapers dropped because their path or type is not valid UTF-8, which dump() throws
    // on. The row survives if its screen name is writable, so this counts lost pictures, not
    // lost rows; the count is what lets the daemon say a monitor will not come back.
    int skippedNonUtf8 = 0;

    // Screen names dropped for the same reason, counted apart because the two halves of a row
    // fail independently: reporting a name loss as a wallpaper loss sends the user looking for
    // a picture that is still there.
    int skippedNames = 0;
};

// $XDG_CONFIG_HOME/wallpaperx/config.json, else $HOME/.config/... Empty when neither is
// usable: a relative XDG_CONFIG_HOME is ignored per the basedir spec, and there is no cwd
// fallback -- a daemon started from / would leave a config file there.
std::filesystem::path defaultConfigPath();

// $XDG_STATE_HOME/wallpaperx, else $HOME/.local/state/wallpaperx: the daemon's own directory,
// apart from the one the user edits. Same rules as above for an unusable value.
std::filesystem::path defaultStateDir();

// Validates structure only; known types, id ranges and path readability are the
// WallpaperManager's rules. Neither load() nor save() throws -- a corrupt file must not
// stop the daemon starting, nor a failed write fail the SetWallpaper behind it.
class ConfigStore {
public:
    static constexpr int kSchemaVersion = 1;

    // The largest config this daemon will read. Public so anything reasoning about a config
    // file's bytes shares the bound rather than keeping a second copy that can drift.
    static constexpr std::uintmax_t kMaxBytes = 1u << 20;

    // An empty path disables persistence, silently. Callers opt in explicitly so a test
    // that says nothing about config cannot touch the developer's own.
    explicit ConfigStore(std::filesystem::path file);

    bool enabled() const { return !m_file.empty(); }
    const std::filesystem::path& file() const { return m_file; }

    LoadResult load() const;

    // Temp file + rename(), so a crash mid-write cannot leave a truncated config. No
    // fsync: 824 us against 9 us for the rename alone (PLAN.md Step 8), on the DBus
    // event-loop thread, buying only survival of a power cut.
    SaveResult save(const std::vector<PersistedWallpaper>& entries) const;

private:
    std::filesystem::path m_file;
};

}  // namespace wallpaperx
