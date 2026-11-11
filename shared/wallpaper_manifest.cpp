// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaper_manifest.h"

#include "json_document.h"
#include "playback_speed.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <format>
#include <limits>
#include <span>
#include <system_error>

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace wallpaperx::manifest {

namespace {

using Json = nlohmann::ordered_json;

// `value` as JSON fit to quote in a sentence: a number, or a short string of printable ASCII. Never
// anything else: dump() recurses once per level of nesting, and a string may be any length.
std::optional<std::string> quotable(const Json& value) {
    constexpr std::size_t kMaxQuotedBytes = 32;
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() > kMaxQuotedBytes || !std::ranges::all_of(text, [](char c) { return c >= 0x20 && c < 0x7f; })) {
            return std::nullopt;
        }
    } else if (!value.is_number()) {
        return std::nullopt;
    }
    return value.dump();
}

// A number as the app's sentences have always written it: printf's %g.
std::string number(double value) { return std::format("{:g}", value); }

std::string propertyProblem(std::string_view key, std::string_view what) {
    return std::format("Property \"{}\" in wallpaper.json {}", key, what);
}

std::expected<Property, std::string> parseProperty(const std::string& key, const Json& spec) {
    const auto refuse = [&key](std::string_view what) { return std::unexpected(propertyProblem(key, what)); };
    // Not quoted back: it can hold anything, a bidirectional override included.
    if (!isPropertyKey(key)) {
        return std::unexpected(std::format("wallpaper.json has a property whose name is not 1 to {} letters, "
                                           "digits, \"_\", \"-\" or \".\".",
                                           kMaxPropertyKeyBytes));
    }
    if (!spec.is_object()) return refuse("is not an object.");
    // The daemon reads these as the wallpaper's position, which is set beside the wallpaper's own.
    if (key == kPositionKey || key.starts_with(std::string(kPositionKey) + '.')) {
        return refuse("is reserved: WallpaperX sets a wallpaper's position itself.");
    }
    if (key == kPausedKey) return refuse("is reserved: WallpaperX pauses a wallpaper itself.");

    Property property;
    property.key = key;
    property.label = key;
    if (const auto label = spec.find("label"); label != spec.end()) {
        if (!label->is_string()) return refuse("has a label that is not text.");
        property.label = label->get<std::string>();
    }

    const auto type = spec.find("type");
    if (type == spec.end() || !type->is_string()) return refuse("has no type.");
    const std::string& typeName = type->get_ref<const std::string&>();
    if (typeName == "float") {
        property.type = Property::Type::Float;
        const auto min = spec.find("min");
        const auto max = spec.find("max");
        if (min == spec.end() || !min->is_number() || max == spec.end() || !max->is_number()) {
            return refuse("needs a numeric min and max.");
        }
        property.minimum = min->get<double>();
        property.maximum = max->get<double>();
        if (!std::isfinite(property.minimum) || !std::isfinite(property.maximum) ||
            !(property.minimum < property.maximum)) {
            return refuse("has a min that is not below its max.");
        }
    } else if (typeName == "bool") {
        property.type = Property::Type::Bool;
    } else if (typeName == "color") {
        property.type = Property::Type::Color;
    } else if (const auto quoted = quotable(*type)) {
        return refuse(std::format("has type {}, which is not float, bool or color.", *quoted));
    } else {
        return refuse("has a type that is not float, bool or color.");
    }

    const auto fallback = spec.find("default");
    if (fallback == spec.end()) return refuse("has no default.");
    // Anything else is held by nothing, and refused below.
    std::optional<Value> value;
    if (fallback->is_number()) {
        value = fallback->get<double>();
    } else if (fallback->is_boolean()) {
        value = fallback->get<bool>();
    } else if (fallback->is_string()) {
        value = fallback->get<std::string>();
    }

    // A key the daemon gives a meaning. Its declared range picks which of the playback speeds are
    // offered, and a default between them is moved to the nearest offered by the daemon's own rule.
    // The sound keys are checked in settleSound(), which knows whether they are kept.
    if (key == kSpeedKey) {
        if (property.type != Property::Type::Float) {
            return refuse("is not a float, and WallpaperX reads a video's speed as one.");
        }
        const std::vector<double> offered = property.choices();
        if (offered.empty()) {
            return refuse(std::format("allows {} to {}, and WallpaperX plays video at {} times its speed.",
                                      number(property.minimum), number(property.maximum), playbackSpeedsText("or")));
        }
        if (const double* given = value ? std::get_if<double>(&*value) : nullptr;
            given && *given >= property.minimum && *given <= property.maximum) {
            value = nearestPlaybackSpeed(*given, offered);
        }
    }
    const std::optional<Value> accepted = value ? property.accept(*value) : std::nullopt;
    if (!accepted) {
        return refuse(property.type == Property::Type::Float
                          ? std::string("has a default outside its min and max.")
                          : std::format("has a default that is not a {}.", property.typeName()));
    }
    property.fallback = *accepted;
    return property;
}

// Why the daemon would refuse a declared `mute` or `volume`, for the same reason as `speed`.
std::optional<std::string> soundPropertyProblem(const Property& property) {
    if (property.key == kMuteKey && property.type != Property::Type::Bool) {
        return propertyProblem(property.key, "is not a bool, and WallpaperX reads \"mute\" as one.");
    }
    if (property.key != kVolumeKey) return std::nullopt;
    if (property.type != Property::Type::Float) {
        return propertyProblem(property.key, "is not a float, and WallpaperX reads a video's volume as one.");
    }
    if (property.minimum < 0.0 || property.maximum > kMaxVolume) {
        return propertyProblem(property.key,
                               std::format("allows {} to {}, and WallpaperX plays a video's volume from 0 to {}.",
                                           number(property.minimum), number(property.maximum), number(kMaxVolume)));
    }
    return std::nullopt;
}

// Where /proc says `fd` is open; empty when it cannot tell.
std::string openedPath(int fd) {
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    std::string target(PATH_MAX, '\0');
    const ssize_t length = ::readlink(link.c_str(), target.data(), target.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= target.size()) return {};
    target.resize(static_cast<std::size_t>(length));
    return target;
}

} // namespace

bool isPositionKey(std::string_view key) { return std::ranges::find(kPositionKeys, key) != std::end(kPositionKeys); }

std::string_view Property::typeName() const {
    switch (type) {
    case Type::Float:
        return "float";
    case Type::Bool:
        return "bool";
    case Type::Color:
        return "color";
    }
    return {};
}

std::vector<double> Property::choices() const {
    std::vector<double> within;
    if (key != kSpeedKey || type != Type::Float) return within;
    for (const double speed : kPlaybackSpeeds) {
        if (speed >= minimum && speed <= maximum) within.push_back(speed);
    }
    return within;
}

std::optional<Value> Property::accept(const Value& value) const {
    switch (type) {
    case Type::Float: {
        const double* number = std::get_if<double>(&value);
        if (number == nullptr || !std::isfinite(*number) || *number < minimum || *number > maximum) {
            return std::nullopt;
        }
        if (const std::vector<double> offered = choices();
            !offered.empty() && std::ranges::find(offered, *number) == offered.end()) {
            return std::nullopt;
        }
        return Value(*number);
    }
    case Type::Bool:
        return std::holds_alternative<bool>(value) ? std::optional<Value>(value) : std::nullopt;
    case Type::Color: {
        const std::string* text = std::get_if<std::string>(&value);
        const std::optional<Colour> colour = text ? parseColour(*text) : std::nullopt;
        return colour ? std::optional(Value(toString(*colour))) : std::nullopt;
    }
    }
    return std::nullopt;
}

const Property* Manifest::find(std::string_view key) const {
    const auto it = std::ranges::find(properties, key, &Property::key);
    return it == properties.end() ? nullptr : &*it;
}

std::expected<Manifest, std::string> parse(std::string_view json) {
    auto parsed = parseJsonDocument(json, kManifestName);
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    const Json& doc = *parsed;
    if (!doc.is_object()) return std::unexpected("wallpaper.json is not a JSON object.");

    const auto version = doc.find("schema_version");
    if (version == doc.end()) return std::unexpected("wallpaper.json has no schema_version.");
    const bool versionOne = (version->is_string() && version->get_ref<const std::string&>() == "1") ||
                            (version->is_number_integer() && version->get<long long>() == 1);
    if (!versionOne) {
        const auto quoted = quotable(*version);
        return std::unexpected(
            quoted ? std::format("wallpaper.json is for schema version {}, which this WallpaperX does not read.", *quoted)
                   : std::string("wallpaper.json is for a schema version this WallpaperX does not read."));
    }

    // Each is required, a string, and non-empty.
    const auto text = [&doc](const char* key) -> std::optional<std::string> {
        const auto it = doc.find(key);
        if (it == doc.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) return std::nullopt;
        return it->get<std::string>();
    };

    Manifest wallpaper;
    const auto type = text("type");
    if (!type || (*type != "static" && *type != "video" && *type != "shader")) {
        return std::unexpected("wallpaper.json needs a type of static, video or shader.");
    }
    wallpaper.type = *type;

    const auto title = text("title");
    if (!title) return std::unexpected("wallpaper.json has no title.");
    wallpaper.title = *title;

    if (const auto author = doc.find("author"); author != doc.end()) {
        if (!author->is_string()) return std::unexpected("wallpaper.json has an author that is not text.");
        wallpaper.author = author->get<std::string>();
    }

    for (const char* key : {"preview", "file"}) {
        const auto name = text(key);
        if (!name) return std::unexpected(std::format("wallpaper.json has no \"{}\".", key));
        if (!isPlainRelativePath(*name)) {
            return std::unexpected(std::format("wallpaper.json's \"{}\" is not a plain path inside the wallpaper.", key));
        }
        (std::string_view(key) == "preview" ? wallpaper.previewName : wallpaper.assetName) = *name;
    }

    if (const auto tags = doc.find("tags"); tags != doc.end()) {
        if (!tags->is_array() || !std::ranges::all_of(*tags, &Json::is_string)) {
            return std::unexpected("wallpaper.json's tags are not a list of text.");
        }
        for (const Json& tag : *tags) wallpaper.tags.push_back(tag.get<std::string>());
    }

    if (const auto properties = doc.find("properties"); properties != doc.end()) {
        if (!properties->is_object()) return std::unexpected("wallpaper.json's properties are not an object.");
        if (properties->size() > kMaxWallpaperProperties) {
            return std::unexpected(std::format("wallpaper.json has {} properties; WallpaperX takes at most {}.",
                                               properties->size(), kMaxWallpaperProperties));
        }
        for (const auto& [key, spec] : properties->items()) {
            auto property = parseProperty(key, spec);
            if (!property) return std::unexpected(property.error());
            wallpaper.properties.push_back(std::move(*property));
        }
    }
    return wallpaper;
}

std::optional<std::string> assetNameIn(std::string_view json) {
    const auto doc = parseJsonDocument(json, kManifestName);
    if (!doc) return std::nullopt;
    const auto file = doc->find("file");
    if (file == doc->end() || !file->is_string() || !isPlainRelativePath(file->get_ref<const std::string&>())) {
        return std::nullopt;
    }
    return file->get<std::string>();
}

std::expected<AudioStreams, std::string> settleSoundFor(Manifest& wallpaper,
                                                        const std::function<AudioStreams()>& videoAudio) {
    const AudioStreams audio = wallpaper.type == "video" ? videoAudio() : AudioStreams::Absent;
    if (auto problem = settleSound(wallpaper, audio)) return std::unexpected(std::move(*problem));
    return audio;
}

std::optional<std::string> settleSound(Manifest& wallpaper, AudioStreams audio) {
    std::vector<Property>& properties = wallpaper.properties;
    if (audio == AudioStreams::Absent) {
        std::erase_if(properties, [](const Property& p) { return p.key == kMuteKey || p.key == kVolumeKey; });
        return std::nullopt;
    }
    for (const Property& property : properties) {
        if (auto problem = soundPropertyProblem(property)) return problem;
    }
    if (audio == AudioStreams::Unknown) return std::nullopt;
    // Taken as declared once the cap is reached: one more would cost the wallpaper its every set.
    const auto declared = [&properties](const char* key) {
        return properties.size() >= kMaxWallpaperProperties || std::ranges::contains(properties, key, &Property::key);
    };
    if (!declared(kMuteKey)) {
        Property mute;
        mute.key = mute.label = kMuteKey;
        mute.type = Property::Type::Bool;
        mute.fallback = false;
        properties.push_back(std::move(mute));
    }
    if (!declared(kVolumeKey)) {
        Property volume;
        volume.key = volume.label = kVolumeKey;
        volume.type = Property::Type::Float;
        volume.fallback = kMaxVolume;
        volume.minimum = 0.0;
        volume.maximum = kMaxVolume;
        properties.push_back(std::move(volume));
    }
    return std::nullopt;
}

std::optional<Value> acceptValue(std::string_view type, const Property* declared, std::string_view key,
                                 const Value& value) {
    if (key == kPausedKey) {
        return type == "video" && std::holds_alternative<bool>(value) ? std::optional(value) : std::nullopt;
    }
    if (!isPositionKey(key)) return declared ? declared->accept(value) : std::nullopt;
    if (key == kPositionKey) {
        const std::string* name = std::get_if<std::string>(&value);
        const std::optional<PositionMode> mode = name ? positionModeFromName(*name) : std::nullopt;
        return mode && positionModeAllowed(type, *mode) ? std::optional(value) : std::nullopt;
    }
    const double* number = std::get_if<double>(&value);
    const bool allowed = number && (key == kPositionZoomKey ? positionZoomAllowed(*number) : positionOffsetAllowed(*number));
    return allowed ? std::optional(value) : std::nullopt;
}

bool isPlainRelativePath(std::string_view name) {
    if (name.empty() || name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return false;
    }
    for (std::size_t from = 0;;) {
        const std::size_t slash = name.find('/', from);
        const std::string_view part = name.substr(from, slash == std::string_view::npos ? slash : slash - from);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string_view::npos) return true;
        from = slash + 1;
    }
}

bool isValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        char32_t code = 0;
        char32_t least = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2, code = lead & 0x1F, least = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3, code = lead & 0x0F, least = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4, code = lead & 0x07, least = 0x10000;
        } else {
            return false;
        }
        if (text.size() - i < length) return false;
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        // Overlong forms, UTF-16 surrogates and anything past U+10FFFF are not UTF-8.
        if (code < least || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) return false;
        i += length;
    }
    return true;
}

std::string systemError(int error) { return std::error_code(error, std::generic_category()).message(); }

int openBeneath(int dirfd, const char* name, int flags) {
    open_how how{};
    how.flags = static_cast<std::uint64_t>(flags | O_CLOEXEC);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    const int fd = static_cast<int>(::syscall(SYS_openat2, dirfd, name, &how, sizeof how));
    // EXDEV also refuses an absolute link, or a ".." out and back in, that lands inside; ENOSYS is a
    // kernel before 5.6, and a sandbox filtering openat2 answers ENOSYS or EPERM.
    if (fd >= 0 || (errno != EXDEV && errno != ENOSYS && errno != EPERM)) return fd;
    // Found with O_PATH, which opens nothing, then checked, then opened through its own descriptor: a
    // device or FIFO outside the folder is never opened, even to be refused.
    const UniqueFd found(::openat(dirfd, name, O_PATH | O_CLOEXEC));
    if (!found) return -1;
    const std::string folder = openedPath(dirfd);
    const std::string file = openedPath(found.get());
    if (folder.empty() || !file.starts_with(folder == "/" ? folder : folder + '/')) {
        errno = EXDEV;
        return -1;
    }
    return ::open(("/proc/self/fd/" + std::to_string(found.get())).c_str(), flags | O_CLOEXEC);
}

std::expected<std::string, int> readToEnd(int fd, std::size_t maxBytes) {
    // The "one byte over" below must not wrap; no string holds this many bytes anyway.
    maxBytes = std::min(maxBytes, std::numeric_limits<std::size_t>::max() - 1);
    struct stat st {};
    const std::size_t size = ::fstat(fd, &st) == 0 && st.st_size > 0 ? static_cast<std::size_t>(st.st_size) : 0;
    if (size > maxBytes) return std::unexpected(EFBIG);
    // To the end, not to st_size: the file may be growing. One byte over says it is too large.
    std::string bytes(size + 1, '\0');
    std::size_t done = 0;
    for (;;) {
        if (done == bytes.size()) {
            if (bytes.size() > maxBytes) return std::unexpected(EFBIG);
            bytes.resize(std::min(bytes.size() * 2, maxBytes + 1));
        }
        const ssize_t got = ::read(fd, bytes.data() + done, bytes.size() - done);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return std::unexpected(errno);
        if (got == 0) break;
        done += static_cast<std::size_t>(got);
    }
    bytes.resize(done);
    return bytes;
}

std::expected<UniqueFd, std::string> FolderContents::open(const std::string& name, std::string missing,
                                                          struct stat& st) {
    // O_NONBLOCK: a FIFO under the name is refused below instead of blocking the open.
    UniqueFd fd(openBeneath(m_dir, name.c_str(), O_RDONLY | O_NONBLOCK | O_NOCTTY));
    if (!fd) {
        switch (const int error = errno) {
        case ENOENT:
        case ENOTDIR:
            return std::unexpected(std::move(missing));
        case EXDEV:
            return std::unexpected(name + " leads outside the wallpaper's folder.");
        default:
            return std::unexpected(name + " cannot be opened: " + systemError(error) + ".");
        }
    }
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) return std::unexpected(name + " is not a regular file.");
    return fd;
}

std::expected<FileRead, std::string> FolderContents::read(const std::string& name, std::size_t maxBytes) {
    FileRead file;
    auto fd = open(name, "The folder has no " + name + ".", file.st);
    if (!fd) return std::unexpected(fd.error());
    auto bytes = readToEnd(fd->get(), maxBytes);
    if (!bytes) {
        return std::unexpected(bytes.error() == EFBIG
                                   ? std::format("{} is larger than {} MiB.", name, maxBytes >> 20)
                                   : name + " cannot be read: " + systemError(bytes.error()) + ".");
    }
    file.bytes = std::move(*bytes);
    return file;
}

std::expected<FileFound, std::string> FolderContents::find(const std::string& name) {
    FileFound file;
    auto fd = open(name, "The folder has no " + name + ", which " + m_manifestName + " names.", file.st);
    if (!fd) return std::unexpected(fd.error());
    file.fd = std::move(*fd);
    return file;
}

std::expected<ManifestRead, Problem> readManifest(Contents& contents) {
    auto manifest = contents.read(kManifestName, kMaxManifestBytes);
    if (!manifest) return std::unexpected(Problem{manifest.error(), std::nullopt});
    auto parsed = parse(manifest->bytes);
    if (!parsed) return std::unexpected(Problem{parsed.error(), manifest->st});
    return ManifestRead{std::move(*parsed), manifest->st};
}

std::expected<Read, Problem> readFrom(Contents& contents) {
    auto manifest = readManifest(contents);
    if (!manifest) return std::unexpected(std::move(manifest.error()));
    const struct stat manifestStat = manifest->st;
    const auto refuse = [&manifestStat](std::string text) { return std::unexpected(Problem{std::move(text), manifestStat}); };
    const auto preview = contents.find(manifest->manifest.previewName);
    if (!preview) return refuse(preview.error());
    auto asset = contents.find(manifest->manifest.assetName);
    if (!asset) return refuse(asset.error());
    return Read{std::move(manifest->manifest), manifestStat, preview->st, std::move(*asset)};
}

std::expected<FolderWallpaper, Problem> readFolder(int parent, const std::string& name, const std::string& path,
                                                   const AudioOf& audioOf) {
    if (!isValidUtf8(path)) {
        return std::unexpected(
            Problem{"Its path is not valid UTF-8, so it cannot be set as a wallpaper. Rename the folder.", std::nullopt});
    }
    const UniqueFd dir(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!dir) {
        const int error = errno;
        struct stat link {};
        if (::fstatat(parent, name.c_str(), &link, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(link.st_mode)) {
            return std::unexpected(Problem{"It is a link, not a folder.", std::nullopt});
        }
        return std::unexpected(Problem{error == ENOTDIR ? std::string("It is not a folder.")
                                                        : "It cannot be opened: " + systemError(error) + ".",
                                       std::nullopt});
    }
    FolderContents contents(dir.get());
    auto read = readFrom(contents);
    if (!read) return std::unexpected(std::move(read.error()));
    std::string assetPath = path + '/' + read->manifest.assetName;
    FolderWallpaper wallpaper{std::move(*read), std::move(assetPath)};
    const auto audio = settleSoundFor(wallpaper.read.manifest, [&wallpaper, &audioOf] { return audioOf(wallpaper.read); });
    if (!audio) return std::unexpected(Problem{audio.error(), wallpaper.read.manifestStat});
    wallpaper.audio = *audio;
    return wallpaper;
}

} // namespace wallpaperx::manifest
