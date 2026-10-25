// SPDX-License-Identifier: GPL-2.0-or-later

#include "resume_record.h"

#include "record_file.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <limits>
#include <system_error>
#include <type_traits>

#include <fcntl.h>
#include <unistd.h>

namespace wallpaperx {
namespace {

constexpr const char* kKeyPoints = "points";
constexpr const char* kKeyMonitorId = "monitor_id";
constexpr const char* kKeyPath = "path";
constexpr const char* kKeyDevice = "device";
constexpr const char* kKeyInode = "inode";
constexpr const char* kKeySize = "size";
constexpr const char* kKeyModified = "modified_ns";
constexpr const char* kKeyPosition = "position";

// A point per monitor, each a path and a few numbers: far below this.
constexpr off_t kMaxRecordBytes = 256 * 1024;

// Only a value T holds exactly: nlohmann's get<T>() would narrow anything else unchecked.
template <typename T>
bool readNumber(const nlohmann::json& object, const char* key, T& out) {
    if (!object.contains(key)) return false;
    const nlohmann::json& field = object.at(key);
    if (field.is_number_unsigned()) {
        const uint64_t value = field.get<uint64_t>();
        if (value > static_cast<uint64_t>(std::numeric_limits<T>::max())) return false;
        out = static_cast<T>(value);
        return true;
    }
    if constexpr (std::is_unsigned_v<T>) {
        return false;
    } else {
        if (!field.is_number_integer()) return false;
        const int64_t value = field.get<int64_t>();
        if (value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max()) return false;
        out = static_cast<T>(value);
        return true;
    }
}

}  // namespace

std::filesystem::path resumeRecordPath(const std::filesystem::path& stateDir) {
    if (stateDir.empty()) return {};
    return stateDir / "resume.json";
}

std::string writeResumeRecord(const std::filesystem::path& stateDir, const std::vector<ResumePoint>& points) {
    const std::filesystem::path file = resumeRecordPath(stateDir);
    if (file.empty()) return "no state directory";

    nlohmann::json list = nlohmann::json::array();
    for (const ResumePoint& point : points) {
        list.push_back({{kKeyMonitorId, point.monitorId},
                        {kKeyPath, point.path},
                        {kKeyDevice, point.device},
                        {kKeyInode, point.inode},
                        {kKeySize, point.size},
                        {kKeyModified, point.modifiedNs},
                        {kKeyPosition, point.position}});
    }
    // dump() throws on a path that is not valid UTF-8; replaced rather than lost, and then it names
    // no file the identity check would accept.
    const std::string text =
        nlohmann::json{{kKeyPoints, std::move(list)}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";

    std::error_code ec;
    std::filesystem::create_directories(stateDir, ec);
    const std::filesystem::path temporary = file.string() + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return std::string("could not create ") + temporary.string() + ": " + std::strerror(errno);
    const bool written = writeAll(fd, text);
    const int writeError = errno;
    if (::close(fd) != 0 || !written) {
        ::unlink(temporary.c_str());
        return std::string("could not write ") + temporary.string() + ": " + std::strerror(written ? errno : writeError);
    }
    if (::rename(temporary.c_str(), file.c_str()) != 0) {
        const int renameError = errno;
        ::unlink(temporary.c_str());
        return std::string("could not replace ") + file.string() + ": " + std::strerror(renameError);
    }
    return {};
}

std::vector<ResumePoint> readResumeRecord(const std::filesystem::path& stateDir) {
    const std::filesystem::path file = resumeRecordPath(stateDir);
    if (file.empty()) return {};
    const std::string text = readRecordFile(file, kMaxRecordBytes);

    // allow_exceptions=false: a torn or foreign file is a value here, not a throw.
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (!root.is_object() || !root.contains(kKeyPoints) || !root.at(kKeyPoints).is_array()) return {};
    std::vector<ResumePoint> points;
    for (const nlohmann::json& entry : root.at(kKeyPoints)) {
        ResumePoint point;
        if (!entry.is_object() || !readNumber(entry, kKeyMonitorId, point.monitorId) || !entry.contains(kKeyPath) ||
            !entry.at(kKeyPath).is_string() || !readNumber(entry, kKeyDevice, point.device) ||
            !readNumber(entry, kKeyInode, point.inode) || !readNumber(entry, kKeySize, point.size) ||
            !readNumber(entry, kKeyModified, point.modifiedNs) || !readNumber(entry, kKeyPosition, point.position) ||
            point.monitorId < 0) {
            continue;
        }
        point.path = entry.at(kKeyPath).get<std::string>();
        points.push_back(std::move(point));
    }
    return points;
}

void removeResumeRecord(const std::filesystem::path& stateDir) {
    const std::filesystem::path file = resumeRecordPath(stateDir);
    if (file.empty()) return;
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

}
