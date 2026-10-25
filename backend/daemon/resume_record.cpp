// SPDX-License-Identifier: GPL-2.0-or-later

#include "resume_record.h"

#include "record_file.h"

#include <nlohmann/json.hpp>

#include <system_error>

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

    return replaceRecordFile(file, text);
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
