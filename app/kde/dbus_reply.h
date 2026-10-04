// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QObject>
#include <QString>

#include <utility>

namespace wallpaperx {

// A message from off-process lands in a label; this keeps one from filling the window.
constexpr qsizetype kMaxMessageLength = 300;

inline QString clamped(QString message)
{
    message = message.trimmed();
    return message.size() > kMaxMessageLength ? message.left(kMaxMessageLength) + QStringLiteral("…") : message;
}

// Nobody owns the service's name. NameHasNoOwner has no QDBusError enumerator.
inline bool isUnreachable(const QDBusError& error)
{
    return error.type() == QDBusError::ServiceUnknown || error.name() == u"org.freedesktop.DBus.Error.NameHasNoOwner";
}

inline bool isNoAnswer(const QDBusError& error)
{
    return error.type() == QDBusError::NoReply || error.type() == QDBusError::Timeout
           || error.type() == QDBusError::TimedOut;
}

// Calls `onReply(const QDBusMessage&)` with the reply or error to `pending`, only while `context`
// lives. The watcher is the context's child, so a context destroyed first takes the call's
// tracking with it and nothing is left to answer it. `context` must not be null.
template <typename OnReply>
void whenAnswered(QObject* context, QDBusPendingCall pending, OnReply onReply)
{
    Q_ASSERT(context != nullptr);
    auto* watcher = new QDBusPendingCallWatcher(std::move(pending), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
                     [onReply = std::move(onReply)](QDBusPendingCallWatcher* done) {
                         done->deleteLater();
                         onReply(done->reply());
                     });
}

} // namespace wallpaperx
