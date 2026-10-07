// SPDX-License-Identifier: GPL-2.0-or-later

// app/main.cpp and app/single_instance.cpp: the app built from this tree, run as separate
// processes on a private bus, handing a second launch's files to the first over
// org.freedesktop.Application. Goals S15.83, S15.84, S15.93-S15.95 in GOALS.md.

#include "fake_kscreen.h"
#include "single_instance.h"
#include "wpx_fixtures.h"
#include "wpx_package.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

namespace {

constexpr const char* kGoals = "S15.83, S15.84";
constexpr int kStartMs = 30000;
// A hand-over is one bus call; the app gives up on its peer after 10 s.
constexpr int kHandOverMs = 20000;

// The environment every app run gets: offscreen, logging to stderr, and every folder it could
// write under `root`, so nothing lands in the user's own home.
QProcessEnvironment appEnvironment(const QString& root) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* name : {"WAYLAND_DISPLAY", "DISPLAY", "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID"}) {
        env.remove(QString::fromLatin1(name));
    }
    env.insert(QStringLiteral("HOME"), root + QStringLiteral("/home"));
    for (const char* name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"}) {
        env.insert(QString::fromLatin1(name), root + QStringLiteral("/xdg/") + QString::fromLatin1(name));
    }
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
    return env;
}

// Runs the app as `sh -c script app`, so an argument can be bytes that are not UTF-8.
void startApp(QProcess& app, const QString& root, const QString& workingDirectory, const QString& script,
              QProcess::ProcessChannelMode channels = QProcess::MergedChannels) {
    app.setProcessEnvironment(appEnvironment(root));
    app.setWorkingDirectory(workingDirectory);
    app.setProcessChannelMode(channels);
    app.setProgram(QStringLiteral("/bin/sh"));
    app.setArguments({QStringLiteral("-c"), script, QStringLiteral(WALLPAPERX_APP_BINARY)});
    app.start();
}

bool nameOwned() {
    return QDBusConnection::sessionBus().interface()->isServiceRegistered(QString::fromLatin1(kApplicationBusName)).value();
}

// Stops `app` if it is still running, SIGTERM first.
void stop(QProcess& app) {
    if (app.state() != QProcess::NotRunning) {
        app.terminate();
        if (!app.waitForFinished(5000)) {
            app.kill();
            static_cast<void>(app.waitForFinished(3000));
        }
    }
}

// Waits for `app` to exit; false at the bound, with the process killed.
bool exitsWithin(QProcess& app, int ms) {
    const bool finished = app.waitForFinished(ms);
    if (!finished) {
        stop(app);
    }
    return finished;
}

// S15.84: a path as a file URI and back.
void uriRoundTrip() {
    // GOAL: S15.84 -- a path handed over as a file URI comes back as exactly its bytes, for any
    //       bytes: valid UTF-8, bytes that are not UTF-8, '%', '#', '?' and spaces; and the URI
    //       itself is ASCII, so it is always a valid D-Bus string.
    // WHY-THIS-SHAPE: fixed edge cases plus random bytes, compared as bytes through the path's
    //       text (decodeFileName/encodeFileName), which is how the receiving instance opens it.
    // CONTROL: app/single_instance.cpp, fileUri()'s `QString::fromLatin1(path.toPercentEncoding("/"))`
    //       made `QString::fromLocal8Bit(path)` -> red, "S15.84: 2f 74 6d 70 2f 63 61 66 e9 ... came back as
    //       ..."; restored.
    QList<QByteArray> paths{"/tmp/plain.wpx", "/tmp/caf\xe9.wpx", "/tmp/caf\xc3\xa9.wpx", "/tmp/a b%20#frag?q.wpx", "/tmp/%E9.wpx"};
    QRandomGenerator random(15084);
    for (int i = 0; i < 200; ++i) {
        QByteArray bytes("/", 1);
        for (int n = int(random.bounded(1, 20)); n > 0; --n) {
            const char c = char(1 + random.bounded(255));
            bytes.append(c == '/' ? 'x' : c);
        }
        paths.append(bytes);
    }
    for (const QByteArray& path : std::as_const(paths)) {
        const QString uri = fileUri(path);
        const auto back = pathOfFileUri(uri);
        const QByteArray bytes = back ? encodeFileName(*back) : QByteArray();
        expect(back && bytes == path && QStringView(uri).isValidUtf16() && uri.toLatin1().size() == uri.size() &&
                   std::ranges::all_of(uri, [](QChar c) { return c.unicode() < 0x80; }),
               QStringLiteral("S15.84: %1 came back as %2 through %3").arg(QString::fromLatin1(path.toHex(' ')), QString::fromLatin1(bytes.toHex(' ')), uri));
    }

    // GOAL: S15.84, S15.93 -- an Open call's URI names a file here only when it has no host or the
    //       host localhost, an absolute path, and no NUL byte in it: file://otherhost/..., file:name
    //       and file:///...%00... are refused, never read as a local path; file://localhost/...
    //       keeps its bytes.
    // CONTROL: app/library/wpx_package.cpp, localPathOfFileUrl's host test made `false` -> red,
    //       "S15.84: file://otherhost/share/x.wpx gave "/share/x.wpx""; its
    //       `if (!path.startsWith('/') || path.contains('\0')) {` made `if (false) {` -> red, "S15.84:
    //       file:x.wpx gave "x.wpx"" and the %00 row; `|| path.contains('\0')` removed -> red, the
    //       %00 row alone, "S15.84: file:///tmp/x.wpx%00.jpg gave "/tmp/x.wpx<NUL>.jpg""; restored.
    for (const char* refused : {"file://otherhost/share/x.wpx", "file:x.wpx", "https://example.org/x.wpx", "file:///tmp/x.wpx%00.jpg"}) {
        const auto got = pathOfFileUri(QString::fromLatin1(refused));
        // A NUL shown as <NUL>: the report is a C string, which would end there.
        expect(!got, QStringLiteral("S15.84: %1 gave \"%2\"").arg(QLatin1String(refused), got.value_or(QString()).replace(QChar(0), QStringLiteral("<NUL>"))));
    }
    const auto local = pathOfFileUri(QStringLiteral("file://localhost/tmp/caf%E9.wpx"));
    expect(local && encodeFileName(*local) == QByteArray("/tmp/caf\xe9.wpx"),
           QStringLiteral("S15.84: file://localhost/tmp/caf%E9.wpx gave %1").arg(local ? QString::fromLatin1(encodeFileName(*local).toHex(' ')) : QStringLiteral("nothing")));
}

// S15.83: the command line's options.
void options(const QString& root) {
    struct Case {
        QString arguments;
        int exitCode;
        // Where the usage goes: stdout for help, stderr for a refusal.
        bool toStdout;
        // What a refusal names first; empty for help.
        QString names;
    };
    // A real package, so a path ended at the NUL would name a file that exists.
    const QString nulUrl = QStringLiteral("file://") + QString::fromLatin1(QFile::encodeName(root).toPercentEncoding("/")) + QStringLiteral("/nul.wpx%00.jpg");
    if (!writeZip(root + QStringLiteral("/nul.wpx"), planEntries(titled("Nul"))).isEmpty()) {
        harnessFailure("S15.93: nul.wpx could not be written");
        return;
    }
    const Case cases[] = {
        {QStringLiteral("--help"), 0, true, {}},
        {QStringLiteral("foo.wpx --help"), 0, true, {}},
        {QStringLiteral("-x -h"), 0, true, {}},
        {QStringLiteral("--bogus"), 2, false, QStringLiteral("wallpaperx: --bogus is not an option wallpaperx takes.")},
        {QStringLiteral("file://otherhost/share/x.wpx"), 2, false, QStringLiteral("wallpaperx: file://otherhost/share/x.wpx is not a file on this computer.")},
        {QStringLiteral("file:x.wpx"), 2, false, QStringLiteral("wallpaperx: file:x.wpx is not a file on this computer.")},
        {nulUrl, 2, false, QStringLiteral("wallpaperx: ") + nulUrl + QStringLiteral(" is not a file on this computer.")},
    };
    // GOAL: S15.83 -- `--help` or `-h` anywhere before `--` prints the usage to stdout and exits 0,
    //       whatever else is given; an option it does not know, or a file: URL that does not name
    //       a file on this computer (another host, or no absolute path), exits 2 naming that
    //       argument, with the usage on stderr. None starts the app.
    // WHY-THIS-SHAPE: stdout and stderr are read apart, since "exit 0 with the usage" printed to
    //       stderr is the refusal path with its status changed.
    // CONTROL: app/main.cpp, the Refused branch's `return 2;` made `return 0;` -> red, "S15.83:
    //       `--bogus` exited 0", the same for both file: URLs, and the S15.93 and S15.94 refusals;
    //       parseCommandLine's `argument == "--help" || argument == "-h"` made `i == 1 && (...)` (help
    //       only as the first argument) -> red, "S15.83: `foo.wpx --help` exited 2" and the same for
    //       `-x -h`; `std::fputs(kUsage, stdout);` made `stderr` -> red, "S15.83: `--help` exited 0 ...
    //       stdout []" and the other two help rows; app/library/wpx_package.cpp, localPathOfFileUrl's
    //       host test `!(url.host().isEmpty() || url.host() == u"localhost")` made `false` -> red,
    //       "S15.83: `file://otherhost/share/x.wpx` exited -1 (finished 0)"; its
    //       `if (!path.startsWith('/') || path.contains('\0')) {` made `if (false) {` -> red, "S15.83:
    //       `file:x.wpx` exited -1 (finished 0)" and the S15.93 row (each such run starts the app on
    //       the private bus, stopped at the bound); restored.
    // GOAL: S15.93 -- a file: URL whose path decodes to a NUL byte (".../nul.wpx%00.jpg", nul.wpx
    //       real) is refused on the command line like any URL that names no file here: exit 2.
    // CONTROL: app/library/wpx_package.cpp, localPathOfFileUrl's `if (!path.startsWith('/') || path.contains('\0')) {`
    //       made `if (!path.startsWith('/')) {` -> red, "S15.93: `file://.../nul.wpx%00.jpg` exited -1
    //       (finished 0)" (the app started, importing nul.wpx, and was stopped at the bound); restored.
    for (const Case& c : cases) {
        QProcess app;
        startApp(app, root, root, QStringLiteral("exec \"$0\" ") + c.arguments, QProcess::SeparateChannels);
        const bool finished = exitsWithin(app, 10000);
        const QString out = QString::fromLocal8Bit(app.readAllStandardOutput());
        const QString err = QString::fromLocal8Bit(app.readAllStandardError());
        const QString usageWhere = c.toStdout ? out : err;
        // One arg() call: an argument holding "%00" would itself be read as a placeholder.
        expect(finished && app.exitStatus() == QProcess::NormalExit && app.exitCode() == c.exitCode &&
                   usageWhere.contains(QLatin1String("Usage: wallpaperx")) && (c.names.isEmpty() || err.contains(c.names)),
               QStringLiteral("%1: `%2` exited %3 (finished %4); stdout [%5]; stderr [%6]")
                   .arg(c.arguments == nulUrl ? QStringLiteral("S15.93") : QStringLiteral("S15.83"), c.arguments,
                        QString::number(finished ? app.exitCode() : -1), QString::number(finished), out.trimmed().left(200), err.trimmed().left(300)));
    }

    // GOAL: S15.94 -- a relative argument given from a working folder that no longer exists is
    //       refused: exit 2, "wallpaperx: <it> is a relative path, and the current folder cannot be
    //       found.", the usage on stderr, and the app never starts; it is not resolved against "/",
    //       though there it names a real package.
    // WHY-THIS-SHAPE: the shell removes its own working folder before exec, so getcwd fails in the
    //       app as it does after a file manager deleted the folder; the argument is a real package's
    //       absolute path without its leading "/".
    // CONTROL: app/main.cpp, parseCommandLine()'s refusal `refused = refused.value_or(Refused{argument, "a relative path, and the current folder cannot be found"});`
    //       made `files.append('/' + argument);` -> red, "S15.94: `tmp/.../nul.wpx` from a removed
    //       working folder exited -1 (finished 0); stderr []" (the app started, imported Nul, and was
    //       stopped at the bound); restored.
    const QString gone = root + QStringLiteral("/gone");
    const QString relative = (root + QStringLiteral("/nul.wpx")).mid(1);
    if (!QDir().mkpath(gone)) {
        harnessFailure("S15.94: the working folder to remove could not be made");
        return;
    }
    QProcess fromGone;
    startApp(fromGone, root, root, QStringLiteral("cd '%1' && rmdir '%1' && exec \"$0\" '%2'").arg(gone, relative), QProcess::SeparateChannels);
    const bool goneFinished = exitsWithin(fromGone, 10000);
    const QString goneErr = QString::fromLocal8Bit(fromGone.readAllStandardError());
    expect(goneFinished && fromGone.exitStatus() == QProcess::NormalExit && fromGone.exitCode() == 2 && goneErr.contains(QLatin1String("Usage: wallpaperx")) &&
               goneErr.contains(QStringLiteral("wallpaperx: %1 is a relative path, and the current folder cannot be found.").arg(relative)),
           QStringLiteral("S15.94: `%1` from a removed working folder exited %2 (finished %3); stderr [%4]")
               .arg(relative, QString::number(goneFinished ? fromGone.exitCode() : -1), QString::number(goneFinished), goneErr.trimmed().left(300)));
}

// S15.331: Qt's own options, before and after `--`.
void qtOptions(const QString& scratch) {
    // A home of its own, so its library is not the one handOver() counts.
    const QString root = scratch + QStringLiteral("/qt-options");
    const QString here = root + QStringLiteral("/work");
    const QString wallpapers = root + QStringLiteral("/xdg/XDG_DATA_HOME/wallpaperx/wallpapers");
    if (!QDir().mkpath(here) || !writeZip(here + QStringLiteral("/-reverse"), planEntries(titled("Reverse"))).isEmpty()) {
        harnessFailure("S15.331: the package named -reverse could not be written");
        return;
    }
    // GOAL: S15.331 -- after `--` an argument Qt would take as its own option is a file name: `--
    //       -reverse`, run where a package of that name is, imports it; `-- -platform foo.wpx` starts
    //       the app rather than handing Qt a platform named foo.wpx (which aborts). Before `--`, Qt
    //       still takes its options: `-platform offscreen --bogus` is refused naming --bogus, not -platform.
    // WHY-THIS-SHAPE: each run is the first instance, so what it does with its arguments is its own;
    //       the import is read from the library folder, and "still running after 3 s with the bus
    //       name held" is what an app that did not abort looks like.
    // CONTROL: app/main.cpp via control_batch.sh, 2026-10-05, each reverted: Qt given the whole argv
    //       (`int qtArgc = argc;`, the operands loop starting at argc) -> "FAIL: S15.331: `-- -reverse`
    //       left the library listing []" and "S15.331: `-- -platform foo.wpx` ... running 0"; Qt given
    //       nothing (`int qtArgc = 1;`, the operands loop from 1), via control_run.sh -> "FAIL: S15.331:
    //       `-platform offscreen --bogus` exited 2 (finished 1); refused with [wallpaperx: -platform is not
    //       an option wallpaperx takes.]". (`-reverse --help` was tried first and stayed green under that
    //       break: --help wins over any unknown option.)
    {
        QProcess app;
        startApp(app, root, here, QStringLiteral("exec \"$0\" -- -reverse"));
        QStringList titles;
        const bool imported = waitFor([&] {
            titles.clear();
            for (const Wallpaper& w : scanLibrary(wallpapers).value_or(QList<Wallpaper>())) {
                titles.append(w.displayTitle());
            }
            return titles.contains(QStringLiteral("Reverse")) || app.state() == QProcess::NotRunning;
        }, kStartMs);
        const QString said = QString::fromLocal8Bit(app.readAll());
        stop(app);
        expect(imported && titles.contains(QStringLiteral("Reverse")),
               QStringLiteral("S15.331: `-- -reverse` left the library listing [%1]; it said: %2").arg(titles.join(u", "), said.trimmed().left(300)));
    }
    {
        QProcess app;
        startApp(app, root, here, QStringLiteral("exec \"$0\" -- -platform foo.wpx"));
        const bool up = waitFor([&] { return nameOwned() || app.state() == QProcess::NotRunning; }, kStartMs) && nameOwned();
        static_cast<void>(waitFor([] { return false; }, 3000));
        const bool running = up && app.state() == QProcess::Running;
        const QString said = QString::fromLocal8Bit(app.readAll());
        stop(app);
        expect(running, QStringLiteral("S15.331: `-- -platform foo.wpx` took the bus name %1, running %2 after 3 s, exit status %3; it said: %4")
                            .arg(up).arg(running).arg(int(app.exitStatus())).arg(said.trimmed().left(300)));
    }
    {
        // Qt takes `-platform offscreen`, so the first argument the app itself refuses is --bogus. `--help`
        // would not do here: it wins over an unknown option, so it passes whether or not Qt took any.
        QProcess app;
        startApp(app, root, here, QStringLiteral("exec \"$0\" -platform offscreen --bogus"), QProcess::SeparateChannels);
        const bool finished = exitsWithin(app, 10000);
        const QString err = QString::fromLocal8Bit(app.readAllStandardError());
        // The refusal line alone: Qt's own locale notice can come first and fill any fixed-size excerpt.
        QString refusal;
        for (const QString& line : err.split(u'\n')) {
            if (line.contains(QLatin1String("is not an option"))) refusal = line;
        }
        expect(finished && app.exitStatus() == QProcess::NormalExit && app.exitCode() == 2 &&
                   refusal == QLatin1String("wallpaperx: --bogus is not an option wallpaperx takes."),
               QStringLiteral("S15.331: `-platform offscreen --bogus` exited %1 (finished %2); refused with [%3]")
                   .arg(finished ? app.exitCode() : -1).arg(finished).arg(refusal));
    }
}

// S15.83: a name owned by something that is not a WallpaperX.
void unansweredHandOver(const QString& root) {
    // GOAL: S15.83 -- a launch that finds the app's bus name owned but cannot hand its files over
    //       says so and exits 1, rather than opening a second window over the same library.
    // WHY-THIS-SHAPE: this test process owns the name with no object behind it, so the call the
    //       launch makes is refused at once instead of waiting out the 10 s bound.
    // CONTROL: app/main.cpp, the HandOverFailed case's `return 1;` made `break;` -> red, "S15.83: a
    //       launch whose hand-over was refused exited -1 (finished 0) ..."; restored.
    QDBusConnection owner = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("unanswering-owner"));
    const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> owned =
        owner.isConnected() ? owner.interface()->registerService(QString::fromLatin1(kApplicationBusName)) : QDBusReply<QDBusConnectionInterface::RegisterServiceReply>();
    if (!owned.isValid() || owned.value() != QDBusConnectionInterface::ServiceRegistered) {
        notVerified("S15.83 (a refused hand-over)", "this test could not own the app's bus name");
        QDBusConnection::disconnectFromBus(QStringLiteral("unanswering-owner"));
        return;
    }
    QProcess app;
    startApp(app, root, root, QStringLiteral("exec \"$0\" nothing.wpx"));
    const bool finished = exitsWithin(app, kHandOverMs);
    const QString said = QString::fromLocal8Bit(app.readAll());
    owner.interface()->unregisterService(QString::fromLatin1(kApplicationBusName));
    QDBusConnection::disconnectFromBus(QStringLiteral("unanswering-owner"));
    expect(finished && app.exitCode() == 1 && said.contains(QLatin1String("WallpaperX is already running but did not answer")),
           QStringLiteral("S15.83: a launch whose hand-over was refused exited %1 (finished %2) and said: %3")
               .arg(finished ? app.exitCode() : -1).arg(finished).arg(said.trimmed().left(400)));
}

// The org.freedesktop.Application calls made on the bus while it runs, as dbus-monitor prints them.
class Monitor {
public:
    Monitor() {
        m_process.setProcessChannelMode(QProcess::MergedChannels);
        m_process.start(QStringLiteral("dbus-monitor"), {QStringLiteral("--session"), QStringLiteral("interface='org.freedesktop.Application'")});
        m_ok = m_process.waitForStarted(5000) && m_process.waitForReadyRead(5000);
    }
    ~Monitor() { stop(m_process); }
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    bool ok() const { return m_ok; }
    // Everything printed until `marker` appears, or `ms` pass, and then whatever follows within a
    // short pause, so a second call printed right behind the first is counted too. How soon
    // dbus-monitor prints is its own scheduling, so a fixed pause alone could miss a call a
    // correct app made; the bound is one no correct hand-over comes near.
    QString takeUntil(QLatin1StringView marker, int ms) {
        const QDeadlineTimer deadline(ms);
        m_seen += QString::fromLocal8Bit(m_process.readAll());
        while (!m_seen.contains(marker) && !deadline.hasExpired() && m_process.state() == QProcess::Running) {
            if (m_process.waitForReadyRead(int(std::max<qint64>(1, deadline.remainingTime())))) {
                m_seen += QString::fromLocal8Bit(m_process.readAll());
            }
        }
        while (m_process.waitForReadyRead(200)) {
            m_seen += QString::fromLocal8Bit(m_process.readAll());
        }
        return std::exchange(m_seen, {});
    }

private:
    QProcess m_process;
    QString m_seen;
    bool m_ok = false;
};

// S15.83, S15.84
void handOver(const QString& root) {
    const QString first = root + QStringLiteral("/first");
    const QString second = root + QStringLiteral("/second");
    const QString wallpapers = root + QStringLiteral("/xdg/XDG_DATA_HOME/wallpaperx/wallpapers");
    const auto slip = zipBytes(root + QStringLiteral("/slip.scratch"), planEntries(titled("Slip")) + Entries{{QStringLiteral("../evil.txt"), "evil"}});
    const auto hosted = zipBytes(root + QStringLiteral("/hosted.scratch"), planEntries(titled("Hosted")));
    if (!QDir().mkpath(first) || !QDir().mkpath(second) || !writeZip(second + QStringLiteral("/ocean.wpx"), planEntries(QByteArray(kPlanManifest))).isEmpty() ||
        !writeRawName(second, "caf\xe9.wpx", zipBytes(root + QStringLiteral("/forest.scratch"), planEntries(titled("Forest"))).value_or(QByteArray())) ||
        !slip || !writeBytes(second + QStringLiteral("/slip.wpx"), *slip) || !hosted || !writeBytes(second + QStringLiteral("/hosted.wpx"), *hosted)) {
        harnessFailure("S15.83: the packages to hand over could not be built");
        return;
    }
    const RawNamesRemover removeRawNames(second, {"caf\xe9.wpx"});
    QProcess app;
    startApp(app, root, first, QStringLiteral("exec \"$0\""));
    const bool started = waitFor([&] { return nameOwned() || app.state() == QProcess::NotRunning; }, kStartMs) && nameOwned();
    if (!started) {
        expect(false, QStringLiteral("S15.83: the first instance never took %1; it said: %2")
                          .arg(QLatin1String(kApplicationBusName), QString::fromLocal8Bit(app.readAll()).trimmed().left(600)));
        stop(app);
        return;
    }
    Monitor monitor;
    if (!monitor.ok()) {
        notVerified("S15.83, S15.84 (the calls on the bus)", "dbus-monitor could not watch the private bus");
    }

    // GOAL: S15.83, S15.84 -- a second launch given .wpx files, by paths relative to its own
    //       working folder, one whose name is not UTF-8, and one as a file://localhost URL, hands
    //       them to the running instance with one Open call and exits 0 at once; the running
    //       instance imports each (a package that would write outside its folder is refused), and
    //       the second opens no window of its own.
    // WHY-THIS-SHAPE: the two instances run in different working folders, so a relative path
    //       resolved by the wrong one names nothing; the non-UTF-8 name is passed as raw bytes
    //       through sh, since QProcess arguments are text. What arrived is judged by what the first
    //       instance imported (read from its library folder), and the call itself by dbus-monitor.
    //       Imports run one at a time in the order given, so slip is given first: it has settled
    //       before three titles can be listed, and a wrongly accepted Slip is always among them.
    // CONTROL: app/main.cpp, the HandedOver case's `return 0;` made `break;` -> red, "S15.83: the
    //       second launch exited -1 (finished 0, 20102 ms) ..."; parseCommandLine()'s relative branch
    //       `files.append(*here + '/' + argument);` made `files.append(argument);` -> red, "S15.83: after
    //       the hand-over the library lists [Hosted]" (every other name relative, so none found; the
    //       Open call then carries "file://slip.wpx"); app/single_instance.cpp, fileUri()'s
    //       percent-encoding made `QString::fromLocal8Bit(path)` -> red, "S15.83: after the hand-over
    //       the library lists [Hosted, Ocean Waves]" and "S15.84: the Open call carried [...]"; restored.
    QProcess launch;
    QElapsedTimer clock;
    clock.start();
    startApp(launch, root, second,
             QStringLiteral("exec \"$0\" slip.wpx ocean.wpx \"$(printf 'caf\\351.wpx')\" \"file://localhost$(pwd -P)/hosted.wpx\""));
    const bool finished = exitsWithin(launch, kHandOverMs);
    const qint64 took = clock.elapsed();
    const QString launchSaid = QString::fromLocal8Bit(launch.readAll());
    expect(finished && launch.exitCode() == 0,
           QStringLiteral("S15.83: the second launch exited %1 (finished %2, %3 ms) and said: %4")
               .arg(finished ? launch.exitCode() : -1).arg(finished).arg(took).arg(launchSaid.trimmed().left(400)));
    QStringList titles;
    QStringList folders;
    const bool imported = waitFor([&] {
        const auto scan = scanLibrary(wallpapers);
        titles.clear();
        folders.clear();
        for (const Wallpaper& w : scan.value_or(QList<Wallpaper>())) {
            titles.append(w.displayTitle());
            folders.append(w.fileName);
        }
        return titles.size() >= 3;
    }, kHandOverMs);
    expect(imported && titles == QStringList{QStringLiteral("Forest"), QStringLiteral("Hosted"), QStringLiteral("Ocean Waves")},
           QStringLiteral("S15.83: after the hand-over the library lists [%1]").arg(titles.join(u", ")));

    // GOAL: S15.83 -- the package whose entry climbs out of its folder writes nothing anywhere:
    //       wallpapers/ holds exactly the imported folders, and nothing lands beside it.
    // WHY-THIS-SHAPE: "../evil.txt" resolves against the unfinished folder inside wallpapers/, so
    //       it lands in wallpapers/ itself, which the library's scan never lists (a plain file);
    //       listing every entry there, hidden ones included, catches it under any name.
    // CONTROL: app/library/importer.cpp, planEntries' `if (!isPlainRelativePath(entry.name)) {`
    //       made `if (false) {` -> red, "S15.83: wallpapers/ holds [..., evil.txt, ...] beside the
    //       imported [...]", with "the library lists [Forest, Hosted, Ocean Waves, Slip]"; restored.
    QStringList present = QDir(wallpapers).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    present.sort();
    folders.sort();
    expect(imported && present == folders && !QFileInfo::exists(root + QStringLiteral("/xdg/XDG_DATA_HOME/wallpaperx/evil.txt")),
           QStringLiteral("S15.83: wallpapers/ holds [%1] beside the imported [%2]").arg(present.join(u", "), folders.join(u", ")));
    if (monitor.ok()) {
        const QString calls = monitor.takeUntil(QLatin1StringView("member=Open"), kHandOverMs);
        const QString forest = QStringLiteral("file://") + QString::fromLatin1(QFile::encodeName(second).toPercentEncoding("/")) + QStringLiteral("/caf%E9.wpx");
        expect(calls.count(QLatin1String("member=Open")) == 1 && calls.contains(QLatin1String("string \"") + forest + u'"'),
               QStringLiteral("S15.84: the Open call carried [%1], not one Open with \"%2\"").arg(calls.simplified().left(700), forest));
    }

    // GOAL: S15.83 -- after `--`, `--help` is a file name like any other: the launch hands it over
    //       in an Open call and exits 0, printing no usage.
    // CONTROL: app/main.cpp, parseCommandLine()'s `if (options && argument == "--") {` made
    //       `if (false) {` -> red, "S15.83: `-- --help` exited 0 (finished 1) and said [Usage:
    //       wallpaperx ..." (the help won), and S15.95's `-- file:lit.wpx`, refused as an unknown
    //       option; restored.
    QProcess dashed;
    startApp(dashed, root, second, QStringLiteral("exec \"$0\" -- --help"));
    const bool dashedFinished = exitsWithin(dashed, kHandOverMs);
    const QString dashedSaid = QString::fromLocal8Bit(dashed.readAll());
    const QString dashedCalls = monitor.ok() ? monitor.takeUntil(QLatin1StringView("/--help\""), kHandOverMs) : QString();
    expect(dashedFinished && dashed.exitCode() == 0 && !dashedSaid.contains(QLatin1String("Usage: wallpaperx")) &&
               (!monitor.ok() || (dashedCalls.count(QLatin1String("member=Open")) == 1 && dashedCalls.contains(QString::fromLatin1(QFile::encodeName(second).toPercentEncoding("/")) + QStringLiteral("/--help\"")))),
           QStringLiteral("S15.83: `-- --help` exited %1 (finished %2) and said [%3]; the calls [%4]")
               .arg(dashedFinished ? dashed.exitCode() : -1).arg(dashedFinished).arg(dashedSaid.trimmed().left(300), dashedCalls.simplified().left(400)));

    // GOAL: S15.83 -- a second launch with no files asks the running instance to come forward
    //       (one Activate call) and exits 0.
    // CONTROL: app/main.cpp, the HandedOver case's `return 0;` made `break;` -> red, "S15.83: a
    //       launch with no files exited -1 (finished 0) ..."; app/single_instance.cpp's
    //       `uris.isEmpty() ? QStringLiteral("Activate") : QStringLiteral("Open")` made to send Open
    //       always -> red, "S15.83: a launch with no files made the calls [... member=Open ...]"; restored.
    QProcess bare;
    startApp(bare, root, second, QStringLiteral("exec \"$0\""));
    const bool bareFinished = exitsWithin(bare, kHandOverMs);
    expect(bareFinished && bare.exitCode() == 0 && app.state() == QProcess::Running,
           QStringLiteral("S15.83: a launch with no files exited %1 (finished %2), the first instance %3; it said: %4")
               .arg(bareFinished ? bare.exitCode() : -1).arg(bareFinished)
               .arg(app.state() == QProcess::Running ? QStringLiteral("still running") : QStringLiteral("gone"), QString::fromLocal8Bit(bare.readAll()).trimmed().left(300)));
    if (monitor.ok()) {
        const QString calls = monitor.takeUntil(QLatin1StringView("member=Activate"), kHandOverMs);
        expect(calls.count(QLatin1String("member=Activate")) == 1 && !calls.contains(QLatin1String("member=Open")),
               QStringLiteral("S15.83: a launch with no files made the calls [%1]").arg(calls.simplified().left(500)));
    }

    // GOAL: S15.93 -- an Open call whose URI decodes to a NUL byte (".../nul.wpx%00.jpg", nul.wpx a
    //       real package titled "Opened with a NUL") imports nothing: that title is never listed.
    // GOAL: S15.94 -- an absolute argument given from a working folder that no longer exists is
    //       handed over and imported as ever.
    // GOAL: S15.95 -- after `--`, an argument starting "file:" is a file name: `-- file:lit.wpx`, run
    //       where a file of that name is, hands it over and it is imported.
    // WHY-THIS-SHAPE: the Open call is made by this test, and answered before the next launch, so
    //       the running instance takes the three in that order; imports run one at a time in order,
    //       so once Absolute is listed that import would have finished too. Its absence is graded
    //       only after that barrier, else it proves nothing.
    // CONTROL: app/library/wpx_package.cpp, localPathOfFileUrl's `|| path.contains('\0')` removed ->
    //       red, "S15.93: after an Open call for .../nul.wpx%00.jpg the library lists [Absolute, ...,
    //       Nul, ...]"; app/main.cpp, parseCommandLine()'s `} else if (argument.startsWith('/')) {` made
    //       `} else if (here && argument.startsWith('/')) {` -> red, "S15.94: an absolute path from a
    //       removed working folder exited 2 ..."; the `file:` branch's `options && argument.startsWith("file:")`
    //       made `argument.startsWith("file:")` -> red, "S15.95: `-- file:lit.wpx` exited 2 ..."; restored.
    const QString gone = root + QStringLiteral("/gone-later");
    // Titled apart from options()'s nul.wpx, so only the Open call can list it.
    if (!writeZip(second + QStringLiteral("/nul.wpx"), planEntries(titled("Opened with a NUL"))).isEmpty() ||
        !writeZip(second + QStringLiteral("/absolute.wpx"), planEntries(titled("Absolute"))).isEmpty() ||
        !writeZip(second + QStringLiteral("/file:lit.wpx"), planEntries(titled("Literal"))).isEmpty() || !QDir().mkpath(gone)) {
        harnessFailure("S15.93: the packages for the last hand-overs could not be built");
        stop(app);
        return;
    }
    QDBusMessage open = QDBusMessage::createMethodCall(QString::fromLatin1(kApplicationBusName), QString::fromLatin1(kApplicationObjectPath),
                                                       QStringLiteral("org.freedesktop.Application"), QStringLiteral("Open"));
    open << QStringList{QStringLiteral("file://") + QString::fromLatin1(QFile::encodeName(second).toPercentEncoding("/")) + QStringLiteral("/nul.wpx%00.jpg")}
         << QVariantMap();
    const QDBusMessage opened = QDBusConnection::sessionBus().call(open, QDBus::Block, kHandOverMs);
    QProcess absolute;
    startApp(absolute, root, root, QStringLiteral("cd '%1' && rmdir '%1' && exec \"$0\" '%2/absolute.wpx'").arg(gone, second));
    const bool absoluteFinished = exitsWithin(absolute, kHandOverMs);
    const QString absoluteSaid = QString::fromLocal8Bit(absolute.readAll());
    QProcess literal;
    startApp(literal, root, second, QStringLiteral("exec \"$0\" -- file:lit.wpx"));
    const bool literalFinished = exitsWithin(literal, kHandOverMs);
    const QString literalSaid = QString::fromLocal8Bit(literal.readAll());
    QStringList listed;
    const auto lists = [&](const char* title) { return listed.contains(QLatin1String(title)); };
    const bool barrier = waitFor([&] {
        listed.clear();
        for (const Wallpaper& w : scanLibrary(wallpapers).value_or(QList<Wallpaper>())) {
            listed.append(w.displayTitle());
        }
        return lists("Absolute") && lists("Literal");
    }, kHandOverMs) || lists("Absolute");
    if (opened.type() == QDBusMessage::ErrorMessage) {
        harnessFailure("S15.93: the Open call could not be made: " + opened.errorMessage().toStdString());
    } else if (!barrier) {
        notVerified("S15.93", "Absolute, imported after the Open call, was never listed, so the absence of Nul proves nothing");
    } else {
        expect(!lists("Opened with a NUL"), QStringLiteral("S15.93: after an Open call for %1 the library lists [%2]").arg(QStringLiteral(".../nul.wpx%00.jpg"), listed.join(u", ")));
    }
    expect(absoluteFinished && absolute.exitCode() == 0 && lists("Absolute"),
           QStringLiteral("S15.94: an absolute path from a removed working folder exited %1 (finished %2), the library lists [%3]; it said: %4")
               .arg(QString::number(absoluteFinished ? absolute.exitCode() : -1), QString::number(absoluteFinished), listed.join(u", "), absoluteSaid.trimmed().left(300)));
    expect(literalFinished && literal.exitCode() == 0 && lists("Literal"),
           QStringLiteral("S15.95: `-- file:lit.wpx` exited %1 (finished %2), the library lists [%3]; it said: %4")
               .arg(QString::number(literalFinished ? literal.exitCode() : -1), QString::number(literalFinished), listed.join(u", "), literalSaid.trimmed().left(300)));
    stop(app);
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "single_instance";
    if (const auto refused = refuseUnlessUnderDbusRunSession()) {
        return *refused;
    }
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified(kGoals, "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    QCoreApplication app(argc, argv);
    if (const auto refused = refuseUnlessPrivateBus()) {
        return *refused;
    }
    // Spelled as getcwd() spells it, symlinks resolved: the app builds the paths it hands over from
    // its working folder, and TMPDIR may lie behind a link.
    const QString root = QDir(scratch.path()).canonicalPath();
    if (root.isEmpty()) {
        harnessFailure("the scratch folder's path could not be resolved");
        static_cast<void>(wallpaperx::testing::reportHarnessFaults("in this test file, not in the app"));
        return 1;
    }
    runPhase("uriRoundTrip", uriRoundTrip);
    runPhase("options", [&] { options(root); });
    runPhase("qtOptions", [&] { qtOptions(root); });
    runPhase("unansweredHandOver", [&] { unansweredHandOver(root); });
    runPhase("handOver", [&] { handOver(root); });

    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file or its fixtures, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
