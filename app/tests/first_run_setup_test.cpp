// SPDX-License-Identifier: GPL-2.0-or-later

// S16.1-S16.34: app/setup/first_run_setup.cpp end to end, in this run's private home and on its
// private bus, with a stand-in for systemd's user manager that owns org.freedesktop.systemd1 itself
// (the bus can start nothing). The files, the `current` link, the unit and the calls are checked.
//
//   wallpaperx-first-run-setup-test [BUNDLE]   BUNDLE: a folder holding daemon/ and plasma/ for the
//                                              "installs" phase to install from instead of its own.
//
// Phases that need a daemon running from the runtime copy use a copy of dbus-test-tool, run as
// `black-hole --session --name=...`: setup compares /proc/<MainPID>/exe with the copy, so it must
// be a real process started from that file.

#include "fake_kscreen.h"
#include "first_run_setup.h"
#include "wpx_fixtures.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QDir>
#include <QCloseEvent>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWindow>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <thread>

#include <dlfcn.h>
#include <unistd.h>

namespace {
// The package-swap fallback is reached only where a filesystem refuses RENAME_EXCHANGE; these let a
// phase refuse it here, and refuse moving the new package in, without touching other renames.
std::atomic<bool> g_refuseExchange{false};
std::atomic<bool> g_refuseMovingPackageIn{false};
std::atomic<int> g_exchangesRefused{0};
std::atomic<int> g_movesRefused{0};
// Refuses moving a new `current` link in, so a phase can make the switch back fail after the switch.
std::atomic<bool> g_refuseCurrentSwitch{false};
std::atomic<int> g_currentSwitchesRefused{0};
// Holds one rename on setup's pool thread until the phase lets it go, so a phase can destroy setup in
// the middle of a file step: 1 holds moving the copy's .partial folder in, 2 the unit file's commit.
std::atomic<int> g_holdRename{0};
std::atomic<bool> g_renameHeld{false};
std::atomic<bool> g_renameReleased{false};
} // namespace

extern "C" int renameat2(int oldDir, const char* oldPath, int newDir, const char* newPath, unsigned int flags) noexcept {
    if (g_refuseExchange && (flags & RENAME_EXCHANGE)) {
        ++g_exchangesRefused;
        errno = EINVAL;
        return -1;
    }
    using Real = int (*)(int, const char*, int, const char*, unsigned int);
    static const Real real = reinterpret_cast<Real>(::dlsym(RTLD_NEXT, "renameat2"));
    return real(oldDir, oldPath, newDir, newPath, flags);
}

extern "C" int rename(const char* from, const char* to) noexcept {
    if (g_refuseMovingPackageIn && std::string_view(from).ends_with("/.com.wallpaperx.wallpaper.new")) {
        ++g_movesRefused;
        errno = EXDEV;
        return -1;
    }
    if (g_refuseCurrentSwitch && std::string_view(from).ends_with("/.current.new")) {
        ++g_currentSwitchesRefused;
        errno = EXDEV;
        return -1;
    }
    if (int hold = g_holdRename.load();
        ((hold == 1 && std::string_view(from).ends_with(".partial")) || (hold == 2 && std::string_view(to).ends_with("/wallpaperx-daemon.service"))) &&
        g_holdRename.compare_exchange_strong(hold, 0)) {
        g_renameHeld = true;
        g_renameHeld.notify_all();
        g_renameReleased.wait(false);
    }
    using Real = int (*)(const char*, const char*);
    static const Real real = reinterpret_cast<Real>(::dlsym(RTLD_NEXT, "rename"));
    return real(from, to);
}

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;
namespace fs = std::filesystem;

namespace {

constexpr const char* kGoals = "S16.1-S16.34 first-run setup";
constexpr const char* kVersion = "9.9.9-test";
constexpr const char* kOld = "0.0.1";
constexpr int kRunMs = 45000; // above setup's 30-second daemon limit
const QString kUnitPath = QStringLiteral("/org/freedesktop/systemd1/unit/wallpaperx_2ddaemon_2eservice");
const QString kNotTheDaemon = QStringLiteral("io.wallpaperx.NotTheDaemon");

// The unit's properties, answered by hand so one answer can be held back. Called on QtDBus's own
// thread, hence the lock.
class UnitObject : public QDBusVirtualObject {
public:
    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != QLatin1String("org.freedesktop.DBus.Properties") || message.member() != QLatin1String("Get")) return false;
        const QString property = message.arguments().value(1).toString();
        QMutexLocker lock(&m_lock);
        QVariant value;
        if (property == QLatin1String("ActiveState")) {
            value = m_activeState;
        } else if (property == QLatin1String("FragmentPath")) {
            value = m_fragmentPath;
        } else if (property == QLatin1String("MainPID")) {
            ++m_mainPidGets;
            if (m_errorNextMainPid) {
                m_errorNextMainPid = false;
                ++m_mainPidErrors;
                connection.send(message.createErrorReply(QDBusError::Failed, QStringLiteral("stand-in refuses this MainPID read")));
                return true;
            }
            if (m_holdNextMainPid) {
                m_holdNextMainPid = false;
                m_held = message;
                m_heldOn = connection;
                return true;
            }
            value = m_mainPid;
        } else {
            return false;
        }
        connection.send(message.createReply(QVariant::fromValue(QDBusVariant(value))));
        return true;
    }

    void setActiveState(const QString& state) { QMutexLocker lock(&m_lock); m_activeState = state; }
    void setFragmentPath(const QString& path) { QMutexLocker lock(&m_lock); m_fragmentPath = path; }
    void setMainPid(uint pid) { QMutexLocker lock(&m_lock); m_mainPid = pid; }
    uint mainPid() { QMutexLocker lock(&m_lock); return m_mainPid; }
    int mainPidGets() { QMutexLocker lock(&m_lock); return m_mainPidGets; }
    void holdNextMainPid() { QMutexLocker lock(&m_lock); m_holdNextMainPid = true; }
    void errorNextMainPid() { QMutexLocker lock(&m_lock); m_errorNextMainPid = true; }
    int mainPidErrors() { QMutexLocker lock(&m_lock); return m_mainPidErrors; }
    bool holding() { QMutexLocker lock(&m_lock); return m_held.type() == QDBusMessage::MethodCallMessage; }
    // Answers the held question now, with `pid`.
    void release(uint pid) {
        QMutexLocker lock(&m_lock);
        m_heldOn.send(m_held.createReply(QVariant::fromValue(QDBusVariant(QVariant(pid)))));
        m_held = QDBusMessage();
    }

private:
    QMutex m_lock;
    QString m_activeState = QStringLiteral("inactive");
    QString m_fragmentPath;
    uint m_mainPid = 0;
    int m_mainPidGets = 0;
    bool m_holdNextMainPid = false;
    bool m_errorNextMainPid = false;
    int m_mainPidErrors = 0;
    QDBusMessage m_held;
    QDBusConnection m_heldOn{QString()};
};

// How the stand-in's restart behaves. Here: the "daemon" is this process taking the name on a second
// connection. Process: a copy of dbus-test-tool started from runtime/current. None: no one takes the
// name. OtherPid: this process takes it, but MainPID names another process.
enum class Daemon { Here, Process, None, OtherPid };
struct Behaviour {
    QString jobResult = QStringLiteral("done");
    QString activeAfter = QStringLiteral("active");
    Daemon daemon = Daemon::Here;
    bool signalBeforeReply = false;
    bool resetFailedErrors = false;
    bool restartErrors = false;
    QString fragmentPath; // reported instead of the unit file when set
    int registerDelayMs = 0;
    bool holdConfirmingMainPid = false;
    // Answers a RestartUnit after the first (the rollback's) this much later.
    int delayRollbackRestartMs = 0;
};

class StandInSystemd : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.systemd1.Manager")
public:
    StandInSystemd(QString unitFile, QString runtime, Behaviour behaviour = {})
        : behaviour(std::move(behaviour))
        , m_bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("standin-systemd")))
        , m_unitFile(std::move(unitFile))
        , m_runtime(std::move(runtime)) {}
    ~StandInSystemd() override {
        stopProcesses();
        m_bus.unregisterObject(kUnitPath);
        QDBusConnection::disconnectFromBus(QStringLiteral("standin-daemon"));
        QDBusConnection::disconnectFromBus(QStringLiteral("standin-systemd"));
    }

    bool own() {
        return m_bus.registerObject(QStringLiteral("/org/freedesktop/systemd1"), this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals) &&
               m_bus.registerVirtualObject(kUnitPath, &unit) && m_bus.registerService(QStringLiteral("org.freedesktop.systemd1"));
    }

    // Starts `program` as a black-hole holding `name`, and waits until the bus says that process holds it.
    QProcess* launch(const QString& program, const QString& name) {
        auto* process = new QProcess(this);
        process->setProgram(program);
        process->setArguments({QStringLiteral("black-hole"), QStringLiteral("--session"), QStringLiteral("--name=") + name});
        process->setProcessChannelMode(QProcess::ForwardedChannels);
        process->start();
        if (!process->waitForStarted(5000)) return nullptr;
        m_processes << process;
        const uint pid = uint(process->processId());
        if (!waitUntil([&] { return QDBusConnection::sessionBus().interface()->servicePid(name).value() == pid; }, 5000)) return nullptr;
        return process;
    }
    void runAsMain(QProcess* process) {
        mainProcess = process;
        unit.setMainPid(uint(process->processId()));
        unit.setActiveState(QStringLiteral("active"));
    }

    Behaviour behaviour;
    UnitObject unit;
    QStringList calls;
    QProcess* mainProcess = nullptr;
    std::function<void()> onRestart;
    std::function<void()> onReload;
    // Set when the delayed rollback RestartUnit is answered.
    bool rollbackAnswered = false;

public slots:
    bool EnableUnitFiles(const QStringList& files, bool runtime, bool force) {
        calls << QStringLiteral("EnableUnitFiles %1 runtime=%2 force=%3").arg(files.join(u','), QString::number(runtime), QString::number(force));
        return true;
    }
    void Reload() {
        calls << QStringLiteral("Reload");
        if (onReload) onReload();
        // Loaded from where systemd would find it, as long as it is there.
        unit.setFragmentPath(!behaviour.fragmentPath.isEmpty() ? behaviour.fragmentPath : QFile::exists(m_unitFile) ? m_unitFile : QString());
    }
    QDBusObjectPath LoadUnit(const QString& name) {
        calls << QStringLiteral("LoadUnit %1").arg(name);
        return QDBusObjectPath(kUnitPath);
    }
    // Not recorded: a question, like the unit's properties. Loaded once its file exists, as at a
    // login after an install.
    QDBusObjectPath GetUnit(const QString& name) {
        if (!QFile::exists(m_unitFile)) {
            sendErrorReply(QStringLiteral("org.freedesktop.systemd1.NoSuchUnit"), QStringLiteral("Unit %1 not loaded.").arg(name));
            return {};
        }
        return QDBusObjectPath(kUnitPath);
    }
    void ResetFailedUnit(const QString& name) {
        calls << QStringLiteral("ResetFailedUnit %1").arg(name);
        if (behaviour.resetFailedErrors) sendErrorReply(QDBusError::Failed, QStringLiteral("Unit wallpaperx-daemon.service is not loaded."));
    }
    void Subscribe() { calls << QStringLiteral("Subscribe"); }
    // Not recorded: answered after every call sent before it on the same connection (see drained()).
    void Barrier() {}
    QDBusObjectPath RestartUnit(const QString& name, const QString& mode) {
        calls << QStringLiteral("RestartUnit %1 %2").arg(name, mode);
        if (onRestart) onRestart();
        if (behaviour.delayRollbackRestartMs > 0 && m_jobs > 0) {
            setDelayedReply(true);
            const QDBusMessage call = message();
            QTimer::singleShot(behaviour.delayRollbackRestartMs, this, [this, call] {
                rollbackAnswered = true;
                m_bus.send(call.createReply(QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/freedesktop/systemd1/job/99")))));
            });
            return {};
        }
        if (behaviour.restartErrors) {
            sendErrorReply(QDBusError::Failed, QStringLiteral("stand-in refuses the restart"));
            return {};
        }
        const uint id = ++m_jobs;
        const QDBusObjectPath job(QStringLiteral("/org/freedesktop/systemd1/job/%1").arg(id));
        auto end = [this, id, job, name] {
            unit.setActiveState(behaviour.activeAfter);
            startDaemon();
            emit JobRemoved(id, job, name, behaviour.jobResult);
        };
        // Signal before reply: both go out on this connection in order, so setup sees the job end first.
        if (behaviour.signalBeforeReply) {
            end();
        } else {
            QTimer::singleShot(0, this, end);
        }
        return job;
    }

signals:
    void JobRemoved(uint id, const QDBusObjectPath& job, const QString& unit, const QString& result);

private:
    void stopProcesses() {
        for (QProcess* process : std::as_const(m_processes)) {
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(5000);
            }
        }
    }
    void startDaemon() {
        switch (behaviour.daemon) {
        case Daemon::Here:
        case Daemon::OtherPid:
            unit.setMainPid(behaviour.daemon == Daemon::Here ? uint(::getpid()) : uint(::getppid()));
            QTimer::singleShot(behaviour.registerDelayMs, this, [this] {
                if (behaviour.holdConfirmingMainPid) unit.holdNextMainPid();
                QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("standin-daemon"))
                    .registerService(QString::fromLatin1(FirstRunSetup::kDaemonService));
            });
            break;
        case Daemon::None:
            unit.setMainPid(uint(::getpid()));
            break;
        case Daemon::Process: {
            // As systemd does: the old main process goes, the unit's ExecStart starts a new one. No
            // wait for the name here, so no nested event loop runs inside this bus call.
            stopProcesses();
            auto* process = new QProcess(this);
            process->setProgram(m_runtime + QStringLiteral("/current/bin/wallpaperx-daemon"));
            process->setArguments({QStringLiteral("black-hole"), QStringLiteral("--session"),
                                   QStringLiteral("--name=") + QString::fromLatin1(FirstRunSetup::kDaemonService)});
            process->setProcessChannelMode(QProcess::ForwardedChannels);
            process->start();
            const bool started = process->waitForStarted(5000);
            m_processes << process;
            mainProcess = started ? process : nullptr;
            unit.setMainPid(started ? uint(process->processId()) : 0);
            break;
        }
        }
    }

    QDBusConnection m_bus;
    QString m_unitFile;
    QString m_runtime;
    QList<QProcess*> m_processes;
    uint m_jobs = 0;
};

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QString readAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

fs::path native(const QString& path) { return fs::path(QFile::encodeName(path).toStdString()); }

// A source build's version: the stamp records the folder's name. Phases lay copies out under it by hand.
SetupVersion asSource(const QString& version) { return {version, version}; }

// daemon/ with a library behind a soname link, plasma/ with the module folder, each with a build id.
// `daemonFrom`: a program to copy as the daemon instead of a script.
bool makeBundle(const QString& bundle, const QString& daemonFrom = {}) {
    const QString exe = bundle + QStringLiteral("/daemon/bin/wallpaperx-daemon");
    std::error_code ec;
    bool ok = true;
    if (daemonFrom.isEmpty()) {
        ok = writeFile(exe, "#!/bin/sh\nexit 0\n");
    } else {
        QDir().mkpath(QFileInfo(exe).path());
        ok = QFile::copy(daemonFrom, exe);
    }
    ok = ok && QFile::setPermissions(exe, QFile::permissions(exe) | QFile::ExeOwner) &&
         writeFile(bundle + QStringLiteral("/daemon/lib/libstandin.so.1.2"), "lib") &&
         writeFile(bundle + QStringLiteral("/daemon/share/wallpaperx/shaders/a.spv"), "spv") &&
         writeFile(bundle + QStringLiteral("/daemon/build-id"), "0011223344556677\n") &&
         writeFile(bundle + QStringLiteral("/plasma/wallpapers/com.wallpaperx.wallpaper/metadata.json"), "{}") &&
         writeFile(bundle + QStringLiteral("/plasma/wallpapers/com.wallpaperx.wallpaper/build-id"), "0123456789abcdef\n") &&
         writeFile(bundle + QStringLiteral("/plasma/wallpapers/com.wallpaperx.wallpaper/contents/ui/WallpaperXPlugin/qmldir"),
                   "module WallpaperXPlugin\n");
    fs::create_symlink("libstandin.so.1.2", native(bundle + QStringLiteral("/daemon/lib/libstandin.so.1")), ec);
    return ok && !ec;
}

QProcessEnvironment plasmaWayland() {
    QProcessEnvironment env;
    env.insert(QStringLiteral("XDG_CURRENT_DESKTOP"), QStringLiteral("KDE"));
    env.insert(QStringLiteral("KDE_SESSION_VERSION"), QStringLiteral("6"));
    env.insert(QStringLiteral("XDG_SESSION_TYPE"), QStringLiteral("wayland"));
    return env;
}

QString unitValue(const QString& unit, const QString& key) {
    for (const QString& line : unit.split(u'\n')) {
        if (line.startsWith(key + u'=')) return line.mid(key.size() + 1);
    }
    return {};
}

// Every entry under `root`, with a link's target or a file's content hash and permissions.
QStringList snapshot(const QString& root) {
    QStringList out;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo entry = it.nextFileInfo();
        QString line = entry.filePath();
        if (entry.isSymLink()) {
            line += QStringLiteral(" -> ") + entry.symLinkTarget();
        } else if (entry.isFile()) {
            QFile file(entry.filePath());
            line += u' ' + QString::number(int(entry.permissions()), 16) + u' ' +
                    QString::fromLatin1(file.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex() : "?");
        }
        out << line;
    }
    out.sort();
    return out;
}

// One phase's own folders: its bundle, its library and its data home, under `root/name`.
struct Install {
    SetupPaths paths;
    QString root;
    QString runtime() const { return paths.library + QStringLiteral("/runtime"); }
    QString package() const { return paths.dataHome + QStringLiteral("/plasma/wallpapers/com.wallpaperx.wallpaper"); }
    QString unitFile() const { return paths.dataHome + QStringLiteral("/systemd/user/wallpaperx-daemon.service"); }
    QString bundlePackage() const { return paths.bundle + QStringLiteral("/plasma/wallpapers/com.wallpaperx.wallpaper"); }
    QString stamp() const { return runtime() + QStringLiteral("/stamp"); }
};

QString g_scratch;

std::optional<Install> freshInstall(const QString& name, const QString& daemonFrom = {}, const QString& library = {}) {
    Install install;
    install.root = g_scratch + u'/' + name;
    install.paths = {install.root + QStringLiteral("/bundle"), library.isEmpty() ? install.root + QStringLiteral("/data/wallpaperx") : library,
                     install.root + QStringLiteral("/data")};
    if (!makeBundle(install.paths.bundle, daemonFrom)) {
        harnessFailure(qPrintable(QStringLiteral("the bundle for phase %1 could not be written").arg(name)));
        return std::nullopt;
    }
    return install;
}

// What a finished run at `version` leaves, laid by hand: the copy, `current`, the stamp, the
// wallpapers folder, the unit and the package.
bool layInstalled(const Install& install, const QString& version) {
    std::error_code ec;
    fs::create_directories(native(install.runtime()), ec);
    fs::copy(native(install.paths.bundle + QStringLiteral("/daemon")), native(install.runtime() + u'/' + version),
             fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) return false;
    fs::remove(native(install.runtime() + QStringLiteral("/current")), ec);
    fs::create_symlink(native(version), native(install.runtime() + QStringLiteral("/current")), ec);
    if (ec) return false;
    fs::create_directories(native(install.package()).parent_path(), ec);
    fs::copy(native(install.bundlePackage()), native(install.package()), fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    const auto unit = serviceUnitText(install.paths.library);
    return !ec && unit && writeFile(install.stamp(), version.toUtf8() + '\n') && QDir().mkpath(install.paths.library + QStringLiteral("/wallpapers")) &&
           writeFile(install.unitFile(), *unit);
}

// Starts a run and waits for its end; false if it did not end in time.
bool runToEnd(FirstRunSetup& setup, int ms = kRunMs) {
    bool ended = false;
    const auto connection = QObject::connect(&setup, &FirstRunSetup::finished, [&] { ended = true; });
    setup.start();
    const bool inTime = waitUntil([&] { return ended; }, ms);
    QObject::disconnect(connection);
    return inTime;
}

QString describe(const FirstRunSetup& setup) {
    return QStringLiteral("state %1, step %2, said \"%3\"").arg(int(setup.state())).arg(int(setup.step())).arg(setup.message());
}

QString dbusTestTool() {
    static const QString path = QStandardPaths::findExecutable(QStringLiteral("dbus-test-tool"));
    return path;
}

// True once the stand-in has handled every call setup sent before now, so a call sent without
// waiting is counted too: the bus keeps one sender's messages to one receiver in order, and the
// stand-in handles them in order.
bool drained() {
    const QDBusMessage barrier = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"), QStringLiteral("/org/freedesktop/systemd1"),
                                                                QStringLiteral("org.freedesktop.systemd1.Manager"), QStringLiteral("Barrier"));
    QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(barrier);
    return waitUntil([&] { return call.isFinished(); }, 5000) && !call.isError();
}

// Setup switches back first to the copy the unit's main process runs, so a phase updating from 0.0.1
// runs a daemon from it: a copy of dbus-test-tool laid there (freshInstall with dbusTestTool()),
// holding a name that is not the daemon's.
QProcess* runOldAsMain(const Install& install, StandInSystemd& systemd) {
    QProcess* old = systemd.launch(install.runtime() + QStringLiteral("/0.0.1/bin/wallpaperx-daemon"), kNotTheDaemon);
    if (old != nullptr) systemd.runAsMain(old);
    return old;
}

int countOf(const QStringList& calls, const QString& call) { return int(calls.count(call)); }
const QString kRestartCall = QStringLiteral("RestartUnit wallpaperx-daemon.service replace");
const QString kResetCall = QStringLiteral("ResetFailedUnit wallpaperx-daemon.service");

// snapshot() with `root` cut from each line, so two folders' contents compare.
QStringList relativeSnapshot(const QString& root) {
    QStringList out = snapshot(root);
    for (QString& line : out) line.replace(root, QString());
    return out;
}

// What reached Qt's message handler while a phase ran.
QStringList g_warnings;
QtMessageHandler g_previousHandler = nullptr;
void captureWarning(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    g_warnings << message;
    if (g_previousHandler != nullptr) g_previousHandler(type, context, message);
}

// S16.9
void refused() {
    const auto install = freshInstall(QStringLiteral("refused"));
    if (!install || !layInstalled(*install, QString::fromLatin1(kOld))) {
        fault(kGoals, QStringLiteral("could not lay out the refused phase's previous install"));
        return;
    }
    QProcessEnvironment flatpak = plasmaWayland();
    flatpak.insert(QStringLiteral("FLATPAK_ID"), QStringLiteral("com.valvesoftware.Steam"));
    QProcessEnvironment gnome = plasmaWayland();
    gnome.insert(QStringLiteral("XDG_CURRENT_DESKTOP"), QStringLiteral("GNOME"));
    QProcessEnvironment plasma5 = plasmaWayland();
    plasma5.insert(QStringLiteral("KDE_SESSION_VERSION"), QStringLiteral("5"));
    QProcessEnvironment x11 = plasmaWayland();
    x11.insert(QStringLiteral("XDG_SESSION_TYPE"), QStringLiteral("x11"));
    QProcessEnvironment waylandByDisplay = x11;
    waylandByDisplay.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("wayland-0"));

    expect(unsupportedSession(plasmaWayland(), true).value_or(QString()).contains(QLatin1String("Flatpak")),
           QStringLiteral("/.flatpak-info alone is refused as Flatpak"));
    expect(!unsupportedSession(waylandByDisplay, false), QStringLiteral("XDG_SESSION_TYPE=x11 with WAYLAND_DISPLAY set is accepted as Wayland"));
    expect(!unsupportedSession(plasmaWayland(), false), QStringLiteral("a Plasma 6 Wayland session is accepted"));

    // GOAL: S16.9 -- each refusal says its reason and touches no file, even with an install to damage.
    // WHY-THIS-SHAPE: a whole-tree snapshot before and after, not "no stamp": a refusal that created the
    //   library or rewrote `current` would pass a stamp-only check.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp start() rewriting the stamp before the session
    //   check: red, "FAIL: refused for not Plasma 6 touched no file" (creating the library instead
    //   stayed green: the install already has it). The FLATPAK_ID test and the WAYLAND_DISPLAY fallback
    //   each dropped: red on their own lines. 2026-11-06, checkSystemd()'s NameHasOwner answer ignored:
    //   red, "FAIL: refused without a systemd user manager: state 4" and "... for no systemd touched no file".
    const QString flatpakInfo = install->root + QStringLiteral("/flatpak-info");
    if (!writeFile(flatpakInfo, "[Application]\n")) {
        fault(kGoals, QStringLiteral("could not write the stand-in /.flatpak-info"));
        return;
    }
    const QStringList before = snapshot(install->root);
    {
        // GOAL: S16.25 -- a SetupPaths::flatpakInfo naming an existing file is refused as Flatpak (the
        //   no-systemd case below shows a name with no file behind it is not).
        // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp start() passing `false` for the marker file:
        //   red, "FAIL: a flatpakInfo naming an existing file is refused as Flatpak: state 1, step 0".
        SetupPaths paths = install->paths;
        paths.flatpakInfo = flatpakInfo;
        FirstRunSetup setup(QDBusConnection::sessionBus(), paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
        setup.start();
        expect(setup.state() == FirstRunSetup::State::Refused && setup.message().contains(QLatin1String("Flatpak")),
               QStringLiteral("a flatpakInfo naming an existing file is refused as Flatpak: %1").arg(describe(setup)));
        expect(snapshot(install->root) == before, QStringLiteral("refused for the Flatpak marker file touched no file"));
    }
    const struct {
        const char* reason;
        QProcessEnvironment environment;
        const char* says;
    } cases[] = {{"Flatpak (FLATPAK_ID)", flatpak, "Flatpak"},
                 {"not Plasma (GNOME)", gnome, "Plasma 6"},
                 {"not Plasma 6", plasma5, "Plasma 6"},
                 {"not Wayland", x11, "Wayland"}};
    for (const auto& c : cases) {
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), c.environment);
        setup.start();
        expect(setup.state() == FirstRunSetup::State::Refused && setup.message().contains(QLatin1String(c.says)),
               QStringLiteral("refused at once for %1, naming %2: %3").arg(QLatin1String(c.reason), QLatin1String(c.says), describe(setup)));
        expect(snapshot(install->root) == before, QStringLiteral("refused for %1 touched no file").arg(QLatin1String(c.reason)));
    }
    // No stand-in owns org.freedesktop.systemd1 yet: this phase runs before any that make one. The
    // marker file named here does not exist, so this run gets past the Flatpak test.
    SetupPaths noMarker = install->paths;
    noMarker.flatpakInfo = install->root + QStringLiteral("/no-flatpak-info");
    FirstRunSetup setup(QDBusConnection::sessionBus(), noMarker, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
    const bool ended = runToEnd(setup, 10000);
    expect(ended && setup.state() == FirstRunSetup::State::Refused && setup.message().contains(QLatin1String("systemd")) &&
               !setup.message().contains(QLatin1String("Flatpak")),
           QStringLiteral("refused without a systemd user manager: %1").arg(describe(setup)));
    expect(snapshot(install->root) == before, QStringLiteral("refused for no systemd touched no file"));
}

// S16.1 (first run), S16.6 (the call order), S16.8 (package replaced whole)
void installs(const QString& ownBundle) {
    auto install = freshInstall(QStringLiteral("installs"));
    if (!install) return;
    if (!ownBundle.isEmpty()) install->paths.bundle = ownBundle;
    const QString runtime = install->runtime();
    // A previous version's copy, `current` naming it, and a previous package with a file the new one lacks.
    std::error_code ec;
    if (!writeFile(runtime + QStringLiteral("/0.0.1/bin/wallpaperx-daemon"), "old") || !writeFile(install->package() + QStringLiteral("/old-only"), "old")) {
        fault(kGoals, QStringLiteral("could not lay out the previous install"));
        return;
    }
    fs::create_symlink("0.0.1", native(runtime + QStringLiteral("/current")), ec);

    StandInSystemd systemd(install->unitFile(), runtime);
    if (!systemd.own()) {
        notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
    expect(setup.need() == FirstRunSetup::Need::FirstRun, QStringLiteral("with no stamp it is a first run: %1").arg(int(setup.need())));
    const bool ended = runToEnd(setup);
    expect(ended && setup.state() == FirstRunSetup::State::Done, QStringLiteral("setup finished: %1").arg(describe(setup)));

    const QString version = QString::fromLatin1(kVersion);
    const QString unit = readAll(install->unitFile());
    const QString exec = runtime + QStringLiteral("/current/bin/wallpaperx-daemon");
    const QStringList expectedCalls{QStringLiteral("EnableUnitFiles wallpaperx-daemon.service runtime=0 force=1"), QStringLiteral("Reload"),
                                    QStringLiteral("LoadUnit wallpaperx-daemon.service"), QStringLiteral("ResetFailedUnit wallpaperx-daemon.service"),
                                    QStringLiteral("Subscribe"), QStringLiteral("RestartUnit wallpaperx-daemon.service replace")};
    expect(QFileInfo(install->paths.library + QStringLiteral("/wallpapers")).isDir(), QStringLiteral("the wallpapers folder exists"));
    expect(QFileInfo(runtime + u'/' + version + QStringLiteral("/bin/wallpaperx-daemon")).isExecutable(), QStringLiteral("the daemon was copied, executable"));
    if (ownBundle.isEmpty()) {
        expect(QFileInfo(runtime + u'/' + version + QStringLiteral("/lib/libstandin.so.1")).isSymLink(), QStringLiteral("a library link was copied as a link"));
    }
    expect(QFile::symLinkTarget(runtime + QStringLiteral("/current")) == runtime + u'/' + version, QStringLiteral("current names the new copy"));
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp removeOldCopies() listing a folder that is not
    //   there: red, "FAIL: the previous copy is gone"; RestartUnit's mode "replace" made "fail": red,
    //   "FAIL: systemd was asked: ...".
    expect(!QFileInfo::exists(runtime + QStringLiteral("/0.0.1")), QStringLiteral("the previous copy is gone"));
    expect(readAll(install->stamp()) == version + u'\n', QStringLiteral("the stamp holds this version"));
    expect(unitValue(unit, QStringLiteral("ExecStart")) == exec && unitValue(unit, QStringLiteral("ConditionFileIsExecutable")) == exec,
           QStringLiteral("the unit runs and is conditioned on %1:\n%2").arg(exec, unit));
    expect(QFileInfo(install->package() + QStringLiteral("/contents/ui/WallpaperXPlugin/qmldir")).isFile() &&
               !QFileInfo::exists(install->package() + QStringLiteral("/old-only")),
           QStringLiteral("the package was replaced whole, module inside"));
    expect(!QFileInfo::exists(install->paths.dataHome + QStringLiteral("/plasma/wallpapers/.com.wallpaperx.wallpaper.new")),
           QStringLiteral("no temporary package is left beside it"));
    expect(systemd.calls == expectedCalls, QStringLiteral("systemd was asked: %1").arg(systemd.calls.join(QStringLiteral("; "))));
    expect(setup.need() == FirstRunSetup::Need::UpToDate, QStringLiteral("the next start finds nothing to do: %1").arg(int(setup.need())));

    std::printf("--- what setup left ---\n");
    for (const QString& line : snapshot(install->root + QStringLiteral("/data"))) std::printf("%s\n", qUtf8Printable(line));
    std::printf("--- %s ---\n%s--- systemd was asked ---\n%s\n", qUtf8Printable(install->unitFile()), qUtf8Printable(unit),
                qUtf8Printable(systemd.calls.join(u'\n')));
}

// S16.1: need() from the files alone.
void needPieces() {
    const auto install = freshInstall(QStringLiteral("need"));
    const QString version = QString::fromLatin1(kVersion);
    if (!install || !layInstalled(*install, version)) {
        fault(kGoals, QStringLiteral("could not lay out the installed state for need()"));
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(version), plasmaWayland());
    if (setup.need() != FirstRunSetup::Need::UpToDate) {
        expect(false, QStringLiteral("need() is UpToDate for a complete install at this version: %1").arg(int(setup.need())));
        return;
    }
    // GOAL: S16.1 -- each installed piece alone, missing or wrong, makes need() say Update (no stamp:
    //   FirstRun), and putting it back makes it UpToDate again.
    // WHY-THIS-SHAPE: one piece at a time against a verified-complete baseline; removing everything at
    //   once would pass while any single clause of need() was gone.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp need() with each clause dropped in turn: red,
    //   "FAIL: need() is 3 with the wallpapers folder missing" (3 is Update), likewise "the unit file",
    //   "the package's metadata.json"; 2026-11-06, isCompleteCopy() without its executable test: red,
    //   "FAIL: need() is 3 with the copy's executable missing: 0" and "... not executable"; 2026-11-05,
    //   the build-id comparison dropped: red, "... with the installed package's
    //   build-id edited"; the unit compared by existence only: red, "FAIL: need() is Update with the
    //   unit edited by hand: 0"; `current` below.
    // COVERAGE-ASSERT: the count of pieces changed and restored is asserted.
    int checked = 0;
    const auto aside = [&](const QString& what, const QString& path, FirstRunSetup::Need expected) {
        std::error_code ec;
        fs::rename(native(path), native(path + QStringLiteral(".aside")), ec);
        if (ec) {
            fault(kGoals, QStringLiteral("could not move %1 aside").arg(path));
            return;
        }
        expect(setup.need() == expected, QStringLiteral("need() is %1 with %2 missing: %3").arg(int(expected)).arg(what).arg(int(setup.need())));
        fs::rename(native(path + QStringLiteral(".aside")), native(path), ec);
        expect(!ec && setup.need() == FirstRunSetup::Need::UpToDate, QStringLiteral("need() is UpToDate again with %1 back").arg(what));
        ++checked;
    };
    const auto replaced = [&](const QString& what, const QString& path, const QByteArray& bytes) {
        const QString was = readAll(path);
        if (!writeFile(path, bytes)) {
            fault(kGoals, QStringLiteral("could not rewrite %1").arg(path));
            return;
        }
        expect(setup.need() == FirstRunSetup::Need::Update, QStringLiteral("need() is Update with %1: %2").arg(what).arg(int(setup.need())));
        expect(writeFile(path, was.toUtf8()) && setup.need() == FirstRunSetup::Need::UpToDate,
               QStringLiteral("need() is UpToDate again with %1 undone").arg(what));
        ++checked;
    };
    const auto Update = FirstRunSetup::Need::Update;
    aside(QStringLiteral("the wallpapers folder"), install->paths.library + QStringLiteral("/wallpapers"), Update);
    aside(QStringLiteral("the current link"), install->runtime() + QStringLiteral("/current"), Update);
    aside(QStringLiteral("the copy's executable"), install->runtime() + u'/' + version + QStringLiteral("/bin/wallpaperx-daemon"), Update);
    aside(QStringLiteral("the unit file"), install->unitFile(), Update);
    aside(QStringLiteral("the package's metadata.json"), install->package() + QStringLiteral("/metadata.json"), Update);
    aside(QStringLiteral("the installed package's build-id"), install->package() + QStringLiteral("/build-id"), Update);
    aside(QStringLiteral("the bundle package's build-id"), install->bundlePackage() + QStringLiteral("/build-id"), Update);
    aside(QStringLiteral("the stamp"), install->stamp(), FirstRunSetup::Need::FirstRun);
    replaced(QStringLiteral("the installed package's build-id edited"), install->package() + QStringLiteral("/build-id"), "fedcba9876543210\n");
    replaced(QStringLiteral("the bundle package's build-id malformed"), install->bundlePackage() + QStringLiteral("/build-id"), "0123456789ABCDEF\n");
    replaced(QStringLiteral("the stamp naming another version"), install->stamp(), "0.0.1\n");
    // S16.24: the unit is compared by content, so a unit that exists but is not this build's counts.
    const QString unitWas = readAll(install->unitFile());
    QStringList olderLines = unitWas.split(u'\n');
    const qsizetype setting = olderLines.indexOf(QRegularExpression(QStringLiteral("\\w+=.*")));
    if (setting < 0) {
        fault(kGoals, QStringLiteral("the unit has no setting line to leave out"));
        return;
    }
    olderLines.removeAt(setting);
    const auto otherLibrary = serviceUnitText(install->paths.library + QStringLiteral("-elsewhere"));
    replaced(QStringLiteral("the unit edited by hand"), install->unitFile(), (unitWas + QStringLiteral("\n# edited\n")).toUtf8());
    replaced(QStringLiteral("the unit from an older template"), install->unitFile(), olderLines.join(u'\n').toUtf8());
    replaced(QStringLiteral("the unit for another library path"), install->unitFile(), otherLibrary.value_or(QByteArray("?")));
    {
        // The other copy is complete, so only the comparison with this version can catch it.
        // CONTROL: 2026-11-05, need()'s `current != fsPath(m_version->folder) ||` dropped: red, "FAIL: need() is
        //   Update with current naming another copy" (without the complete copy it passed).
        const QString current = install->runtime() + QStringLiteral("/current");
        std::error_code ec;
        fs::copy(native(install->paths.bundle + QStringLiteral("/daemon")), native(install->runtime() + u'/' + QString::fromLatin1(kOld)),
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        fs::remove(native(current), ec);
        fs::create_symlink(native(QString::fromLatin1(kOld)), native(current), ec);
        expect(setup.need() == Update, QStringLiteral("need() is Update with current naming another copy"));
        fs::remove(native(current), ec);
        fs::create_symlink(native(version), native(current), ec);
        expect(!ec && setup.need() == FirstRunSetup::Need::UpToDate, QStringLiteral("need() is UpToDate again with current back"));
        ++checked;
    }
    {
        const QString exe = install->runtime() + u'/' + version + QStringLiteral("/bin/wallpaperx-daemon");
        const QFile::Permissions was = QFile::permissions(exe);
        QFile::setPermissions(exe, was & ~(QFile::ExeOwner | QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther));
        expect(setup.need() == Update, QStringLiteral("need() is Update with the copy's executable not executable"));
        QFile::setPermissions(exe, was);
        expect(setup.need() == FirstRunSetup::Need::UpToDate, QStringLiteral("need() is UpToDate again with the executable bit back"));
        ++checked;
    }
    expect(checked == 16, QStringLiteral("need() was checked against 16 pieces, not %1").arg(checked));

    // NotBundled: neither half beside the app.
    SetupPaths nothing = install->paths;
    nothing.bundle = install->root + QStringLiteral("/empty");
    expect(FirstRunSetup(QDBusConnection::sessionBus(), nothing, asSource(version), plasmaWayland()).need() == FirstRunSetup::Need::NotBundled,
           QStringLiteral("need() is NotBundled with nothing beside the app"));
}

// S16.2: the version, and a source bundle without a daemon build id.
void version() {
    const auto install = freshInstall(QStringLiteral("version"));
    if (!install || !layInstalled(*install, QString::fromLatin1(kOld))) {
        fault(kGoals, QStringLiteral("could not lay out the version phase"));
        return;
    }
    const QString idFile = install->paths.bundle + QStringLiteral("/daemon/build-id");
    const QString app = QStringLiteral("0.1.0");
    if (!writeFile(idFile, "0123456789abcdef\n")) {
        fault(kGoals, QStringLiteral("could not write the daemon build id"));
        return;
    }
    const QString withId = QStringLiteral("0.1.0+0123456789abcdef");
    const auto source = setupVersion(app, BuildKind::Source, install->paths.bundle);
    expect(source && source->stamp == withId && source->folder == withId,
           QStringLiteral("a source build's stamp and folder are the app's +build id: \"%1\" \"%2\"")
               .arg(source ? source->stamp : source.error(), source ? source->folder : QString()));
    const auto release = setupVersion(app, BuildKind::Release, install->paths.bundle);
    expect(release && release->stamp == app && release->folder == withId,
           QStringLiteral("a release build's stamp is plain and its folder the app's +build id: \"%1\" \"%2\"")
               .arg(release ? release->stamp : release.error(), release ? release->folder : QString()));
    for (const QByteArray& bad : {QByteArray(""), QByteArray("0123456789ABCDEF"), QByteArray("0123456789abcde"), QByteArray("0123456789abcdef0"),
                                  QByteArray("0123456789abcdeg"), QByteArray("../../../etc/x")}) {
        writeFile(idFile, bad + '\n');
        const auto v = setupVersion(app, BuildKind::Source, install->paths.bundle);
        expect(!v && v.error().contains(idFile), QStringLiteral("a malformed build id \"%1\" is an error naming the file: \"%2\"")
                                                     .arg(QString::fromLatin1(bad), v ? v->folder : QString()));
    }
    QFile::remove(idFile);
    const auto missing = setupVersion(app, BuildKind::Source, install->paths.bundle);
    expect(!missing && missing.error().contains(idFile), QStringLiteral("a missing build id is an error naming the file"));
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp setupVersion() returning the plain version for
    //   a release with no build id: red, "FAIL: a release bundle without a build id is an error naming the
    //   file: \"0.1.0\"".
    const auto releaseMissing = setupVersion(app, BuildKind::Release, install->paths.bundle);
    expect(!releaseMissing && releaseMissing.error().contains(idFile),
           QStringLiteral("a release bundle without a build id is an error naming the file: \"%1\"")
               .arg(releaseMissing ? releaseMissing->folder : releaseMissing.error()));
    // The bundle itself is whole again, so the run below reaches copyDaemon() and fails on the version
    // alone, not in checkBundle() first.
    if (!writeFile(idFile, "0011223344556677\n")) {
        fault(kGoals, QStringLiteral("could not put the daemon build id back"));
        return;
    }

    // GOAL: S16.2 -- a run with no valid version fails with that sentence and writes no copy, no
    //   `current`, no stamp, no unit and no package.
    // WHY-THIS-SHAPE: snapshots of runtime/, the package and the unit, not "state is Failed": a run
    //   that copied before noticing would still end Failed. The bundle is whole here: with its build id
    //   missing too, checkBundle() would fail the run before copyDaemon() and its version check ran.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp copyDaemon() making runtime/broken before
    //   its version check: red, "FAIL: a run with no version changed nothing under runtime/".
    StandInSystemd systemd(install->unitFile(), install->runtime());
    if (!systemd.own()) {
        notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, missing, plasmaWayland());
    expect(setup.need() == FirstRunSetup::Need::Update, QStringLiteral("need() is Update when the version is an error: %1").arg(int(setup.need())));
    const QStringList runtime = snapshot(install->runtime());
    const QStringList package = snapshot(install->package());
    const QString unit = readAll(install->unitFile());
    const bool ended = runToEnd(setup);
    expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(idFile),
           QStringLiteral("a run with no version fails naming %1: %2").arg(idFile, describe(setup)));
    expect(snapshot(install->runtime()) == runtime, QStringLiteral("a run with no version changed nothing under runtime/"));
    expect(snapshot(install->package()) == package && readAll(install->unitFile()) == unit,
           QStringLiteral("a run with no version left the package and the unit alone"));
    expect(systemd.calls.isEmpty(), QStringLiteral("a run with no version asked systemd nothing: %1").arg(systemd.calls.join(u';')));
}

// S16.7, S16.17: a bundle with one of its four required pieces missing.
void halfBundle() {
    // GOAL: S16.17 -- a bundle missing the daemon executable, the daemon's build id, the package's
    //   metadata.json or the package's build id fails the run naming that file, before anything
    //   changes: runtime/, the unit, the package and the stamp as they were, and systemd asked nothing.
    // WHY-THIS-SHAPE: an install at an older version, so a run that went ahead would have a copy to
    //   make and a package to swap; on an up-to-date install a late check would change nothing either.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp checkBundle() not called: red, "FAIL: a bundle
    //   without the daemon executable changed nothing under runtime/"; each of checkBundle()'s four
    //   tests dropped in turn: red, "FAIL: a bundle without <that piece> fails the run naming ...: state 2".
    // COVERAGE-ASSERT: the number of pieces tried is asserted.
    const struct {
        const char* what;
        const char* file; // under the bundle
    } pieces[] = {{"the daemon executable", "daemon/bin/wallpaperx-daemon"},
                  {"the daemon's build id", "daemon/build-id"},
                  {"the package's metadata.json", "plasma/wallpapers/com.wallpaperx.wallpaper/metadata.json"},
                  {"the package's build id", "plasma/wallpapers/com.wallpaperx.wallpaper/build-id"}};
    int tried = 0;
    for (const auto& piece : pieces) {
        const auto install = freshInstall(QStringLiteral("half-bundle-%1").arg(tried));
        if (!install || !layInstalled(*install, QString::fromLatin1(kOld)) || !writeFile(install->package() + QStringLiteral("/old-only"), "old")) {
            fault(kGoals, QStringLiteral("could not lay out the half-bundle phase"));
            return;
        }
        const QString missing = install->paths.bundle + u'/' + QLatin1String(piece.file);
        if (!QFile::remove(missing)) {
            fault(kGoals, QStringLiteral("could not remove %1").arg(missing));
            return;
        }
        StandInSystemd systemd(install->unitFile(), install->runtime());
        if (!systemd.own()) {
            notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
            return;
        }
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
        const QStringList runtime = snapshot(install->runtime());
        const QStringList package = snapshot(install->package());
        const QString unit = readAll(install->unitFile());
        const bool ended = runToEnd(setup);
        const QString what = QLatin1String(piece.what);
        expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(missing),
               QStringLiteral("a bundle without %1 fails the run naming %2: %3").arg(what, missing, describe(setup)));
        expect(snapshot(install->runtime()) == runtime, QStringLiteral("a bundle without %1 changed nothing under runtime/").arg(what));
        expect(snapshot(install->package()) == package && readAll(install->unitFile()) == unit,
               QStringLiteral("a bundle without %1 left the package and the unit alone").arg(what));
        expect(systemd.calls.isEmpty(), QStringLiteral("a bundle without %1 asked systemd nothing: %2").arg(what, systemd.calls.join(u';')));
        ++tried;
    }
    expect(tried == 4, QStringLiteral("four missing pieces were tried, not %1").arg(tried));
}

// S16.4, S16.8: the daemon already runs this version's copy; only the package changed.
void noRestart() {
    if (dbusTestTool().isEmpty()) {
        notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from the runtime copy");
        return;
    }
    const auto install = freshInstall(QStringLiteral("no-restart"), dbusTestTool());
    if (!install) return;
    StandInSystemd systemd(install->unitFile(), install->runtime(), {.daemon = Daemon::Process});
    if (!systemd.own()) {
        notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
    if (!runToEnd(setup) || setup.state() != FirstRunSetup::State::Done || systemd.mainProcess == nullptr) {
        fault(kGoals, QStringLiteral("the first run, which this phase builds on, did not finish: %1").arg(describe(setup)));
        return;
    }
    QProcess* const daemon = systemd.mainProcess;
    const qint64 pid = daemon->processId();
    // The plugin changes, the daemon does not; an old copy waits to be removed.
    const QString newOnly = install->bundlePackage() + QStringLiteral("/contents/ui/New.qml");
    if (!writeFile(newOnly, "Item {}\n") || !writeFile(install->bundlePackage() + QStringLiteral("/build-id"), "fedcba9876543210\n") ||
        !writeFile(install->package() + QStringLiteral("/old-only"), "old") || !writeFile(install->runtime() + QStringLiteral("/0.0.1/x"), "old")) {
        fault(kGoals, QStringLiteral("could not change the bundle's package"));
        return;
    }
    expect(setup.need() == FirstRunSetup::Need::Update, QStringLiteral("a changed package build id makes need() Update"));
    systemd.calls.clear();
    const bool ended = runToEnd(setup);
    // GOAL: S16.4 -- with MainPID already running runtime/<version>'s executable, the run restarts
    //   nothing and the same process survives; S16.8 -- the changed package is still swapped in whole.
    // WHY-THIS-SHAPE: a real process started from the copy, since setup compares /proc/<pid>/exe; the
    //   stand-in's own pid would never match and so could only ever test the restart branch.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp startDaemon() never taking the no-restart
    //   branch: red, "FAIL: a plugin-only update asked systemd only to enable, reload and load".
    expect(ended && setup.state() == FirstRunSetup::State::Done, QStringLiteral("the plugin-only update finished: %1").arg(describe(setup)));
    expect(systemd.calls == QStringList{QStringLiteral("EnableUnitFiles wallpaperx-daemon.service runtime=0 force=1"), QStringLiteral("Reload"),
                                        QStringLiteral("LoadUnit wallpaperx-daemon.service")},
           QStringLiteral("a plugin-only update asked systemd only to enable, reload and load: %1").arg(systemd.calls.join(QStringLiteral("; "))));
    expect(systemd.mainProcess == daemon && daemon->state() == QProcess::Running && daemon->processId() == pid,
           QStringLiteral("the same daemon process %1 is still running").arg(pid));
    expect(QFileInfo(install->package() + QStringLiteral("/contents/ui/New.qml")).isFile() && !QFileInfo::exists(install->package() + QStringLiteral("/old-only")),
           QStringLiteral("the package was swapped whole: the new-only file is in, the old-only file gone"));
    expect(readAll(install->package() + QStringLiteral("/build-id")) == QLatin1String("fedcba9876543210\n"), QStringLiteral("the installed build-id is the bundle's"));
    expect(!QFileInfo::exists(install->runtime() + QStringLiteral("/0.0.1")), QStringLiteral("the old copy went once the running daemon was confirmed"));
    expect(setup.need() == FirstRunSetup::Need::UpToDate, QStringLiteral("after the plugin-only update need() is UpToDate"));

    // S16.7: the installed package's build-id edited, then removed, from outside: put back each time.
    for (const bool remove : {false, true}) {
        const QString installedId = install->package() + QStringLiteral("/build-id");
        if (remove ? !QFile::remove(installedId) : !writeFile(installedId, "0000000000000000\n")) {
            fault(kGoals, QStringLiteral("could not alter the installed build-id"));
            return;
        }
        expect(setup.need() == FirstRunSetup::Need::Update, QStringLiteral("an installed build-id %1 makes need() Update").arg(remove ? "removed" : "edited"));
        const bool putBack = runToEnd(setup);
        expect(putBack && setup.state() == FirstRunSetup::State::Done && readAll(installedId) == QLatin1String("fedcba9876543210\n"),
               QStringLiteral("an installed build-id %1 is put back: %2").arg(remove ? "removed" : "edited", describe(setup)));
    }
}

// S16.5: the no-restart path still waits for the daemon's name, and times out keeping old copies.
void noRestartTimeout() {
    if (dbusTestTool().isEmpty()) {
        notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from the runtime copy");
        return;
    }
    const auto install = freshInstall(QStringLiteral("no-restart-timeout"), dbusTestTool());
    const QString version = QString::fromLatin1(kVersion);
    if (!install || !layInstalled(*install, version) || !writeFile(install->runtime() + QStringLiteral("/0.0.1/x"), "old")) {
        fault(kGoals, QStringLiteral("could not lay out the no-restart-timeout phase"));
        return;
    }
    StandInSystemd systemd(install->unitFile(), install->runtime(), {.daemon = Daemon::Process});
    QProcess* daemon = systemd.own() ? systemd.launch(install->runtime() + u'/' + version + QStringLiteral("/bin/wallpaperx-daemon"), kNotTheDaemon) : nullptr;
    if (daemon == nullptr) {
        notVerified(kGoals, "the stand-in systemd or the daemon running from the copy could not be started");
        return;
    }
    systemd.runAsMain(daemon);
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(version), plasmaWayland());
    const bool ended = runToEnd(setup);
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp startDaemon() going straight to finishFiles()
    //   on the no-restart branch: red, "FAIL: the no-restart path fails at the limit ...: state 2".
    expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(QLatin1String("did not start within")),
           QStringLiteral("the no-restart path fails at the limit when the name is never held by MainPID: %1").arg(describe(setup)));
    expect(!systemd.calls.join(u';').contains(QLatin1String("RestartUnit")), QStringLiteral("the no-restart path asked for no restart"));
    expect(QFileInfo::exists(install->runtime() + QStringLiteral("/0.0.1/x")), QStringLiteral("the no-restart timeout kept the old copy"));
}

// S16.4: whatever else MainPID runs, the run restarts.
void restartCases() {
    if (dbusTestTool().isEmpty()) {
        notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from a copy");
        return;
    }
    const QString version = QString::fromLatin1(kVersion);
    const QString daemonName = QString::fromLatin1(FirstRunSetup::kDaemonService);
    // GOAL: S16.4 -- MainPID running another copy, a deleted copy, or another binary is restarted.
    // WHY-THIS-SHAPE: each case runs a real process whose /proc/<pid>/exe is that file; a pid with no
    //   such process would make every case restart for the trivial reason that nothing matches.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp runsExecutable() true for any live pid: red on
    //   each of "FAIL: MainPID running another copy restarts", "... a deleted copy ...", "... another binary ...".
    const auto restarts = [&](const QString& name, const QString& what, auto prepare) {
        const auto install = freshInstall(name, dbusTestTool());
        if (!install) return;
        StandInSystemd systemd(install->unitFile(), install->runtime(), {.daemon = Daemon::Process});
        if (!systemd.own()) {
            notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
            return;
        }
        QProcess* running = prepare(*install, systemd);
        if (running == nullptr) {
            fault(kGoals, QStringLiteral("could not prepare the case: %1").arg(what));
            return;
        }
        systemd.runAsMain(running);
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(version), plasmaWayland());
        const bool ended = runToEnd(setup);
        expect(systemd.calls.contains(QStringLiteral("RestartUnit wallpaperx-daemon.service replace")) && ended &&
                   setup.state() == FirstRunSetup::State::Done,
               QStringLiteral("MainPID running %1 restarts: %2; asked %3").arg(what, describe(setup), systemd.calls.join(QStringLiteral("; "))));
    };
    restarts(QStringLiteral("restart-other-copy"), QStringLiteral("another copy"), [&](const Install& install, StandInSystemd& systemd) {
        return layInstalled(install, QString::fromLatin1(kOld))
                   ? systemd.launch(install.runtime() + QStringLiteral("/0.0.1/bin/wallpaperx-daemon"), daemonName)
                   : nullptr;
    });
    restarts(QStringLiteral("restart-deleted-copy"), QStringLiteral("a deleted copy"), [&](const Install& install, StandInSystemd& systemd) -> QProcess* {
        if (!layInstalled(install, version)) return nullptr;
        QProcess* process = systemd.launch(install.runtime() + u'/' + version + QStringLiteral("/bin/wallpaperx-daemon"), daemonName);
        std::error_code ec;
        const fs::path copy = native(install.runtime() + u'/' + version);
        fs::remove_all(copy, ec);
        fs::copy(native(install.paths.bundle + QStringLiteral("/daemon")), copy, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        return ec ? nullptr : process;
    });
    restarts(QStringLiteral("restart-other-binary"), QStringLiteral("another binary"), [&](const Install& install, StandInSystemd& systemd) {
        return layInstalled(install, version) ? systemd.launch(dbusTestTool(), daemonName) : nullptr;
    });
}

// S16.4: a previous run switched `current` and then failed to restart.
void interruptedSwitch() {
    if (dbusTestTool().isEmpty()) {
        notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from a copy");
        return;
    }
    const auto install = freshInstall(QStringLiteral("interrupted"), dbusTestTool());
    const QString version = QString::fromLatin1(kVersion);
    if (!install || !layInstalled(*install, QString::fromLatin1(kOld))) {
        fault(kGoals, QStringLiteral("could not lay out the interrupted switch"));
        return;
    }
    StandInSystemd systemd(install->unitFile(), install->runtime(), {.daemon = Daemon::Process});
    QProcess* old = systemd.own() ? systemd.launch(install->runtime() + QStringLiteral("/0.0.1/bin/wallpaperx-daemon"),
                                                   QString::fromLatin1(FirstRunSetup::kDaemonService))
                                  : nullptr;
    std::error_code ec;
    fs::copy(native(install->paths.bundle + QStringLiteral("/daemon")), native(install->runtime() + u'/' + version),
             fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    fs::remove(native(install->runtime() + QStringLiteral("/current")), ec);
    fs::create_symlink(native(version), native(install->runtime() + QStringLiteral("/current")), ec);
    if (old == nullptr || ec) {
        fault(kGoals, QStringLiteral("could not lay out the interrupted switch"));
        return;
    }
    systemd.runAsMain(old);
    bool oldAtRestart = false;
    systemd.onRestart = [&] { oldAtRestart = QFileInfo::exists(install->runtime() + QStringLiteral("/0.0.1/bin/wallpaperx-daemon")); };
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(version), plasmaWayland());
    const bool ended = runToEnd(setup);
    // GOAL: S16.4 -- `current` already new but the daemon on the old copy: restart, and the old copy
    //   is still there when the restart is asked for, gone only after confirmation.
    // WHY-THIS-SHAPE: the old copy is sampled inside the stand-in's RestartUnit, the last point before
    //   confirmation; checking only the end state cannot tell "removed after" from "removed before".
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp runsExecutable() true for any live pid: red,
    //   "FAIL: an interrupted switch restarts the daemon ...".
    expect(ended && setup.state() == FirstRunSetup::State::Done && systemd.calls.contains(QStringLiteral("RestartUnit wallpaperx-daemon.service replace")),
           QStringLiteral("an interrupted switch restarts the daemon: %1; asked %2").arg(describe(setup), systemd.calls.join(QStringLiteral("; "))));
    expect(oldAtRestart, QStringLiteral("the old copy was still there when the restart was asked for"));
    expect(!QFileInfo::exists(install->runtime() + QStringLiteral("/0.0.1")), QStringLiteral("the old copy went after confirmation"));
}

// S16.3: every way the restart can fail keeps the old copy and the old package, and writes no stamp.
void failsKeepingOld(const QString& name, Behaviour behaviour, const QString& says) {
    if (dbusTestTool().isEmpty()) {
        notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from the previous copy");
        return;
    }
    const auto install = freshInstall(name, dbusTestTool());
    if (!install || !layInstalled(*install, QString::fromLatin1(kOld)) || !writeFile(install->package() + QStringLiteral("/old-only"), "old")) {
        fault(kGoals, QStringLiteral("could not lay out phase %1").arg(name));
        return;
    }
    StandInSystemd systemd(install->unitFile(), install->runtime(), behaviour);
    if (!systemd.own() || runOldAsMain(*install, systemd) == nullptr) {
        notVerified(kGoals, "the stand-in systemd or the daemon running from the previous copy could not be started");
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
    const bool ended = runToEnd(setup);
    // GOAL: S16.3 -- the run fails saying why, and the old copy, the old package and the old stamp stay.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp startDaemon() removing old copies before the
    //   restart: red in each of the five failure phases, "FAIL: <phase>: the old copy is kept". Each
    //   failure path disabled: onJobRemoved() ignoring the result, checkStarted() ignoring the state,
    //   the limit raised to 60 s, RestartUnit's error not treated as one: red, "FAIL: <phase>: the run
    //   fails saying ..." in that phase; 2026-11-06, confirmDaemon() not comparing MainPID: red, the
    //   same in name_other_pid.
    expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(says),
           QStringLiteral("%1: the run fails saying \"%2\": %3").arg(name, says, describe(setup)));
    expect(QFileInfo(install->runtime() + QStringLiteral("/0.0.1/bin/wallpaperx-daemon")).isExecutable(), QStringLiteral("%1: the old copy is kept").arg(name));
    expect(QFileInfo::exists(install->package() + QStringLiteral("/old-only")), QStringLiteral("%1: the old package is kept").arg(name));
    expect(readAll(install->stamp()) == QStringLiteral("0.0.1\n"), QStringLiteral("%1: no stamp is written").arg(name));

    // GOAL: S16.15 -- a failure after `current` was switched points it back at the previous copy, says
    //   that copy is kept, and asks systemd once more for ResetFailedUnit then RestartUnit("replace"),
    //   since this run had asked for a restart.
    // WHY-THIS-SHAPE: setup ends the run once systemd has answered the rollback's calls; they are
    //   still counted after a barrier call (drained()), so one sent without waiting is counted too,
    //   and an exact count, not "at least two", sees a third.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp: finish() not calling switchBack(): red,
    //   "FAIL: job_failed: current names the previous copy again"; switchBack() never restarting: red,
    //   "FAIL: job_failed: the rollback asked systemd to restart once more: ..."; 2026-11-06,
    //   restartDaemon() not setting `restarted`: red, "FAIL: restart_error: the rollback asked systemd
    //   to restart once more".
    if (!drained()) {
        fault(kGoals, QStringLiteral("%1: the stand-in did not answer the barrier call").arg(name));
        return;
    }
    expect(QFile::symLinkTarget(install->runtime() + QStringLiteral("/current")) == install->runtime() + QStringLiteral("/0.0.1"),
           QStringLiteral("%1: current names the previous copy again").arg(name));
    // A stand-in refusing every restart refuses the rollback's too, and setup waits for that answer.
    const QString kept = behaviour.restartErrors ? QStringLiteral("but systemd did not restart it (stand-in refuses the restart)")
                                                 : QStringLiteral("Your previous copy of it is kept.");
    expect(setup.message().contains(kept), QStringLiteral("%1: the message says \"%2\": %3").arg(name, kept, describe(setup)));
    // GOAL: S16.30 -- a refused rollback restart never claims the previous copy is kept.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp switchedBack() ignoring its problems: red,
    //   "FAIL: restart_error: a refused rollback restart does not say the copy is kept".
    if (behaviour.restartErrors) {
        expect(!setup.message().contains(QLatin1String("is kept")),
               QStringLiteral("%1: a refused rollback restart does not say the copy is kept: %2").arg(name, describe(setup)));
    }
    // GOAL: S16.29 -- a unit whose text this run did not change is not reloaded by the rollback.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp replacedUnitText() returning the old text even
    //   when equal: red, "FAIL: job_failed: an unchanged unit gets no second Reload".
    expect(countOf(systemd.calls, QStringLiteral("Reload")) == 1,
           QStringLiteral("%1: an unchanged unit gets no second Reload: %2").arg(name, systemd.calls.join(QStringLiteral("; "))));
    expect(countOf(systemd.calls, kRestartCall) == 2 && countOf(systemd.calls, kResetCall) == 2 &&
               systemd.calls.mid(systemd.calls.size() - 2) == QStringList{kResetCall, kRestartCall},
           QStringLiteral("%1: the rollback asked systemd to restart once more: %2").arg(name, systemd.calls.join(QStringLiteral("; "))));
}

// S16.15: a failure after the restart, with the switch back failing too.
void switchBackFails() {
    if (dbusTestTool().isEmpty()) {
        notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from the previous copy");
        return;
    }
    const auto install = freshInstall(QStringLiteral("switch-back-fails"), dbusTestTool());
    if (!install || !layInstalled(*install, QString::fromLatin1(kOld))) {
        fault(kGoals, QStringLiteral("could not lay out the switch-back phase"));
        return;
    }
    StandInSystemd systemd(install->unitFile(), install->runtime(), {.jobResult = QStringLiteral("failed")});
    if (!systemd.own() || runOldAsMain(*install, systemd) == nullptr) {
        notVerified(kGoals, "the stand-in systemd or the daemon running from the previous copy could not be started");
        return;
    }
    // From the restart on, moving a new `current` in fails: the forward switch is done by then.
    systemd.onRestart = [] { g_refuseCurrentSwitch = true; };
    g_currentSwitchesRefused = 0;
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
    const bool ended = runToEnd(setup);
    g_refuseCurrentSwitch = false;
    // GOAL: S16.15 -- when pointing `current` back fails, the message says so and does not claim the
    //   previous copy is kept, and no rollback restart is asked for.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp switchBackFiles() ignoring pointCurrentAt()'s
    //   error: red, "FAIL: a failed switch back is reported and does not claim the previous copy is kept".
    // COVERAGE-ASSERT: the refused switches are counted, so a run that never tried to switch back
    //   cannot pass.
    expect(g_currentSwitchesRefused > 0, QStringLiteral("the switch back was tried and refused: %1 refused").arg(g_currentSwitchesRefused.load()));
    expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(QLatin1String("could not point")) &&
               !setup.message().contains(QLatin1String("is kept")),
           QStringLiteral("a failed switch back is reported and does not claim the previous copy is kept: %1").arg(describe(setup)));
    expect(QFile::symLinkTarget(install->runtime() + QStringLiteral("/current")) == install->runtime() + u'/' + QString::fromLatin1(kVersion),
           QStringLiteral("after a failed switch back current still names the new copy"));
    expect(drained() && countOf(systemd.calls, kRestartCall) == 1,
           QStringLiteral("a failed switch back asks for no second restart: %1").arg(systemd.calls.join(QStringLiteral("; "))));
}

// S16.16: a runtime copy that fails the intact check is rebuilt whole, and a daemon running from it
// is restarted.
void damagedCopy() {
    const QString version = QString::fromLatin1(kVersion);
    // GOAL: S16.16 -- a copy whose build id is edited or missing, or whose executable is missing, makes
    //   need() say Update even with `current` naming it; the run rebuilds it whole (an extra file goes)
    //   and leaves no hidden .partial or .old folder under runtime/.
    // WHY-THIS-SHAPE: the rebuilt copy is compared file by file with the bundle's daemon folder, so a
    //   run that patched the one bad file in place, leaving the extra one, fails.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp isIntactCopy() without the build-id comparison:
    //   red, "FAIL: need() is Update with the copy's build-id edited, with an extra file: 0";
    //   isCompleteCopy() without its executable test: red, "FAIL: need() is Update with the copy's
    //   executable removed: 0". 2026-11-05, copyDaemon() rebuilding only a copy with no executable: red,
    //   "FAIL: with the copy's build-id edited, with an extra file the copy was rebuilt whole, ...".
    // COVERAGE-ASSERT: the number of damaged cases run is asserted.
    const struct {
        const char* what;
        const char* remove; // under the copy, or nothing
        const char* idBytes; // written as the copy's build-id, or nothing
    } cases[] = {{"the copy's build-id edited, with an extra file", nullptr, "ffffffffffffffff\n"},
                 {"the copy's build-id removed", "build-id", nullptr},
                 {"the copy's executable removed", "bin/wallpaperx-daemon", nullptr}};
    int ran = 0;
    for (const auto& c : cases) {
        const auto install = freshInstall(QStringLiteral("damaged-%1").arg(ran));
        if (!install || !layInstalled(*install, version)) {
            fault(kGoals, QStringLiteral("could not lay out the damaged-copy phase"));
            return;
        }
        const QString copy = install->runtime() + u'/' + version;
        const bool damaged = (c.remove == nullptr || QFile::remove(copy + u'/' + QLatin1String(c.remove))) &&
                             (c.idBytes == nullptr || (writeFile(copy + QStringLiteral("/build-id"), c.idBytes) && writeFile(copy + QStringLiteral("/extra"), "x")));
        StandInSystemd systemd(install->unitFile(), install->runtime());
        if (!damaged || !systemd.own()) {
            fault(kGoals, QStringLiteral("could not damage the copy or own the stand-in's name"));
            return;
        }
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(version), plasmaWayland());
        const QString what = QLatin1String(c.what);
        expect(setup.need() == FirstRunSetup::Need::Update, QStringLiteral("need() is Update with %1: %2").arg(what).arg(int(setup.need())));
        const bool ended = runToEnd(setup);
        expect(ended && setup.state() == FirstRunSetup::State::Done, QStringLiteral("a run with %1 finishes: %2").arg(what, describe(setup)));
        expect(relativeSnapshot(copy) == relativeSnapshot(install->paths.bundle + QStringLiteral("/daemon")),
               QStringLiteral("with %1 the copy was rebuilt whole, as the bundle's daemon").arg(what));
        const QStringList hidden = QDir(install->runtime()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).filter(QRegularExpression(QStringLiteral("^\\.")));
        expect(hidden.isEmpty(), QStringLiteral("with %1 nothing hidden is left under runtime/: %2").arg(what, hidden.join(u' ')));
        expect(setup.need() == FirstRunSetup::Need::UpToDate, QStringLiteral("after rebuilding %1 need() is UpToDate").arg(what));
        ++ran;
    }
    expect(ran == 3, QStringLiteral("three damaged copies were tried, not %1").arg(ran));

    if (dbusTestTool().isEmpty()) {
        notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from a damaged copy");
        return;
    }
    const auto install = freshInstall(QStringLiteral("damaged-running"), dbusTestTool());
    if (!install) return;
    StandInSystemd systemd(install->unitFile(), install->runtime(), {.daemon = Daemon::Process});
    if (!systemd.own()) {
        notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(version), plasmaWayland());
    if (!runToEnd(setup) || setup.state() != FirstRunSetup::State::Done || systemd.mainProcess == nullptr ||
        !writeFile(install->runtime() + u'/' + version + QStringLiteral("/build-id"), "ffffffffffffffff\n")) {
        fault(kGoals, QStringLiteral("the first run, which the damaged-running case builds on, did not finish: %1").arg(describe(setup)));
        return;
    }
    const qint64 before = systemd.mainProcess->processId();
    systemd.calls.clear();
    // GOAL: S16.16 -- a damaged copy under the current folder, with the daemon running from it, is
    //   rebuilt and the daemon restarted: its running executable is no longer the copy's.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp copyDaemon() rebuilding only a copy with no
    //   executable: red, "FAIL: a damaged copy the daemon runs from is rebuilt and the daemon restarted".
    const bool ended = runToEnd(setup);
    expect(ended && setup.state() == FirstRunSetup::State::Done && systemd.calls.contains(kRestartCall) && systemd.mainProcess != nullptr &&
               systemd.mainProcess->processId() != before,
           QStringLiteral("a damaged copy the daemon runs from is rebuilt and the daemon restarted: %1; asked %2")
               .arg(describe(setup), systemd.calls.join(QStringLiteral("; "))));
    expect(readAll(install->runtime() + u'/' + version + QStringLiteral("/build-id")) == QLatin1String("0011223344556677\n"),
           QStringLiteral("the damaged copy the daemon ran from carries the bundle's build id again"));
}

// S16.18: a release build, whose stamp is the plain version and whose copy is named by content.
void releaseBuild() {
    if (dbusTestTool().isEmpty()) {
        notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from the runtime copy");
        return;
    }
    const QString app = QStringLiteral("0.1.0");
    const auto install = freshInstall(QStringLiteral("release"), dbusTestTool());
    if (!install) return;
    const QString runtime = install->runtime();
    StandInSystemd systemd(install->unitFile(), runtime, {.daemon = Daemon::Process});
    const auto first = setupVersion(app, BuildKind::Release, install->paths.bundle);
    if (!first || !systemd.own()) {
        notVerified(kGoals, "the release version or the stand-in could not be set up");
        return;
    }
    const QString folder1 = QStringLiteral("0.1.0+0011223344556677");
    // GOAL: S16.18 -- a release run leaves runtime/<version>+<daemon build id>, `current` naming it,
    //   and a stamp holding the plain version; a second run restarts nothing.
    // WHY-THIS-SHAPE: a real process runs from the copy, so "restarts nothing" is decided by setup's
    //   /proc comparison against the folder, which only matches if the folder name is used for it.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp: need() comparing the stamp with the folder:
    //   red, "FAIL: after a release run need() is UpToDate: 3"; the Stamp step writing the folder: red,
    //   "FAIL: a release run's stamp is the plain version"; copyDaemon() naming the copy by the stamp:
    //   red, "FAIL: a release run's copy is runtime/0.1.0+0011223344556677"; startDaemon() looking for
    //   the stamp's folder: red, "FAIL: a second release run restarts nothing".
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, *first, plasmaWayland());
    const bool ended = runToEnd(setup);
    expect(ended && setup.state() == FirstRunSetup::State::Done && systemd.calls.value(systemd.calls.size() - 1) == kRestartCall,
           QStringLiteral("a release first run finishes, restarting the daemon: %1").arg(describe(setup)));
    expect(QFile::symLinkTarget(runtime + QStringLiteral("/current")) == runtime + u'/' + folder1 &&
               QFileInfo(runtime + u'/' + folder1 + QStringLiteral("/bin/wallpaperx-daemon")).isExecutable(),
           QStringLiteral("a release run's copy is runtime/%1, named by current").arg(folder1));
    expect(readAll(install->stamp()) == QStringLiteral("0.1.0\n"), QStringLiteral("a release run's stamp is the plain version: \"%1\"").arg(readAll(install->stamp())));
    expect(setup.need() == FirstRunSetup::Need::UpToDate, QStringLiteral("after a release run need() is UpToDate: %1").arg(int(setup.need())));
    if (systemd.mainProcess == nullptr) {
        fault(kGoals, QStringLiteral("no daemon process runs after the release first run"));
        return;
    }
    const qint64 pid1 = systemd.mainProcess->processId();
    systemd.calls.clear();
    const bool secondEnded = runToEnd(setup);
    expect(secondEnded && setup.state() == FirstRunSetup::State::Done &&
               systemd.calls == QStringList{QStringLiteral("EnableUnitFiles wallpaperx-daemon.service runtime=0 force=1"), QStringLiteral("Reload"),
                                            QStringLiteral("LoadUnit wallpaperx-daemon.service")} &&
               systemd.mainProcess->processId() == pid1,
           QStringLiteral("a second release run restarts nothing: %1; asked %2").arg(describe(setup), systemd.calls.join(QStringLiteral("; "))));

    // A changed daemon under the same version: a new build id.
    if (!writeFile(install->paths.bundle + QStringLiteral("/daemon/share/wallpaperx/shaders/a.spv"), "spv2") ||
        !writeFile(install->paths.bundle + QStringLiteral("/daemon/build-id"), "aaaaaaaaaaaaaaaa\n")) {
        fault(kGoals, QStringLiteral("could not change the bundle's daemon"));
        return;
    }
    const QString folder2 = QStringLiteral("0.1.0+aaaaaaaaaaaaaaaa");
    const auto second = setupVersion(app, BuildKind::Release, install->paths.bundle);
    FirstRunSetup changed(QDBusConnection::sessionBus(), install->paths, second.value_or(SetupVersion{}), plasmaWayland());
    bool oldAtRestart = false;
    systemd.onRestart = [&] { oldAtRestart = QFileInfo::exists(runtime + u'/' + folder1 + QStringLiteral("/bin/wallpaperx-daemon")); };
    systemd.calls.clear();
    // GOAL: S16.18 -- the same version with a changed daemon is an Update to a new folder: the daemon is
    //   restarted, the old folder is still there when the restart is asked for and gone after, and the
    //   stamp stays plain.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp startDaemon() removing old copies before the
    //   restart: red, "FAIL: the old release folder was still there when the restart was asked for";
    //   finish() not calling switchBack(): red, "FAIL: a failed restart within one release version
    //   points current back at 0.1.0+aaaaaaaaaaaaaaaa".
    expect(changed.need() == FirstRunSetup::Need::Update, QStringLiteral("a release with a changed daemon makes need() Update: %1").arg(int(changed.need())));
    const bool changedEnded = runToEnd(changed);
    expect(changedEnded && changed.state() == FirstRunSetup::State::Done && systemd.calls.contains(kRestartCall) &&
               systemd.mainProcess != nullptr && systemd.mainProcess->processId() != pid1,
           QStringLiteral("a release with a changed daemon restarts it: %1; asked %2").arg(describe(changed), systemd.calls.join(QStringLiteral("; "))));
    expect(oldAtRestart, QStringLiteral("the old release folder was still there when the restart was asked for"));
    expect(QFile::symLinkTarget(runtime + QStringLiteral("/current")) == runtime + u'/' + folder2 && !QFileInfo::exists(runtime + u'/' + folder1),
           QStringLiteral("after a changed daemon current names %1 and %2 is gone").arg(folder2, folder1));
    expect(readAll(install->stamp()) == QStringLiteral("0.1.0\n") && changed.need() == FirstRunSetup::Need::UpToDate,
           QStringLiteral("after a changed daemon the stamp is still plain and need() is UpToDate"));

    // The same again, and the restart fails: the rollback is reachable within one version.
    if (!writeFile(install->paths.bundle + QStringLiteral("/daemon/build-id"), "bbbbbbbbbbbbbbbb\n")) {
        fault(kGoals, QStringLiteral("could not change the bundle's daemon again"));
        return;
    }
    FirstRunSetup failing(QDBusConnection::sessionBus(), install->paths, setupVersion(app, BuildKind::Release, install->paths.bundle).value_or(SetupVersion{}),
                          plasmaWayland());
    systemd.onRestart = {};
    systemd.behaviour.jobResult = QStringLiteral("failed");
    const bool failingEnded = runToEnd(failing);
    expect(failingEnded && failing.state() == FirstRunSetup::State::Failed && failing.message().contains(QLatin1String("Your previous copy of it is kept.")) &&
               QFile::symLinkTarget(runtime + QStringLiteral("/current")) == runtime + u'/' + folder2,
           QStringLiteral("a failed restart within one release version points current back at %1: %2").arg(folder2, describe(failing)));
    systemd.behaviour.jobResult = QStringLiteral("done");
}

// S16.18: an older release's copy in a folder named by the plain version.
void releaseFromPlainFolder() {
    const QString app = QStringLiteral("0.1.0");
    const auto install = freshInstall(QStringLiteral("release-plain"));
    if (!install || !layInstalled(*install, app)) {
        fault(kGoals, QStringLiteral("could not lay out the plain-folder install"));
        return;
    }
    const QString runtime = install->runtime();
    StandInSystemd systemd(install->unitFile(), runtime);
    const auto version = setupVersion(app, BuildKind::Release, install->paths.bundle);
    if (!version || !systemd.own()) {
        notVerified(kGoals, "the release version or the stand-in could not be set up");
        return;
    }
    bool plainAtRestart = false;
    systemd.onRestart = [&] { plainAtRestart = QFileInfo::exists(runtime + QStringLiteral("/0.1.0/bin/wallpaperx-daemon")); };
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, *version, plasmaWayland());
    // GOAL: S16.18 -- an install whose copy is in a plain-named folder (stamp 0.1.0, current -> 0.1.0)
    //   is an Update to 0.1.0+<id>: restarted, with 0.1.0 kept until the daemon is confirmed.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp need() without the `current` comparison: red,
    //   "FAIL: a plain-named release folder makes need() Update: 0"; startDaemon() removing old copies
    //   before the restart: red, "FAIL: the plain-named folder was kept until the daemon was confirmed".
    expect(setup.need() == FirstRunSetup::Need::Update, QStringLiteral("a plain-named release folder makes need() Update: %1").arg(int(setup.need())));
    const bool ended = runToEnd(setup);
    expect(ended && setup.state() == FirstRunSetup::State::Done && systemd.calls.contains(kRestartCall),
           QStringLiteral("an update from a plain-named folder restarts the daemon: %1").arg(describe(setup)));
    expect(plainAtRestart && !QFileInfo::exists(runtime + QStringLiteral("/0.1.0")),
           QStringLiteral("the plain-named folder was kept until the daemon was confirmed, then removed"));
    expect(QFile::symLinkTarget(runtime + QStringLiteral("/current")) == runtime + QStringLiteral("/0.1.0+0011223344556677"),
           QStringLiteral("after the update current names 0.1.0+0011223344556677"));
}

// S16.5: a unit file elsewhere that systemd reads instead of ours.
void fragmentOverride() {
    const auto install = freshInstall(QStringLiteral("fragment"));
    if (!install) return;
    const QString overriding = install->root + QStringLiteral("/config/systemd/user/wallpaperx-daemon.service");
    if (!writeFile(overriding, "[Service]\nExecStart=/bin/true\n")) {
        fault(kGoals, QStringLiteral("could not write the overriding unit"));
        return;
    }
    StandInSystemd systemd(install->unitFile(), install->runtime(), {.fragmentPath = overriding});
    if (!systemd.own()) {
        notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
    g_warnings.clear();
    g_previousHandler = qInstallMessageHandler(captureWarning);
    const bool ended = runToEnd(setup);
    qInstallMessageHandler(g_previousHandler);
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp installService() accepting any FragmentPath:
    //   red, "FAIL: an overriding unit fails the run naming .../wallpaperx-daemon.service: state 2".
    expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(overriding),
           QStringLiteral("an overriding unit fails the run naming %1: %2").arg(overriding, describe(setup)));
    expect(!systemd.calls.join(u';').contains(QLatin1String("RestartUnit")) && !QFileInfo::exists(install->stamp()),
           QStringLiteral("with an overriding unit nothing is restarted and no stamp is written"));
    // GOAL: S16.15 -- a first-run failure says nothing about a kept copy (there is none) and sends no
    //   rollback calls; S16.20 -- setup's warning goes through Qt's message handler.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp finish() adding the kept-copy sentence with no
    //   rollback: red, "FAIL: a first-run failure says nothing about a kept copy"; 2026-11-06, end()'s
    //   qWarning deleted: red, "FAIL: setup's warning reached Qt's message handler".
    expect(!setup.message().contains(QLatin1String("previous copy")) && drained() && !systemd.calls.contains(kResetCall),
           QStringLiteral("a first-run failure says nothing about a kept copy: %1").arg(describe(setup)));
    expect(g_warnings.filter(QStringLiteral("setup did not finish")).size() == 1,
           QStringLiteral("setup's warning reached Qt's message handler: %1").arg(g_warnings.join(u'|')));

}

// S16.15: the override found after `current` was switched.
void fragmentAfterSwitch() {
    const QString overriding = g_scratch + QStringLiteral("/fragment-switched-override/wallpaperx-daemon.service");
    if (!writeFile(overriding, "[Service]\nExecStart=/bin/true\n")) {
        fault(kGoals, QStringLiteral("could not write the overriding unit"));
        return;
    }
    {
        // GOAL: S16.15 -- a failure after the switch but before any restart (here the FragmentPath
        //   check) points `current` back, says the copy is kept, and asks for no restart at all.
        // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp switchBackFiles() restarting whatever
        //   `restarted` says: red, "FAIL: an override after a switch swaps back without a restart".
        if (dbusTestTool().isEmpty()) {
            notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from the previous copy");
            return;
        }
        const auto switched = freshInstall(QStringLiteral("fragment-switched"), dbusTestTool());
        if (!switched || !layInstalled(*switched, QString::fromLatin1(kOld))) {
            fault(kGoals, QStringLiteral("could not lay out the switched override case"));
            return;
        }
        StandInSystemd again(switched->unitFile(), switched->runtime(), {.fragmentPath = overriding});
        if (!again.own() || runOldAsMain(*switched, again) == nullptr) {
            notVerified(kGoals, "the stand-in systemd or the daemon running from the previous copy could not be started");
            return;
        }
        FirstRunSetup update(QDBusConnection::sessionBus(), switched->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
        const bool done = runToEnd(update);
        expect(done && update.state() == FirstRunSetup::State::Failed && update.message().contains(QLatin1String("Your previous copy of it is kept.")) &&
                   QFile::symLinkTarget(switched->runtime() + QStringLiteral("/current")) == switched->runtime() + QStringLiteral("/0.0.1"),
               QStringLiteral("an override after a switch points current back and says the copy is kept: %1").arg(describe(update)));
        expect(drained() && !again.calls.contains(kResetCall) && !again.calls.contains(kRestartCall),
               QStringLiteral("an override after a switch swaps back without a restart: %1").arg(again.calls.join(QStringLiteral("; "))));
    }
}

// S16.19: a FragmentPath that cannot be compared with the written unit.
void fragmentUncomparable() {
    {
        // GOAL: S16.19 -- a FragmentPath that cannot be compared (it does not exist) fails the run with
        //   "could not compare" and tells the user to remove nothing.
        // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp installService() ignoring fs::equivalent's
        //   error: red, "FAIL: a FragmentPath that cannot be compared fails saying so, advising nothing:
        //   ... said \"systemd reads the WallpaperX service from ...".
        const auto unreadable = freshInstall(QStringLiteral("fragment-uncomparable"));
        if (!unreadable) return;
        const QString nowhere = unreadable->root + QStringLiteral("/nowhere/wallpaperx-daemon.service");
        StandInSystemd third(unreadable->unitFile(), unreadable->runtime(), {.fragmentPath = nowhere});
        if (!third.own()) {
            notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
            return;
        }
        FirstRunSetup run(QDBusConnection::sessionBus(), unreadable->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
        const bool done = runToEnd(run);
        expect(done && run.state() == FirstRunSetup::State::Failed && run.message().contains(QLatin1String("could not compare")) &&
                   run.message().contains(nowhere) && !run.message().contains(QLatin1String("Remove")),
               QStringLiteral("a FragmentPath that cannot be compared fails saying so, advising nothing: %1").arg(describe(run)));
    }
}

// S16.6: answers that come in an unusual order or with an error that is no reason to stop.
void succeedsDespite(const QString& name, Behaviour behaviour, const QString& what) {
    const auto install = freshInstall(name);
    if (!install) return;
    StandInSystemd systemd(install->unitFile(), install->runtime(), behaviour);
    if (!systemd.own()) {
        notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
    const bool ended = runToEnd(setup);
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp: restartDaemon() not looking up m_endedJobs
    //   (red, "FAIL: a job that ended before RestartUnit answered is honoured"), and ResetFailedUnit
    //   given a failure sentence (red, "FAIL: a ResetFailedUnit error does not stop setup").
    expect(ended && setup.state() == FirstRunSetup::State::Done && QFileInfo::exists(install->stamp()), QStringLiteral("%1: %2").arg(what, describe(setup)));
}

// S16.6: after "Try again", an answer meant for the previous run changes nothing.
void lateAnswer() {
    const auto install = freshInstall(QStringLiteral("late-answer"));
    if (!install) return;
    // Run 1: the daemon takes its name 8 s in, and the MainPID answer that would confirm it is held
    // back, so the 30-second limit ends run 1 while that call is outstanding (QtDBus's own 25-second
    // call timeout, counted from the 8 s mark, has not run out).
    StandInSystemd systemd(install->unitFile(), install->runtime(), {.registerDelayMs = 8000, .holdConfirmingMainPid = true});
    if (!systemd.own()) {
        notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
    if (!runToEnd(setup) || setup.state() != FirstRunSetup::State::Failed || !systemd.unit.holding()) {
        fault(kGoals, QStringLiteral("run 1 did not end at the limit with the MainPID answer held: %1").arg(describe(setup)));
        return;
    }
    // Run 2: MainPID names another process, so run 2 can never confirm on its own.
    systemd.behaviour = {.daemon = Daemon::OtherPid};
    const int getsBefore = systemd.unit.mainPidGets();
    bool ended = false;
    const auto connection = QObject::connect(&setup, &FirstRunSetup::finished, [&] { ended = true; });
    setup.start();
    // Run 2 is waiting for the daemon once it has asked MainPID three times: for the copy it would
    // switch back to, before the restart, and to confirm.
    if (!waitUntil([&] { return systemd.unit.mainPidGets() >= getsBefore + 3 || ended; }, 10000) || ended) {
        QObject::disconnect(connection);
        fault(kGoals, QStringLiteral("run 2 did not reach the wait for the daemon: %1").arg(describe(setup)));
        return;
    }
    systemd.unit.release(uint(::getpid()));
    const bool inTime = waitUntil([&] { return ended; }, kRunMs);
    QObject::disconnect(connection);
    // GOAL: S16.6 -- run 1's late MainPID answer, which would confirm run 1's daemon, does not confirm
    //   run 2's: run 2 still ends Failed at its own limit, with no stamp.
    // WHY-THIS-SHAPE: run 2 is built to fail on its own, so the late answer is the only thing that
    //   could make it succeed; a run 2 that succeeds anyway could not tell a guarded answer from one used.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp callInRun() without its run-number guard: red,
    //   "FAIL: a late answer from the run before did not confirm this run's daemon: state 2".
    expect(inTime && setup.state() == FirstRunSetup::State::Failed && !QFileInfo::exists(install->stamp()),
           QStringLiteral("a late answer from the run before did not confirm this run's daemon: %1").arg(describe(setup)));
}

// S16.8: the package swap where the installed package is a link, and the fallback without exchange.
void packageSwap() {
    {
        const auto install = freshInstall(QStringLiteral("package-link"));
        const QString elsewhere = install ? install->root + QStringLiteral("/elsewhere/pkg") : QString();
        std::error_code ec;
        if (!install || !writeFile(elsewhere + QStringLiteral("/metadata.json"), "old") || !writeFile(elsewhere + QStringLiteral("/old-only"), "old")) {
            fault(kGoals, QStringLiteral("could not lay out the linked package"));
            return;
        }
        fs::create_directories(native(install->package()).parent_path(), ec);
        fs::create_symlink(native(elsewhere), native(install->package()), ec);
        StandInSystemd systemd(install->unitFile(), install->runtime());
        if (ec || !systemd.own()) {
            notVerified(kGoals, "the linked package or the stand-in could not be set up");
            return;
        }
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
        const bool ended = runToEnd(setup);
        // GOAL: S16.8 -- a linked old package is replaced by a real folder, and the folder it pointed
        //   at is not written through or deleted.
        // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp installPackage() copying over the installed
        //   package in place: red, "FAIL: a linked package is replaced by a folder ...".
        expect(ended && setup.state() == FirstRunSetup::State::Done && !QFileInfo(install->package()).isSymLink() &&
                   QFileInfo(install->package() + QStringLiteral("/contents/ui/WallpaperXPlugin/qmldir")).isFile(),
               QStringLiteral("a linked package is replaced by a folder holding the new package: %1").arg(describe(setup)));
        expect(readAll(elsewhere + QStringLiteral("/metadata.json")) == QLatin1String("old") && QFileInfo::exists(elsewhere + QStringLiteral("/old-only")) &&
                   !QFileInfo::exists(elsewhere + QStringLiteral("/contents")),
               QStringLiteral("the folder the link named is untouched"));
    }
    // GOAL: S16.8 -- without RENAME_EXCHANGE the old package goes aside and the new one in; if moving
    //   the new one in fails, the old one is put back and the run fails.
    // WHY-THIS-SHAPE: renameat2 and rename are interposed in this test binary only, failing just the
    //   exchange and the move of `.com.wallpaperx.wallpaper.new`; tmpfs supports exchange, so without
    //   this the fallback is never reached.
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp installPackage() not moving the old package
    //   back: red, "FAIL: a failed move puts the old package back".
    // COVERAGE-ASSERT: the number of refused exchanges and moves is asserted, so a run that took the
    //   ordinary path cannot pass as the fallback.
    for (const bool moveFails : {false, true}) {
        if (moveFails && dbusTestTool().isEmpty()) {
            notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from the previous copy");
            return;
        }
        const auto install = freshInstall(moveFails ? QStringLiteral("package-fallback-fails") : QStringLiteral("package-fallback"),
                                          moveFails ? dbusTestTool() : QString());
        // The failing case updates an install whose daemon runs, so a copy to roll back to exists (S16.15 below).
        if (!install || (moveFails && !layInstalled(*install, QString::fromLatin1(kOld))) ||
            !writeFile(install->package() + QStringLiteral("/metadata.json"), "old") || !writeFile(install->package() + QStringLiteral("/old-only"), "old")) {
            fault(kGoals, QStringLiteral("could not lay out the fallback package"));
            return;
        }
        StandInSystemd systemd(install->unitFile(), install->runtime());
        if (!systemd.own() || (moveFails && runOldAsMain(*install, systemd) == nullptr)) {
            notVerified(kGoals, "the stand-in systemd or the daemon running from the previous copy could not be started");
            return;
        }
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
        g_exchangesRefused = 0;
        g_movesRefused = 0;
        g_refuseExchange = true;
        g_refuseMovingPackageIn = moveFails;
        const bool ended = runToEnd(setup);
        g_refuseExchange = false;
        g_refuseMovingPackageIn = false;
        expect(g_exchangesRefused > 0 && (!moveFails || g_movesRefused > 0),
               QStringLiteral("the fallback was reached: %1 exchanges and %2 moves refused").arg(g_exchangesRefused.load()).arg(g_movesRefused.load()));
        const bool leftovers = QFileInfo::exists(install->paths.dataHome + QStringLiteral("/plasma/wallpapers/.com.wallpaperx.wallpaper.old")) ||
                               QFileInfo::exists(install->paths.dataHome + QStringLiteral("/plasma/wallpapers/.com.wallpaperx.wallpaper.new"));
        if (moveFails) {
            expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(QLatin1String("Could not install the wallpaper plugin")),
                   QStringLiteral("a failed move fails the run: %1").arg(describe(setup)));
            expect(readAll(install->package() + QStringLiteral("/metadata.json")) == QLatin1String("old") &&
                       QFileInfo::exists(install->package() + QStringLiteral("/old-only")),
                   QStringLiteral("a failed move puts the old package back"));
            expect(readAll(install->stamp()) == QStringLiteral("0.0.1\n"), QStringLiteral("a failed move writes no stamp"));
            // GOAL: S16.15 -- a failure after the new daemon is confirmed (here installing the plugin)
            //   keeps `current` on the new copy, claims no kept copy and asks for no second restart.
            // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp confirmDaemon() not clearing the
            //   rollback: red, "FAIL: a failure after confirmation leaves current on the new copy".
            expect(QFile::symLinkTarget(install->runtime() + QStringLiteral("/current")) == install->runtime() + u'/' + QString::fromLatin1(kVersion) &&
                       !setup.message().contains(QLatin1String("previous copy")) && drained() && countOf(systemd.calls, kRestartCall) == 1,
                   QStringLiteral("a failure after confirmation leaves current on the new copy: %1; asked %2")
                       .arg(describe(setup), systemd.calls.join(QStringLiteral("; "))));
        } else {
            expect(ended && setup.state() == FirstRunSetup::State::Done && QFileInfo(install->package() + QStringLiteral("/contents/ui/WallpaperXPlugin/qmldir")).isFile() &&
                       !QFileInfo::exists(install->package() + QStringLiteral("/old-only")),
                   QStringLiteral("without exchange the new package replaces the old whole: %1").arg(describe(setup)));
        }
        expect(!leftovers, QStringLiteral("no .old or .new package is left beside it (move %1)").arg(moveFails ? "failed" : "worked"));
    }
}

// S16.10: the unit's escaping.
void unitEscaping() {
    {
        const QString library = g_scratch + QStringLiteral("/escaping/da ta%x/wallpaperx");
        const auto install = freshInstall(QStringLiteral("escaping"), {}, library);
        if (!install) return;
        StandInSystemd systemd(install->unitFile(), install->runtime());
        if (!systemd.own()) {
            notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
            return;
        }
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
        const bool ended = runToEnd(setup);
        expect(ended && setup.state() == FirstRunSetup::State::Done, QStringLiteral("a library path with a space and %% installs: %1").arg(describe(setup)));
        const QString unit = readAll(install->unitFile());
        QString escaped = library + QStringLiteral("/runtime/current/bin/wallpaperx-daemon");
        escaped.replace(u'%', QStringLiteral("%%"));
        // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp serviceUnitText() not doubling %: red,
        //   "FAIL: a space and % give a quoted ExecStart ...".
        expect(unitValue(unit, QStringLiteral("ExecStart")) == QStringLiteral(":\"%1\"").arg(escaped) &&
                   unitValue(unit, QStringLiteral("ConditionFileIsExecutable")) == escaped,
               QStringLiteral("a space and %% give a quoted ExecStart and a %%%% condition:\n%1").arg(unit));
        const QString analyze = QStandardPaths::findExecutable(QStringLiteral("systemd-analyze"));
        if (analyze.isEmpty()) {
            notVerified(kGoals, "systemd-analyze is not installed, so the escaped unit is not checked by systemd itself");
        } else {
            QProcess verify;
            verify.setProcessChannelMode(QProcess::MergedChannels);
            verify.start(analyze, {QStringLiteral("--user"), QStringLiteral("verify"), QStringLiteral("--man=no"), install->unitFile()});
            const bool done = verify.waitForFinished(30000);
            const QString said = QString::fromUtf8(verify.readAll());
            std::printf("--- systemd-analyze verify ---\n%s\n", qUtf8Printable(said));
            if (!done || said.contains(QLatin1String("Failed to initialize manager")) || said.contains(QLatin1String("Failed to connect"))) {
                notVerified(kGoals, "systemd-analyze could not run here: " + said.left(200).toStdString());
            } else {
                // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp serviceUnitText() writing ExecStart unquoted:
                //   red, "FAIL: systemd-analyze verify accepts the escaped unit".
                expect(verify.exitStatus() == QProcess::NormalExit && verify.exitCode() == 0 && !said.contains(install->unitFile() + u':'),
                       QStringLiteral("systemd-analyze verify accepts the escaped unit: exit %1").arg(verify.exitCode()));
            }
        }
    }
    // CONTROL: 2026-11-05, app/setup/first_run_setup.cpp serviceUnitText()'s refused pattern never matching:
    //   red, "FAIL: a library path with ... is refused with a sentence ...".
    const auto install = freshInstall(QStringLiteral("escaping-refused"));
    if (!install) return;
    StandInSystemd systemd(install->unitFile(), install->runtime());
    if (!systemd.own()) {
        notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
        return;
    }
    int refusedCount = 0;
    for (const QString& bad : {QStringLiteral("\""), QStringLiteral("\\"), QStringLiteral("*"), QStringLiteral("?"), QStringLiteral("["), QStringLiteral("\x01")}) {
        SetupPaths paths = install->paths;
        paths.library = install->root + QStringLiteral("/lib") + bad + QStringLiteral("x/wallpaperx");
        paths.dataHome = install->root + QStringLiteral("/data-") + QString::number(refusedCount);
        FirstRunSetup setup(QDBusConnection::sessionBus(), paths, asSource(QString::fromLatin1(kVersion)), plasmaWayland());
        const bool ended = runToEnd(setup);
        expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(QLatin1String("systemd cannot run a program")) &&
                   !QFileInfo::exists(paths.dataHome + QStringLiteral("/systemd/user/wallpaperx-daemon.service")),
               QStringLiteral("a library path with U+%1 is refused with a sentence and no unit: %2").arg(int(bad.at(0).unicode()), 4, 16, QChar(u'0')).arg(describe(setup)));
        ++refusedCount;
    }
    expect(refusedCount == 6, QStringLiteral("six refused characters were tried"));
}

const QString kNewCurrent = QString::fromLatin1(kVersion);

// An update from 0.0.1 with a daemon running from it, as failsKeepingOld lays it.
std::optional<Install> updateFromOld(const QString& name) {
    if (dbusTestTool().isEmpty()) {
        notVerified(kGoals, "dbus-test-tool is not installed, so no daemon can run from the previous copy");
        return std::nullopt;
    }
    auto install = freshInstall(name, dbusTestTool());
    if (!install || !layInstalled(*install, QString::fromLatin1(kOld))) {
        fault(kGoals, QStringLiteral("could not lay out phase %1").arg(name));
        return std::nullopt;
    }
    return install;
}

bool copyBundleDaemon(const Install& install, const QString& to) {
    std::error_code ec;
    fs::create_directories(native(to).parent_path(), ec);
    fs::copy(native(install.paths.bundle + QStringLiteral("/daemon")), native(to), fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    return !ec;
}

bool pointCurrent(const Install& install, const QString& folder) {
    std::error_code ec;
    fs::remove(native(install.runtime() + QStringLiteral("/current")), ec);
    fs::create_symlink(native(folder), native(install.runtime() + QStringLiteral("/current")), ec);
    return !ec;
}

QString currentTarget(const Install& install) { return QFile::symLinkTarget(install.runtime() + QStringLiteral("/current")); }

// S16.26: the interrupted switch of interruptedSwitch(), and then the restart fails.
void interruptedRollback() {
    const auto install = updateFromOld(QStringLiteral("interrupted-rollback"));
    if (!install) return;
    if (!copyBundleDaemon(*install, install->runtime() + u'/' + kNewCurrent) || !pointCurrent(*install, kNewCurrent)) {
        fault(kGoals, QStringLiteral("could not lay out the interrupted switch"));
        return;
    }
    StandInSystemd systemd(install->unitFile(), install->runtime(), {.jobResult = QStringLiteral("failed")});
    if (!systemd.own() || runOldAsMain(*install, systemd) == nullptr) {
        notVerified(kGoals, "the stand-in systemd or the daemon running from the previous copy could not be started");
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(kNewCurrent), plasmaWayland());
    const bool ended = runToEnd(setup);
    // GOAL: S16.26 -- with `current` already on the new copy and the daemon on 0.0.1, a failed
    //   restart points `current` back at 0.0.1 (the copy the daemon runs, not the one `current` named),
    //   restarts once more and says the copy is kept.
    // WHY-THIS-SHAPE: `current` names the new folder before the run, so a rollback target read from
    //   `current` is the new copy itself; only a target read from the running process passes.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp previousCopy() always taking the target from
    //   `current`: red, "FAIL: an interrupted switch whose restart fails points current back at 0.0.1"
    //   and "... rollback restarts once more".
    expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(QLatin1String("Your previous copy of it is kept.")) &&
               currentTarget(*install) == install->runtime() + QStringLiteral("/0.0.1"),
           QStringLiteral("an interrupted switch whose restart fails points current back at 0.0.1: %1; current %2").arg(describe(setup), currentTarget(*install)));
    expect(drained() && countOf(systemd.calls, kRestartCall) == 2 && systemd.calls.mid(systemd.calls.size() - 2) == QStringList{kResetCall, kRestartCall},
           QStringLiteral("an interrupted switch's rollback restarts once more: %1").arg(systemd.calls.join(QStringLiteral("; "))));
}

// S16.27: a failed restart with no copy under runtime/ that the daemon provably runs: `current` decides.
void previousCopyCases() {
    // GOAL: S16.27 -- with no process provably running a copy under runtime/ (no daemon, a daemon run
    //   from a 0.0.1 folder outside runtime/, one running a deleted 0.0.1 executable), a failed restart
    //   goes back to the complete copy `current` named before the run: `current` back on 0.0.1, the copy
    //   said kept, one more restart. With no daemon and `current` already on the new folder, or naming a
    //   0.0.1 with no executable, there is nothing to go back to: `current` stays on the new copy, no kept
    //   copy is claimed and no second restart is asked.
    // WHY-THIS-SHAPE: the first three keep runtime/0.0.1 complete and vary only the process, so only the
    //   `current` fallback rolls them back; the last two hold the process at none and vary only what
    //   `current` names, so a fallback that skipped the own-folder or completeness check rolls them back.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp previousCopy() without its `current` fallback:
    //   red, "FAIL: with no daemon (MainPID 0) a failed restart goes back to the copy current named", and
    //   the outside and deleted cases; without isCompleteCopy(): red, "FAIL: with no daemon and current on
    //   a 0.0.1 with no executable ..."; without `folder == ownName`: red, "... current already on the new
    //   folder a failed restart does not switch back".
    // COVERAGE-ASSERT: the number of cases run is asserted.
    int tried = 0;
    const auto failedRestart = [&](const QString& name, const QString& what, bool rollsBack, auto prepare) {
        const auto install = updateFromOld(name);
        if (!install) return;
        StandInSystemd systemd(install->unitFile(), install->runtime(), {.jobResult = QStringLiteral("failed")});
        if (!systemd.own() || !prepare(*install, systemd)) {
            fault(kGoals, QStringLiteral("could not prepare the case: %1").arg(what));
            return;
        }
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(kNewCurrent), plasmaWayland());
        const bool ended = runToEnd(setup);
        const QString wanted = install->runtime() + u'/' + (rollsBack ? QString::fromLatin1(kOld) : kNewCurrent);
        const bool kept = setup.message().contains(QLatin1String("Your previous copy of it is kept."));
        expect(ended && setup.state() == FirstRunSetup::State::Failed && currentTarget(*install) == wanted &&
                   (rollsBack ? kept : !setup.message().contains(QLatin1String("previous copy"))) && drained() &&
                   countOf(systemd.calls, kRestartCall) == (rollsBack ? 2 : 1),
               QStringLiteral("with %1 a failed restart %2: %3; current %4; asked %5")
                   .arg(what, rollsBack ? QStringLiteral("goes back to the copy current named") : QStringLiteral("does not switch back"),
                        describe(setup), currentTarget(*install), systemd.calls.join(QStringLiteral("; "))));
        ++tried;
    };
    failedRestart(QStringLiteral("previous-none"), QStringLiteral("no daemon (MainPID 0)"), true, [](const Install&, StandInSystemd&) { return true; });
    failedRestart(QStringLiteral("previous-outside"), QStringLiteral("a daemon run from outside runtime/"), true, [](const Install& install, StandInSystemd& systemd) {
        const QString elsewhere = install.root + QStringLiteral("/elsewhere/0.0.1");
        QProcess* p = copyBundleDaemon(install, elsewhere) ? systemd.launch(elsewhere + QStringLiteral("/bin/wallpaperx-daemon"), kNotTheDaemon) : nullptr;
        if (p != nullptr) systemd.runAsMain(p);
        return p != nullptr;
    });
    failedRestart(QStringLiteral("previous-deleted"), QStringLiteral("a daemon running a deleted executable"), true, [](const Install& install, StandInSystemd& systemd) {
        QProcess* p = runOldAsMain(install, systemd);
        std::error_code ec;
        fs::remove_all(native(install.runtime() + QStringLiteral("/0.0.1")), ec);
        return p != nullptr && !ec && copyBundleDaemon(install, install.runtime() + QStringLiteral("/0.0.1"));
    });
    failedRestart(QStringLiteral("previous-current-new"), QStringLiteral("no daemon and current already on the new folder"), false,
                  [](const Install& install, StandInSystemd&) {
        return copyBundleDaemon(install, install.runtime() + u'/' + kNewCurrent) && pointCurrent(install, kNewCurrent);
    });
    failedRestart(QStringLiteral("previous-incomplete"), QStringLiteral("no daemon and current on a 0.0.1 with no executable"), false,
                  [](const Install& install, StandInSystemd&) { return QFile::remove(install.runtime() + QStringLiteral("/0.0.1/bin/wallpaperx-daemon")); });
    expect(tried == 5, QStringLiteral("five previous-copy cases were run, not %1").arg(tried));
}

// S16.28: the daemon runs the new copy by the time setup decides on the restart, and then never takes
// its name: from 0.0.1 at the start of the run, or already before it.
void noRestartSwitchBack() {
    for (const bool cameDuringRun : {true, false}) {
        const auto install = updateFromOld(cameDuringRun ? QStringLiteral("no-restart-came") : QStringLiteral("no-restart-before"));
        if (!install) return;
        const QString fresh = install->runtime() + u'/' + kNewCurrent;
        StandInSystemd systemd(install->unitFile(), install->runtime());
        QProcess* running = nullptr;
        if (!copyBundleDaemon(*install, fresh) || !systemd.own() || (cameDuringRun && runOldAsMain(*install, systemd) == nullptr) ||
            (running = systemd.launch(fresh + QStringLiteral("/bin/wallpaperx-daemon"), kNotTheDaemon + QStringLiteral("2"))) == nullptr) {
            notVerified(kGoals, "the stand-in systemd or the daemons running from the copies could not be started");
            return;
        }
        if (cameDuringRun) {
            // After the rollback target was read (0.0.1) and before startDaemon() reads MainPID again.
            systemd.onReload = [&] { systemd.unit.setMainPid(uint(running->processId())); };
        } else {
            systemd.runAsMain(running);
        }
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(kNewCurrent), plasmaWayland());
        const bool ended = runToEnd(setup);
        // GOAL: S16.28 -- a failure on the no-restart path switches back only when the daemon came onto
        //   the new copy during the run, and then starts the old copy again: `current` back on 0.0.1, the
        //   copy said kept, ResetFailedUnit and RestartUnit asked. A daemon on the new copy since before
        //   the run is never switched back under, though `current` named a complete 0.0.1.
        // WHY-THIS-SHAPE: both cases start with `current` on a complete 0.0.1 and differ only in what ran
        //   at the start, so a rule keyed on `current` alone gets one of them wrong.
        // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp startDaemon()'s no-restart branch resetting
        //   the rollback, or not setting `restarted`: red, "FAIL: a daemon that came onto the new copy
        //   during the run is switched back and restarted". previousCopy() without its first line (the
        //   process runs this version's copy) stayed green: `folder == ownName` rejects the same case;
        //   without both: red, "FAIL: a daemon on the new copy since before the run is not switched back under".
        expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(QLatin1String("did not start within")),
               QStringLiteral("the no-restart path fails at the limit: %1").arg(describe(setup)));
        const bool answered = drained();
        if (cameDuringRun) {
            expect(currentTarget(*install) == install->runtime() + QStringLiteral("/0.0.1") &&
                       setup.message().contains(QLatin1String("Your previous copy of it is kept.")) && answered &&
                       systemd.calls.mid(systemd.calls.size() - 2) == QStringList{kResetCall, kRestartCall} && countOf(systemd.calls, kRestartCall) == 1,
                   QStringLiteral("a daemon that came onto the new copy during the run is switched back and restarted: %1; current %2; asked %3")
                       .arg(describe(setup), currentTarget(*install), systemd.calls.join(QStringLiteral("; "))));
        } else {
            expect(currentTarget(*install) == fresh && !setup.message().contains(QLatin1String("previous copy")) && answered &&
                       !systemd.calls.join(u';').contains(QLatin1String("RestartUnit")),
                   QStringLiteral("a daemon on the new copy since before the run is not switched back under: %1; current %2; asked %3")
                       .arg(describe(setup), currentTarget(*install), systemd.calls.join(QStringLiteral("; "))));
        }
    }
}

// S16.29: the unit text this run replaced is put back.
void unitRestore() {
    const QString overriding = g_scratch + QStringLiteral("/unit-restore-override/wallpaperx-daemon.service");
    // GOAL: S16.29 -- a run that replaced a different unit text puts that text back on a
    //   rollback and asks Reload first: before ResetFailedUnit and RestartUnit after a failed restart,
    //   alone after a failure before any restart.
    // WHY-THIS-SHAPE: the calls after the forward ones are compared whole, so a Reload after the
    //   restart, or a restore with no Reload, fails.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp switchBackFiles() skipping the unit: red,
    //   "FAIL: after a failed restart the unit's previous text is back" and "... before any restart ...";
    //   its Reload added after the restart calls: red, "FAIL: after a failed restart the rollback asks
    //   Reload, ResetFailedUnit, RestartUnit".
    for (const bool beforeRestart : {false, true}) {
        const auto install = updateFromOld(beforeRestart ? QStringLiteral("unit-restore-early") : QStringLiteral("unit-restore"));
        if (!install) return;
        const QByteArray older = "# an older template\n" + readAll(install->unitFile()).toUtf8();
        Behaviour behaviour{.jobResult = QStringLiteral("failed")};
        if (beforeRestart) behaviour = {.fragmentPath = overriding};
        StandInSystemd systemd(install->unitFile(), install->runtime(), behaviour);
        if (!writeFile(install->unitFile(), older) || (beforeRestart && !writeFile(overriding, "[Service]\nExecStart=/bin/true\n")) ||
            !systemd.own() || runOldAsMain(*install, systemd) == nullptr) {
            notVerified(kGoals, "the older unit, the stand-in or the daemon running from the previous copy could not be set up");
            return;
        }
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(kNewCurrent), plasmaWayland());
        const bool ended = runToEnd(setup);
        const QString when = beforeRestart ? QStringLiteral("after a failure before any restart") : QStringLiteral("after a failed restart");
        expect(ended && setup.state() == FirstRunSetup::State::Failed && readAll(install->unitFile()).toUtf8() == older,
               QStringLiteral("%1 the unit's previous text is back: %2").arg(when, describe(setup)));
        const bool answered = drained();
        const QString forwardLast = beforeRestart ? QStringLiteral("LoadUnit wallpaperx-daemon.service") : kRestartCall;
        const QStringList rollback = systemd.calls.mid(systemd.calls.indexOf(forwardLast) + 1);
        const QStringList wanted = beforeRestart ? QStringList{QStringLiteral("Reload")} : QStringList{QStringLiteral("Reload"), kResetCall, kRestartCall};
        expect(answered && rollback == wanted, QStringLiteral("%1 the rollback asks %2: %3").arg(when, wanted.join(QStringLiteral(", ")), systemd.calls.join(QStringLiteral("; "))));
    }
}

// S16.30: the run ends only once systemd has answered the rollback's restart.
void rollbackWaits() {
    const auto install = updateFromOld(QStringLiteral("rollback-waits"));
    if (!install) return;
    StandInSystemd systemd(install->unitFile(), install->runtime(), {.jobResult = QStringLiteral("failed"), .delayRollbackRestartMs = 1500});
    if (!systemd.own() || runOldAsMain(*install, systemd) == nullptr) {
        notVerified(kGoals, "the stand-in systemd or the daemon running from the previous copy could not be started");
        return;
    }
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(kNewCurrent), plasmaWayland());
    bool answeredAtEnd = false;
    QObject::connect(&setup, &FirstRunSetup::finished, [&] { answeredAtEnd = systemd.rollbackAnswered; });
    const bool ended = runToEnd(setup);
    // GOAL: S16.30 -- finished() is emitted only after systemd answered the rollback's RestartUnit.
    // WHY-THIS-SHAPE: the stand-in holds that answer 1.5 s and records sending it; checking the call
    //   list after the run cannot tell a call sent and waited for from one sent and forgotten.
    // CONTROL: 2026-11-06 (re-run after callInTurn() moved onto callInRun()), app/setup/first_run_setup.cpp
    //   switchBack() sending the calls and ending at once: red, "FAIL: the run ended only after systemd
    //   answered the rollback's restart".
    expect(ended && setup.state() == FirstRunSetup::State::Failed && countOf(systemd.calls, kRestartCall) == 2 && answeredAtEnd &&
               setup.message().contains(QLatin1String("Your previous copy of it is kept.")),
           QStringLiteral("the run ended only after systemd answered the rollback's restart: %1; asked %2").arg(describe(setup), systemd.calls.join(QStringLiteral("; "))));
}

// S16.31: the stand-in runs on a thread of its own, since setup's destructor blocks the main thread.
void destroyedMidRun() {
    const auto install = updateFromOld(QStringLiteral("destroyed-mid-run"));
    if (!install) return;
    const QByteArray older = "# an older template\n" + readAll(install->unitFile()).toUtf8();
    if (!writeFile(install->unitFile(), older)) {
        fault(kGoals, QStringLiteral("could not write the older unit"));
        return;
    }
    QThread thread;
    auto* anchor = new QObject;
    anchor->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, anchor, &QObject::deleteLater);
    thread.start();
    StandInSystemd* systemd = nullptr;
    QProcess old;
    struct Finally {
        std::function<void()> f;
        ~Finally() { f(); }
    } stop{[&] {
        if (systemd != nullptr) QMetaObject::invokeMethod(anchor, [&] { delete systemd; }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
        old.kill();
        old.waitForFinished(5000);
    }};
    bool owned = false;
    QMetaObject::invokeMethod(anchor, [&] {
        systemd = new StandInSystemd(install->unitFile(), install->runtime(), {.daemon = Daemon::None});
        owned = systemd->own();
    }, Qt::BlockingQueuedConnection);
    const auto callsNow = [&] {
        QStringList calls;
        QMetaObject::invokeMethod(anchor, [&] { calls = systemd->calls; }, Qt::BlockingQueuedConnection);
        return calls;
    };
    old.setProgram(install->runtime() + QStringLiteral("/0.0.1/bin/wallpaperx-daemon"));
    old.setArguments({QStringLiteral("black-hole"), QStringLiteral("--session"), QStringLiteral("--name=") + kNotTheDaemon});
    old.setProcessChannelMode(QProcess::ForwardedChannels);
    old.start();
    if (!owned || !old.waitForStarted(5000) ||
        !waitUntil([&] { return QDBusConnection::sessionBus().interface()->servicePid(kNotTheDaemon).value() == uint(old.processId()); }, 5000)) {
        notVerified(kGoals, "the threaded stand-in systemd or the daemon running from the previous copy could not be started");
        return;
    }
    systemd->unit.setMainPid(uint(old.processId()));
    systemd->unit.setActiveState(QStringLiteral("active"));
    auto setup = std::make_unique<FirstRunSetup>(QDBusConnection::sessionBus(), install->paths, asSource(kNewCurrent), plasmaWayland());
    setup->start();
    // Daemon::None: past the restart the run waits 30 s for a name nobody takes.
    if (!waitUntil([&] { return callsNow().contains(kRestartCall) || setup->state() != FirstRunSetup::State::Running; }, 15000) ||
        setup->state() != FirstRunSetup::State::Running) {
        fault(kGoals, QStringLiteral("the run did not reach its restart and stay running: %1").arg(describe(*setup)));
        return;
    }
    g_warnings.clear();
    g_previousHandler = qInstallMessageHandler(captureWarning);
    QElapsedTimer clock;
    clock.start();
    setup.reset();
    const qint64 took = clock.elapsed();
    qInstallMessageHandler(g_previousHandler);
    // Read without running this thread's event loop: whatever the stand-in holds was answered during the destructor.
    const QStringList calls = callsNow();
    // GOAL: S16.31 -- a setup destroyed mid-run, after its restart, points `current` back, puts
    //   the unit's previous text back and asks Reload, ResetFailedUnit and RestartUnit "fail", within
    //   setup's 3-second budget.
    // WHY-THIS-SHAPE: the destructor's calls block the main thread, so a stand-in on it could answer
    //   nothing and every call would wait out the budget; on its own thread it answers, and the calls
    //   are read straight after, before this thread's event loop could deliver anything late.
    // CONTROL: 2026-11-06 (re-run after the destructor gained its wait), app/setup/first_run_setup.cpp
    //   ~FirstRunSetup() returning at once: red, "FAIL: a setup destroyed mid-run points current back";
    //   its mode "fail" made "replace": red,
    //   "FAIL: a setup destroyed mid-run asks Reload, ResetFailedUnit, RestartUnit fail"; a 4-second
    //   sleep in it: red, "FAIL: a setup destroyed mid-run switched back within 3.5 s".
    expect(currentTarget(*install) == install->runtime() + QStringLiteral("/0.0.1") && readAll(install->unitFile()).toUtf8() == older,
           QStringLiteral("a setup destroyed mid-run points current back and puts the unit back: current %1").arg(currentTarget(*install)));
    expect(calls.mid(calls.indexOf(kRestartCall) + 1) ==
               QStringList{QStringLiteral("Reload"), kResetCall, QStringLiteral("RestartUnit wallpaperx-daemon.service fail")},
           QStringLiteral("a setup destroyed mid-run asks Reload, ResetFailedUnit, RestartUnit fail: %1").arg(calls.join(QStringLiteral("; "))));
    expect(took < 3500, QStringLiteral("a setup destroyed mid-run switched back within 3.5 s: %1 ms").arg(took));
    expect(g_warnings.filter(QStringLiteral("Your previous copy of it is kept.")).size() == 1,
           QStringLiteral("a setup destroyed mid-run warns that the previous copy is kept: %1").arg(g_warnings.join(u'|')));
}

// S16.32: the MainPID read made before anything changes answers an error.
void earlyMainPidError() {
    const auto install = updateFromOld(QStringLiteral("early-main-pid-error"));
    if (!install) return;
    const QString fresh = install->runtime() + u'/' + kNewCurrent;
    StandInSystemd systemd(install->unitFile(), install->runtime(), {.jobResult = QStringLiteral("failed")});
    if (!copyBundleDaemon(*install, fresh) || !pointCurrent(*install, kNewCurrent) || !systemd.own() || runOldAsMain(*install, systemd) == nullptr) {
        notVerified(kGoals, "the interrupted switch, the stand-in or the daemon running from the previous copy could not be set up");
        return;
    }
    systemd.unit.errorNextMainPid();
    FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(kNewCurrent), plasmaWayland());
    const bool ended = runToEnd(setup);
    const bool answered = drained();
    // GOAL: S16.32 -- when the early MainPID read errors, setup carries on to the restart and `current`
    //   alone picks the rollback target.
    // WHY-THIS-SHAPE: the interrupted-switch layout (S16.26), where the process (0.0.1) and `current`
    //   (the new folder, so no target) disagree: ending at the job's "(failed)" with `current` left on the
    //   new copy and one RestartUnit shows both; a plain update, where both name 0.0.1, could not.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp findPreviousCopy()'s MainPID read given a failure
    //   text: red, "FAIL: an early MainPID read that errors leaves setup going, ...: state 4, step 1"; callInRun()
    //   dropping an optional call's error answer: red, the same line with "state 1" (stalled to the 45 s limit).
    // COVERAGE-ASSERT: the refused read is counted, so a stand-in that never refused cannot pass.
    expect(systemd.unit.mainPidErrors() == 1, QStringLiteral("the stand-in refused the early MainPID read: %1 refused").arg(systemd.unit.mainPidErrors()));
    expect(ended && setup.state() == FirstRunSetup::State::Failed && setup.message().contains(QLatin1String("(failed)")) && currentTarget(*install) == fresh &&
               !setup.message().contains(QLatin1String("previous copy")) && answered && countOf(systemd.calls, kRestartCall) == 1,
           QStringLiteral("an early MainPID read that errors leaves setup going, and current decides: %1; current %2; asked %3")
               .arg(describe(setup), currentTarget(*install), systemd.calls.join(QStringLiteral("; "))));
}

// Arms the rename hold for one phase, and lets it go however the phase ends.
struct RenameHold {
    explicit RenameHold(int which) {
        g_renameHeld = false;
        g_renameReleased = false;
        g_holdRename = which;
    }
    ~RenameHold() {
        g_holdRename = 0;
        release();
    }
    static void release() {
        g_renameReleased = true;
        g_renameReleased.notify_all();
    }
};

// S16.33, S16.34: setup destroyed while its copy step, or its unit write, runs on the pool thread.
void quitDuringStep(bool duringWrite) {
    const QString step = duringWrite ? QStringLiteral("unit write") : QStringLiteral("copy");
    const auto install = updateFromOld(duringWrite ? QStringLiteral("quit-during-write") : QStringLiteral("quit-during-copy"));
    if (!install) return;
    const QByteArray older = "# an older template\n" + readAll(install->unitFile()).toUtf8();
    StandInSystemd systemd(install->unitFile(), install->runtime());
    // No daemon runs, so the rollback target is the 0.0.1 `current` names (S16.27).
    if (!writeFile(install->unitFile(), older) || !systemd.own()) {
        notVerified(kGoals, "the older unit or the stand-in could not be set up");
        return;
    }
    auto setup = std::make_unique<FirstRunSetup>(QDBusConnection::sessionBus(), install->paths, asSource(kNewCurrent), plasmaWayland());
    RenameHold hold(duringWrite ? 2 : 1);
    setup->start();
    if (!waitUntil([&] { return g_renameHeld.load() || setup->state() != FirstRunSetup::State::Running; }, 15000) || !g_renameHeld) {
        fault(kGoals, QStringLiteral("the %1 was never held on the pool thread: %2").arg(step, describe(*setup)));
        return;
    }
    // Let go well after the destructor has begun; only the destructor's own wait can order it before.
    std::jthread releaser([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        RenameHold::release();
    });
    setup.reset();
    const bool releasedFirst = g_renameReleased.load();
    // A step the destructor did not wait for finishes now; its effect is what the checks below must see.
    const bool settled = QThreadPool::globalInstance()->waitForDone(15000);
    // GOAL: S16.33 -- a setup destroyed during the copy step waits for it, then leaves `current` on the
    //   previous copy; S16.34 -- one destroyed during the unit write still puts the old unit text back.
    // WHY-THIS-SHAPE: the step is held inside its rename by this binary's rename() until a thread lets it
    //   go 300 ms later, so the destructor provably starts mid-step; the pool is drained before the files
    //   are read, so a step finishing after the destructor is seen. A large copy would be a race.
    // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp ~FirstRunSetup() without m_work.waitForFinished():
    //   red, "FAIL: a setup destroyed during the copy returned only after that step was let go" and "...
    //   leaves current on the previous copy: current .../9.9.9-test"; red, "FAIL: a setup destroyed during
    //   the unit write puts the old unit text back" (its releasedFirst stays green: the unanswered Reload
    //   holds that destructor 3 s). installService() reading the old text in the write's continuation: red,
    //   the same unit-write line.
    expect(releasedFirst, QStringLiteral("a setup destroyed during the %1 returned only after that step was let go").arg(step));
    if (duringWrite) {
        expect(settled && readAll(install->unitFile()).toUtf8() == older,
               QStringLiteral("a setup destroyed during the unit write puts the old unit text back:\n%1").arg(readAll(install->unitFile())));
    } else {
        expect(settled && currentTarget(*install) == install->runtime() + QStringLiteral("/0.0.1") &&
                   !QFileInfo::exists(install->runtime() + QStringLiteral("/.current.new")),
               QStringLiteral("a setup destroyed during the copy leaves current on the previous copy: current %1").arg(currentTarget(*install)));
    }
}

// S16.13: closing the last window, and quit(), while a run is under way. Needs QGuiApplication (main()).
class ClosableWindow : public QWindow {
public:
    bool refuseClose = false;

protected:
    void closeEvent(QCloseEvent* event) override {
        if (refuseClose) event->ignore();
    }
};

void closeMidRun() {
    {
        const auto install = freshInstall(QStringLiteral("close-mid-run"));
        if (!install) return;
        // The daemon takes its name 1.5 s after the restart, so the run outlives the close by that much.
        StandInSystemd systemd(install->unitFile(), install->runtime(), {.registerDelayMs = 1500});
        if (!systemd.own()) {
            notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
            return;
        }
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(kNewCurrent), plasmaWayland());
        ClosableWindow window;
        window.show();
        bool closedWhileRunning = false;
        bool finishedSeen = false;
        bool timedOut = false;
        systemd.onRestart = [&] {
            closedWhileRunning = setup.state() == FirstRunSetup::State::Running;
            window.close();
        };
        QObject::connect(&setup, &FirstRunSetup::finished, [&] { finishedSeen = true; });
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, [&] {
            timedOut = true;
            QCoreApplication::exit(2);
        });
        guard.start(20000);
        QTimer::singleShot(0, &setup, &FirstRunSetup::start);
        QGuiApplication::exec();
        // GOAL: S16.13 -- with setup Running, closing the last window does not end the
        //   application; it ends once the run has ended.
        // WHY-THIS-SHAPE: a real exec() under QGuiApplication, since only a running event loop quits
        //   for its last window; waitUntil()'s processEvents never does.
        // CONTROL: 2026-11-06, app/setup/first_run_setup.cpp setState() never taking m_keepAlive: red,
        //   "FAIL: closing the last window mid-run ended the app only after the run ended".
        if (!closedWhileRunning) {
            fault(kGoals, QStringLiteral("the window was not closed while setup ran: %1").arg(describe(setup)));
        } else {
            expect(!timedOut && finishedSeen && setup.state() == FirstRunSetup::State::Done,
                   QStringLiteral("closing the last window mid-run ended the app only after the run ended: %1%2")
                       .arg(describe(setup), timedOut ? QStringLiteral(" (the app never quit)") : QString()));
        }
    }
    {
        const auto install = freshInstall(QStringLiteral("quit-mid-run"));
        if (!install) return;
        StandInSystemd systemd(install->unitFile(), install->runtime(), {.daemon = Daemon::None});
        if (!systemd.own()) {
            notVerified(kGoals, "the stand-in could not own org.freedesktop.systemd1 on this bus");
            return;
        }
        FirstRunSetup setup(QDBusConnection::sessionBus(), install->paths, asSource(kNewCurrent), plasmaWayland());
        ClosableWindow window;
        window.show();
        bool timedOut = false;
        systemd.onRestart = [&] { QTimer::singleShot(0, qApp, &QCoreApplication::quit); };
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, [&] {
            timedOut = true;
            QCoreApplication::exit(2);
        });
        guard.start(20000);
        QTimer::singleShot(0, &setup, &FirstRunSetup::start);
        QGuiApplication::exec();
        // GOAL: S16.13 -- quit() ends the application while setup runs and a window is shown.
        // WHY-THIS-SHAPE: Qt 6's quit() asks every window to close and gives up if one refuses, so the
        //   check is that exec() returned before the run ended, with the window still up when asked.
        // CONTROL: 2026-11-06, test-side: the window refusing its close event (refuseClose = true): red,
        //   "FAIL: quit() ended the app while setup ran". No production change in FirstRunSetup can make
        //   this false; Setup.qml, which once refused closes, is not loaded here.
        expect(!timedOut && setup.state() == FirstRunSetup::State::Running,
               QStringLiteral("quit() ended the app while setup ran: %1%2").arg(describe(setup), timedOut ? QStringLiteral(" (the app never quit)") : QString()));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "first_run_setup";
    if (const auto refusedHere = refuseUnlessUnderDbusRunSession()) return *refusedHere;
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified(kGoals, "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    // A GUI application on the offscreen platform, for close_mid_run: only it quits on its last window.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (const auto refusedHere = refuseUnlessPrivateBus()) return *refusedHere;
    g_scratch = scratch.path();
    const QString ownBundle = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();

    // "refused" first: its no-systemd case needs no stand-in to own the name yet.
    runPhase("refused", [&] { refused(); });
    runPhase("installs", [&] { installs(ownBundle); });
    runPhase("need", [&] { needPieces(); });
    runPhase("version", [&] { version(); });
    runPhase("half_bundle", [&] { halfBundle(); });
    runPhase("no_restart", [&] { noRestart(); });
    runPhase("no_restart_timeout", [&] { noRestartTimeout(); });
    runPhase("restart_cases", [&] { restartCases(); });
    runPhase("interrupted_switch", [&] { interruptedSwitch(); });
    runPhase("job_failed", [&] { failsKeepingOld(QStringLiteral("job_failed"), {.jobResult = QStringLiteral("failed")}, QStringLiteral("(failed)")); });
    runPhase("inactive", [&] { failsKeepingOld(QStringLiteral("inactive"), {.activeAfter = QStringLiteral("inactive")}, QStringLiteral("it is inactive")); });
    runPhase("restart_error", [&] { failsKeepingOld(QStringLiteral("restart_error"), {.restartErrors = true}, QStringLiteral("could not restart")); });
    runPhase("name_never_taken", [&] { failsKeepingOld(QStringLiteral("name_never_taken"), {.daemon = Daemon::None}, QStringLiteral("did not start within")); });
    runPhase("name_other_pid", [&] { failsKeepingOld(QStringLiteral("name_other_pid"), {.daemon = Daemon::OtherPid}, QStringLiteral("did not start within")); });
    runPhase("fragment_override", [&] { fragmentOverride(); });
    runPhase("fragment_after_switch", [&] { fragmentAfterSwitch(); });
    runPhase("fragment_uncomparable", [&] { fragmentUncomparable(); });
    runPhase("reset_failed_error", [&] {
        succeedsDespite(QStringLiteral("reset_failed_error"), {.resetFailedErrors = true}, QStringLiteral("a ResetFailedUnit error does not stop setup"));
    });
    runPhase("early_job_signal", [&] {
        succeedsDespite(QStringLiteral("early_job_signal"), {.signalBeforeReply = true}, QStringLiteral("a job that ended before RestartUnit answered is honoured"));
    });
    runPhase("switch_back_fails", [&] { switchBackFails(); });
    runPhase("damaged_copy", [&] { damagedCopy(); });
    runPhase("release", [&] { releaseBuild(); });
    runPhase("release_plain_folder", [&] { releaseFromPlainFolder(); });
    runPhase("late_answer", [&] { lateAnswer(); });
    runPhase("package_swap", [&] { packageSwap(); });
    runPhase("unit_escaping", [&] { unitEscaping(); });
    runPhase("interrupted_rollback", [&] { interruptedRollback(); });
    runPhase("previous_copy_cases", [&] { previousCopyCases(); });
    runPhase("no_restart_switch_back", [&] { noRestartSwitchBack(); });
    runPhase("unit_restore", [&] { unitRestore(); });
    runPhase("rollback_waits", [&] { rollbackWaits(); });
    runPhase("destroyed_mid_run", [&] { destroyedMidRun(); });
    runPhase("early_main_pid_error", [&] { earlyMainPidError(); });
    runPhase("quit_during_copy", [&] { quitDuringStep(false); });
    runPhase("quit_during_write", [&] { quitDuringStep(true); });
    // Last: it runs exec() and quits the application.
    runPhase("close_mid_run", [&] { closeMidRun(); });
    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file or its fixtures, not in the app");
    if (failed || faulted) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

#include "first_run_setup_test.moc"
