// SPDX-License-Identifier: GPL-2.0-or-later

#include "config_store.h"

#include "log_escape.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wallpaperx {
namespace {

constexpr const char* kAppDir = "wallpaperx";
constexpr const char* kFileName = "config.json";

// A 32-monitor config is about 4 kB. The cap is not about legitimate files: it bounds how
// long startup can spend, and how much it can allocate, on a file someone else wrote.
constexpr std::uintmax_t kMaxBytes = ConfigStore::kMaxBytes;

// Every field name in one place: a typo in the reader that matches a typo in the writer is
// invisible until someone else's build reads the file.
constexpr const char* kKeyVersion = "version";
constexpr const char* kKeyMonitors = "monitors";
constexpr const char* kKeyMonitorId = "monitor_id";
constexpr const char* kKeyType = "type";
constexpr const char* kKeyPath = "path";
constexpr const char* kKeyOutput = "output";
constexpr const char* kKeyOutputSize = "output_size";
constexpr const char* kKeyWidth = "width";
constexpr const char* kKeyHeight = "height";
constexpr const char* kKeyProperties = "properties";
constexpr const char* kKeyPause = "pause";
constexpr const char* kKeySound = "sound";

// load() returns from a dozen places; this is what keeps the descriptor from leaking out
// of any of them.
class Fd {
public:
    explicit Fd(int fd) : m_fd(fd) {}
    ~Fd() {
        if (m_fd >= 0) ::close(m_fd);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return m_fd; }

    // For the one close that must be checked rather than silent: a full disk surfaces
    // there, not at write().
    int release() {
        const int fd = m_fd;
        m_fd = -1;
        return fd;
    }

private:
    int m_fd;
};

// Raw: the only consumer records these into the health fields, which sanitize once. Escaping
// here too would double every backslash in the path by the time anyone reads it.
std::string describe(const std::filesystem::path& file, const std::string& what) {
    return file.string() + ": " + what;
}

// Reads a string field only if present AND actually a string. contains() + is_string() rather
// than value() or operator[]: a const operator[] on a missing key is a JSON_ASSERT, which
// aborts rather than throwing, and value() throws type_error on a null. A hand-edited file
// reaches both, so neither may appear anywhere in this file.
bool readString(const nlohmann::json& object, const char* key, std::string& out) {
    if (!object.contains(key)) return false;
    const nlohmann::json& field = object.at(key);
    if (!field.is_string()) return false;
    out = field.get<std::string>();
    return true;
}

// is_number_integer() is false for 1.5 and for true, but true for any width -- and
// get<int>() narrows silently, so 2^32 + 1 arrives as 1 and passes whatever range check
// the caller applies afterwards. The width has to be rejected here, before the narrowing.
bool readInt(const nlohmann::json& field, int& out) {
    if (!field.is_number_integer()) return false;
    if (field.is_number_unsigned()) {
        const std::uint64_t raw = field.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) return false;
        out = static_cast<int>(raw);
        return true;
    }
    const std::int64_t raw = field.get<std::int64_t>();
    if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(raw);
    return true;
}

bool readMonitorId(const nlohmann::json& object, int& out) {
    if (!object.contains(kKeyMonitorId)) return false;
    return readInt(object.at(kKeyMonitorId), out);
}

// A size is a measurement the screen sends again, so an unreadable one never costs the row it
// rides on: it comes back 0x0 and the manager refuses and reports it.
OutputSize readOutputSize(const nlohmann::json& field) {
    int width = 0;
    int height = 0;
    if (!field.is_object() || !field.contains(kKeyWidth) || !field.contains(kKeyHeight) ||
        !readInt(field.at(kKeyWidth), width) || !readInt(field.at(kKeyHeight), height) || width < 0 ||
        height < 0) {
        return {};
    }
    return OutputSize{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

}  // namespace

// No nlohmann::json: destroying a non-empty one allocates inside its noexcept destructor, and
// this runs on every manager call that saves, where an allocation failure must stay a
// std::bad_alloc rather than std::terminate. Keys in the order dump() sorts them.
std::string toJsonText(const OutputSize& size) {
    return std::string("{\"") + kKeyHeight + "\":" + std::to_string(size.height) + ",\"" + kKeyWidth +
           "\":" + std::to_string(size.width) + "}";
}

OutputSize outputSizeFromJson(const std::string& text) {
    const nlohmann::json field = nlohmann::json::parse(text, nullptr, false);
    return field.is_discarded() ? OutputSize{} : readOutputSize(field);
}

std::filesystem::path defaultConfigPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        const std::filesystem::path base(xdg);
        // The spec says a relative value must be ignored, not resolved against the cwd.
        if (base.is_absolute()) return base / kAppDir / kFileName;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        const std::filesystem::path base(home);
        if (base.is_absolute()) return base / ".config" / kAppDir / kFileName;
    }
    return {};
}

std::filesystem::path defaultStateDir() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
        const std::filesystem::path base(xdg);
        if (base.is_absolute()) return base / kAppDir;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        const std::filesystem::path base(home);
        if (base.is_absolute()) return base / ".local" / "state" / kAppDir;
    }
    return {};
}

ConfigStore::ConfigStore(std::filesystem::path file) : m_file(std::move(file)) {}

LoadResult ConfigStore::load() const {
    LoadResult result;
    if (!enabled()) return result;

    // One catch around the whole body. Every individual step below is written not to
    // throw, but this is the function whose caller is a constructor running inside
    // main()'s try -- anything escaping here stops the daemon booting because of a text
    // file, so the guarantee is structural rather than a matter of having checked.
    try {
        // Opened once and inspected through the descriptor. Checking the path and then
        // opening it are two lookups of a name anything can rebind in between, so a FIFO
        // swapped in after a passing check used to be openable. O_NONBLOCK is what makes
        // opening safe here: on a FIFO it returns immediately instead of awaiting a writer.
        const Fd fd(::open(m_file.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (fd.get() < 0) {
            if (errno == ENOENT) return result;
            // "No config yet" and "could not tell" are different answers. Folding them
            // together reported a healthy daemon that had silently restored nothing.
            result.error = describe(m_file, std::string("could not be opened: ") + std::strerror(errno));
            return result;
        }
        result.fileExisted = true;

        struct stat info {};
        if (::fstat(fd.get(), &info) != 0) {
            result.error = describe(m_file, std::string("could not be checked: ") + std::strerror(errno));
            return result;
        }
        if (!S_ISREG(info.st_mode)) {
            result.error = describe(m_file, "is not a regular file");
            return result;
        }
        const std::uintmax_t size = static_cast<std::uintmax_t>(info.st_size);
        if (size > kMaxBytes) {
            // Unusable on a property of the file itself, so moving it aside is safe even
            // though none of it was read.
            result.preservable = true;
            result.error = describe(m_file, "is " + std::to_string(size) + " bytes, over the " +
                                                std::to_string(kMaxBytes) + " byte limit");
            return result;
        }

        // Read to a byte past the cap so a file that grew since the fstat is rejected
        // rather than read unbounded.
        std::string text;
        char buffer[8192];
        for (;;) {
            const ssize_t n = ::read(fd.get(), buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) {
                result.error = describe(m_file, std::string("could not be read: ") + std::strerror(errno));
                return result;
            }
            if (n == 0) break;
            text.append(buffer, static_cast<std::size_t>(n));
            if (text.size() > kMaxBytes) {
                result.preservable = true;
                result.error = describe(m_file, "is over the " + std::to_string(kMaxBytes) + " byte limit");
                return result;
            }
        }

        // Only here: every rejection past this point is a statement about bytes we hold.
        result.preservable = true;

        // nlohmann's lexer treats NUL as end of input, so `{...}\0junk` parses clean and
        // the tail is silently dropped -- the file the daemon believes it read is then not
        // the file on disk.
        if (text.find('\0') != std::string::npos) {
            result.error = describe(m_file, "contains a NUL byte");
            return result;
        }

        // allow_exceptions=false: a parse failure is a value here, not a throw.
        const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
        if (root.is_discarded()) {
            result.error = describe(m_file, "is not valid JSON");
            return result;
        }
        if (!root.is_object()) {
            result.error = describe(m_file, "is not a JSON object");
            return result;
        }

        int version = 0;
        if (!root.contains(kKeyVersion) || !readInt(root.at(kKeyVersion), version) ||
            version != kSchemaVersion) {
            result.error = describe(m_file, "has an unrecognised schema version");
            return result;
        }
        if (!root.contains(kKeyMonitors) || !root.at(kKeyMonitors).is_array()) {
            result.error = describe(m_file, "has no monitors array");
            return result;
        }
        // Not judged here, like pause below: see PersistedWallpaper::propertiesJson.
        if (root.contains(kKeySound)) result.soundJson = root.at(kKeySound).dump();

        for (const nlohmann::json& element : root.at(kKeyMonitors)) {
            PersistedWallpaper entry;
            if (!element.is_object() || !readMonitorId(element, entry.monitorId) ||
                !readString(element, kKeyType, entry.type) ||
                !readString(element, kKeyPath, entry.path)) {
                ++result.malformedEntries;
                continue;
            }
            // Optional, unlike the three above: a config written before connector names
            // existed has no such key and is not malformed. Present but not a string is.
            if (element.contains(kKeyOutput) && !readString(element, kKeyOutput, entry.output)) {
                ++result.malformedEntries;
                continue;
            }
            // Optional, and like properties below, not judged here: see PersistedWallpaper::propertiesJson.
            if (element.contains(kKeyOutputSize)) entry.outputSizeJson = element.at(kKeyOutputSize).dump();
            // A config written before properties existed has none.
            if (element.contains(kKeyProperties)) entry.propertiesJson = element.at(kKeyProperties).dump();
            if (element.contains(kKeyPause)) entry.pauseJson = element.at(kKeyPause).dump();
            if (element.contains(kKeySound)) entry.soundJson = element.at(kKeySound).dump();
            result.entries.push_back(std::move(entry));
        }
    } catch (const std::exception& e) {
        result.entries.clear();
        result.soundJson.clear();
        result.error = describe(m_file, std::string("could not be read: ") + e.what());
    } catch (...) {
        result.entries.clear();
        result.soundJson.clear();
        result.error = describe(m_file, "could not be read");
    }
    return result;
}

SaveResult ConfigStore::save(const PersistedConfig& config) const {
    SaveResult result;
    if (!enabled()) return result;

    const std::filesystem::path temp = m_file.string() + ".tmp";
    try {
        nlohmann::json monitors = nlohmann::json::array();
        for (const PersistedWallpaper& entry : config.monitors) {
            // dump() throws on invalid UTF-8 and Linux paths are arbitrary bytes, so a
            // single unusual filename would otherwise cost every monitor its wallpaper.
            // Dropping just this entry keeps the rest, and the count tells the caller a
            // monitor will not come back.
            //
            // Half by half: the two are independently valid, and one gate over both cost a
            // screen its name because the picture on it had an unusual filename -- leaving
            // that screen anonymous after a restart, which no renaming of the image undoes.
            const bool wallpaperOk = isValidUtf8(entry.path) && isValidUtf8(entry.type);
            const bool nameOk = isValidUtf8(entry.output);
            if (!wallpaperOk && (!entry.type.empty() || !entry.path.empty())) {
                ++result.skippedNonUtf8;
            }
            // Counted wherever the name is lost, not only on rows that survive: a name this
            // file cannot carry leaves the screen anonymous either way.
            if (!nameOk && !entry.output.empty()) ++result.skippedNames;
            // Nothing left worth a row.
            if (!wallpaperOk && (!nameOk || entry.output.empty()) && !entry.holdsMonitorSettings()) continue;

            // Both keys stay present whatever survives: load() reads a row missing either one
            // as malformed, which would turn a degraded write into a lost row at the next start.
            nlohmann::json row{{kKeyMonitorId, entry.monitorId},
                               {kKeyType, wallpaperOk ? entry.type : ""},
                               {kKeyPath, wallpaperOk ? entry.path : ""}};
            // Only beside a wallpaper they belong to, and only when there are some, for the reason
            // `output` below gives.
            if (wallpaperOk && (!entry.type.empty() || !entry.path.empty()) && !entry.propertiesJson.empty()) {
                // Every caller passes text dump() produced. Anything else costs this field, not the
                // whole write.
                nlohmann::json properties = nlohmann::json::parse(entry.propertiesJson, nullptr, false);
                if (!properties.is_discarded()) row[kKeyProperties] = std::move(properties);
            }
            // Only when there is one: a config for an install that never named a screen
            // stays byte-identical to what a build without this field writes.
            if (nameOk && !entry.output.empty()) {
                row[kKeyOutput] = entry.output;
                if (!entry.outputSizeJson.empty()) {
                    // Same contract as properties above.
                    nlohmann::json size = nlohmann::json::parse(entry.outputSizeJson, nullptr, false);
                    if (!size.is_discarded()) row[kKeyOutputSize] = std::move(size);
                }
            }
            // Beside anything or nothing: it belongs to the monitor id. Absent means play, so a config
            // that never set one is byte-identical to what a build without this field writes.
            if (!entry.pauseJson.empty()) {
                nlohmann::json pause = nlohmann::json::parse(entry.pauseJson, nullptr, false);
                if (!pause.is_discarded()) row[kKeyPause] = std::move(pause);
            }
            // The same: absent means on.
            if (!entry.soundJson.empty()) {
                nlohmann::json sound = nlohmann::json::parse(entry.soundJson, nullptr, false);
                if (!sound.is_discarded()) row[kKeySound] = std::move(sound);
            }
            monitors.push_back(std::move(row));
        }
        nlohmann::json root{{kKeyVersion, kSchemaVersion}, {kKeyMonitors, std::move(monitors)}};
        if (!config.soundJson.empty()) {
            nlohmann::json sound = nlohmann::json::parse(config.soundJson, nullptr, false);
            if (!sound.is_discarded()) root[kKeySound] = std::move(sound);
        }
        const std::string text = root.dump(2) + "\n";

        constexpr int kFlags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW;
        int fd = ::open(temp.c_str(), kFlags, 0644);
        // Only on the first save and after someone removes the directory, and only for the
        // errno that says so: any other failure is about the file, not the directory.
        if (fd < 0 && errno == ENOENT) {
            std::error_code ec;
            std::filesystem::create_directories(m_file.parent_path(), ec);
            if (ec) {
                result.error = describe(m_file.parent_path(), "could not be created: " + ec.message());
                return result;
            }
            fd = ::open(temp.c_str(), kFlags, 0644);
        }
        if (fd < 0) {
            result.error =
                describe(temp, std::string("could not be opened for writing: ") + std::strerror(errno));
            return result;
        }
        // Owned from here so the error returns below, and anything that throws between
        // here and the close, cannot leak it.
        Fd out(fd);

        size_t written = 0;
        while (written < text.size()) {
            const ssize_t n = ::write(out.get(), text.data() + written, text.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                const std::string why = n < 0 ? std::strerror(errno) : "short write";
                ::unlink(temp.c_str());
                result.error = describe(temp, "could not be written: " + why);
                return result;
            }
            written += static_cast<size_t>(n);
        }

        // Checked, not just called: a full disk surfaces here rather than at write().
        if (::close(out.release()) != 0) {
            const std::string why = std::strerror(errno);
            ::unlink(temp.c_str());
            result.error = describe(temp, "could not be written: " + why);
            return result;
        }

        // Atomic within the directory: a reader sees either the old file or the new one,
        // never a half-written one.
        if (::rename(temp.c_str(), m_file.c_str()) != 0) {
            const std::string why = std::strerror(errno);
            ::unlink(temp.c_str());
            result.error = describe(m_file, "could not be replaced: " + why);
            return result;
        }
    } catch (const std::exception& e) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        result.error = describe(m_file, std::string("could not be saved: ") + e.what());
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        result.error = describe(m_file, "could not be saved");
    }
    return result;
}

}  // namespace wallpaperx
