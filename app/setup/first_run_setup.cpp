// SPDX-License-Identifier: GPL-2.0-or-later

#include "first_run_setup.h"

#include "library/wpx_package.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace wallpaperx {

namespace {

namespace fs = std::filesystem;

constexpr const char* kSystemd = "org.freedesktop.systemd1";
constexpr const char* kManagerPath = "/org/freedesktop/systemd1";
constexpr const char* kManager = "org.freedesktop.systemd1.Manager";
constexpr const char* kUnitInterface = "org.freedesktop.systemd1.Unit";
constexpr const char* kServiceInterface = "org.freedesktop.systemd1.Service";
constexpr const char* kPackageId = "com.wallpaperx.wallpaper";
constexpr const char* kDaemonExecutable = "bin/wallpaperx-daemon";
constexpr const char* kUnitTemplate = ":/wallpaperx/setup/wallpaperx-daemon.service.in";
// The daemon takes its bus name once its GPU and settings are up, a few seconds on a slow first start.
constexpr int kDaemonStartMs = 30000;
// Each of a failed run's switch-back calls; the window stays on "Setting up" meanwhile.
constexpr int kSwitchBackCallMs = 5000;
// All of them, when the application is quitting: within what a logout waits for an application.
// Not counted: the wait for a copy or unit write under way, so a quit during a large copy waits for it.
constexpr int kQuitSwitchBackMs = 3000;

fs::path fsPath(const QString& path) { return fs::path(QFile::encodeName(path).toStdString()); }
QString shown(const fs::path& path) { return QFile::decodeName(QByteArray::fromStdString(path.string())); }
QString why(const std::error_code& ec) { return QString::fromLocal8Bit(ec.message()); }

struct Layout {
    fs::path bundleDaemon;
    fs::path bundlePackage;
    fs::path wallpapers;
    fs::path runtime;
    fs::path stamp;
    fs::path unitFile;
    fs::path package;
};

Layout layoutOf(const SetupPaths& paths) {
    const fs::path library = fsPath(paths.library);
    const fs::path dataHome = fsPath(paths.dataHome);
    const fs::path runtime = library / "runtime";
    return {fsPath(paths.bundle) / "daemon",
            fsPath(paths.bundle) / "plasma" / "wallpapers" / kPackageId,
            fsPath(wallpapersDirectory(paths.library)),
            runtime,
            runtime / "stamp",
            dataHome / "systemd" / "user" / FirstRunSetup::kUnitName,
            dataHome / "plasma" / "wallpapers" / kPackageId};
}

bool isExecutableFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && ::access(path.c_str(), X_OK) == 0;
}

// A version names a folder under runtime/, beside `current` and `stamp`.
bool isFolderName(const QString& version) {
    return !version.isEmpty() && !version.startsWith(u'.') && !version.contains(u'/') && version != QLatin1String("current") &&
           version != QLatin1String("stamp");
}

std::expected<void, QString> writeAtomically(const fs::path& path, const QByteArray& bytes) {
    QSaveFile file(shown(path));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return std::unexpected(QStringLiteral("Could not write %1: %2").arg(shown(path), file.errorString()));
    }
    return {};
}

std::optional<QByteArray> readSmallFile(const fs::path& path, qint64 limit = 256) {
    QFile file(shown(path));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    return file.read(limit);
}

// The content hash backend/packaging/content_id.cmake wrote into `dir`; nothing if it is missing or malformed.
std::optional<QString> buildIdIn(const fs::path& dir) {
    const std::optional<QByteArray> bytes = readSmallFile(dir / "build-id");
    if (!bytes) return std::nullopt;
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{16}$"));
    const QString id = QString::fromLatin1(bytes->trimmed());
    if (!hex.match(id).hasMatch()) return std::nullopt;
    return id;
}

// A finished runtime copy of some build: every copy is made from a bundle carrying a build id.
bool isCompleteCopy(const fs::path& copy) { return buildIdIn(copy) && isExecutableFile(copy / kDaemonExecutable); }

// A runtime copy of the bundle's daemon as it is now: a folder name alone says nothing of a damaged copy.
bool isIntactCopy(const fs::path& copy, const fs::path& bundleDaemon) {
    const std::optional<QString> bundled = buildIdIn(bundleDaemon);
    return bundled && buildIdIn(copy) == bundled && isCompleteCopy(copy);
}

QString incomplete(const fs::path& missing) {
    return QStringLiteral("WallpaperX's own files are incomplete: %1 is missing. In Steam, verify the integrity of "
                          "WallpaperX's files.").arg(shown(missing));
}

QString noBuildId(const fs::path& dir) {
    return QStringLiteral("%1 beside WallpaperX has no valid build id. In Steam, verify the integrity of WallpaperX's "
                          "files; in a source build, rebuild the backend, then the app.").arg(shown(dir / "build-id"));
}

// Both halves, checked before anything is changed: a half that is missing would otherwise be found
// only after the daemon had been switched and restarted.
std::expected<void, QString> checkBundle(const Layout& layout) {
    if (!isExecutableFile(layout.bundleDaemon / kDaemonExecutable)) return std::unexpected(incomplete(layout.bundleDaemon / kDaemonExecutable));
    // Without them the next start could not tell what is installed, and would install it again.
    if (!buildIdIn(layout.bundleDaemon)) return std::unexpected(noBuildId(layout.bundleDaemon));
    std::error_code ec;
    if (!fs::is_regular_file(layout.bundlePackage / "metadata.json", ec)) return std::unexpected(incomplete(layout.bundlePackage / "metadata.json"));
    if (!buildIdIn(layout.bundlePackage)) return std::unexpected(noBuildId(layout.bundlePackage));
    return {};
}

std::expected<void, QString> createLibrary(const Layout& layout) {
    std::error_code ec;
    fs::create_directories(layout.wallpapers, ec);
    if (ec) return std::unexpected(QStringLiteral("Could not create the wallpaper library at %1: %2").arg(shown(layout.wallpapers), why(ec)));
    return {};
}

// Puts the folder `incoming` in place of `target`, whole, and removes what `target` held: exchanged
// where the filesystem can, else the old one moved aside first and put back if the new one will not go in.
std::error_code replaceWhole(const fs::path& incoming, const fs::path& target) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(target, ec))) {
        fs::rename(incoming, target, ec);
    } else if (::renameat2(AT_FDCWD, incoming.c_str(), AT_FDCWD, target.c_str(), RENAME_EXCHANGE) != 0) {
        const fs::path outgoing = target.parent_path() / ("." + target.filename().string() + ".old");
        fs::remove_all(outgoing, ec);
        fs::rename(target, outgoing, ec);
        if (!ec) {
            fs::rename(incoming, target, ec);
            std::error_code back;
            if (ec) fs::rename(outgoing, target, back);
        }
        if (!ec) {
            std::error_code old;
            fs::remove_all(outgoing, old);
        }
    }
    // After an exchange `incoming` holds the old folder.
    std::error_code leftover;
    fs::remove_all(incoming, leftover);
    return ec;
}

// Swapped by rename, so `current` names one copy or the other at every moment.
std::error_code pointCurrentAt(const Layout& layout, const fs::path& name) {
    const fs::path next = layout.runtime / ".current.new";
    std::error_code ec;
    fs::remove(next, ec);
    fs::create_symlink(name, next, ec);
    if (!ec) fs::rename(next, layout.runtime / "current", ec);
    return ec;
}

// Points `current` at this version's copy, made from the bundle unless it already is one. A folder
// under that name with other content is damaged, never another build's, so remaking it in place
// leaves a daemon running from it on the same build until the restart its deleted executable causes.
std::expected<void, QString> copyDaemon(const Layout& layout, const std::expected<SetupVersion, QString>& version) {
    if (!version) return std::unexpected(version.error());
    const QString& folder = version->folder;
    if (!isFolderName(folder)) return std::unexpected(QStringLiteral("This build's version, \"%1\", cannot name a folder.").arg(folder));
    std::error_code ec;
    fs::create_directories(layout.runtime, ec);
    if (ec) return std::unexpected(QStringLiteral("Could not create %1: %2").arg(shown(layout.runtime), why(ec)));

    const fs::path name = fsPath(folder);
    const fs::path copy = layout.runtime / name;

    // Built beside and put in place whole, so the folder under the version's name is always a finished copy.
    if (!isIntactCopy(copy, layout.bundleDaemon)) {
        const fs::path partial = layout.runtime / ("." + name.string() + ".partial");
        fs::remove_all(partial, ec);
        fs::copy(layout.bundleDaemon, partial, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (!ec) ec = replaceWhole(partial, copy);
        if (ec) {
            std::error_code cleanup;
            fs::remove_all(partial, cleanup);
            return std::unexpected(QStringLiteral("Could not copy the WallpaperX service into %1: %2").arg(shown(copy), why(ec)));
        }
    }
    if (const std::error_code pointed = pointCurrentAt(layout, name)) {
        return std::unexpected(QStringLiteral("Could not point %1 at the new copy: %2").arg(shown(layout.runtime / "current"), why(pointed)));
    }
    return {};
}

std::expected<void, QString> writeUnit(const Layout& layout, const QByteArray& text) {
    std::error_code ec;
    fs::create_directories(layout.unitFile.parent_path(), ec);
    if (ec) return std::unexpected(QStringLiteral("Could not create %1: %2").arg(shown(layout.unitFile.parent_path()), why(ec)));
    return writeAtomically(layout.unitFile, text);
}

// The text the unit file has, when it has another than `text`.
std::optional<QByteArray> replacedUnitText(const Layout& layout, const QByteArray& text) {
    QFile old(shown(layout.unitFile));
    if (!old.open(QIODevice::ReadOnly)) return std::nullopt;
    QByteArray bytes = old.readAll();
    if (bytes == text) return std::nullopt;
    return bytes;
}

// Every copy but the one `current` names. A copy that will not go is left for the next update.
std::expected<void, QString> removeOldCopies(const Layout& layout, const QString& folder) {
    std::error_code ec;
    fs::directory_iterator it(layout.runtime, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path name = it->path().filename();
        if (name == fsPath(folder) || name == "current" || name == "stamp") continue;
        std::error_code removeEc;
        fs::remove_all(it->path(), removeEc);
        if (removeEc) qWarning("wallpaperx: setup could not remove the old copy %s: %s", it->path().c_str(), removeEc.message().c_str());
    }
    if (ec) qWarning("wallpaperx: setup could not list %s: %s", layout.runtime.c_str(), ec.message().c_str());
    return {};
}

// Written beside the installed package and swapped in by rename: plasmashell keeps the old module
// mapped, and a file rewritten in place under it can crash it.
std::expected<void, QString> installPackage(const Layout& layout) {
    const fs::path parent = layout.package.parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) return std::unexpected(QStringLiteral("Could not create %1: %2").arg(shown(parent), why(ec)));
    const fs::path incoming = parent / ("." + layout.package.filename().string() + ".new");
    fs::remove_all(incoming, ec);
    fs::copy(layout.bundlePackage, incoming, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        std::error_code cleanup;
        fs::remove_all(incoming, cleanup);
        return std::unexpected(QStringLiteral("Could not copy the wallpaper plugin into %1: %2").arg(shown(parent), why(ec)));
    }
    if (const std::error_code replaced = replaceWhole(incoming, layout.package)) {
        return std::unexpected(QStringLiteral("Could not install the wallpaper plugin at %1: %2").arg(shown(layout.package), why(replaced)));
    }
    return {};
}

// Through /proc, so a daemon whose copy was deleted under it, or one run from anywhere else, is not it.
bool runsExecutable(uint pid, const fs::path& executable) {
    if (pid == 0) return false;
    std::error_code ec;
    return fs::equivalent(fs::path("/proc") / std::to_string(pid) / "exe", executable, ec) && !ec;
}

// The copy under runtime/ a failed run goes back to: the one process `pid` provably runs, else the
// one `current` names. Only a complete copy other than `own` counts, and none when the process
// already runs `own`'s: there is then nothing this run moves the daemon away from.
std::optional<fs::path> previousCopy(const Layout& layout, uint pid, const QString& own) {
    const fs::path ownName = fsPath(own);
    if (runsExecutable(pid, layout.runtime / ownName / kDaemonExecutable)) return std::nullopt;
    std::error_code ec;
    // exe is <folder>/bin/wallpaperx-daemon; a deleted or replaced one, or one elsewhere, fails runsExecutable.
    fs::path folder = fs::read_symlink(fs::path("/proc") / std::to_string(pid) / "exe", ec).parent_path().parent_path().filename();
    if (pid == 0 || ec || !runsExecutable(pid, layout.runtime / folder / kDaemonExecutable)) {
        folder = fs::read_symlink(layout.runtime / "current", ec);
        if (ec) return std::nullopt;
    }
    if (!isFolderName(shown(folder)) || folder == ownName || !isCompleteCopy(layout.runtime / folder)) return std::nullopt;
    return folder;
}

QDBusMessage managerMessage(const QString& method, const QVariantList& arguments) {
    QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(kSystemd), QString::fromLatin1(kManagerPath),
                                                       QString::fromLatin1(kManager), method);
    call.setArguments(arguments);
    return call;
}

// A D-Bus error's text may end without a period.
QString withSentence(const QString& message, const QString& sentence) {
    return message + (message.endsWith(u'.') ? QStringLiteral(" ") : QStringLiteral(". ")) + sentence;
}

QString switchedBack(const QStringList& problems) {
    if (problems.isEmpty()) return QStringLiteral("Your previous copy of it is kept.");
    return QStringLiteral("Setup pointed the service back at your previous copy of it, but %1.").arg(problems.join(QStringLiteral("; ")));
}

} // namespace

std::expected<QByteArray, QString> serviceUnitText(const QString& library) {
    QFile file(QString::fromLatin1(kUnitTemplate));
    if (!file.open(QIODevice::ReadOnly)) return std::unexpected(QStringLiteral("This build carries no service template."));
    QString text = QString::fromUtf8(file.readAll());

    const QString exec = library + QStringLiteral("/runtime/current/") + QLatin1String(kDaemonExecutable);
    // What systemd refuses in an executable path however it is quoted, and * ? [, its glob characters.
    static const QRegularExpression refused(QStringLiteral("[\\x00-\\x1f\"'*?\\[\\\\\\x7f]"));
    if (exec.contains(refused) || QString::fromUtf8(QFile::encodeName(exec)) != exec) {
        return std::unexpected(QStringLiteral("systemd cannot run a program from %1: the folder's name has a quote, a "
                                              "backslash, one of * ? [ or a character that is not text.").arg(exec));
    }
    QString escaped = exec;
    escaped.replace(u'%', QStringLiteral("%%"));
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9/._+,:=@~-]+$"));
    // Quoted, with the ":" prefix that keeps a $ in the path from being read as a variable.
    const QString execStart = plain.match(exec).hasMatch() ? exec : QStringLiteral(":\"%1\"").arg(escaped);
    text.replace(QStringLiteral("@WALLPAPERX_DAEMON_EXEC_START@"), execStart);
    text.replace(QStringLiteral("@WALLPAPERX_DAEMON_EXECUTABLE@"), escaped);
    if (text.contains(QStringLiteral("@WALLPAPERX_"))) return std::unexpected(QStringLiteral("This build's service template has a value setup does not fill."));
    return text.toUtf8();
}

std::expected<SetupVersion, QString> setupVersion(const QString& appVersion, BuildKind kind, const QString& bundle) {
    const fs::path daemon = fsPath(bundle) / "daemon";
    const std::optional<QString> id = buildIdIn(daemon);
    if (!id && kind == BuildKind::Release) return std::unexpected(noBuildId(daemon));
    if (!id) {
        return std::unexpected(QStringLiteral("This source build's bundle has no valid build id at %1. Rebuild the backend, "
                                              "then the app.").arg(shown(daemon / "build-id")));
    }
    const QString folder = appVersion + u'+' + *id;
    return SetupVersion{kind == BuildKind::Release ? appVersion : folder, folder};
}

SetupPaths SetupPaths::forThisApp() {
    return {QCoreApplication::applicationDirPath(), defaultLibraryDirectory(),
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation), QStringLiteral("/.flatpak-info")};
}

std::optional<QString> unsupportedSession(const QProcessEnvironment& environment, bool flatpakInfoExists) {
    if (flatpakInfoExists || environment.contains(QStringLiteral("FLATPAK_ID"))) {
        return QStringLiteral("WallpaperX does not work with the Flatpak version of Steam, whose sandbox keeps it from "
                              "installing its wallpaper service. Install Steam from your distribution's own packages "
                              "and install WallpaperX there.");
    }
    const QStringList desktops = environment.value(QStringLiteral("XDG_CURRENT_DESKTOP")).split(u':');
    if (!desktops.contains(QStringLiteral("KDE")) || environment.value(QStringLiteral("KDE_SESSION_VERSION")) != QLatin1String("6")) {
        return QStringLiteral("WallpaperX needs the KDE Plasma 6 desktop, and this session is not one.");
    }
    if (environment.value(QStringLiteral("XDG_SESSION_TYPE")) != QLatin1String("wayland") &&
        environment.value(QStringLiteral("WAYLAND_DISPLAY")).isEmpty()) {
        return QStringLiteral("WallpaperX needs a Wayland session. Log out, choose Plasma (Wayland) on the login "
                              "screen, and start WallpaperX again.");
    }
    return std::nullopt;
}

FirstRunSetup::FirstRunSetup(QDBusConnection bus, SetupPaths paths, std::expected<SetupVersion, QString> version,
                             QProcessEnvironment environment,
                             QObject* parent)
    : QObject(parent)
    , m_bus(std::move(bus))
    , m_paths(std::move(paths))
    , m_version(std::move(version))
    , m_environment(std::move(environment))
    , m_daemonDeadline(new QTimer(this)) {
    m_daemonDeadline->setSingleShot(true);
    m_daemonDeadline->setInterval(kDaemonStartMs);
    connect(m_daemonDeadline, &QTimer::timeout, this, [this] {
        finish(State::Failed, QStringLiteral("The WallpaperX service did not start within %1 seconds.").arg(kDaemonStartMs / 1000));
    });
    auto* watcher = new QDBusServiceWatcher(QString::fromLatin1(kDaemonService), m_bus,
                                            QDBusServiceWatcher::WatchForRegistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, &FirstRunSetup::onDaemonRegistered);
}

FirstRunSetup::~FirstRunSetup() {
    if (m_state != State::Running || !m_rollback) return;
    // A copy or unit write still under way would otherwise undo the switch back when it finished.
    m_work.waitForFinished();
    endRestart();
    // The event loop has ended, so the calls block. "fail": a job already queued on the unit, the
    // session's stop at logout or this run's own restart, is left alone; Restart=on-failure then
    // starts a failing new daemon from `current`, which names the old copy again.
    QStringList problems;
    const auto calls = switchBackFiles(QStringLiteral("fail"), problems);
    if (calls) {
        const QDeadlineTimer deadline(kQuitSwitchBackMs);
        for (const ManagerCall& call : *calls) {
            const QDBusMessage reply = m_bus.call(managerMessage(call.method, call.arguments), QDBus::Block, int(std::max<qint64>(1, deadline.remainingTime())));
            if (reply.type() != QDBusMessage::ReplyMessage && !call.failure.isEmpty()) problems << call.failure.arg(reply.errorMessage());
        }
    }
    qWarning("wallpaperx: setup was ended before it finished. %s", qUtf8Printable(calls ? switchedBack(problems) : calls.error()));
}

FirstRunSetup::Need FirstRunSetup::need() const {
    const Layout layout = layoutOf(m_paths);
    std::error_code ec;
    if (!fs::exists(layout.bundleDaemon, ec) && !fs::exists(layout.bundlePackage, ec)) return Need::NotBundled;
    const std::optional<QByteArray> stamp = readSmallFile(layout.stamp);
    if (!stamp) return Need::FirstRun;
    if (!m_version || stamp->trimmed() != m_version->stamp.toUtf8()) return Need::Update;
    const fs::path current = fs::read_symlink(layout.runtime / "current", ec);
    if (ec || current != fsPath(m_version->folder) || !isIntactCopy(layout.runtime / current, layout.bundleDaemon) ||
        !fs::is_directory(layout.wallpapers, ec) || !fs::is_regular_file(layout.package / "metadata.json", ec)) {
        return Need::Update;
    }
    // The unit's text comes from this build's template, not from the daemon's folder the version follows.
    const auto unit = serviceUnitText(m_paths.library);
    if (!unit || readSmallFile(layout.unitFile, unit->size() + 1) != *unit) return Need::Update;
    // The installed package carries the build id it was copied with, so a rebuilt, repaired or
    // replaced package on either side differs. A daemon already running this version's copy is not
    // restarted for it (startDaemon).
    const std::optional<QString> bundled = buildIdIn(layout.bundlePackage);
    if (!bundled || bundled != buildIdIn(layout.package)) return Need::Update;
    return Need::UpToDate;
}

void FirstRunSetup::start() {
    if (m_state == State::Running) return;
    ++m_run;
    m_restart = {};
    m_endedJobs.clear();
    m_rollback.reset();
    setState(State::Running);
    m_step = Step::Library;
    m_message.clear();
    emit changed();

    if (const auto refusal = unsupportedSession(m_environment, !m_paths.flatpakInfo.isEmpty() && QFileInfo::exists(m_paths.flatpakInfo))) {
        finish(State::Refused, *refusal);
        return;
    }
    if (!m_bus.isConnected()) {
        finish(State::Refused, QStringLiteral("There is no session bus, so WallpaperX cannot reach systemd to install its service."));
        return;
    }
    checkSystemd();
}

void FirstRunSetup::setState(State state) {
    m_state = state;
    if (state == State::Running) {
        m_keepAlive.emplace();
    } else {
        m_keepAlive.reset();
    }
}

void FirstRunSetup::setStep(Step step) {
    if (m_step == step) return;
    m_step = step;
    emit changed();
}

void FirstRunSetup::finish(State state, const QString& message) {
    if (m_state != State::Running) return;
    endRestart();
    if (state == State::Done || !m_rollback) {
        end(state, message);
        return;
    }
    // A new number, so the steps' late answers are ignored while the switch back runs.
    ++m_run;
    switchBack(state, message);
}

void FirstRunSetup::end(State state, const QString& message) {
    m_rollback.reset();
    setState(state);
    m_message = message;
    if (state != State::Done) qWarning("wallpaperx: setup did not finish: %s", qUtf8Printable(m_message));
    emit changed();
    emit finished();
}

std::expected<QList<FirstRunSetup::ManagerCall>, QString> FirstRunSetup::switchBackFiles(const QString& restartMode, QStringList& problems) {
    const Layout layout = layoutOf(m_paths);
    if (const std::error_code ec = pointCurrentAt(layout, m_rollback->copy)) {
        return std::unexpected(QStringLiteral("Setup could not point %1 back at your previous copy of it, %2: %3.")
                                   .arg(shown(layout.runtime / "current"), shown(m_rollback->copy), why(ec)));
    }
    QList<ManagerCall> calls;
    if (m_rollback->unit) {
        if (const auto restored = writeAtomically(layout.unitFile, *m_rollback->unit); !restored) {
            problems << QStringLiteral("its service file could not be put back (%1)").arg(restored.error());
        } else {
            calls << ManagerCall{QStringLiteral("Reload"), {}, QStringLiteral("systemd did not reload the service file put back (%1)")};
        }
    }
    if (m_rollback->restarted) {
        const QString unit = QString::fromLatin1(kUnitName);
        calls << ManagerCall{QStringLiteral("ResetFailedUnit"), {unit}, {}}
              << ManagerCall{QStringLiteral("RestartUnit"), {unit, restartMode}, QStringLiteral("systemd did not restart it (%1)")};
    }
    return calls;
}

void FirstRunSetup::switchBack(State state, const QString& message) {
    QStringList problems;
    auto calls = switchBackFiles(QStringLiteral("replace"), problems);
    if (!calls) {
        end(state, withSentence(message, calls.error()));
        return;
    }
    callInTurn(std::move(*calls), std::move(problems), [this, state, message](const QStringList& found) {
        end(state, withSentence(message, switchedBack(found)));
    });
}

void FirstRunSetup::callInTurn(QList<ManagerCall> calls, QStringList problems, std::function<void(const QStringList&)> done) {
    if (calls.isEmpty()) {
        done(problems);
        return;
    }
    const ManagerCall next = calls.takeFirst();
    callInRun(managerMessage(next.method, next.arguments), {},
              [this, failure = next.failure, calls = std::move(calls), problems = std::move(problems), done = std::move(done)](const QDBusMessage& reply) mutable {
        if (reply.type() != QDBusMessage::ReplyMessage && !failure.isEmpty()) problems << failure.arg(reply.errorMessage());
        callInTurn(std::move(calls), std::move(problems), std::move(done));
    }, kSwitchBackCallMs);
}

void FirstRunSetup::disconnectJobs() {
    m_bus.disconnect(QString::fromLatin1(kSystemd), QString::fromLatin1(kManagerPath), QString::fromLatin1(kManager),
                     QStringLiteral("JobRemoved"), this, SLOT(onJobRemoved(uint, QDBusObjectPath, QString, QString)));
}

void FirstRunSetup::endRestart() {
    m_daemonDeadline->stop();
    if (std::holds_alternative<AwaitingJob>(m_restart)) disconnectJobs();
    m_restart = {};
}

template <typename Work, typename Next>
void FirstRunSetup::runOffThread(Step step, Work work, Next next) {
    using Result = std::invoke_result_t<Work&>;
    setStep(step);
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, run = m_run, next = std::move(next)] {
        watcher->deleteLater();
        if (run != m_run || m_state != State::Running) return;
        Result result = watcher->result();
        if (!result) {
            finish(State::Failed, result.error());
            return;
        }
        if constexpr (std::is_void_v<typename Result::value_type>) {
            next();
        } else {
            next(std::move(*result));
        }
    });
    const QFuture<Result> future = QtConcurrent::run(std::move(work));
    m_work = QFuture<void>(future);
    watcher->setFuture(future);
}

void FirstRunSetup::checkSystemd() {
    const QDBusMessage ask = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                                            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"))
                             << QString::fromLatin1(kSystemd);
    callInRun(ask, {}, [this](const QDBusMessage& reply) {
        if (reply.type() != QDBusMessage::ReplyMessage || !reply.arguments().value(0).toBool()) {
            finish(State::Refused, QStringLiteral("WallpaperX runs its wallpaper service through systemd, and systemd is "
                                                  "not running a session for you here."));
            return;
        }
        const Layout layout = layoutOf(m_paths);
        const auto library = [layout]() -> std::expected<void, QString> {
            if (const auto complete = checkBundle(layout); !complete) return complete;
            return createLibrary(layout);
        };
        runOffThread(Step::Library, library, [this, layout] {
            setStep(Step::Daemon);
            findPreviousCopy([this, layout] {
                runOffThread(Step::Daemon, [layout, version = m_version] { return copyDaemon(layout, version); }, [this] { installService(); });
            });
        });
    });
}

// Read before anything is changed. Neither systemd answer is needed: without one the process is
// unknown and `current` alone decides.
void FirstRunSetup::findPreviousCopy(std::function<void()> next) {
    if (!m_version) {
        next();
        return;
    }
    const auto decide = [this, next](uint pid) {
        if (auto copy = previousCopy(layoutOf(m_paths), pid, m_version->folder)) m_rollback = Rollback{std::move(*copy)};
        next();
    };
    callManager(QStringLiteral("GetUnit"), {QString::fromLatin1(kUnitName)}, {}, [this, decide](const QDBusMessage& unit) {
        if (unit.type() != QDBusMessage::ReplyMessage) {
            decide(0);
            return;
        }
        m_unitPath = unit.arguments().value(0).value<QDBusObjectPath>().path();
        readUnitProperty(QString::fromLatin1(kServiceInterface), QStringLiteral("MainPID"), {},
                         [decide](const QVariant& pid) { decide(pid.toUInt()); });
    });
}

void FirstRunSetup::callInRun(const QDBusMessage& message, const QString& failure, std::function<void(const QDBusMessage&)> done,
                              int timeoutMs) {
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, timeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, failure, run = m_run, done = std::move(done)] {
        watcher->deleteLater();
        if (run != m_run || m_state != State::Running) return;
        const QDBusMessage reply = watcher->reply();
        if (reply.type() != QDBusMessage::ReplyMessage && !failure.isEmpty()) {
            finish(State::Failed, QStringLiteral("%1: %2").arg(failure, reply.errorMessage()));
            return;
        }
        done(reply);
    });
}

void FirstRunSetup::callManager(const QString& method, const QVariantList& arguments, const QString& failure,
                                std::function<void(const QDBusMessage&)> done) {
    callInRun(managerMessage(method, arguments), failure, std::move(done));
}

void FirstRunSetup::readUnitProperty(const QString& interface, const QString& property, const QString& failure,
                                     std::function<void(const QVariant&)> done) {
    const QDBusMessage get = QDBusMessage::createMethodCall(QString::fromLatin1(kSystemd), m_unitPath,
                                                            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"))
                             << interface << property;
    callInRun(get, failure, [done = std::move(done)](const QDBusMessage& reply) {
        done(reply.type() == QDBusMessage::ReplyMessage ? reply.arguments().value(0).value<QDBusVariant>().variant() : QVariant());
    });
}

void FirstRunSetup::installService() {
    const auto text = serviceUnitText(m_paths.library);
    if (!text) {
        finish(State::Failed, text.error());
        return;
    }
    const Layout layout = layoutOf(m_paths);
    // Read here, not by the write on the pool thread, so a quit during that write can still put it back.
    if (m_rollback) m_rollback->unit = replacedUnitText(layout, *text);
    runOffThread(Step::Service, [layout, text = *text] { return writeUnit(layout, text); }, [this] {
        const QString unit = QString::fromLatin1(kUnitName);
        callManager(QStringLiteral("EnableUnitFiles"), {QStringList{unit}, false, true},
                    QStringLiteral("systemd could not enable the WallpaperX service"), [this, unit](const QDBusMessage&) {
            callManager(QStringLiteral("Reload"), {}, QStringLiteral("systemd could not reload its services"), [this, unit](const QDBusMessage&) {
                callManager(QStringLiteral("LoadUnit"), {unit}, QStringLiteral("systemd could not load the WallpaperX service"),
                            [this](const QDBusMessage& reply) {
                    m_unitPath = reply.arguments().value(0).value<QDBusObjectPath>().path();
                    readUnitProperty(QString::fromLatin1(kUnitInterface), QStringLiteral("FragmentPath"),
                                     QStringLiteral("systemd did not say where it reads the WallpaperX service from"),
                                     [this](const QVariant& fragment) {
                        const fs::path written = layoutOf(m_paths).unitFile;
                        const QString read = fragment.toString();
                        if (read.isEmpty()) {
                            finish(State::Failed, QStringLiteral("systemd does not see the service setup wrote at %1.").arg(shown(written)));
                            return;
                        }
                        std::error_code ec;
                        const bool same = fs::equivalent(fsPath(read), written, ec);
                        if (ec) {
                            finish(State::Failed, QStringLiteral("Setup could not compare the WallpaperX service systemd reads, %1, "
                                                                 "with the one it wrote at %2: %3.")
                                                      .arg(read, shown(written), why(ec)));
                        } else if (!same) {
                            finish(State::Failed, QStringLiteral("systemd reads the WallpaperX service from %1 instead of the one "
                                                                 "setup wrote at %2. Remove %1 and start WallpaperX again.")
                                                      .arg(read, shown(written)));
                        } else {
                            startDaemon();
                        }
                    });
                });
            });
        });
    });
}

// The unit's ExecStart always names runtime/current, so what decides a restart is whether the
// running daemon is this version's copy. A unit whose other settings changed applies them from the
// daemon's next start: restarting for them would blank every screen for no change anyone sees.
void FirstRunSetup::startDaemon() {
    readUnitProperty(QString::fromLatin1(kServiceInterface), QStringLiteral("MainPID"),
                     QStringLiteral("systemd did not say which process runs the WallpaperX service"), [this](const QVariant& pid) {
        const fs::path copy = layoutOf(m_paths).runtime / fsPath(m_version->folder) / kDaemonExecutable;
        if (runsExecutable(pid.toUInt(), copy)) {
            // A rollback is still set only if the daemon came onto this copy during the run (see
            // previousCopy), so a failure before it is confirmed has to start the old one again.
            if (m_rollback) m_rollback->restarted = true;
            awaitDaemon();
        } else {
            restartDaemon();
        }
    });
}

void FirstRunSetup::restartDaemon() {
    m_restart = AwaitingJob{};
    m_bus.connect(QString::fromLatin1(kSystemd), QString::fromLatin1(kManagerPath), QString::fromLatin1(kManager),
                  QStringLiteral("JobRemoved"), this, SLOT(onJobRemoved(uint, QDBusObjectPath, QString, QString)));
    const QString unit = QString::fromLatin1(kUnitName);
    // A unit that used up its start limit refuses a restart until this; one that has not answers an
    // error, which is no reason to stop.
    callManager(QStringLiteral("ResetFailedUnit"), {unit}, {}, [this, unit](const QDBusMessage&) {
        // Job signals are sent only while some client has subscribed.
        callManager(QStringLiteral("Subscribe"), {}, QStringLiteral("systemd would not report on its jobs"), [this, unit](const QDBusMessage&) {
            m_daemonDeadline->start();
            // Counted as asked from here: an error answer, a timeout among them, may still have queued the job.
            if (m_rollback) m_rollback->restarted = true;
            callManager(QStringLiteral("RestartUnit"), {unit, QStringLiteral("replace")},
                        QStringLiteral("systemd could not restart the WallpaperX service"), [this](const QDBusMessage& reply) {
                const QString job = reply.arguments().value(0).value<QDBusObjectPath>().path();
                m_restart = AwaitingJob{job};
                if (const auto ended = m_endedJobs.constFind(job); ended != m_endedJobs.cend()) {
                    onJobRemoved(0, QDBusObjectPath(job), QString::fromLatin1(kUnitName), *ended);
                }
            });
        });
    });
}

void FirstRunSetup::onJobRemoved(uint, const QDBusObjectPath& job, const QString& unit, const QString& result) {
    const auto* awaiting = std::get_if<AwaitingJob>(&m_restart);
    if (m_state != State::Running || awaiting == nullptr || unit != QLatin1String(kUnitName)) return;
    if (awaiting->path.isEmpty()) {
        m_endedJobs.insert(job.path(), result);
        return;
    }
    if (job.path() != awaiting->path) return;
    if (result != QLatin1String("done")) {
        finish(State::Failed, QStringLiteral("systemd could not restart the WallpaperX service (%1).").arg(result));
        return;
    }
    checkStarted();
}

void FirstRunSetup::checkStarted() {
    readUnitProperty(QString::fromLatin1(kUnitInterface), QStringLiteral("ActiveState"),
                     QStringLiteral("systemd did not say whether the WallpaperX service is running"), [this](const QVariant& state) {
        const QString active = state.toString();
        if (active != QLatin1String("active") && active != QLatin1String("activating") && active != QLatin1String("reloading")) {
            finish(State::Failed, QStringLiteral("systemd did not start the WallpaperX service: it is %1. `journalctl --user -u "
                                                 "wallpaperx-daemon` says why.").arg(active));
            return;
        }
        awaitDaemon();
    });
}

void FirstRunSetup::awaitDaemon() {
    if (std::holds_alternative<AwaitingJob>(m_restart)) disconnectJobs();
    m_restart = AwaitingDaemon{};
    if (!m_daemonDeadline->isActive()) m_daemonDeadline->start();
    confirmDaemon();
}

void FirstRunSetup::onDaemonRegistered() {
    if (m_state == State::Running && std::holds_alternative<AwaitingDaemon>(m_restart)) confirmDaemon();
}

// Confirmed when the bus name is held by the process systemd started: an older daemon, or one
// started some other way, does not count.
void FirstRunSetup::confirmDaemon() {
    const QDBusMessage ask = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                                            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetConnectionUnixProcessID"))
                             << QString::fromLatin1(kDaemonService);
    callInRun(ask, {}, [this](const QDBusMessage& owner) {
        // No owner yet: its registration calls this again.
        if (!std::holds_alternative<AwaitingDaemon>(m_restart) || owner.type() != QDBusMessage::ReplyMessage) return;
        const uint ownerPid = owner.arguments().value(0).toUInt();
        readUnitProperty(QString::fromLatin1(kServiceInterface), QStringLiteral("MainPID"),
                         QStringLiteral("systemd did not say which process runs the WallpaperX service"), [this, ownerPid](const QVariant& pid) {
            if (!std::holds_alternative<AwaitingDaemon>(m_restart) || pid.toUInt() == 0 || pid.toUInt() != ownerPid) return;
            endRestart();
            // The new copy is proven: from here a failure keeps it, and the old copies go.
            m_rollback.reset();
            finishFiles();
        });
    });
}

void FirstRunSetup::finishFiles() {
    const Layout layout = layoutOf(m_paths);
    runOffThread(Step::OldCopy, [layout, folder = m_version->folder] { return removeOldCopies(layout, folder); }, [this, layout] {
        runOffThread(Step::Plugin, [layout] { return installPackage(layout); }, [this, layout] {
            runOffThread(Step::Stamp, [layout, stamp = m_version->stamp] { return writeAtomically(layout.stamp, stamp.toUtf8() + '\n'); },
                         [this] { finish(State::Done); });
        });
    });
}

} // namespace wallpaperx
