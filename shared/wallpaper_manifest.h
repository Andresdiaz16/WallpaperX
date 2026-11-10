// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The rules of a wallpaper's wallpaper.json, and the reading of a wallpaper folder: one definition for
// the app and wpxctl. Standard library and nlohmann only, since the backend links neither Qt Quick nor
// libzip. What needs those stays with the caller and is handed in: the files of a .wpx archive (a
// Contents of its own) and whether a video has sound (AudioOf).

#include "audio_streams.h"
#include "property_rules.h"
#include "unique_fd.h"
#include "wallpaper_position.h"

#include <cstddef>
#include <expected>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <sys/stat.h>

namespace wallpaperx::manifest {

inline constexpr const char* kManifestName = "wallpaper.json";
inline constexpr std::size_t kMaxManifestBytes = 1 << 20;
inline constexpr double kMaxVolume = 1.0;

inline constexpr const char* kSpeedKey = "speed";
// The keys of a video's own sound: offered for every video with sound, declared or not, and removed
// from a wallpaper known to have none.
inline constexpr const char* kMuteKey = "mute";
inline constexpr const char* kVolumeKey = "volume";
// Holds a video on its picture on the screen it is set for, whatever that screen's pause mode. Taken
// by every video without being declared; refused in wallpaper.json.
inline constexpr const char* kPausedKey = "paused";

// The keys every wallpaper takes besides its own; "position" and any "position.*" are refused in
// wallpaper.json.
inline constexpr std::string_view kPositionKeys[] = {kPositionKey, kPositionZoomKey, kPositionXKey, kPositionYKey};
bool isPositionKey(std::string_view key);
// A wallpaper's own properties, the sound keys added included: the daemon's cap less room for the
// position keys and "paused", which are sent with them.
inline constexpr std::size_t kMaxWallpaperProperties = kMaxProperties - std::size(kPositionKeys) - 1;

// A property value as wallpaper.json and the daemon carry it: a number, a bool, or text -- a
// "#rrggbb" colour or a position mode's name.
using Value = std::variant<double, bool, std::string>;

// One editable property a wallpaper declares.
struct Property {
    enum class Type { Float, Bool, Color };

    std::string key;
    std::string label;
    Type type = Type::Float;
    // A double, a bool, or a lower-case "#rrggbb", by `type`.
    Value fallback;
    // Float only; minimum < maximum.
    double minimum = 0.0;
    double maximum = 0.0;

    // "float", "bool" or "color".
    std::string_view typeName() const;
    // The only values a float may take, when it has a fixed few: for "speed", the playback speeds
    // inside its declared range. Empty for any other property.
    std::vector<double> choices() const;
    // `value` as this property stores it, or nothing when it cannot hold it: one of choices() for a
    // float that has them, else a finite number in range for a float, a bool for a bool, "#rrggbb"
    // (kept lower-case) for a colour.
    std::optional<Value> accept(const Value& value) const;

    friend bool operator==(const Property&, const Property&) = default;
};

// What a wallpaper.json says.
struct Manifest {
    // "static", "video" or "shader": the values SetWallpaper takes.
    std::string type;
    std::string title;
    std::string author;
    std::vector<std::string> tags;
    // Plain relative paths inside the wallpaper, not yet checked to name a file there.
    std::string previewName;
    std::string assetName;
    // In the order wallpaper.json lists them.
    std::vector<Property> properties;

    const Property* find(std::string_view key) const;
};

// The error is a sentence for the user.
std::expected<Manifest, std::string> parse(std::string_view json);

// Gives a video with sound its own sound controls, taking the declared ones where there are, and takes
// them from a wallpaper known to have none: they would set nothing anyone can hear. One whose sound is
// Unknown keeps them as declared. What is kept is held to the daemon's rule; the error is a sentence.
std::optional<std::string> settleSound(Manifest& wallpaper, AudioStreams audio);
// settleSound() by the sound `wallpaper` is taken to have: `videoAudio`'s answer for a video, asked of
// no other type, and Absent for the rest. Answers that sound, or why the sound keys are refused.
std::expected<AudioStreams, std::string> settleSoundFor(Manifest& wallpaper,
                                                        const std::function<AudioStreams()>& videoAudio);

// The "file" a wallpaper.json names, by parse()'s bounds and its rule for that path and nothing else
// checked, so also from one parse() refuses. Nothing when it names none that is a plain relative path.
std::optional<std::string> assetNameIn(std::string_view json);

// `value` as `key` keeps it on a wallpaper of `type`, or nothing when it is refused. `declared` is the
// wallpaper's property named `key`, or null. A position key takes a mode `type` is shown in, or a finite
// number within the key's bounds; a video takes kPausedKey as a bool; anything else only as declared.
std::optional<Value> acceptValue(std::string_view type, const Property* declared, std::string_view key,
                                 const Value& value);

// Refuses what could reach outside a folder: absolute, empty parts, ".", "..", backslashes, NUL.
bool isPlainRelativePath(std::string_view name);
bool isValidUtf8(std::string_view text);

// `errno` as the sentence fragment the system gives it.
std::string systemError(int error);

// ---- the files ------------------------------------------------------------------------------------

// Opens `name` below the folder `dirfd`, refusing with EXDEV a file that, links followed, is not
// inside it.
int openBeneath(int dirfd, const char* name, int flags);

// What `fd` holds from its offset to the end: EFBIG when that is more than `maxBytes`, else the errno
// of a failed read.
std::expected<std::string, int> readToEnd(int fd, std::size_t maxBytes);

struct FileRead {
    std::string bytes;
    struct stat st {};
};

struct FileFound {
    struct stat st {};
    // The file, open, in a folder; none in an archive.
    UniqueFd fd;
};

// The files of one wallpaper, wherever they are kept. Messages are sentences for the user.
class Contents {
public:
    virtual ~Contents() = default;
    virtual std::expected<FileRead, std::string> read(const std::string& name, std::size_t maxBytes) = 0;
    // The file wallpaper.json names as `name`.
    virtual std::expected<FileFound, std::string> find(const std::string& name) = 0;
};

// The files of the folder open as `dirfd`, which stays the caller's. Each must, its links followed, be
// a regular file inside it.
class FolderContents final : public Contents {
public:
    explicit FolderContents(int dirfd) : m_dir(dirfd) {}

    std::expected<FileRead, std::string> read(const std::string& name, std::size_t maxBytes) override;
    std::expected<FileFound, std::string> find(const std::string& name) override;

private:
    std::expected<UniqueFd, std::string> open(const std::string& name, std::string missing, struct stat& st);

    int m_dir;
};

// Why a wallpaper cannot be used, with wallpaper.json's stat when it was read.
struct Problem {
    std::string text;
    std::optional<struct stat> manifestStat;
};

struct Read {
    Manifest manifest;
    struct stat manifestStat {};
    struct stat previewStat {};
    // The asset; open when read from a folder.
    FileFound asset;
};

struct ManifestRead {
    Manifest manifest;
    struct stat st {};
};

// wallpaper.json alone, by readFrom()'s rules: what a wallpaper declares, the files it names unchecked.
std::expected<ManifestRead, Problem> readManifest(Contents& contents);

// The one validator for a folder and an archive: wallpaper.json, then every file it names. The sound
// keys are not judged: that needs the asset asked for sound, which readFolder() does.
std::expected<Read, Problem> readFrom(Contents& contents);

// Whether a video, read from its folder with its asset open, has sound.
using AudioOf = std::function<AudioStreams(const Read& video)>;

struct FolderWallpaper {
    Read read;
    // `path` + "/" + the asset's name: the file the daemon is to open.
    std::string assetPath;
    // Absent for a static or shader wallpaper, `audioOf`'s answer for a video.
    AudioStreams audio = AudioStreams::Unknown;
};

// The wallpaper folder `name` in `parent` (a descriptor, or AT_FDCWD), whose path is `path`. The folder
// itself must not be a link, its path must be UTF-8 (the daemon takes it as a D-Bus string), and the
// sound keys are settled by `audioOf`'s answer.
std::expected<FolderWallpaper, Problem> readFolder(int parent, const std::string& name, const std::string& path,
                                                   const AudioOf& audioOf);

} // namespace wallpaperx::manifest
