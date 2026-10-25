// SPDX-License-Identifier: GPL-2.0-or-later

#include "record_file.h"

#include <cerrno>

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

}
