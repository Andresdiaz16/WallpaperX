// SPDX-License-Identifier: GPL-2.0-or-later

// ScreenWindows.qml against the real thing: Plasma's own task manager model, fed by a nested
// headless KWin with two outputs that isolated_run.sh starts for this entry alone, and real windows
// opened, maximized, made fullscreen and minimized by a client process. What it grades is the whole
// path a desktop takes -- the file's filters, Plasma's model applying them to KWin's windows, and
// the item reporting the result over the bus to a stub daemon (tests/report_stub.h).
//
// The scene is this process: WallpaperSurface.qml in a layer-shell background window, as
// plasmashell's desktop is, so the scene's own window is not a task. It lands on the first output;
// a layer surface could not be put on the second from QML or through QWindow::setScreen here, so
// the second output is where the client puts the window that must NOT count. The client is this
// binary again (`client`), driven over its stdin, so its windows belong to another process as a
// user's would, and it says which output each of its windows is on, so a window KWin placed
// somewhere else than the step needs is a skip rather than a verdict.
//
// Every expectation is a state the reports must settle on, waited for with a bound.

#include "tests/pump.h"
#include "tests/report_stub.h"
#include "tests/test_report.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QGuiApplication>
#include <QPainter>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QRasterWindow>
#include <QScreen>
#include <QSocketNotifier>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QWindow>

#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;
using wallpaperx::testing::pumpFor;
using wallpaperx::testing::pumpUntil;
using wallpaperx::testing::ReportStub;

// The focus report: SetOutputFocusState(name, "window" | "desktop" | "none", maximized).
const QString kFocus = QStringLiteral("SetOutputFocusState");
const QString kOutput = QStringLiteral("WPX-KWIN");
constexpr int kSettleLimitMs = 8000;
// Long enough for KWin to map and place a new window, so the client's answer names its output.
constexpr int kPlaceMs = 1500;

// ---- the client ------------------------------------------------------------------------------ //

class Painted : public QRasterWindow {
public:
    explicit Painted(QColor colour) : m_colour(colour) {}

protected:
    void paintEvent(QPaintEvent*) override { QPainter(this).fillRect(QRect(QPoint(), size()), m_colour); }

private:
    QColor m_colour;
};

// One command per line on stdin, each answered "done <command>" once asked for:
//   open                  a window, which KWin places on the first output
//   max, full, normal, min  that window
//   full-second           a second window, fullscreen on the second output
//   where                 answered "at <first window's output> <second window's output>"
int runClient(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const QList<QScreen*> screens = QGuiApplication::screens();
    Painted first(Qt::darkBlue);
    Painted second(Qt::darkRed);
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QByteArray pending;
    const auto screenOf = [](const QWindow& w) {
        return w.isVisible() && w.screen() != nullptr ? w.screen()->name().toUtf8() : QByteArray("-");
    };
    QObject::connect(&input, &QSocketNotifier::activated, [&] {
        char buffer[256];
        const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (n <= 0) {
            app.quit();
            return;
        }
        pending.append(buffer, n);
        for (qsizetype end; (end = pending.indexOf('\n')) >= 0;) {
            const QByteArray command = pending.left(end).trimmed();
            pending.remove(0, end + 1);
            if (command == "open") {
                first.resize(320, 200);
                first.show();
            } else if (command == "max") {
                first.showMaximized();
            } else if (command == "full") {
                first.showFullScreen();
            } else if (command == "normal") {
                first.showNormal();
            } else if (command == "min") {
                first.showMinimized();
            } else if (command == "full-second") {
                // Shown first: Qt names the output in the fullscreen request only for a window that
                // already exists, and KWin places a new one on the first output.
                second.resize(320, 200);
                second.show();
                second.setScreen(screens.value(1));
                second.showFullScreen();
            } else if (command == "where") {
                std::printf("at %s %s\n", screenOf(first).constData(), screenOf(second).constData());
            } else if (command == "quit") {
                app.quit();
            }
            std::printf("done %s\n", command.constData());
            std::fflush(stdout);
        }
    });
    return app.exec();
}

// ---- the scene ------------------------------------------------------------------------------- //

// The focus as the report names it. The wallpaper here is a layer-shell background that takes no
// keyboard focus, so KWin never activates it: every state without a focused window is "none".
struct Facts {
    QString focus = QStringLiteral("none");
    bool maximized = false;
    bool operator==(const Facts&) const = default;
};
Facts focusedWindow(bool maximized) { return {QStringLiteral("window"), maximized}; }

std::string text(const Facts& f) { return "(" + f.focus.toStdString() + "," + (f.maximized ? "T" : "F") + ")"; }

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && QByteArray(argv[1]) == "client") return runClient(argc, argv);
    wallpaperx::testing::g_testName = "screen_windows_kwin_test";
    const char* kGoals = "the window reports against Plasma's real task manager model on a nested KWin";

    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        notVerified(kGoals, "no nested compositor was started for this entry (isolated_run.sh says why above)");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    qputenv("QT_QPA_PLATFORM", "wayland");
    QGuiApplication app(argc, argv);
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.size() < 2) {
        notVerified(kGoals, "the nested compositor offered " + std::to_string(screens.size()) + " output(s), not two");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    QDBusConnection bus = wallpaperx::testing::stubConnection();
    ReportStub stub;
    if (!wallpaperx::testing::claimDaemonName(bus, stub)) {
        notVerified(kGoals, "no session bus this run could own io.wallpaperx.Daemon on");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));
    const QString surfaceUrl =
        QUrl::fromLocalFile(QStringLiteral(WALLPAPERX_BACKEND_SOURCE_DIR "/kde/kpackage/contents/ui/WallpaperSurface.qml"))
            .toString();
    QQmlComponent component(&engine);
    component.setData(QStringLiteral(R"(
import QtQuick
import QtQuick.Window
import org.kde.layershell as LayerShell
Window {
    width: 320; height: 200; color: "black"; visible: true
    LayerShell.Window.layer: LayerShell.Window.LayerBackground
    LayerShell.Window.anchors: LayerShell.Window.AnchorTop | LayerShell.Window.AnchorBottom | LayerShell.Window.AnchorLeft | LayerShell.Window.AnchorRight
    LayerShell.Window.keyboardInteractivity: LayerShell.Window.KeyboardInteractivityNone
    property alias surface: loader.item
    Loader { id: loader; anchors.fill: parent; source: "%1"
             onLoaded: { item.monitorId = 0; item.output = "%2"; } }
}
)")
                          .arg(surfaceUrl, kOutput)
                          .toUtf8(),
                      QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + QStringLiteral("/kwin_scene.qml")));
    std::unique_ptr<QObject> wallpaper(component.create());
    QObject* surface = wallpaper ? wallpaper->property("surface").value<QObject*>() : nullptr;
    if (surface == nullptr) {
        for (const QQmlError& error : component.errors()) std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
        notVerified(kGoals, "a layer-shell wallpaper window with WallpaperSurface.qml could not be made "
                            "(org.kde.layershell or the plugin module missing)");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    const auto last = [&]() -> std::optional<Facts> {
        const std::vector<ReportStub::Call> calls = stub.calls(kFocus, kOutput);
        if (calls.empty()) return std::nullopt;
        return Facts{calls.back().args.value(1).toString(), calls.back().args.value(2).toBool()};
    };
    const auto settlesOn = [&](const Facts& want) { return pumpUntil([&] { return last() == want; }, kSettleLimitMs); };
    const auto history = [&](std::size_t mark) { return ReportStub::describe(stub.calls(kFocus, kOutput, mark)); };

    // The precondition every step is measured from: the wallpaper reports no window, and its watcher
    // is on a real output whose name the client's answers can be compared with.
    pumpFor(500);
    auto* window = qobject_cast<QWindow*>(wallpaper.get());
    const QString wallpaperScreen = window != nullptr && window->screen() != nullptr ? window->screen()->name() : QString();
    QObject* windowsLoader = surface->findChild<QObject*>(QStringLiteral("wallpaperxWindows"));
    QObject* watcher = windowsLoader != nullptr ? windowsLoader->property("item").value<QObject*>() : nullptr;
    const bool noWindow = pumpUntil(
        [&] {
            const std::optional<Facts> now = last();
            return now && now->focus != QLatin1String("window") && !now->maximized;
        },
        kSettleLimitMs);
    if (watcher == nullptr || wallpaperScreen.isEmpty() || !noWindow) {
        notVerified(kGoals, "the wallpaper never loaded its window watcher on an output and settled on reporting no "
                            "window (reports: " + history(0) + ")");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    std::fprintf(stderr, "  the wallpaper is on %s\n", qPrintable(wallpaperScreen));
    // GOAL: with no window open the screen reports no focus at all ("none"), not the desktop: this
    //       wallpaper takes no keyboard focus, so KWin never activates it, here or in any step below
    //       that ends with no window focused.
    // WHY-THIS-SHAPE: the precondition above accepts any state without a focused window, so a report of
    //       "desktop" is graded here as the plugin's, not skipped as a harness that never settled.
    // CONTROL: via control_batch.sh, 2026-10-11, `&& root.Window.active` dropped from desktopFocused in
    //       backend/kde/kpackage/contents/ui/ScreenWindows.qml -> this line red, with "showing the desktop
    //       reports no window and no focused desktop", "minimizing a maximized window reports no window" and
    //       "a fullscreen window on the other output does not count here". Reverted.
    check(settlesOn({}), "with no window open the screen reports \"none\", not the desktop");

    QProcess client;
    client.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    client.start(QCoreApplication::applicationFilePath(), {QStringLiteral("client")});
    if (!client.waitForStarted(5000)) {
        notVerified(kGoals, "the client process could not be started");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    // The client's output since the last command was sent. Cleared before each, so a command's
    // acknowledgement can only be its own: "done normal" left from the first restore would
    // otherwise answer for the second at once, however the client fared.
    // CONTROL: observed 2026-10-01 through control_batch.sh, the client made to ignore the second
    //       "normal" (no state change, no answer). With the buffer kept across commands -> "FAIL:
    //       restoring it from fullscreen reports a window that is not maximized", blaming the plugin;
    //       as written -> "FAIL (harness): the client never acknowledged 'normal'", no plugin line.
    QByteArray answers;
    const auto send = [&](const char* command) {
        answers.clear();
        client.write(QByteArray(command) + '\n');
        const QByteArray done = QByteArray("done ") + command + '\n';
        const bool acknowledged = pumpUntil(
            [&] {
                answers += client.readAllStandardOutput();
                return answers.contains(done);
            },
            5000);
        if (!acknowledged) wallpaperx::testing::harnessFailure(std::string("the client never acknowledged '") + command + "'");
        return acknowledged;
    };
    // The outputs the client's two windows are on, as it last reported them.
    const auto where = [&]() -> std::pair<QString, QString> {
        if (!send("where")) return {};
        const qsizetype at = answers.lastIndexOf("at ");
        const QList<QByteArray> parts = answers.mid(at).split('\n').value(0).split(' ');
        return {QString::fromUtf8(parts.value(1)), QString::fromUtf8(parts.value(2))};
    };

    // One step: a command and the state this screen must settle on.
    const auto settleCheck = [&](std::size_t mark, const Facts& want, const char* what) {
        const bool settled = settlesOn(want);
        check(settled, what);
        if (!settled) std::fprintf(stderr, "  wanted %s; saw %s\n", text(want).c_str(), history(mark).c_str());
    };
    const auto step = [&](const char* command, const Facts& want, const char* what) {
        const std::size_t mark = stub.mark();
        if (send(command)) settleCheck(mark, want, what);
    };

    // GOAL: a window opened on this screen is reported focused; maximized and fullscreen windows are
    //       reported maximized, a normal one not; Plasma's model, as the file configures it, sees all
    //       of it through KWin.
    // WHY-THIS-SHAPE: the open step asks where KWin put the window before grading it, so a window
    //       placed on the other output is the skip the file header promises, not a FAIL against the
    //       plugin; asked after the placement wait, since a window not yet mapped reports Qt's guess.
    // CONTROL: the IsFullScreen term dropped from ScreenWindows.qml's recount
    //       (backend/kde/kpackage/contents/ui/ScreenWindows.qml) -> "making it fullscreen reports a
    //       maximized window" red. This file's controls observed 2026-10-01 through control_batch.sh,
    //       each under its own nested KWin, and reverted. The placement order observed by hand: the
    //       client made to put its window fullscreen on the second output at "open" -> NOT VERIFIED,
    //       "KWin put the client's window on 'Virtual-1'", Skipped, no FAIL line.
    {
        const std::size_t mark = stub.mark();
        if (!send("open")) {
            send("quit");
            client.waitForFinished(5000);
            static_cast<void>(wallpaperx::testing::reportCheckFailures());
            static_cast<void>(wallpaperx::testing::reportHarnessFaults("in this test's client, not in the plugin"));
            return 1;
        }
        pumpFor(kPlaceMs);
        const QString openedOn = where().first;
        if (openedOn != wallpaperScreen) {
            notVerified(kGoals, "KWin put the client's window on '" + openedOn.toStdString() +
                                    "', not on the wallpaper's output");
            send("quit");
            client.waitForFinished(5000);
            if (wallpaperx::testing::reportHarnessFaults("in this test's client, not in the plugin")) return 1;
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        settleCheck(mark, focusedWindow(false), "a window opened on this screen is reported focused");
    }
    step("max", focusedWindow(true), "maximizing it reports a maximized window");

    // GOAL: "show desktop" turns both facts off while it is on: windows are hidden, not minimized.
    // CONTROL: `!KWindowSystem.showingDesktop &&` dropped from both bindings in ScreenWindows.qml ->
    //       "showing the desktop reports no window" red.
    QQmlComponent toggle(&engine);
    toggle.setData("import QtQml\nimport org.kde.kwindowsystem\nQtObject { function show(on) { KWindowSystem.showingDesktop = on } }\n",
                   QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + QStringLiteral("/kwin_toggle.qml")));
    std::unique_ptr<QObject> desktop(toggle.create());
    if (desktop == nullptr) {
        notVerified("the show-desktop step", "org.kde.kwindowsystem could not be loaded to turn it on");
    } else {
        QMetaObject::invokeMethod(desktop.get(), "show", Q_ARG(QVariant, true));
        check(settlesOn({}), "showing the desktop reports no window and no focused desktop");
        QMetaObject::invokeMethod(desktop.get(), "show", Q_ARG(QVariant, false));
        check(settlesOn(focusedWindow(true)), "and leaving it reports the maximized window again");
    }

    step("normal", focusedWindow(false), "restoring it reports a window that is not maximized");
    step("full", focusedWindow(true), "making it fullscreen reports a maximized window");
    step("normal", focusedWindow(false), "restoring it from fullscreen reports a window that is not maximized");
    step("max", focusedWindow(true), "maximizing it again");
    // GOAL: a minimized window counts nowhere, though it is still maximized.
    // CONTROL: `filterMinimized: false` in ScreenWindows.qml -> this line red, and the other-output
    //       line below with it (the minimized maximized window still counts here).
    step("min", {}, "minimizing a maximized window reports no window");

    // GOAL: a window confirmed fullscreen and focused on the other output does not count on this
    //       one: this screen settles on no window, and once settled no report claims one.
    // WHY-THIS-SHAPE: graded only from the client's confirmation that the window is on the other
    //       output. The client has to show it before it can ask for the other output, and KWin places
    //       a new window on this one first, so a window counted here in that moment is correct and
    //       the harness's own doing. A window that flashed "focused" here while it moved is therefore
    //       not graded -- coverage this step gives up.
    // CONTROL: `filterByScreen: false` in ScreenWindows.qml -> "...on the other output does not count
    //       here" red. `filterMinimized: false` (above) -> the same line red.
    // COVERAGE-ASSERT: the client must say the window is on the other output; one KWin left here would
    //       make the step about nothing.
    {
        const std::size_t before = stub.mark();
        if (send("full-second")) {
            pumpFor(kPlaceMs);
            const auto [firstAt, secondAt] = where();
            if (secondAt.isEmpty() || secondAt == wallpaperScreen) {
                notVerified("the other-output step", "the client's second window is on '" + secondAt.toStdString() +
                                                         "', not on an output other than the wallpaper's");
            } else {
                const bool settled = settlesOn({});
                const std::size_t mark = stub.mark();
                pumpFor(kPlaceMs);
                bool claimed = false;
                for (const ReportStub::Call& call : stub.calls(kFocus, kOutput, mark)) {
                    if (call.args.value(1).toString() != QLatin1String("none") || call.args.value(2).toBool()) claimed = true;
                }
                check(settled && !claimed && last() == Facts{},
                      "a fullscreen window on the other output does not count here");
                if (!settled || claimed) std::fprintf(stderr, "  this screen saw %s\n", history(before).c_str());
            }
        }
    }

    send("quit");
    client.closeWriteChannel();
    if (!client.waitForFinished(5000)) {
        client.kill();
        client.waitForFinished(2000);
    }
    if (wallpaperx::testing::reportCheckFailures()) return 1;
    if (wallpaperx::testing::reportHarnessFaults("in this test's client, not in the plugin")) return 1;
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
