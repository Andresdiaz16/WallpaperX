// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaper_file.h"

#include "vulkan/gpu_fault.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace wallpaperx {

int openWallpaperFile(const std::string& file, const char* component) {
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;

    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        const int failed = errno;
        ::close(fd);
        errno = failed;
        return -1;
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(fd);
        throw WallpaperContentError(describeRejection(component, "the wallpaper is not a regular file", file));
    }
    // The open is what it was for, and a regular file's reads ignore it; cleared so nothing reading
    // through this descriptor has to be written against one set two ways.
    if (const int flags = ::fcntl(fd, F_GETFL); flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}

std::string describeRejection(const char* component, const std::string& reason, const std::string& path) {
    return std::string(component) + ": " + reason + ": " + path;
}

}  // namespace wallpaperx
