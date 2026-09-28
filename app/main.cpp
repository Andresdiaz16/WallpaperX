// SPDX-License-Identifier: GPL-2.0-or-later

#include "kde/kscreen_client.h"

#include <QDBusConnection>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("wallpaperx"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("WallpaperX"));
    QGuiApplication::setDesktopFileName(QStringLiteral("com.wallpaperx.app"));

    // Declared before the engine so it outlives every binding that reads it.
    wallpaperx::KScreenClient kscreen(QDBusConnection::sessionBus());

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.setInitialProperties({{QStringLiteral("kscreen"), QVariant::fromValue(&kscreen)}});
    engine.loadFromModule("WallpaperX", "Main");

    kscreen.refresh();
    return QGuiApplication::exec();
}
