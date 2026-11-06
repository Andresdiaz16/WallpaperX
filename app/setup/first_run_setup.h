// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QEventLoopLocker>
#include <QFuture>
#include <QHash>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <variant>

class QDBusMessage;
class QTimer;

namespace wallpaperx {

// Where setup copies from and to. The application's are forThisApp(); a test passes its own.
struct SetupPaths {
    // The folder the app runs from, holding the bundle: daemon/{bin,share,build-id} and
    // plasma/wallpapers/com.wallpaperx.wallpaper/.
    QString bundle;
    // ~/.local/share/wallpaperx: the wallpapers, and runtime/ with the daemon's copies.
    QString library;
    // ~/.local/share: systemd/user/ and plasma/wallpapers/ are under it.
    QString dataHome;
    // The file Flatpak puts in its sandbox's root, /.flatpak-info for the app. Empty: never in Flatpak.
    QString flatpakInfo;

    static SetupPaths forThisApp();
};

// Why this session cannot run WallpaperX, as a sentence for the user; nothing when it can.
// Whether systemd runs for the user is not in the environment and is asked on the bus instead.
std::optional<QString> unsupportedSession(const QProcessEnvironment& environment, bool flatpakInfoExists);

// The daemon's unit as setup writes it for a library at `library`; the error is a sentence for the user.
std::expected<QByteArray, QString> serviceUnitText(const QString& library);

// Source: built from this repository, where rebuilding keeps the project version.
enum class BuildKind { Source, Release };

// What one install is known by. `folder` names the daemon's copy under runtime/ and always carries
// the daemon's build id, so a changed daemon is always a new copy beside the running one; `stamp`
// is what the stamp records, the plain version for a release.
struct SetupVersion {
    QString stamp;
    QString folder;
};

// The app's version, "+<build id>" read from the bundle's daemon/build-id added to the folder and,
// for a source build, to the stamp. The error is a sentence for the user: a bundle without a build id.
std::expected<SetupVersion, QString> setupVersion(const QString& appVersion, BuildKind kind, const QString& bundle);

// Installs what the app needs beside itself into the user's home: the library folder, a copy of
// the daemon run by a systemd user unit, and the wallpaper package. It never starts the daemon
// itself and never touches a screen's wallpaper.
//
// Every step is safe to run again, so a run that failed is retried by running it whole. The stamp
// is written last, so a run cut short is found again at the next start.
//
// While a run is under way the application does not quit for its last window closing: a window
// closed mid-run, by the user or by the session at logout, goes, and the run ends behind it. An
// explicit quit still ends the application, and the destructor then switches back as a failure would.
class FirstRunSetup : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application with the bus and folders it should use.")
    Q_PROPERTY(State state READ state NOTIFY changed)
    Q_PROPERTY(Step step READ step NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)

public:
    // NotBundled: nothing beside the app to install from, as when a package manager installed it;
    // there is then nothing for setup to do. Update covers a different version, anything missing, a
    // unit that is not the one setup would write, and a runtime copy or installed wallpaper package
    // whose build-id is not the bundle's.
    enum class Need { UpToDate, NotBundled, FirstRun, Update };
    // Refused: this session cannot run WallpaperX at all, and `message` says why.
    enum class State { Idle, Running, Done, Refused, Failed };
    Q_ENUM(State)
    enum class Step { Library, Daemon, Service, OldCopy, Plugin, Stamp };
    Q_ENUM(Step)

    static constexpr const char* kUnitName = "wallpaperx-daemon.service";
    static constexpr const char* kDaemonService = "io.wallpaperx.Daemon";

    // A `version` that is an error is reported by the run, which then fails.
    FirstRunSetup(QDBusConnection bus, SetupPaths paths, std::expected<SetupVersion, QString> version,
                  QProcessEnvironment environment, QObject* parent = nullptr);
    ~FirstRunSetup() override;

    // Read from the files alone; asks nothing on the bus.
    Need need() const;
    State state() const { return m_state; }
    Step step() const { return m_step; }
    QString message() const { return m_message; }

public slots:
    // Runs every step; nothing while a run is under way.
    void start();

signals:
    void changed();
    // The run ended: Done, Refused or Failed.
    void finished();

private slots:
    void onJobRemoved(uint id, const QDBusObjectPath& job, const QString& unit, const QString& result);
    void onDaemonRegistered();

private:
    void setState(State state);
    void setStep(Step step);
    // A failure while m_rollback is set first switches back, and the run ends once systemd has
    // answered the calls that needs.
    void finish(State state, const QString& message = {});
    void end(State state, const QString& message);
    // One call on systemd's manager; `failure` is a clause taking the error as %1, empty when an
    // error is no problem.
    struct ManagerCall {
        QString method;
        QVariantList arguments;
        QString failure;
    };
    // Points `current` back at m_rollback's copy and puts back the unit file this run replaced,
    // adding to `problems` what went wrong; the calls systemd then needs, in order, restarting in
    // `restartMode` if this run had restarted. The error: `current` could not be pointed back,
    // as a sentence, and nothing else was attempted.
    std::expected<QList<ManagerCall>, QString> switchBackFiles(const QString& restartMode, QStringList& problems);
    void switchBack(State state, const QString& message);
    // Makes each call after the one before has been answered, then `done` with the problems.
    void callInTurn(QList<ManagerCall> calls, QStringList problems, std::function<void(const QStringList&)> done);
    // Sets m_rollback to the copy a failed run goes back to, if there is one (previousCopy in the .cpp).
    void findPreviousCopy(std::function<void()> next);
    // Runs `work`, which returns std::expected<T, QString>, on a pool thread, then `next` here with
    // the T if it succeeded. Defined in the .cpp, the only place it is used.
    template <typename Work, typename Next>
    void runOffThread(Step step, Work work, Next next);
    void checkSystemd();
    void installService();
    // Restarts the daemon unless the unit's main process already runs this version's copy.
    void startDaemon();
    void restartDaemon();
    void awaitDaemon();
    void disconnectJobs();
    // Leaves the restart: stops the deadline and the job signals.
    void endRestart();
    void checkStarted();
    void confirmDaemon();
    void finishFiles();
    // Sends `message`; `done` gets the reply unless a new run began or this one ended meanwhile. An
    // error fails the run with `failure`, or goes to `done` when `failure` is empty. -1: the bus's timeout.
    void callInRun(const QDBusMessage& message, const QString& failure, std::function<void(const QDBusMessage&)> done,
                   int timeoutMs = -1);
    void callManager(const QString& method, const QVariantList& arguments, const QString& failure,
                     std::function<void(const QDBusMessage&)> done);
    // `done` gets an invalid QVariant for an error when `failure` is empty.
    void readUnitProperty(const QString& interface, const QString& property, const QString& failure,
                          std::function<void(const QVariant&)> done);

    QDBusConnection m_bus;
    const SetupPaths m_paths;
    const std::expected<SetupVersion, QString> m_version;
    const QProcessEnvironment m_environment;

    State m_state = State::Idle;
    Step m_step = Step::Library;
    QString m_message;

    // Each run's callbacks carry its number, so one answering after a retry began is ignored.
    quint64 m_run = 0;

    // Held exactly while the state is Running; taken and released only by setState.
    std::optional<QEventLoopLocker> m_keepAlive;

    // The pool-thread step last started; the destructor waits for it before switching back.
    QFuture<void> m_work;

    // Set before anything is changed, when there is another copy to go back to, until the new
    // copy's daemon is confirmed: the folder a failed run points `current` back at, the unit file
    // text this run replaces, and whether the daemon was restarted, or came, onto the new copy
    // during the run (so the old one has to be started again).
    struct Rollback {
        std::filesystem::path copy;
        std::optional<QByteArray> unit;
        bool restarted = false;
    };
    std::optional<Rollback> m_rollback;

    // The restart, from asking for it until the daemon is confirmed: first the job systemd runs it
    // as (its path empty until RestartUnit answers; job signals are connected only meanwhile), then
    // the daemon that job started, or the one already running this copy.
    struct AwaitingJob {
        QString path;
    };
    struct AwaitingDaemon {};
    std::variant<std::monostate, AwaitingJob, AwaitingDaemon> m_restart;
    // Jobs on the unit that ended before RestartUnit answered, by path, with their results.
    QHash<QString, QString> m_endedJobs;
    QString m_unitPath;
    // Armed while the restart is under way.
    QTimer* m_daemonDeadline = nullptr;
};

} // namespace wallpaperx
