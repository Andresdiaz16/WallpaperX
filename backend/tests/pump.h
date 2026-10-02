// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Running the Qt event loop from a test's main thread, for the item and QML tests.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <functional>

namespace wallpaperx::testing {

// Runs the event loop until `done` or the deadline. Bounded, and the caller must have a branch
// for false: a run that never reached its precondition must say so rather than fall through.
inline bool pumpUntil(const std::function<bool()>& done, int limitMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > limitMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

// Runs the event loop for `ms`, waiting for nothing: the span a "this must not happen" is read over.
inline void pumpFor(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

}  // namespace wallpaperx::testing
