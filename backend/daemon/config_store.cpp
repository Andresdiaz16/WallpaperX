#include "config_store.h"

#include "log_escape.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace wallpaperx {
namespace {

constexpr const char* kAppDir = "wallpaperx";
constexpr const char* kFileName = "config.json";

// Every field name in one place: a typo in the reader that matches a typo in the writer is
// invisible until someone else's build reads the file.
constexpr const char* kKeyVersion = "version";
constexpr const char* kKeyMonitors = "monitors";
constexpr const char* kKeyMonitorId = "monitor_id";
constexpr const char* kKeyType = "type";
constexpr const char* kKeyPath = "path";

std::string describe(const std::filesystem::path& file, const std::string& what) {
    return sanitizeForLog(file.string(), 4096, LogFraming::Bare) + ": " + what;
}

// Reads a string field only if it is present AND actually a string.
//
// contains() + is_string() rather than value() or operator[]: a const operator[] on a
// missing key is a JSON_ASSERT, which aborts the process rather than throwing anything a
// caller could catch, and value() throws type_error on a null. Both are reachable from a
// hand-edited file, so neither may appear anywhere in this file.
bool readString(const nlohmann::json& object, const char* key, std::string& out) {
    if (!object.contains(key)) return false;
    const nlohmann::json& field = object.at(key);
    if (!field.is_string()) return false;
    out = field.get<std::string>();
    return true;
}

bool readMonitorId(const nlohmann::json& object, int& out) {
    if (!object.contains(kKeyMonitorId)) return false;
    const nlohmann::json& field = object.at(kKeyMonitorId);
    // is_number_integer() is false for 1.5 and for true -- JSON has one number type and
    // nlohmann will happily hand back 1 for either if asked with get<int>().
    if (!field.is_number_integer()) return false;
    out = field.get<int>();
    return true;
}

}  // namespace

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

ConfigStore::ConfigStore(std::filesystem::path file) : m_file(std::move(file)) {}

LoadResult ConfigStore::load() const {
    LoadResult result;
    if (!enabled()) return result;

    // One catch around the whole body. Every individual step below is written not to
    // throw, but this is the function whose caller is a constructor running inside
    // main()'s try -- anything escaping here stops the daemon booting because of a text
    // file, so the guarantee is structural rather than a matter of having checked.
    try {
        std::error_code ec;
        if (!std::filesystem::exists(m_file, ec) || ec) {
            return result;  // No config yet is the ordinary first-run case, not an error.
        }
        result.fileExisted = true;

        std::ifstream in(m_file, std::ios::binary);
        if (!in) {
            result.error = describe(m_file, "could not be opened for reading");
            return result;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

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

        if (!root.contains(kKeyVersion) || !root.at(kKeyVersion).is_number_integer() ||
            root.at(kKeyVersion).get<int>() != kSchemaVersion) {
            result.error = describe(m_file, "has an unrecognised schema version");
            return result;
        }
        if (!root.contains(kKeyMonitors) || !root.at(kKeyMonitors).is_array()) {
            result.error = describe(m_file, "has no monitors array");
            return result;
        }

        for (const nlohmann::json& element : root.at(kKeyMonitors)) {
            PersistedWallpaper entry;
            if (!element.is_object() || !readMonitorId(element, entry.monitorId) ||
                !readString(element, kKeyType, entry.type) ||
                !readString(element, kKeyPath, entry.path)) {
                ++result.malformedEntries;
                continue;
            }
            result.entries.push_back(std::move(entry));
        }
    } catch (const std::exception& e) {
        result.entries.clear();
        result.error = describe(m_file, std::string("could not be read: ") + e.what());
    } catch (...) {
        result.entries.clear();
        result.error = describe(m_file, "could not be read");
    }
    return result;
}

SaveResult ConfigStore::save(const std::vector<PersistedWallpaper>& entries) const {
    SaveResult result;
    if (!enabled()) return result;

    const std::filesystem::path temp = m_file.string() + ".tmp";
    try {
        nlohmann::json monitors = nlohmann::json::array();
        for (const PersistedWallpaper& entry : entries) {
            // dump() throws on invalid UTF-8 and Linux paths are arbitrary bytes, so a
            // single unusual filename would otherwise cost every monitor its wallpaper.
            // Dropping just this entry keeps the rest, and the count tells the caller a
            // monitor will not come back.
            if (!isValidUtf8(entry.path) || !isValidUtf8(entry.type)) {
                ++result.skippedNonUtf8;
                continue;
            }
            monitors.push_back(nlohmann::json{{kKeyMonitorId, entry.monitorId},
                                              {kKeyType, entry.type},
                                              {kKeyPath, entry.path}});
        }
        const nlohmann::json root{{kKeyVersion, kSchemaVersion}, {kKeyMonitors, std::move(monitors)}};
        const std::string text = root.dump(2) + "\n";

        constexpr int kFlags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
        int fd = ::open(temp.c_str(), kFlags, 0644);
        if (fd < 0) {
            // Only on the first save and after someone removes the directory. Doing it
            // unconditionally would put a mkdir on the DBus thread for every SetWallpaper.
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

        size_t written = 0;
        while (written < text.size()) {
            const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                const std::string why = n < 0 ? std::strerror(errno) : "short write";
                ::close(fd);
                ::unlink(temp.c_str());
                result.error = describe(temp, "could not be written: " + why);
                return result;
            }
            written += static_cast<size_t>(n);
        }

        // Checked, not just called: a full disk surfaces here rather than at write().
        if (::close(fd) != 0) {
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
