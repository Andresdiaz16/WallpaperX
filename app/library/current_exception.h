// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QException>
#include <QString>

#include <exception>

namespace wallpaperx {

// The exception being handled, seen through the QUnhandledException QtConcurrent wraps it in.
// Call only inside a catch block, or an onFailed handler.
inline QString describeCurrentException()
{
    try {
        try {
            throw;
        } catch (const QUnhandledException& wrapped) {
            if (wrapped.exception()) {
                std::rethrow_exception(wrapped.exception());
            }
            throw;
        }
    } catch (const std::exception& e) {
        return QString::fromLocal8Bit(e.what());
    } catch (...) {
    }
    return QStringLiteral("an unknown error");
}

} // namespace wallpaperx
