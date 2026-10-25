// SPDX-License-Identifier: GPL-2.0-or-later

#include "record_file.h"

#include <cerrno>
#include <cstring>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wallpaperx {

std::string readRecordFile(const std::filesystem::path& file, off_t maxBytes) {
    const int fd = ::open(file.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return {};

    std::string text;
    struct stat info {};
    if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size <= maxBytes) {
        char buffer[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            text.append(buffer, static_cast<size_t>(n));
            if (text.size() > static_cast<size_t>(maxBytes)) {
                text.clear();
                break;
            }
        }
    }
    ::close(fd);
    return text;
}

bool writeAll(int fd, const std::string& text) {
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

std::string replaceRecordFile(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
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

std::optional<FileIdentity> fileIdentityOf(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;
    return FileIdentity{static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino),
                        static_cast<int64_t>(st.st_size),
                        static_cast<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec};
}

}
