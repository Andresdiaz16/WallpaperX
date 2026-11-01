// SPDX-License-Identifier: GPL-2.0-or-later

#include "remembered_settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

namespace wallpaperx {

namespace {

constexpr int kSaveDelayMs = 1000;

} // namespace

RememberedSettings::RememberedSettings(QString file)
    : m_file(std::move(file))
{
    m_saveDue.setSingleShot(true);
    m_saveDue.setInterval(kSaveDelayMs);
    QObject::connect(&m_saveDue, &QTimer::timeout, &m_saveDue, [this] { save(); });

    QFile in(m_file);
    if (!in.open(QIODevice::ReadOnly)) {
        if (in.exists()) {
            qWarning("wallpaperx: %s could not be read: %s", qUtf8Printable(m_file), qUtf8Printable(in.errorString()));
        }
        return;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll(), &error);
    if (!doc.isObject()) {
        // Started afresh; the next change overwrites it.
        qWarning("wallpaperx: %s is not a settings file (%s); starting without it", qUtf8Printable(m_file),
                 qUtf8Printable(error.errorString()));
        return;
    }
    const QJsonObject screens = doc.object();
    for (auto screen = screens.begin(); screen != screens.end(); ++screen) {
        const QJsonObject wallpapers = screen->toObject();
        for (auto wallpaper = wallpapers.begin(); wallpaper != wallpapers.end(); ++wallpaper) {
            if (QVariantMap values = wallpaper->toObject().toVariantMap(); !values.isEmpty()) {
                m_values[screen.key()].insert(wallpaper.key(), std::move(values));
            }
        }
    }
}

RememberedSettings::~RememberedSettings()
{
    if (m_saveDue.isActive()) {
        save();
    }
}

QVariantMap RememberedSettings::values(const QString& connector, const QString& wallpaperPath) const
{
    return m_values.value(connector).value(wallpaperPath);
}

void RememberedSettings::remember(const QString& connector, const QString& wallpaperPath, const QVariantMap& changed)
{
    if (values(connector, wallpaperPath) == changed) {
        return;
    }
    if (changed.isEmpty()) {
        auto screen = m_values.find(connector);
        screen->remove(wallpaperPath);
        if (screen->isEmpty()) {
            m_values.erase(screen);
        }
    } else {
        m_values[connector].insert(wallpaperPath, changed);
    }
    m_saveDue.start();
}

void RememberedSettings::save()
{
    m_saveDue.stop();
    QJsonObject screens;
    for (auto screen = m_values.cbegin(); screen != m_values.cend(); ++screen) {
        QJsonObject wallpapers;
        for (auto wallpaper = screen->cbegin(); wallpaper != screen->cend(); ++wallpaper) {
            wallpapers.insert(wallpaper.key(), QJsonObject::fromVariantMap(*wallpaper));
        }
        screens.insert(screen.key(), wallpapers);
    }
    QDir().mkpath(QFileInfo(m_file).path());
    QSaveFile out(m_file);
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(screens).toJson(QJsonDocument::Compact)) < 0 ||
        !out.commit()) {
        qWarning("wallpaperx: %s could not be written: %s", qUtf8Printable(m_file), qUtf8Printable(out.errorString()));
    }
}

QString defaultRememberedSettingsFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) +
           QStringLiteral("/wallpaperx/screen-settings.json");
}

} // namespace wallpaperx
