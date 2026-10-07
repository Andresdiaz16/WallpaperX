// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// One standard stream -- stdout or stderr -- redirected into a file for a span, so a test can count
// what the code under test wrote there.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace wallpaperx::testing {

// The file is named and kept under $TMPDIR (the entry's scratch directory) while the capture runs,
// and its path is printed on the real stderr first: a process that dies before the capture ends --
// a ctest timeout's SIGKILL, an abort -- leaves what was captured readable there. restore(), which
// the destructor runs, writes the captured text on to the real stream and then removes the file, so a
// run that ends normally loses nothing and leaves nothing behind.
class StreamCapture {
public:
    explicit StreamCapture(int stream) : m_stream(stream) {
        const char* dir = std::getenv("TMPDIR");
        m_path = std::string(dir != nullptr && *dir != '\0' ? dir : "/tmp") + "/wallpaperx_capture.XXXXXX";
        flush();
        m_read = ::mkostemp(m_path.data(), O_CLOEXEC);
        if (m_read < 0) {
            m_path.clear();
            return;
        }
        std::fprintf(stderr, "  (fd %d of this process is being captured in %s until the phase ends)\n", stream,
                     m_path.c_str());
        std::fflush(stderr);
        m_saved = ::dup(stream);
        m_ok = m_saved >= 0 && ::dup2(m_read, stream) >= 0;
    }
    ~StreamCapture() {
        restore();
        if (m_read >= 0) ::close(m_read);
    }
    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;

    bool ok() const { return m_ok; }

    // Everything written so far, readable while capturing and after restore().
    std::string text() const {
        flush();
        std::string out;
        char buffer[4096];
        off_t at = 0;
        for (ssize_t n; m_read >= 0 && (n = ::pread(m_read, buffer, sizeof buffer, at)) > 0; at += n) {
            out.append(buffer, static_cast<size_t>(n));
        }
        return out;
    }

    // How many times `needle` appears in what was captured.
    int count(std::string_view needle) const {
        const std::string all = text();
        int found = 0;
        for (size_t at = all.find(needle); !needle.empty() && at != std::string::npos;
             at = all.find(needle, at + needle.size())) {
            ++found;
        }
        return found;
    }

    // Lines captured so far that contain every one of `needles`.
    int linesWith(std::initializer_list<std::string> needles) const {
        const std::string all = text();
        int lines = 0;
        size_t start = 0;
        while (start < all.size()) {
            size_t end = all.find('\n', start);
            if (end == std::string::npos) end = all.size();
            const std::string_view line(all.data() + start, end - start);
            if (std::all_of(needles.begin(), needles.end(),
                            [&line](const std::string& needle) { return line.find(needle) != std::string_view::npos; })) {
                ++lines;
            }
            start = end + 1;
        }
        return lines;
    }

    // Puts the real stream back, writes what was captured on to it, and removes the file. Idempotent.
    void restore() {
        if (m_saved >= 0) {
            flush();
            ::dup2(m_saved, m_stream);
            ::close(m_saved);
            m_saved = -1;
            const std::string captured = text();
            std::FILE* real = m_stream == STDOUT_FILENO ? stdout : stderr;
            std::fwrite(captured.data(), 1, captured.size(), real);
            std::fflush(real);
        }
        if (!m_path.empty()) {
            ::unlink(m_path.c_str());
            m_path.clear();
        }
    }

private:
    void flush() const {
        std::fflush(m_stream == STDOUT_FILENO ? stdout : stderr);
    }

    int m_stream;
    std::string m_path;
    int m_read = -1;
    int m_saved = -1;
    bool m_ok = false;
};

}  // namespace wallpaperx::testing
