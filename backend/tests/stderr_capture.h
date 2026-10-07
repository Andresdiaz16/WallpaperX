// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// stderr, redirected for a span, so a test can count what the code under test logged -- read as Qt
// text. The redirection, the file and what happens to both are StreamCapture's.

#include "tests/stream_capture.h"

#include <QString>

#include <unistd.h>

namespace wallpaperx::testing {

class StderrCapture : public StreamCapture {
public:
    StderrCapture() : StreamCapture(STDERR_FILENO) {}
    // Everything written while capturing. Ends the capture first, so the text is complete.
    QString text() {
        restore();
        return QString::fromStdString(StreamCapture::text());
    }
};

}  // namespace wallpaperx::testing
