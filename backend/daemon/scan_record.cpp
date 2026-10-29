// SPDX-License-Identifier: GPL-2.0-or-later

#include "scan_record.h"

#include "record_file.h"

#include <nlohmann/json.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <system_error>

namespace wallpaperx {
namespace {

constexpr const char* kKeyRule = "rule";
constexpr const char* kKeyCodec = "libavcodec";
constexpr const char* kKeyFormat = "libavformat";
constexpr const char* kKeyScans = "scans";
constexpr const char* kKeyPath = "path";
constexpr const char* kKeyDevice = "device";
constexpr const char* kKeyInode = "inode";
constexpr const char* kKeySize = "size";
constexpr const char* kKeyModified = "modified_ns";
constexpr const char* kKeyMissingReference = "missing_reference";
constexpr const char* kKeyThroughout = "missing_reference_throughout";
constexpr const char* kKeyParameterSets = "parameter_set_change";

// An entry per video file shown, each a path and a few numbers: far below this.
constexpr off_t kMaxRecordBytes = 256 * 1024;

bool readFlag(const nlohmann::json& object, const char* key, bool& out) {
    if (!object.contains(key) || !object.at(key).is_boolean()) return false;
    out = object.at(key).get<bool>();
    return true;
}

}  // namespace

std::filesystem::path scanRecordPath(const std::filesystem::path& stateDir) {
    if (stateDir.empty()) return {};
    return stateDir / "processor-scans.json";
}

std::string writeScanRecord(const std::filesystem::path& stateDir, const std::vector<SavedScan>& scans) {
    const std::filesystem::path file = scanRecordPath(stateDir);
    if (file.empty()) return "no state directory";

    nlohmann::json list = nlohmann::json::array();
    for (const SavedScan& scan : scans) {
        list.push_back({{kKeyPath, scan.path},
                        {kKeyDevice, scan.file.device},
                        {kKeyInode, scan.file.inode},
                        {kKeySize, scan.file.size},
                        {kKeyModified, scan.file.modifiedNs},
                        {kKeyMissingReference, scan.found.missingReference},
                        {kKeyThroughout, scan.found.missingReferenceThroughout},
                        {kKeyParameterSets, scan.found.parameterSetsChangeOffReset}});
    }
    const nlohmann::json root{{kKeyRule, kSoftwareScanRule},
                              {kKeyCodec, avcodec_version()},
                              {kKeyFormat, avformat_version()},
                              {kKeyScans, std::move(list)}};
    // dump() throws on a path that is not valid UTF-8; replaced rather than lost, and then it names no file a
    // restored wallpaper's path matches.
    return replaceRecordFile(file, root.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
}

std::vector<SavedScan> readScanRecord(const std::filesystem::path& stateDir) {
    const std::filesystem::path file = scanRecordPath(stateDir);
    if (file.empty()) return {};
    const nlohmann::json root = nlohmann::json::parse(readRecordFile(file, kMaxRecordBytes), nullptr, false);
    uint32_t rule = 0;
    unsigned codec = 0;
    unsigned format = 0;
    if (!root.is_object() || !readNumber(root, kKeyRule, rule) || rule != kSoftwareScanRule ||
        !readNumber(root, kKeyCodec, codec) || codec != avcodec_version() || !readNumber(root, kKeyFormat, format) ||
        format != avformat_version() || !root.contains(kKeyScans) || !root.at(kKeyScans).is_array()) {
        return {};
    }
    std::vector<SavedScan> scans;
    for (const nlohmann::json& entry : root.at(kKeyScans)) {
        SavedScan scan;
        if (!entry.is_object() || !entry.contains(kKeyPath) || !entry.at(kKeyPath).is_string() ||
            !readNumber(entry, kKeyDevice, scan.file.device) || !readNumber(entry, kKeyInode, scan.file.inode) ||
            !readNumber(entry, kKeySize, scan.file.size) || scan.file.size < 0 ||
            !readNumber(entry, kKeyModified, scan.file.modifiedNs) ||
            !readFlag(entry, kKeyMissingReference, scan.found.missingReference) ||
            !readFlag(entry, kKeyThroughout, scan.found.missingReferenceThroughout) ||
            !readFlag(entry, kKeyParameterSets, scan.found.parameterSetsChangeOffReset)) {
            continue;
        }
        scan.path = entry.at(kKeyPath).get<std::string>();
        scans.push_back(std::move(scan));
    }
    return scans;
}

void removeScanRecord(const std::filesystem::path& stateDir) {
    const std::filesystem::path file = scanRecordPath(stateDir);
    if (file.empty()) return;
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

}
