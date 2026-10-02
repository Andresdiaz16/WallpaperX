// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// stderr, redirected into a file for a span, so a test can count what the code under test logged.

#include <QFile>
#include <QString>

#include <cstdio>

#include <fcntl.h>
#include <unistd.h>

namespace wallpaperx::testing {

class StderrCapture {
public:
    explicit StderrCapture(const QString& file) : m_file(file) {
        std::fflush(stderr);
        m_saved = ::dup(STDERR_FILENO);
        const int fd = ::open(QFile::encodeName(file).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        m_ok = m_saved >= 0 && fd >= 0 && ::dup2(fd, STDERR_FILENO) >= 0;
        if (fd >= 0) ::close(fd);
    }
    ~StderrCapture() { restore(); }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    void restore() {
        if (m_saved < 0) return;
        std::fflush(stderr);
        ::dup2(m_saved, STDERR_FILENO);
        ::close(m_saved);
        m_saved = -1;
    }
    bool ok() const { return m_ok; }
    // What was written so far. Restores first, so the text is complete.
    QString text() {
        restore();
        QFile log(m_file);
        return log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()) : QString();
    }

private:
    QString m_file;
    int m_saved = -1;
    bool m_ok = false;
};

}  // namespace wallpaperx::testing
