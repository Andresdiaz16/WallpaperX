// SPDX-License-Identifier: GPL-2.0-or-later

#include "daemon/apply_controller.h"
#include "daemon/daemon_client.h"
#include "daemon/refresh_rate_reporter.h"
#include "daemon/remembered_settings.h"
#include "kde/kscreen_client.h"
#include "kde/kscreen_source.h"
#include "kde/plasma_controller.h"
#include "library/importer.h"
#include "library/library_manager.h"
#include "library/preview_image_provider.h"
#include "setup/first_run_setup.h"
#include "single_instance.h"

#include <QDBusConnection>
#include <QGuiApplication>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QSocketNotifier>
#include <QUrl>
#include <QWindow>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <optional>
#include <variant>

#include <sys/socket.h>
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

// `arguments` excludes the program name. --help anywhere before "--" wins over an argument
// refused, so its exit status never depends on what else was given.
CommandLine parseCommandLine(const QList<QByteArray>& arguments)
{
    QList<QByteArray> files;
    std::optional<Refused> refused;
    bool options = true;
    const std::optional<QByteArray> here = currentDirectory();
    for (const QByteArray& argument : arguments) {
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

int signalWriteFd = -1;

void reportSignal(int)
{
    const int saved = errno;
    const char byte = 0;
    [[maybe_unused]] const ssize_t written = ::write(signalWriteFd, &byte, 1);
    errno = saved;
}

// SIGTERM (as at logout), SIGINT and SIGHUP end the event loop, so main() unwinds and what is built there
// writes what it holds, settings not yet saved among them. The same signal again ends the process at
// once; a different one does not.
void quitOnSignals(QCoreApplication& app)
{
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds) != 0) {
        qWarning("wallpaperx: socketpair failed (%s), so a logout may lose the last second's setting changes",
                 std::strerror(errno));
        return;
    }
    signalWriteFd = fds[1];
    auto* notifier = new QSocketNotifier(fds[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [notifier] {
        // The byte stays unread; disabled, the notifier does not report it again.
        notifier->setEnabled(false);
        QCoreApplication::quit();
    });
    struct sigaction action {};
    action.sa_handler = reportSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_RESETHAND;
    for (const int quitting : {SIGTERM, SIGINT, SIGHUP}) {
        ::sigaction(quitting, &action, nullptr);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    // Qt takes its own options (-reverse, -platform X, ...) out of argv and knows no "--", so it is
    // shown only what comes before one; it rewrites nothing past the count it is given.
    const int operandsAt = int(std::find_if(argv + 1, argv + argc, [](const char* a) { return std::strcmp(a, "--") == 0; }) - argv);
    // Referenced by QGuiApplication for its whole life.
    int qtArgc = operandsAt;
    QGuiApplication app(qtArgc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("wallpaperx"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("WallpaperX"));
    QGuiApplication::setDesktopFileName(QStringLiteral("com.wallpaperx.app"));

    QList<QByteArray> arguments;
    for (int i = 1; i < qtArgc; ++i) {
        arguments.append(argv[i]);
    }
    for (int i = operandsAt; i < argc; ++i) {
        arguments.append(argv[i]);
    }
    const CommandLine commandLine = parseCommandLine(arguments);
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

    quitOnSignals(app);
    // Declared before the engine so they outlive every binding that reads them.
    wallpaperx::KScreenClient kscreen(wallpaperx::systemKScreenSource(QDBusConnection::sessionBus()));
    const QString libraryDirectory = wallpaperx::defaultLibraryDirectory();
    using wallpaperx::LibraryManager;
    LibraryManager library(wallpaperx::wallpapersDirectory(libraryDirectory), LibraryManager::kSettleMs,
                           LibraryManager::kPollMs, LibraryManager::Watching::Inotify, LibraryManager::kHoldMs,
                           wallpaperx::defaultSteamRoots());
    wallpaperx::Importer importer(libraryDirectory, wallpaperx::wallpapersDirectory(libraryDirectory));
    QObject::connect(&importer, &wallpaperx::Importer::imported, &library, &wallpaperx::LibraryManager::reveal);
    wallpaperx::DaemonClient daemon(QDBusConnection::sessionBus());
    wallpaperx::PlasmaController plasma(QDBusConnection::sessionBus());
    wallpaperx::ApplyController applier(kscreen, library, daemon, plasma, wallpaperx::defaultRememberedSettingsFile());
    wallpaperx::RefreshRateReporter refreshRates(kscreen, daemon);

    const wallpaperx::SetupPaths setupPaths = wallpaperx::SetupPaths::forThisApp();
    constexpr auto buildKind = WALLPAPERX_RELEASE_BUILD ? wallpaperx::BuildKind::Release : wallpaperx::BuildKind::Source;
    wallpaperx::FirstRunSetup setup(QDBusConnection::sessionBus(), setupPaths,
                                    wallpaperx::setupVersion(QStringLiteral(WALLPAPERX_APP_VERSION), buildKind, setupPaths.bundle),
                                    QProcessEnvironment::systemEnvironment());

    QQmlApplicationEngine engine;
    engine.addImageProvider(QString::fromLatin1(wallpaperx::kPreviewProviderId), new wallpaperx::PreviewImageProvider);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    // The window this load made; nothing when it made none, which objectCreationFailed then ends the app for.
    const auto load = [&engine](const char* type, const QVariantMap& properties) -> QWindow* {
        const qsizetype before = engine.rootObjects().size();
        engine.setInitialProperties(properties);
        engine.loadFromModule("WallpaperX", type);
        return engine.rootObjects().size() > before ? qobject_cast<QWindow*>(engine.rootObjects().constLast()) : nullptr;
    };
    const QVariantMap mainProperties{{QStringLiteral("kscreen"), QVariant::fromValue(&kscreen)},
                                     {QStringLiteral("library"), QVariant::fromValue(&library)},
                                     {QStringLiteral("importer"), QVariant::fromValue(&importer)},
                                     {QStringLiteral("daemon"), QVariant::fromValue(&daemon)},
                                     {QStringLiteral("applier"), QVariant::fromValue(&applier)}};
    const auto setupProperties = [&setup](bool blocking) {
        return QVariantMap{{QStringLiteral("setup"), QVariant::fromValue(&setup)}, {QStringLiteral("blocking"), blocking}};
    };
    // The window a launch handing its files over raises.
    QPointer<QWindow> front;
    switch (setup.need()) {
    case wallpaperx::FirstRunSetup::Need::UpToDate:
    case wallpaperx::FirstRunSetup::Need::NotBundled:
        front = load("Main", mainProperties);
        break;
    case wallpaperx::FirstRunSetup::Need::FirstRun: {
        QPointer<QWindow> progress = load("Setup", setupProperties(true));
        front = progress;
        QObject::connect(&setup, &wallpaperx::FirstRunSetup::finished, &app, [&, progress] {
            // A progress window closed during the run was the user quitting.
            if (setup.state() != wallpaperx::FirstRunSetup::State::Done || front != progress || !progress || !progress->isVisible()) return;
            QWindow* const main = load("Main", mainProperties);
            if (main == nullptr) return;
            front = main;
            // Hidden, not deleted: the engine owns its root objects.
            if (progress) progress->close();
        });
        setup.start();
        break;
    }
    case wallpaperx::FirstRunSetup::Need::Update: {
        front = load("Main", mainProperties);
        // Silent unless it fails; then shown beside the main window, never in its way.
        QObject::connect(&setup, &wallpaperx::FirstRunSetup::finished, &app, [&, problem = QPointer<QWindow>()]() mutable {
            if (setup.state() == wallpaperx::FirstRunSetup::State::Done) return;
            // Every window closed during the run was the user quitting; the next launch runs setup again.
            if (!(front && front->isVisible()) && !(problem && problem->isVisible())) return;
            if (problem) {
                problem->show();
            } else {
                problem = load("Setup", setupProperties(false));
            }
        });
        setup.start();
        break;
    }
    }

    service.setHandler([&front, &importer](const QStringList& paths, const QByteArray& activationToken) {
        for (const QString& path : paths) {
            importer.importPath(path);
        }
        QWindow* window = front;
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
    daemon.refresh();
    library.rescan();
    importer.rescan();
    return QGuiApplication::exec();
}
