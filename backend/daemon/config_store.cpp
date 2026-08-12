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
constexpr std::uintmax_t kMaxBytes = 1u << 20;

// Every field name in one place: a typo in the reader that matches a typo in the writer is
// invisible until someone else's build reads the file.
constexpr const char* kKeyVersion = "version";
constexpr const char* kKeyMonitors = "monitors";
constexpr const char* kKeyMonitorId = "monitor_id";
constexpr const char* kKeyType = "type";
constexpr const char* kKeyPath = "path";

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

private:
    int m_fd;
};

// Raw: the only consumer records these into the health fields, which sanitize once. Escaping
// here too would double every backslash in the path by the time anyone reads it.
std::string describe(const std::filesystem::path& file, const std::string& what) {
    return file.string() + ": " + what;
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
