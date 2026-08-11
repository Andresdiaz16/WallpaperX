#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace wallpaperx {

// One monitor's wallpaper as it survives a restart. Everything else in MonitorStatus --
// generation, hasFrame, lastError -- is a fact about the current run and is deliberately
// not persisted.
struct PersistedWallpaper {
    int monitorId = 0;
    std::string type;
    std::string path;
};

struct LoadResult {
    std::vector<PersistedWallpaper> entries;

    // Empty when nothing was wrong, including the ordinary case of no file at all.
    std::string error;

    // Entries dropped for being structurally wrong (not an object, a missing or
    // wrong-typed field). Counted rather than fatal: one broken entry should not cost the
    // user the other three monitors.
    int malformedEntries = 0;

    bool fileExisted = false;
};

struct SaveResult {
    std::string error;

    // Skipped because the path was not valid UTF-8, which nlohmann::json::dump() throws
    // on. Reported so the daemon can say which monitor will not come back, rather than
    // failing the whole write or silently mangling the path into something that restores
    // a different image.
    int skippedNonUtf8 = 0;
};

// $XDG_CONFIG_HOME/wallpaperx/config.json, else $HOME/.config/wallpaperx/config.json.
// Empty when neither is usable -- a relative XDG_CONFIG_HOME is ignored per the XDG
// basedir spec, and a daemon with no usable HOME must not fall back to the working
// directory, since one started from / would leave a config file there.
std::filesystem::path defaultConfigPath();

// Reads and writes the daemon's persisted wallpaper list.
//
// Validates structure only -- that the file is an object of the expected version holding
// an array of {monitor_id, type, path}. Whether a type is one this daemon knows, whether
// an id is in range, and whether a path is readable are the WallpaperManager's rules, and
// re-stating any of them here would be a second copy to drift from the first.
//
// Neither load() nor save() throws. A corrupt config file must not stop the daemon
// starting, and a failed write must not fail the SetWallpaper call that triggered it, so
// both report through their result type instead.
class ConfigStore {
public:
    static constexpr int kSchemaVersion = 1;

    // An empty path disables persistence: load() returns nothing and save() does nothing,
    // both without error. Callers opt in explicitly rather than defaulting to the real
    // location, so a test that says nothing about config cannot read or overwrite the
    // developer's own.
    explicit ConfigStore(std::filesystem::path file);

    bool enabled() const { return !m_file.empty(); }
    const std::filesystem::path& file() const { return m_file; }

    LoadResult load() const;

    // Creates the directory if needed, then writes through a temp file and rename() so a
    // crash mid-write cannot leave a truncated config. No fsync: it costs 824 us against
    // 9 us for the rename alone (measured, PLAN.md Step 8), runs on the DBus event-loop
    // thread, and buys only durability across a power cut for the last wallpaper choice.
    SaveResult save(const std::vector<PersistedWallpaper>& entries) const;

private:
    std::filesystem::path m_file;
};

}  // namespace wallpaperx
