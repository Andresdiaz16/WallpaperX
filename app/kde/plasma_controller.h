// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QString>

#include <expected>
#include <functional>

class QDBusMessage;

namespace wallpaperx {

// The plugin id Plasma knows the WallpaperX wallpaper by (backend/kde/kpackage/metadata.json).
inline constexpr const char* kWallpaperPluginId = "com.wallpaperx.wallpaper";

// The script evaluateScript runs to put the WallpaperX wallpaper on `connector`'s desktops. It
// switches the plugin on and writes nothing else: what to show is the daemon's to say. It throws,
// failing the call, when Plasma shows no desktop of its own there or the plugin is not installed,
// and prints how many desktops it switched.
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
    static constexpr const char* kDefaultService = "org.kde.plasmashell";

    explicit PlasmaController(QDBusConnection bus, QString service = QString::fromLatin1(kDefaultService),
                              int timeoutMs = 25000, QObject* parent = nullptr);

    // Puts the WallpaperX wallpaper on every desktop Plasma keeps for `connector`, on every activity.
    // `done` gets the number switched, and runs only while `context` is alive.
    void activate(QObject* context, const QString& connector, std::function<void(std::expected<int, QString>)> done);

private:
    QDBusConnection m_bus;
    QString m_service;
    int m_timeoutMs;
};

} // namespace wallpaperx
