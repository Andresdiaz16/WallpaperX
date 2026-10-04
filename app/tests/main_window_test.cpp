// SPDX-License-Identifier: GPL-2.0-or-later

// app/qml/Main.qml offscreen, as the app loads it, with a real KScreenClient reading a fake KScreen
// on a private bus, a real LibraryManager and Importer over scratch folders, and the real
// DaemonClient and ApplyController talking to stand-ins for the daemon and plasmashell. Goals
// S15.54, S15.55, S15.82, S15.243, S15.245 and S15.248-S15.250 in GOALS.md.

#include "apply_controller.h"
#include "daemon_client.h"
#include "daemon_rules.h"
#include "fake_kscreen.h"
#include "fake_wallpaperx.h"
#include "importer.h"
#include "plasma_controller.h"
#include "kscreen_client.h"
#include "kscreen_config.h"
#include "kscreen_fixtures.h"
#include "library_manager.h"
#include "qml_load.h"
#include "wpx_fixtures.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QtQuickTest/quicktest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;

namespace {

constexpr const char* kGoals = "S15.54, S15.55, S15.82, S15.243, S15.245, S15.248-S15.250";
constexpr int kWaitMs = 5000;

// Qt answers a platform plugin it cannot load with qFatal, which aborts before this file can
// report anything. That is a property of the Qt install, so it becomes a skip naming it.
QtMessageHandler g_qtHandler = nullptr;
void skipWithoutPlatform(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type == QtFatalMsg && message.contains(QLatin1String("Qt platform plugin"))) {
        notVerified(kGoals, "Qt could not load the offscreen platform plugin: " + message.toStdString());
        std::fflush(stderr);
        std::_Exit(77);
    }
    if (g_qtHandler) {
        g_qtHandler(type, context, message);
    }
}

QQuickItem* pickerIn(QQuickItem* item) {
    for (QQuickItem* child : item->childItems()) {
        if (QLatin1String(child->metaObject()->className()).startsWith(QLatin1String("MonitorPicker"))) {
            return child;
        }
        if (QQuickItem* found = pickerIn(child)) {
            return found;
        }
    }
    return nullptr;
}

void tilesIn(QQuickItem* item, QList<QQuickItem*>& out) {
    for (QQuickItem* child : item->childItems()) {
        if (child->property("modelData").metaType() == QMetaType::fromType<Monitor>()) {
            out.append(child);
        }
        tilesIn(child, out);
    }
}

// The first shown item whose text starts with `prefix`.
QQuickItem* labelStarting(QQuickItem* root, const QString& prefix) {
    for (QQuickItem* item : allInheriting(root, "QQuickItem")) {
        if (item->isVisible() && item->property("text").toString().startsWith(prefix)) {
            return item;
        }
    }
    return nullptr;
}

// Layouts place their children only when the window is polished.
bool settle(QQuickWindow& window) {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    return !QQuickTest::qIsPolishScheduled(&window) || QQuickTest::qWaitForPolish(&window, kWaitMs);
}

struct Screen {
    const char* name;
    qlonglong id;
    qlonglong x, y, width, height;
    QVariant mirrorOf = {};
};

QVariantMap layoutOf(const QList<Screen>& screens) {
    QVariantList outputs;
    for (const Screen& s : screens) {
        outputs.append(output({.name = QString::fromUtf8(s.name), .id = s.id, .x = s.x, .y = s.y, .width = s.width, .height = s.height,
                               .replicationSource = s.mirrorOf}));
    }
    return configOf(outputs);
}

struct Window {
    QQuickWindow& window;
    KScreenClient& client;
    FakeKScreen& fake;

    // Serves `config`, waits until the client holds it and the window is laid out, and returns the
    // picker; nothing, with the check failed, when that does not happen.
    QQuickItem* show(const char* scenario, const QVariantMap& config, QSize size) {
        const auto want = parseKScreenConfig(config);
        window.resize(size);
        fake.config = config;
        client.refresh();
        if (!want || !waitFor([&] { return client.state() == KScreenClient::State::Ready && client.monitors() == *want; }, kWaitMs)) {
            expect(false, QStringLiteral("%1: the client never took the layout; it is state %2").arg(QLatin1String(scenario)).arg(int(client.state())));
            return nullptr;
        }
        if (!waitFor([&] { return window.width() == size.width() && window.height() == size.height(); }, kWaitMs) || !settle(window)) {
            notVerified(scenario, "the offscreen window was not resized and laid out");
            return nullptr;
        }
        QQuickItem* picker = pickerIn(window.contentItem());
        if (!picker) {
            expect(false, QStringLiteral("%1: Main.qml holds no MonitorPicker").arg(QLatin1String(scenario)));
        }
        return picker;
    }
};

// Each tile's labels fit inside it, and it clips; the count of tiles measured.
int expectLabelsFit(const char* scenario, QQuickItem* picker) {
    QList<QQuickItem*> tiles;
    tilesIn(picker, tiles);
    for (QQuickItem* tile : tiles) {
        QQuickItem* labels = nullptr;
        for (QQuickItem* child : tile->childItems()) {
            if (child->inherits("QQuickColumnLayout")) {
                labels = child;
            }
        }
        const QString name = tile->property("modelData").value<Monitor>().outputName;
        expect(labels && labels->implicitHeight() <= tile->height() + 0.5 && tile->clip(),
               QStringLiteral("%1: %2's tile is %3x%4 (clip %5) and its labels need %6")
                   .arg(QLatin1String(scenario), name).arg(tile->width()).arg(tile->height()).arg(tile->clip())
                   .arg(labels ? labels->implicitHeight() : -1));
    }
    return int(tiles.size());
}

void pickerHeight(Window& w) {
    // GOAL: S15.54 -- in the app's window, the monitor picker takes the height its layout asks
    //       for, capped at 45% of the window and never below 220 px: a landscape screen beside a
    //       portrait one in a 1280x800 window gets 360 px (its own ask, 816, is over the cap);
    //       in a 2000 px tall window it gets exactly its ask; four screens side by side, which ask
    //       for 217, get 220.
    // WHY-THIS-SHAPE: the real Main.qml is loaded, since the rule lives in its layout binding;
    //       each case lands in a different branch of max(220, min(ask, 45%)), so dropping any
    //       term changes one of them. Heights are compared to 0.5 px.
    // CONTROL: app/qml/Main.qml, `Layout.preferredHeight: Math.max(220, Math.min(picker.implicitHeight, window.height * 0.45))`
    //       made `Layout.preferredHeight: 220` -> red, "S15.54: landscape beside portrait in 1280x800:
    //       the picker is 220 px tall"; `window.height * 0.45` made `window.height` -> red, the
    //       same case, "... 800 px"; `Math.max(220, Math.min(...))` without the max -> red, "four
    //       side by side in 1280x800: the picker is 216.75 px"; app/qml/MonitorPicker.qml's
    //       `implicitHeight:` line deleted -> red, "landscape beside portrait ... 220 px"; restored.
    struct Case {
        const char* scenario;
        QList<Screen> screens;
        QSize window;
        double want;
    };
    const Case cases[] = {
        {"landscape beside portrait in 1280x800", {{"DP-1", 1, 0, 0, 2560, 1440}, {"DP-2", 2, 2560, 0, 1440, 2560}}, QSize(1280, 800), 360},
        {"landscape beside portrait in 1280x2000", {{"DP-1", 1, 0, 0, 2560, 1440}, {"DP-2", 2, 2560, 0, 1440, 2560}}, QSize(1280, 2000), 816},
        {"four side by side in 1280x800",
         {{"DP-1", 1, 0, 0, 1920, 1080}, {"DP-2", 2, 1920, 0, 1920, 1080}, {"DP-3", 3, 3840, 0, 1920, 1080}, {"DP-4", 4, 5760, 0, 1920, 1080}},
         QSize(1280, 800), 220},
    };
    for (const Case& c : cases) {
        QQuickItem* picker = w.show(c.scenario, layoutOf(c.screens), c.window);
        if (!picker) {
            continue;
        }
        expect(std::abs(picker->height() - c.want) < 0.5,
               QStringLiteral("S15.54: %1: the picker is %2 px tall (it asks for %3), not %4")
                   .arg(QLatin1String(c.scenario)).arg(picker->height()).arg(picker->implicitHeight()).arg(c.want));
    }

    // GOAL: S15.54 -- every tile's labels fit inside the tile, and the tile clips, including a
    //       replica's band under its source in a stacked layout.
    // WHY-THIS-SHAPE: a stacked pair with the lower screen mirrored is the tallest-but-narrowest
    //       case, where a fixed 220 px left the band 25 px tall for labels needing 34.
    // CONTROL: app/qml/Main.qml's preferred height made `220`, as above -> red, "S15.54: stacked,
    //       the lower mirrored: HDMI-A-1's tile is 148.889x24.6667 (clip 1) and its labels need 34";
    //       app/qml/MonitorPicker.qml,
    //       the tile's `clip: true` made `clip: false` -> red, "... DP-1's tile is ... (clip 0)"; restored.
    // COVERAGE-ASSERT: all three tiles, the band among them, were measured.
    if (QQuickItem* picker = w.show("S15.54 stacked, the lower mirrored",
                                    layoutOf({{"DP-1", 1, 0, 0, 1920, 1080}, {"DP-2", 2, 0, 1080, 1920, 1080},
                                              {"HDMI-A-1", 3, 0, 1080, 1920, 1080, qlonglong(2)}}),
                                    QSize(1280, 800))) {
        const int measured = expectLabelsFit("S15.54: stacked, the lower mirrored", picker);
        expect(measured == 3, QStringLiteral("S15.54: %1 tile(s) were measured in the stacked layout, not 3").arg(measured));
    }
}

void selectedLabel(Window& w, QQuickItem* root) {
    const QString dell = QStringLiteral("Dell %2");
    if (!w.show("S15.55", configOf({output({.name = dell, .id = 1})}), QSize(1280, 800))) {
        return;
    }
    if (!w.client.select(dell) || !settle(w.window)) {
        expect(false, QStringLiteral("S15.55: %1 could not be selected").arg(dell));
        return;
    }
    // GOAL: S15.55 -- Main's "Selected" line shows a monitor name holding "%2" as it is.
    // CONTROL: app/qml/Main.qml, `picker.fill(qsTr("Selected: %1 (%2)"), [...])` made
    //       `qsTr("Selected: %1 (%2)").arg(window.kscreen.selectedMonitor.displayName).arg(window.kscreen.selectedOutput)`
    //       -> red, "S15.55: Main's selected line reads "Selected: Dell Dell %2 (Dell %2)""; restored.
    const QQuickItem* line = labelStarting(root, QStringLiteral("Selected: "));
    const QString got = line ? line->property("text").toString() : QStringLiteral("(no line)");
    expect(got == u"Selected: Dell %2 (Dell %2)", QStringLiteral("S15.55: Main's selected line reads \"%1\"").arg(got));
}

void importerWired(Window& w, QQuickItem* root, LibraryManager& library, Importer& importer) {
    library.rescan();
    if (!waitFor([&] { return library.state() == LibraryManager::State::Ready; }, kWaitMs) || !settle(w.window)) {
        expect(false, QStringLiteral("S15.82: Main's empty library never settled Ready"));
        return;
    }
    // GOAL: S15.82 -- the app's window shows the library view, wired to the importer, while no
    //       wallpaper is open (its empty library names the importer's drop folder), and the detail
    //       view once one is opened.
    // WHY-THIS-SHAPE: Main.qml's own StackLayout decides which view shows, so the real Main.qml is
    //       loaded; each state is recognised by text only that view shows while it is current.
    // CONTROL: app/qml/Main.qml, `currentIndex: window.library.selectedPath === "" ? 0 : 1` made
    //       `currentIndex: 1` -> red, "S15.82: with nothing open Main shows no "No wallpapers yet.
    //       ..." line"; made `currentIndex: 0` -> red, "S15.82: with ocean open Main shows no Apply
    //       button"; restored.
    const QString sentence = QStringLiteral("No wallpapers yet. Import a .wpx file, or put one in %1.").arg(importer.dropDirectory());
    const QQuickItem* empty = labelStarting(root, sentence);
    expect(empty != nullptr, QStringLiteral("S15.82: with nothing open Main shows no \"%1\" line").arg(sentence));
    const QString ocean = library.directory() + QStringLiteral("/ocean");
    if (!writeFolder(ocean, planEntries(QByteArray(kPlanManifest))).isEmpty()) {
        testing::harnessFailure("S15.82: the ocean wallpaper could not be written");
        return;
    }
    library.rescan();
    if (!waitFor([&] { return library.wallpapers().size() == 1; }, kWaitMs) || !library.select(ocean) || !settle(w.window)) {
        expect(false, QStringLiteral("S15.82: ocean was never listed and opened"));
        return;
    }
    const QQuickItem* apply = labelStarting(root, QStringLiteral("Apply"));
    expect(apply != nullptr && labelStarting(root, sentence) == nullptr, QStringLiteral("S15.82: with ocean open Main shows no Apply button"));
    library.clearSelection();
}

// The first shown one, as the user would see it.
QQuickItem* firstInheriting(QQuickItem* root, const char* className) {
    for (QQuickItem* item : allInheriting(root, className)) {
        if (item->isVisible()) {
            return item;
        }
    }
    return nullptr;
}

QQuickItem* buttonNamed(QQuickItem* root, const QString& text) {
    for (QQuickItem* button : allInheriting(root, "QQuickAbstractButton")) {
        if (button->isVisible() && button->property("text").toString() == text) {
            return button;
        }
    }
    return nullptr;
}

struct Services {
    FakeDaemon& daemonFake;
    DaemonClient& daemon;
    ApplyController& applier;
};

// Chooses entry `index` of the pause selector as the ComboBox does (see S15.245).
void choosePause(QQuickItem* combo, int index) {
    combo->setProperty("currentIndex", index);
    QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, index));
}

void applyAndPause(Window& w, QQuickItem* root, LibraryManager& library, Services& s) {
    const QString ocean = library.directory() + QStringLiteral("/ocean");
    const QString glow = library.directory() + QStringLiteral("/glow");
    if (!writeFolder(glow, planEntries(shaderManifest())).isEmpty()) {
        testing::harnessFailure("S15.243: the shader wallpaper could not be written");
        return;
    }
    library.rescan();
    if (!w.show("S15.243", layoutOf({{"DP-1", 1, 0, 0, 1920, 1080}, {"HDMI-A-1", 2, 1920, 0, 1920, 1080}}), QSize(1280, 800)) ||
        !waitFor([&] { return library.wallpapers().size() == 2 && s.daemon.state() == DaemonClient::State::Ready; }, kWaitMs) ||
        !w.client.select(QStringLiteral("DP-1"))) {
        expect(false, QStringLiteral("S15.243: DP-1, the two wallpapers and the daemon's status never all became ready"));
        return;
    }

    // GOAL: S15.243 -- in the app's window, Apply can be pressed for a video on a chosen monitor,
    //       and pressing it applies; for a shader it cannot be pressed and the window says why.
    // WHY-THIS-SHAPE: the real Main.qml, so the binding that decides is the one the app runs; the
    //       press is a click on the button, graded by the daemon stand-in receiving the wallpaper.
    // CONTROL: app/daemon/apply_controller.cpp, blockedReason()'s shader branch deleted -> red,
    //       "S15.243: with the shader open Apply is enabled and the reason shown is none";
    //       app/qml/WallpaperDetail.qml Apply's `onClicked: root.applier.apply()` deleted -> red,
    //       "S15.243: Apply for ocean on DP-1 is enabled; clicking it sent 0 SetWallpaperWithProperties"
    //       (both control_batch, 2026-10-02).
    const QString shaderReason = QStringLiteral("WallpaperX cannot draw shader wallpapers yet, so this one cannot be applied.");
    if (!library.select(ocean) || !settle(w.window)) {
        expect(false, QStringLiteral("S15.243: ocean could not be opened"));
        return;
    }
    QQuickItem* apply = buttonNamed(root, QStringLiteral("Apply"));
    const bool enabledForVideo = apply && apply->isEnabled();
    if (apply && enabledForVideo) {
        QTest::mouseClick(&w.window, Qt::LeftButton, {}, apply->mapToScene(QPointF(apply->width() / 2, apply->height() / 2)).toPoint());
    }
    const bool applied = waitFor([&] { return !s.daemonFake.named(QStringLiteral("SetWallpaperWithProperties")).isEmpty() &&
                                              s.applier.applyState() != ApplyController::Progress::Working; }, kWaitMs);
    expect(enabledForVideo && applied && s.applier.applyState() == ApplyController::Progress::Done,
           QStringLiteral("S15.243: Apply for ocean on DP-1 is %1; clicking it sent %2 SetWallpaperWithProperties (%3)")
               .arg(enabledForVideo ? QStringLiteral("enabled") : QStringLiteral("disabled"))
               .arg(s.daemonFake.named(QStringLiteral("SetWallpaperWithProperties")).size())
               .arg(s.applier.applyMessage()));
    if (!library.select(glow) || !settle(w.window)) {
        expect(false, QStringLiteral("S15.243: the shader could not be opened"));
        return;
    }
    apply = buttonNamed(root, QStringLiteral("Apply"));
    const QQuickItem* reason = labelStarting(root, shaderReason);
    expect(apply && !apply->isEnabled() && reason != nullptr,
           QStringLiteral("S15.243: with the shader open Apply is %1 and the reason shown is %2")
               .arg(apply ? (apply->isEnabled() ? QStringLiteral("enabled") : QStringLiteral("disabled")) : QStringLiteral("missing"),
                    reason ? shaderReason : QStringLiteral("none")));
    library.clearSelection();
    settle(w.window);

    // GOAL: S15.245 -- the window's pause selector offers exactly the daemon's four modes, by the
    //       daemon's names (backend/daemon/pause_mode.h); choosing each sends it for the selected
    //       screen and the selector then shows it; a mode the daemon reports that this app does not
    //       know shows "Set by a newer WallpaperX".
    // WHY-THIS-SHAPE: a choice is made as the ComboBox makes it -- currentIndex set, then its
    //       `activated` signal -- rather than through the popup, whose offscreen geometry is not
    //       what is graded here; what is graded is the name each entry sends and what is shown.
    // CONTROL: app/qml/ScreenSettings.qml, the model's `mode: "paused_while_focused"` made
    //       `mode: "paused_while_focus"` -> red, "S15.245: the selector offers [play,always_paused,
    //       paused_while_focus,paused_while_maximized], the daemon takes [...]" and "choosing entry 2
    //       sent "always_paused" ..."; app/daemon/daemon_client.cpp pauseModes()'s `value_or(QString())`
    //       made `value_or(QStringLiteral("play"))` -> red, "S15.245: with a mode the daemon does not
    //       name the selector reads "Never"" (both control_batch, 2026-10-02).
    // COVERAGE-ASSERT: the daemon's table is pinned at four modes, so a table that shrank on both
    //       sides at once cannot pass "offered == daemon" with fewer choices graded. Controlled with
    //       backend/daemon/pause_mode.h's toString() and ScreenSettings.qml's model both cut to
    //       "play" (control_batch, 2026-10-04) -> red, "S15.245: the daemon names 1 mode(s), not 4".
    QQuickItem* combo = firstInheriting(root, "QQuickComboBox");
    if (combo == nullptr) {
        expect(false, QStringLiteral("S15.245: Main shows no pause selector with DP-1 selected"));
        return;
    }
    QStringList offered;
    for (const QVariant& entry : combo->property("model").toList()) {
        offered.append(entry.toMap().value(QStringLiteral("mode")).toString());
    }
    QStringList daemonNames;
    for (const std::string& n : daemonPauseModeNames()) {
        daemonNames.append(QString::fromStdString(n));
    }
    expect(offered == daemonNames, QStringLiteral("S15.245: the selector offers [%1], the daemon takes [%2]").arg(offered.join(u','), daemonNames.join(u',')));
    expect(daemonNames.size() == 4, QStringLiteral("S15.245: the daemon names %1 mode(s), not 4").arg(daemonNames.size()));
    for (int i = 0; i < offered.size(); ++i) {
        const qsizetype before = s.daemonFake.named(QStringLiteral("SetMonitorPause")).size();
        choosePause(combo, i);
        const bool sent = waitFor([&] {
            return s.daemonFake.named(QStringLiteral("SetMonitorPause")).size() > before && s.applier.pauseState() != ApplyController::Progress::Working;
        }, kWaitMs) && settle(w.window);
        const auto calls = s.daemonFake.named(QStringLiteral("SetMonitorPause"));
        const QString got = calls.isEmpty() ? QString() : calls.back().args.value(1).toString();
        expect(sent && got == daemonNames.value(i) && combo->property("currentValue").toString() == daemonNames.value(i),
               QStringLiteral("S15.245: choosing entry %1 sent \"%2\" and the selector shows \"%3\"; the daemon's name is \"%4\"")
                   .arg(i).arg(got, combo->property("currentValue").toString(), daemonNames.value(i)));
    }
    const int id = s.daemonFake.outputs.value(QStringLiteral("DP-1"), -1);
    s.daemonFake.pause.insert(id, QJsonValue::Null);
    s.daemon.refresh();
    const bool unknown = waitFor([&] { return combo->property("displayText").toString() == u"Set by a newer WallpaperX"; }, kWaitMs);
    expect(unknown && id >= 0,
           QStringLiteral("S15.245: with a mode the daemon does not name the selector reads \"%1\"").arg(combo->property("displayText").toString()));
}

void scopedMessages(Window& w, QQuickItem* root, LibraryManager& library, Services& s) {
    const QString ocean = library.directory() + QStringLiteral("/ocean");
    const QString river = library.directory() + QStringLiteral("/river");
    if (!writeFolder(river, planEntries(titled("River"))).isEmpty()) {
        testing::harnessFailure("S15.248-S15.250: the River wallpaper could not be written");
        return;
    }
    library.rescan();
    if (!waitFor([&] { return library.wallpapers().size() == 3; }, kWaitMs) || !w.client.select(QStringLiteral("DP-1")) ||
        !library.select(ocean) || !settle(w.window)) {
        expect(false, QStringLiteral("S15.250: River was never listed, or DP-1 and Ocean could not be selected"));
        return;
    }
    const auto step = [&](bool selected) { return selected && settle(w.window); };

    // GOAL: S15.250 -- in the window, the detail page shows a finished Apply's sentence only while
    //       the wallpaper and screen it was for are selected.
    // WHY-THIS-SHAPE: the label is found by its text among shown items, so a sentence left on the
    //       page under River or HDMI-A-1 is caught as the user would see it.
    // CONTROL: app/daemon/apply_controller.cpp, applyMessage()'s `selected` made `true`
    //       (control_batch, 2026-10-04) -> red, "S15.250: in the window, "Ocean Waves is on DP-1." shows
    //       there 1, with River 1, back 1, on HDMI-A-1 1, back 1". Deleting `emit applyChanged();` from
    //       either selection handler stays green: the same handler emits blockedReasonChanged, and
    //       the label's binding reads applyMessage again on that.
    s.applier.apply();
    const bool done = waitFor([&] { return s.applier.applyState() != ApplyController::Progress::Working; }, kWaitMs) && settle(w.window);
    const auto shown = [&] { return labelStarting(root, QStringLiteral("Ocean Waves is on DP-1.")) != nullptr; };
    const bool there = shown();
    const bool onRiver = step(library.select(river)) && shown();
    const bool backOcean = step(library.select(ocean)) && shown();
    const bool onHdmi = step(w.client.select(QStringLiteral("HDMI-A-1"))) && shown();
    const bool backDp1 = step(w.client.select(QStringLiteral("DP-1"))) && shown();
    expect(done && there && !onRiver && backOcean && !onHdmi && backDp1,
           QStringLiteral("S15.250: in the window, \"Ocean Waves is on DP-1.\" shows there %1, with River %2, back %3, on HDMI-A-1 %4, back %5")
               .arg(there).arg(onRiver).arg(backOcean).arg(onHdmi).arg(backDp1));

    QQuickItem* combo = firstInheriting(root, "QQuickComboBox");
    if (combo == nullptr) {
        expect(false, QStringLiteral("S15.248: Main shows no pause selector with DP-1 selected"));
        return;
    }
    // GOAL: S15.248 -- in the window, a pause change still running shows its mode in DP-1's
    //       selector only; HDMI-A-1's shows what the daemon has for it.
    // CONTROL: app/daemon/apply_controller.cpp, pendingPauseMode()'s `&& m_kscreen.selectedOutput() ==
    //       m_pauseOutput` deleted (control_batch, 2026-10-04) -> red, "S15.248: with DP-1's change
    //       held the selector shows "always_paused" on DP-1, "always_paused" on HDMI-A-1, "always_paused"
    //       back on DP-1 (held 1)"; `emit pauseChanged();` deleted from the KScreen selectionChanged
    //       handler -> red, the same line.
    s.daemonFake.hold.insert(QStringLiteral("SetMonitorPause"));
    choosePause(combo, 1);
    const bool held = waitFor([&] { return !s.daemonFake.held.isEmpty(); }, kWaitMs) && settle(w.window);
    const QString pendingDp1 = combo->property("currentValue").toString();
    const QString pendingHdmi = step(w.client.select(QStringLiteral("HDMI-A-1"))) ? combo->property("currentValue").toString() : QString();
    const QString pendingBack = step(w.client.select(QStringLiteral("DP-1"))) ? combo->property("currentValue").toString() : QString();
    expect(held && pendingDp1 == u"always_paused" && pendingHdmi == u"play" && pendingBack == u"always_paused",
           QStringLiteral("S15.248: with DP-1's change held the selector shows \"%1\" on DP-1, \"%2\" on HDMI-A-1, \"%3\" back on DP-1 (held %4)")
               .arg(pendingDp1, pendingHdmi, pendingBack).arg(held));
    s.daemonFake.hold.clear();
    s.daemonFake.release();
    if (!waitFor([&] { return s.applier.pauseState() != ApplyController::Progress::Working; }, kWaitMs) || !settle(w.window)) {
        testing::harnessFailure("S15.249: the held pause change never finished, so the failure case cannot start");
        return;
    }

    // GOAL: S15.249 -- in the window, a refused pause change's sentence shows under DP-1, the screen
    //       it was for, not under HDMI-A-1, and again on returning to DP-1.
    // WHY-THIS-SHAPE: the window, not only the getter: ScreenSettings' message label is bound to
    //       the pause properties alone, so it changes on a new selection only if the controller
    //       says those changed then.
    // CONTROL: app/daemon/apply_controller.cpp, control_batch, 2026-10-04: pauseMessage() returning
    //       `m_pauseMessage` whatever is selected -> red, "S15.249: in the window the refusal shows on
    //       DP-1 1, on HDMI-A-1 1, back on DP-1 1 (failed 1)"; `emit pauseChanged();` deleted from the
    //       KScreen selectionChanged handler -> red, the same line (the getters were right, the label
    //       stale).
    s.daemonFake.refuse.insert(QStringLiteral("SetMonitorPause"), {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("no")});
    choosePause(combo, 2);
    const bool failed = waitFor([&] { return s.applier.pauseState() == ApplyController::Progress::Failed; }, kWaitMs) && settle(w.window);
    const auto refusal = [&] { return labelStarting(root, QStringLiteral("The pause setting for DP-1 was not changed.")) != nullptr; };
    const bool onDp1 = refusal();
    const bool onOther = step(w.client.select(QStringLiteral("HDMI-A-1"))) && refusal();
    const bool returned = step(w.client.select(QStringLiteral("DP-1"))) && refusal();
    expect(failed && onDp1 && !onOther && returned,
           QStringLiteral("S15.249: in the window the refusal shows on DP-1 %1, on HDMI-A-1 %2, back on DP-1 %3 (failed %4)")
               .arg(onDp1).arg(onOther).arg(returned).arg(failed));
    s.daemonFake.refuse.clear();
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "main_window";
    // Before the QGuiApplication, whose platform theme may connect to the session bus.
    if (const auto refused = refuseUnlessUnderDbusRunSession()) {
        return *refused;
    }
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified(kGoals, "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    // Set here, not only by ctest: a run by hand must never open a window on the desktop.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    g_qtHandler = qInstallMessageHandler(skipWithoutPlatform);
    QGuiApplication app(argc, argv);
    qInstallMessageHandler(g_qtHandler);
    if (const auto refused = refuseUnlessPrivateBus()) {
        return *refused;
    }

    auto fake = std::make_unique<FakeKScreen>(QStringLiteral("io.wallpaperx.test.FakeKScreen.main"));
    if (const QString fault = fake->start(); !fault.isEmpty()) {
        notVerified(kGoals, "the fake KScreen could not stand up: " + fault.toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    // Every getEdid is held, so each screen keeps its connector name as its display name.
    fake->edidMode = FakeKScreen::Mode::Hold;
    auto client = std::make_unique<KScreenClient>(QDBusConnection::sessionBus(), fake->service());
    const QString libraryDirectory = scratch.path() + QStringLiteral("/wallpaperx");
    auto library = std::make_unique<LibraryManager>(wallpapersDirectory(libraryDirectory), 30);
    auto importer = std::make_unique<Importer>(libraryDirectory, wallpapersDirectory(libraryDirectory), 30);
    // Stand-ins for the daemon and plasmashell under test-only names (fake_wallpaperx.h).
    CallLog log;
    auto daemonFake = std::make_unique<FakeDaemon>(QStringLiteral("io.wallpaperx.test.Daemon.main"), &log);
    auto shellFake = std::make_unique<FakeShell>(QStringLiteral("io.wallpaperx.test.PlasmaShell.main"), &log);
    shellFake->state.order = {QStringLiteral("DP-1"), QStringLiteral("HDMI-A-1")};
    shellFake->state.desktops = QJsonArray{plasmaDesktop(0, 0, "A"), plasmaDesktop(1, 1, "A")};
    if (const QString fault = daemonFake->start() + shellFake->start(); !fault.isEmpty()) {
        notVerified(kGoals, "the daemon and plasmashell stand-ins could not stand up: " + fault.toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    auto daemon = std::make_unique<DaemonClient>(QDBusConnection::sessionBus(), daemonFake->service(), DaemonTimeouts{2000});
    auto plasma = std::make_unique<PlasmaController>(QDBusConnection::sessionBus(), shellFake->service(), 2000);
    auto applier = std::make_unique<ApplyController>(*client, *library, *daemon, *plasma);
    daemon->refresh();

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.loadFromModule("WallpaperX", "Main");
    const QVariantMap given{{QStringLiteral("kscreen"), QVariant::fromValue(client.get())},
                            {QStringLiteral("library"), QVariant::fromValue(library.get())},
                            {QStringLiteral("importer"), QVariant::fromValue(importer.get())},
                            {QStringLiteral("daemon"), QVariant::fromValue(daemon.get())},
                            {QStringLiteral("applier"), QVariant::fromValue(applier.get())}};
    std::unique_ptr<QObject> root(component.createWithInitialProperties(given));
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    if (window == nullptr) {
        // Blamed on the file, the test's own initial properties, or a missing Qt module (qml_load.h).
        // CONTROL: app/qml/Main.qml given `required property int neverGiven` -> red, "FAIL:
        //       app/qml/Main.qml did not load: ...Required property neverGiven was not initialized"; restored.
        reportLoadFailure(component, given, QStringLiteral("app/qml/Main.qml"));
    } else if (!QTest::qWaitForWindowExposed(window)) {
        notVerified(kGoals, "the offscreen window was never exposed");
    } else {
        Window w{*window, *client, *fake};
        pickerHeight(w);
        selectedLabel(w, window->contentItem());
        importerWired(w, window->contentItem(), *library, *importer);
        Services services{*daemonFake, *daemon, *applier};
        applyAndPause(w, window->contentItem(), *library, services);
        scopedMessages(w, window->contentItem(), *library, services);
    }

    root.reset();
    applier.reset();
    plasma.reset();
    daemon.reset();
    shellFake.reset();
    daemonFake.reset();
    importer.reset();
    library.reset();
    client.reset();
    fake.reset();
    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file or its fake KScreen, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
