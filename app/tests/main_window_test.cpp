// SPDX-License-Identifier: GPL-2.0-or-later

// app/qml/Main.qml offscreen, as the app loads it, with a real KScreenClient reading a fake KScreen
// on a private bus, and a real LibraryManager and Importer over scratch folders. Goals S15.54,
// S15.55 and S15.82 in GOALS.md.

#include "fake_kscreen.h"
#include "importer.h"
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

constexpr const char* kGoals = "S15.54, S15.55, S15.82";
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

QQuickItem* labelStarting(QQuickItem* item, const QString& prefix) {
    for (QQuickItem* child : item->childItems()) {
        if (child->isVisible() && child->property("text").toString().startsWith(prefix)) {
            return child;
        }
        if (QQuickItem* found = labelStarting(child, prefix)) {
            return found;
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

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.loadFromModule("WallpaperX", "Main");
    const QVariantMap given{{QStringLiteral("kscreen"), QVariant::fromValue(client.get())},
                            {QStringLiteral("library"), QVariant::fromValue(library.get())},
                            {QStringLiteral("importer"), QVariant::fromValue(importer.get())}};
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
    }

    root.reset();
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
