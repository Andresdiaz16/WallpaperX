// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QString>

#include <expected>
#include <functional>

class QDBusMessage;

namespace wallpaperx {

// plasmaActivationScript() for `connector`: shared/plasma_activation.h.
QString activationScript(const QString& connector);
// How many desktops evaluateScript's reply says were switched; the error is a sentence for the user.
std::expected<int, QString> activationResult(const QDBusMessage& reply);

// Plasma's desktop shell, through org.kde.PlasmaShell.evaluateScript.
//
// The bus, service name and timeout are injected so a test can stand up its own shell on a
// private bus; the application passes the session bus and the defaults.
class PlasmaController : public QObject {
    Q_OBJECT

public:
    // Without a service or timeout: Plasma's own shell, waited on as plasma_activation.h says.
    explicit PlasmaController(QDBusConnection bus, QObject* parent = nullptr);
    PlasmaController(QDBusConnection bus, QString service, int timeoutMs, QObject* parent = nullptr);

    // Puts the WallpaperX wallpaper on every desktop Plasma keeps for `connector`, on every activity.
    // `done` gets the number switched, and runs only while `context` is alive.
    void activate(QObject* context, const QString& connector, std::function<void(std::expected<int, QString>)> done);

private:
    QDBusConnection m_bus;
    QString m_service;
    int m_timeoutMs;
};

} // namespace wallpaperx
