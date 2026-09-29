// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>

#include <unistd.h>

namespace wallpaperx {

// Owns one file descriptor; -1 for none.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : m_fd(fd) {}
    UniqueFd(UniqueFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.m_fd, -1));
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd() { reset(); }

    int get() const { return m_fd; }
    explicit operator bool() const { return m_fd >= 0; }
    int release() { return std::exchange(m_fd, -1); }
    void reset(int fd = -1)
    {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_fd = fd;
    }

private:
    int m_fd = -1;
};

} // namespace wallpaperx
