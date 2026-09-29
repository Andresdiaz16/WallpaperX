// SPDX-License-Identifier: GPL-2.0-or-later

#include "kde/kscreen_client.h"
#include "library/importer.h"
#include "library/library_manager.h"
#include "library/preview_image_provider.h"
#include "single_instance.h"

#include <QDBusConnection>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QUrl>
#include <QWindow>

#include <climits>
#include <cstdio>
#include <cstring>
#include <optional>
#include <variant>

#include <unistd.h>

namespace {

constexpr const char* kUsage = "Usage: wallpaperx [FILE.wpx...]\n"
                               "Imports each .wpx into the wallpaper library, into the running WallpaperX if there is one.\n";

struct Help {};
struct Refused {
    QByteArray argument;
    const char* why = "";
};
// The files named, absolute, as the bytes they were given in: a path need not be UTF-8, and
// QCoreApplication::arguments() would decode it lossily.
using CommandLine = std::variant<QList<QByteArray>, Help, Refused>;

// Nothing when it cannot be found, as when it has been removed.
std::optional<QByteArray> currentDirectory()
{
    QByteArray here(PATH_MAX, Qt::Uninitialized);
    if (::getcwd(here.data(), size_t(here.size())) == nullptr) {
        return std::nullopt;
    }
    here.truncate(qsizetype(std::strlen(here.constData())));
    return here;
}

// --help anywhere before "--" wins over an argument refused, so its exit status never depends on
// what else was given.
CommandLine parseCommandLine(int argc, char** argv)
{
    QList<QByteArray> files;
    std::optional<Refused> refused;
    bool options = true;
    const std::optional<QByteArray> here = currentDirectory();
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        if (options && argument == "--") {
            options = false;
        } else if (options && (argument == "--help" || argument == "-h")) {
            return Help{};
        } else if (options && argument.startsWith('-') && argument.size() > 1) {
            refused = refused.value_or(Refused{argument, "not an option wallpaperx takes"});
        } else if (options && argument.startsWith("file:")) {
            if (auto path = wallpaperx::localPathOfFileUrl(QUrl::fromEncoded(argument))) {
                files.append(std::move(*path));
            } else {
                refused = refused.value_or(Refused{argument, "not a file on this computer"});
            }
        } else if (argument.startsWith('/')) {
            files.append(argument);
        } else if (here) {
            files.append(*here + '/' + argument);
        } else {
            refused = refused.value_or(Refused{argument, "a relative path, and the current folder cannot be found"});
        }
    }
    if (refused) {
        return *refused;
    }
    return files;
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("wallpaperx"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("WallpaperX"));
    QGuiApplication::setDesktopFileName(QStringLiteral("com.wallpaperx.app"));

    // After QGuiApplication, which takes its own options out of argv.
    const CommandLine commandLine = parseCommandLine(argc, argv);
    if (std::holds_alternative<Help>(commandLine)) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    if (const auto* refused = std::get_if<Refused>(&commandLine)) {
        std::fprintf(stderr, "wallpaperx: %s is %s.\n%s", refused->argument.constData(), refused->why, kUsage);
        return 2;
    }
    const auto* files = std::get_if<QList<QByteArray>>(&commandLine);

    wallpaperx::ApplicationService service;
    switch (const auto claim = wallpaperx::claimOrHandOver(QDBusConnection::sessionBus(), service, *files); claim.result) {
    case wallpaperx::Claim::Result::First:
        break;
    case wallpaperx::Claim::Result::HandedOver:
        return 0;
    case wallpaperx::Claim::Result::HandOverFailed:
        std::fprintf(stderr, "wallpaperx: WallpaperX is already running but did not answer: %s\n",
                     qUtf8Printable(claim.problem));
        return 1;
    case wallpaperx::Claim::Result::Alone:
        qWarning("wallpaperx: running without a single-instance guard: %s", qUtf8Printable(claim.problem));
        break;
    }

    // Declared before the engine so they outlive every binding that reads them.
    wallpaperx::KScreenClient kscreen(QDBusConnection::sessionBus());
    const QString libraryDirectory = wallpaperx::defaultLibraryDirectory();
    wallpaperx::LibraryManager library(wallpaperx::wallpapersDirectory(libraryDirectory));
    wallpaperx::Importer importer(libraryDirectory, wallpaperx::wallpapersDirectory(libraryDirectory));
    QObject::connect(&importer, &wallpaperx::Importer::imported, &library, &wallpaperx::LibraryManager::reveal);

    QQmlApplicationEngine engine;
    engine.addImageProvider(QString::fromLatin1(wallpaperx::kPreviewProviderId), new wallpaperx::PreviewImageProvider);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.setInitialProperties({{QStringLiteral("kscreen"), QVariant::fromValue(&kscreen)},
                                 {QStringLiteral("library"), QVariant::fromValue(&library)},
                                 {QStringLiteral("importer"), QVariant::fromValue(&importer)}});
    engine.loadFromModule("WallpaperX", "Main");

    service.setHandler([&engine, &importer](const QStringList& paths, const QByteArray& activationToken) {
        for (const QString& path : paths) {
            importer.importPath(path);
        }
        auto* window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QWindow*>(engine.rootObjects().constFirst());
        if (window == nullptr) {
            return;
        }
        // Qt's Wayland plugin raises the window with this token once, then clears it.
        if (!activationToken.isEmpty()) {
            qputenv("XDG_ACTIVATION_TOKEN", activationToken);
        }
        window->show();
        window->raise();
        window->requestActivate();
    });

    for (const QByteArray& file : *files) {
        importer.importPath(wallpaperx::decodeFileName(file));
    }
    kscreen.refresh();
    library.rescan();
    importer.rescan();
    return QGuiApplication::exec();
}
