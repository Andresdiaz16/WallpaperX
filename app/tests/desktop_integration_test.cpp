// SPDX-License-Identifier: GPL-2.0-or-later

// app/com.wallpaperx.app.xml, app/com.wallpaperx.app.desktop and their install rules in
// app/CMakeLists.txt: installed into a scratch prefix, and the prefix's MIME and desktop databases
// built there, never on the system. Goal S15.85 in GOALS.md.
//
//   wallpaperx-desktop-integration-test CMAKE APP_BUILD_DIR

#include "wpx_fixtures.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QMimeDatabase>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

namespace {

constexpr const char* kGoal = "S15.85";

// How a run of a tool ended. `ran` is false when it could not start, did not finish within the
// bound, or died by a signal: a fault in this test's environment, never a verdict on the app. A
// tool that ran and exited non-zero has given its verdict.
struct Ran {
    bool ran = false;
    int exitCode = -1;
    QString said;
    bool ok() const { return ran && exitCode == 0; }
};

// Every run here is bounded well inside the entry's 90 s TIMEOUT: six runs, each normally under a
// second, so even two hung ones leave the verdict time to print.
constexpr int kRunMs = 30000;

Ran run(const QString& program, const QStringList& args, const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment()) {
    QProcess process;
    process.setProcessEnvironment(env);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, args);
    const bool finished = process.waitForStarted(10000) && process.waitForFinished(kRunMs);
    if (!finished && process.state() != QProcess::NotRunning) {
        process.kill();
        static_cast<void>(process.waitForFinished(3000));
    }
    Ran r;
    r.said = QString::fromLocal8Bit(process.readAll());
    r.ran = finished && process.exitStatus() == QProcess::NormalExit;
    r.exitCode = r.ran ? process.exitCode() : -1;
    if (!r.ran) {
        r.said = QStringLiteral("%1 did not run to its end (%2): %3").arg(program, process.errorString(), r.said);
    }
    return r;
}

// The child: asked by queryMime() with XDG_DATA_DIRS and XDG_DATA_HOME set, it prints what
// QMimeDatabase makes of each name, as "name type inherits-zip".
int mimeQuery(const QStringList& names) {
    const QMimeDatabase db;
    for (const QString& name : names) {
        const QMimeType type = db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
        std::printf("%s %s %d %s\n", qPrintable(name), qPrintable(type.name()), type.inherits(QStringLiteral("application/zip")),
                    qPrintable(type.comment().replace(u' ', u'_')));
    }
    return 0;
}

// What the child says of a.wpx and B.WPX; nothing, with a harness fault reported, when it did not
// run to exit 0 or did not answer for both. The child is this test's own code, so its failure is
// never graded as the app's.
std::optional<QMap<QString, QStringList>> queryMime(const QString& dataDirs, const QString& dataHome) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_DATA_DIRS"), dataDirs);
    env.insert(QStringLiteral("XDG_DATA_HOME"), dataHome);
    QMap<QString, QStringList> answers;
    const Ran r = run(QCoreApplication::applicationFilePath(), {QStringLiteral("--mime-query"), QStringLiteral("a.wpx"), QStringLiteral("B.WPX")}, env);
    for (const QString& line : (r.ok() ? r.said : QString()).split(u'\n', Qt::SkipEmptyParts)) {
        const QStringList fields = line.split(u' ');
        if (fields.size() == 4) {
            answers.insert(fields[0], fields.mid(1));
        }
    }
    if (!r.ok() || !answers.contains(QStringLiteral("a.wpx")) || !answers.contains(QStringLiteral("B.WPX"))) {
        harnessFailure(std::string(kGoal) + ": the --mime-query child (XDG_DATA_DIRS " + dataDirs.toStdString() + ") did not answer for both names; exit " +
                       std::to_string(r.exitCode) + ", it said: " + r.said.trimmed().toStdString());
        return std::nullopt;
    }
    return answers;
}

void installed(const QString& cmake, const QString& buildDir, const QString& root) {
    const QString prefix = root + QStringLiteral("/prefix");
    for (const char* tool : {"update-mime-database", "update-desktop-database", "desktop-file-validate"}) {
        if (QStandardPaths::findExecutable(QString::fromLatin1(tool)).isEmpty()) {
            notVerified(kGoal, std::string(tool) + " is not installed");
            return;
        }
    }
    // cmake --install puts everything under $DESTDIR/<prefix> when DESTDIR is set, which is not
    // where this test looks.
    QProcessEnvironment installEnv = QProcessEnvironment::systemEnvironment();
    installEnv.remove(QStringLiteral("DESTDIR"));
    if (const Ran installed = run(cmake, {QStringLiteral("--install"), buildDir, QStringLiteral("--prefix"), prefix}, installEnv); !installed.ok()) {
        harnessFailure(std::string(kGoal) + ": cmake --install into the scratch prefix failed: " + installed.said.toStdString());
        return;
    }
    const QString mime = prefix + QStringLiteral("/share/mime");
    const QString applications = prefix + QStringLiteral("/share/applications");
    const QString desktop = applications + QStringLiteral("/com.wallpaperx.app.desktop");
    // Each database tool is given the scratch folder explicitly, so it writes nowhere else.
    // GOAL: a tool that could not start, hung or crashed is reported as a fault of this test's
    //       environment; only a tool that ran and refused (exit not 0) grades the installed files.
    // WHY-THIS-SHAPE: folding "did not run" into "said no" is the obvious shape, and it files a
    //       missing or broken tool as a defect in the app's MIME or desktop file.
    // CONTROL: by hand, test-side: update-mime-database run as "update-mime-database-absent" ->
    //       "FAIL (harness): S15.85: update-mime-database did not run: ... execve: No such file or
    //       directory" and no FAIL line; the --mime-query child run as "/nonexistent" -> "FAIL
    //       (harness): S15.85: the --mime-query child (XDG_DATA_DIRS /usr/share) did not answer for
    //       both names" and no FAIL line; restored.
    const Ran mimeRun = run(QStringLiteral("update-mime-database"), {mime});
    const Ran desktopRun = run(QStringLiteral("update-desktop-database"), {applications});
    const Ran validateRun = run(QStringLiteral("desktop-file-validate"), {desktop});
    for (const auto& [tool, r] : {std::pair{"update-mime-database", &mimeRun}, std::pair{"update-desktop-database", &desktopRun},
                                  std::pair{"desktop-file-validate", &validateRun}}) {
        if (!r->ran) {
            harnessFailure(std::string(kGoal) + ": " + tool + " did not run: " + r->said.trimmed().toStdString());
            return;
        }
    }
    const bool mimeBuilt = mimeRun.ok();
    const QString mimeSaid = mimeRun.said;
    const bool desktopBuilt = desktopRun.ok();
    const QString desktopSaid = desktopRun.said;
    const bool valid = validateRun.ok();
    const QString validateSaid = validateRun.said;
    // Read by hand: QSettings would take the ';' in MimeType and the '%' in Exec as its own syntax.
    QString exec;
    QStringList types;
    for (const QByteArray& line : readBytes(desktop).value_or(QByteArray()).split('\n')) {
        if (line.startsWith("Exec=")) {
            exec = QString::fromUtf8(line.mid(5));
        } else if (line.startsWith("MimeType=")) {
            types = QString::fromUtf8(line.mid(9)).split(u';', Qt::SkipEmptyParts);
        }
    }
    const auto cache = readBytes(applications + QStringLiteral("/mimeinfo.cache"));

    // GOAL: S15.85 -- the installed desktop entry is valid, opens files (Exec takes %F or %U) and
    //       claims application/x-wallpaperx, and the desktop database built from it maps that type
    //       to com.wallpaperx.app.desktop.
    // WHY-THIS-SHAPE: the app is installed with its own install rules into a scratch prefix and
    //       the databases are built there, which is what Step 16's first-run setup is to do in the
    //       user's data folder; nothing is registered on this system.
    // CONTROL: app/com.wallpaperx.app.desktop, `Exec=wallpaperx %F` made `Exec=wallpaperx` -> red,
    //       "S15.85: the installed entry's Exec is "wallpaperx" ..."; `MimeType=application/x-wallpaperx;`
    //       deleted -> red, "... claims [] ... the desktop database holds no application/x-wallpaperx";
    //       `Type=Application` made `Type=Bogus` -> red, a FAIL line (not a harness fault) quoting
    //       desktop-file-validate's "value "Bogus" for key "Type" ... is not a registered type
    //       value"; restored.
    expect(desktopBuilt && valid && (exec.contains(QLatin1String("%F")) || exec.contains(QLatin1String("%U"))) &&
               types.contains(QStringLiteral("application/x-wallpaperx")) && cache &&
               cache->contains("application/x-wallpaperx=com.wallpaperx.app.desktop;"),
           QStringLiteral("S15.85: the installed entry's Exec is \"%1\" and it claims [%2]; desktop-file-validate said: %3; "
                          "update-desktop-database said: %4; the desktop database %5")
               .arg(exec, types.join(u','), validateSaid.trimmed(), desktopSaid.trimmed(),
                    cache && cache->contains("application/x-wallpaperx=") ? QStringLiteral("maps the type")
                                                                           : QStringLiteral("holds no application/x-wallpaperx")));

    // GOAL: S15.85 -- the installed MIME type, once its database is built, makes a name ending
    //       .wpx, in any case, application/x-wallpaperx ("WallpaperX wallpaper"), a kind of
    //       application/zip; the same names are not that type without the prefix.
    // WHY-THIS-SHAPE: asked of QMimeDatabase in a child process whose XDG_DATA_DIRS lists the
    //       scratch prefix before /usr/share (the MIME database is read once per process); the same
    //       question with /usr/share alone shows the answer comes from the prefix, and a system that
    //       already knows the type makes the check not verified rather than passed.
    // CONTROL: app/com.wallpaperx.app.xml, `<glob pattern="*.wpx"/>` made `*.wpz` -> red, "S15.85:
    //       with the prefix a.wpx is [application/octet-stream ...]"; `<sub-class-of
    //       type="application/zip"/>` deleted -> red, "... a.wpx is [application/x-wallpaperx 0 ...]";
    //       app/CMakeLists.txt's `install(FILES com.wallpaperx.app.xml ...)` deleted -> red, the first
    //       line; restored.
    const auto system = queryMime(QStringLiteral("/usr/share"), root + QStringLiteral("/empty-home"));
    if (!system) {
        return;
    }
    if (system->value(QStringLiteral("a.wpx")).value(0) == u"application/x-wallpaperx") {
        notVerified(kGoal, "this system already knows application/x-wallpaperx, so the prefix's own answer cannot be told apart");
        return;
    }
    const auto withPrefix = queryMime(prefix + QStringLiteral("/share:/usr/share"), root + QStringLiteral("/empty-home"));
    if (!withPrefix) {
        return;
    }
    for (const QString& name : {QStringLiteral("a.wpx"), QStringLiteral("B.WPX")}) {
        const QStringList got = withPrefix->value(name);
        expect(mimeBuilt && got == QStringList{QStringLiteral("application/x-wallpaperx"), QStringLiteral("1"), QStringLiteral("WallpaperX_wallpaper")},
               QStringLiteral("S15.85: with the prefix %1 is [%2] (update-mime-database said: %3); with /usr/share alone [%4]")
                   .arg(name, got.join(u' '), mimeSaid.trimmed(), system->value(name).join(u' ')));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1 && std::strcmp(argv[1], "--mime-query") == 0) {
        QCoreApplication app(argc, argv);
        return mimeQuery(QCoreApplication::arguments().mid(2));
    }
    wallpaperx::testing::g_testName = "desktop_integration";
    if (argc != 3) {
        harnessFailure("usage: wallpaperx-desktop-integration-test CMAKE APP_BUILD_DIR");
        static_cast<void>(wallpaperx::testing::reportHarnessFaults("in how this test was run"));
        return 1;
    }
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified(kGoal, "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    QCoreApplication app(argc, argv);
    runPhase("installed", [&] { installed(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]), scratch.path()); });

    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file or the tools it runs, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
