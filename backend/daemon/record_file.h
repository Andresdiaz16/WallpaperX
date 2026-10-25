// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

#include <sys/types.h>

namespace wallpaperx {

// The whole of a record this daemon could have written, or nothing: a file larger than `maxBytes`,
// not regular, or a symlink is not one of ours. Never blocks on a FIFO.
std::string readRecordFile(const std::filesystem::path& file, off_t maxBytes);

// Writes all of `text`, retrying an interrupted or short write. False on any other failure.
bool writeAll(int fd, const std::string& text);

// Replaces `file` whole with `text`, through a temporary file beside it and a rename, so a reader never sees half
// of one; creates `file`'s directory. Returns the reason it could not, empty on success.
std::string replaceRecordFile(const std::filesystem::path& file, const std::string& text);

// What a path names, as a record taken from its bytes is trusted against: a record read back against other
// bytes would describe something else.
struct FileIdentity {
    uint64_t device = 0;
    uint64_t inode = 0;
    int64_t size = 0;
    int64_t modifiedNs = 0;
    bool operator==(const FileIdentity&) const = default;
};

// What `path` names now; nothing when it cannot be asked.
std::optional<FileIdentity> fileIdentityOf(const std::string& path);

// `object[key]` when it is a number T holds exactly: nlohmann's get<T>() would narrow anything else unchecked.
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

}
