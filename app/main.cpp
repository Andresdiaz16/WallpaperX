// SPDX-License-Identifier: GPL-2.0-or-later

#include "kde/kscreen_client.h"
#include "library/library_manager.h"
#include "library/preview_image_provider.h"

#include <QDBusConnection>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("wallpaperx"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("WallpaperX"));
    QGuiApplication::setDesktopFileName(QStringLiteral("com.wallpaperx.app"));

    // Declared before the engine so they outlive every binding that reads them.
    wallpaperx::KScreenClient kscreen(QDBusConnection::sessionBus());
    wallpaperx::LibraryManager library(wallpaperx::defaultLibraryDirectory());

    QQmlApplicationEngine engine;
    engine.addImageProvider(QString::fromLatin1(wallpaperx::kPreviewProviderId), new wallpaperx::PreviewImageProvider);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.setInitialProperties({{QStringLiteral("kscreen"), QVariant::fromValue(&kscreen)},
                                 {QStringLiteral("library"), QVariant::fromValue(&library)}});
    engine.loadFromModule("WallpaperX", "Main");

    kscreen.refresh();
    library.rescan();
    return QGuiApplication::exec();
}
