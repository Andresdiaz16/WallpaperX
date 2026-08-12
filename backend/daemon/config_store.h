#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace wallpaperx {

// One monitor's wallpaper as it survives a restart. Nothing about the current run
// (generation, hasFrame, lastError) is persisted.
struct PersistedWallpaper {
    int monitorId = 0;
    std::string type;
    std::string path;
};

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

    // Entries whose path is not valid UTF-8, which dump() throws on. Skipping one beats
    // failing the whole write; the count is what lets the daemon say a monitor is lost.
    int skippedNonUtf8 = 0;
};

// $XDG_CONFIG_HOME/wallpaperx/config.json, else $HOME/.config/... Empty when neither is
// usable: a relative XDG_CONFIG_HOME is ignored per the basedir spec, and there is no cwd
// fallback -- a daemon started from / would leave a config file there.
std::filesystem::path defaultConfigPath();

// Validates structure only; known types, id ranges and path readability are the
// WallpaperManager's rules. Neither load() nor save() throws -- a corrupt file must not
// stop the daemon starting, nor a failed write fail the SetWallpaper behind it.
class ConfigStore {
public:
    static constexpr int kSchemaVersion = 1;

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
