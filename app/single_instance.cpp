// SPDX-License-Identifier: GPL-2.0-or-later

#include "single_instance.h"

#include "kde/dbus_reply.h"
#include "library/wpx_package.h"

#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusReply>
#include <QUrl>

namespace wallpaperx {

namespace {

constexpr const char* kInterface = "org.freedesktop.Application";
constexpr int kHandOverTimeoutMs = 10000;

QByteArray activationToken(const QVariantMap& platformData)
{
    return platformData.value(QStringLiteral("activation-token")).toString().toUtf8();
}

// What this launch was given to raise a window with, passed on under the spec's keys.
QVariantMap ownPlatformData()
{
    QVariantMap data;
    if (const QByteArray token = qgetenv("XDG_ACTIVATION_TOKEN"); !token.isEmpty()) {
        data.insert(QStringLiteral("activation-token"), QString::fromUtf8(token));
    }
    if (const QByteArray id = qgetenv("DESKTOP_STARTUP_ID"); !id.isEmpty()) {
        data.insert(QStringLiteral("desktop-startup-id"), QString::fromUtf8(id));
    }
    return data;
}

} // namespace

void ApplicationService::Activate(const QVariantMap& platformData)
{
    if (m_handler) {
        m_handler({}, activationToken(platformData));
    }
}

void ApplicationService::Open(const QStringList& uris, const QVariantMap& platformData)
{
    QStringList paths;
    for (const QString& uri : uris) {
        if (auto path = pathOfFileUri(uri)) {
            paths.append(std::move(*path));
        } else {
            qWarning("wallpaperx: asked to open %s, which is not a file on this computer", qUtf8Printable(uri));
        }
    }
    if (m_handler) {
        m_handler(paths, activationToken(platformData));
    }
}

void ApplicationService::ActivateAction(const QString&, const QVariantList&, const QVariantMap& platformData)
{
    Activate(platformData);
}

QString fileUri(const QByteArray& path)
{
    return QStringLiteral("file://") + QString::fromLatin1(path.toPercentEncoding("/"));
}

std::optional<QString> pathOfFileUri(const QString& uri)
{
    const auto path = localPathOfFileUrl(QUrl(uri));
    return path ? std::optional<QString>(decodeFileName(*path)) : std::nullopt;
}

Claim claimOrHandOver(QDBusConnection bus, ApplicationService& service, const QList<QByteArray>& paths)
{
    if (!bus.isConnected() || bus.interface() == nullptr) {
        return {Claim::Result::Alone, QStringLiteral("there is no session bus")};
    }
    const QString name = QString::fromLatin1(kApplicationBusName);
    const QString path = QString::fromLatin1(kApplicationObjectPath);
    // Before the name, so a hand-over arriving the moment it is held finds the object.
    if (!bus.registerObject(path, &service, QDBusConnection::ExportAllSlots)) {
        return {Claim::Result::Alone, bus.lastError().message()};
    }
    QStringList uris;
    for (const QByteArray& file : paths) {
        uris.append(fileUri(file));
    }
    // Twice: the holder can quit between the refused claim and the hand-over.
    QString problem;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> claimed = bus.interface()->registerService(
            name, QDBusConnectionInterface::DontQueueService, QDBusConnectionInterface::DontAllowReplacement);
        if (!claimed.isValid()) {
            bus.unregisterObject(path);
            return {Claim::Result::Alone, claimed.error().message()};
        }
        if (claimed.value() == QDBusConnectionInterface::ServiceRegistered) {
            return {Claim::Result::First, {}};
        }
        QDBusMessage call = QDBusMessage::createMethodCall(name, path, QString::fromLatin1(kInterface),
                                                           uris.isEmpty() ? QStringLiteral("Activate") : QStringLiteral("Open"));
        if (uris.isEmpty()) {
            call << ownPlatformData();
        } else {
            call << uris << ownPlatformData();
        }
        const QDBusMessage reply = bus.call(call, QDBus::Block, kHandOverTimeoutMs);
        if (reply.type() == QDBusMessage::ReplyMessage) {
            bus.unregisterObject(path);
            return {Claim::Result::HandedOver, {}};
        }
        problem = reply.errorMessage();
        if (!isUnreachable(QDBusError(reply))) {
            break;
        }
    }
    bus.unregisterObject(path);
    return {Claim::Result::HandOverFailed, problem};
}

} // namespace wallpaperx
