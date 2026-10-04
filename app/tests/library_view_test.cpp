// SPDX-License-Identifier: GPL-2.0-or-later

// app/qml/LibraryView.qml and app/qml/WallpaperDetail.qml, offscreen, driven by a real
// LibraryManager over a scratch library of wallpaper folders and a real Importer, and served
// previews by the real PreviewImageProvider. Goals S15.45, S15.46, S15.52, S15.55 and S15.82 in
// GOALS.md.

#include "apply_controller.h"
#include "daemon_client.h"
#include "fake_kscreen.h"
#include "importer.h"
#include "kscreen_client.h"
#include "library_manager.h"
#include "plasma_controller.h"
#include "preview_image_provider.h"
#include "qml_load.h"
#include "wpx_fixtures.h"
#include "wpx_package.h"

#include <QDir>
#include <QGuiApplication>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QtQuickTest/quicktest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

namespace {

constexpr const char* kGoals = "S15.45, S15.46, S15.52, S15.55, S15.82";
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

template <typename T>
void collect(QQuickItem* item, QList<QQuickItem*>& out) {
    for (QQuickItem* child : item->childItems()) {
        if (child->property("modelData").metaType() == QMetaType::fromType<T>()) {
            out.append(child);
        }
        collect<T>(child, out);
    }
}

QQuickItem* firstInheriting(QQuickItem* item, const char* className) {
    const QList<QQuickItem*> found = allInheriting(item, className);
    return found.isEmpty() ? nullptr : found.front();
}

QString textsUnder(QQuickItem* item) {
    QString out;
    for (QQuickItem* child : allInheriting(item, "QQuickItem")) {
        const QVariant text = child->property("text");
        if (child->isVisible() && text.metaType().id() == QMetaType::QString) {
            out += text.toString() + QLatin1Char('|');
        }
    }
    return out;
}

// Rewrites `path` with `bytes` and sets its modification time to `modifiedNs` before closing it, so
// its close is the last event. The time the file then has, or nothing when a step failed.
std::optional<qint64> rewriteWithTime(const QString& path, const QByteArray& bytes, qint64 modifiedNs) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) {
        return std::nullopt;
    }
    const timespec times[2] = {{0, UTIME_OMIT}, {modifiedNs / 1'000'000'000, modifiedNs % 1'000'000'000}};
    struct stat st {};
    const bool done = ::write(fd, bytes.constData(), size_t(bytes.size())) == bytes.size() && ::futimens(fd, times) == 0 && ::fstat(fd, &st) == 0;
    if (::close(fd) != 0 || !done) {
        return std::nullopt;
    }
    return qint64(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
}

QPoint centreOf(const QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

// Layouts place their children only when the window is polished, which the offscreen platform
// does not do until a frame is due; geometry read before that is the unlaid-out default.
bool settleEvents(QQuickWindow& window) {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    return !QQuickTest::qIsPolishScheduled(&window) || QQuickTest::qWaitForPolish(&window, kWaitMs);
}

// A view that does not load is blamed on the file, the test's own initial property, or a missing Qt
// module (qml_load.h). LibraryView takes the importer too; WallpaperDetail does not.
// CONTROL: app/qml/LibraryView.qml given `required property int neverGiven` -> red, "FAIL:
//       app/qml/LibraryView.qml did not load: ...Required property neverGiven was not initialized"; restored.
std::unique_ptr<QQuickItem> load(QQmlEngine& engine, QQuickWindow& window, const char* type, LibraryManager& library, Importer* importer = nullptr,
                                 ApplyController* applier = nullptr) {
    QQmlComponent component(&engine);
    component.loadFromModule("WallpaperX", type);
    QVariantMap given{{QStringLiteral("library"), QVariant::fromValue(&library)}};
    if (importer != nullptr) {
        given.insert(QStringLiteral("importer"), QVariant::fromValue(importer));
    }
    if (applier != nullptr) {
        given.insert(QStringLiteral("applier"), QVariant::fromValue(applier));
    }
    std::unique_ptr<QObject> object(component.createWithInitialProperties(given));
    auto* item = qobject_cast<QQuickItem*>(object.get());
    if (item == nullptr) {
        reportLoadFailure(component, given, QStringLiteral("app/qml/%1.qml").arg(QLatin1String(type)));
        return nullptr;
    }
    object.release();
    item->setParentItem(window.contentItem());
    item->setSize(QSizeF(window.width(), window.height()));
    return std::unique_ptr<QQuickItem>(item);
}

// S15.45, S15.46: the grid.
void grid(QQmlEngine& engine, QQuickWindow& window, LibraryManager& library, Importer& importer, const QString& ocean) {
    const auto view = load(engine, window, "LibraryView", library, &importer);
    if (!view) {
        return;
    }
    if (!settleEvents(window)) {
        notVerified("S15.45, S15.46 (the grid)", "the offscreen window never finished laying the grid out");
        return;
    }
    QList<QQuickItem*> tiles;
    collect<Wallpaper>(view.get(), tiles);
    QQuickItem* usable = nullptr;
    QQuickItem* broken = nullptr;
    for (QQuickItem* tile : tiles) {
        (tile->property("modelData").value<Wallpaper>().usable() ? usable : broken) = tile;
    }
    if (tiles.size() != 2 || usable == nullptr || broken == nullptr) {
        expect(false, QStringLiteral("S15.46: the grid shows %1 tile(s), not one usable and one broken").arg(tiles.size()));
        return;
    }
    QQuickItem* usableArea = firstInheriting(usable, "QQuickMouseArea");
    QQuickItem* brokenArea = firstInheriting(broken, "QQuickMouseArea");

    // GOAL: S15.46 -- a broken wallpaper's tile shows its reason and offers no click; a usable one does.
    // WHY-THIS-SHAPE: the MouseArea's `enabled` is read directly because a click on a broken tile
    //       is also refused by LibraryManager::select(), so the click check below cannot see the
    //       QML gate alone; the usable tile's area being enabled shows the lookup found the right item.
    // CONTROL: app/qml/LibraryView.qml, the MouseArea's `enabled: tile.modelData.usable` made
    //       `enabled: true` -> red, "S15.46: the broken tile's click area is enabled"; the reason
    //       label's `tile.modelData.problem` made `qsTr("Broken")` -> red, "the broken tile does not
    //       show its reason; it shows Broken|broken|"; restored.
    expect(textsUnder(broken).contains(QStringLiteral("wallpaper.json is not valid JSON")),
           QStringLiteral("S15.46: the broken tile does not show its reason; it shows %1").arg(textsUnder(broken)));
    // Read as a property: QQuickMouseArea's `enabled` is its own, and QQuickItem::isEnabled() is not it.
    const auto enabled = [](const QQuickItem* area) { return area && area->property("enabled").toBool(); };
    expect(brokenArea && usableArea && !enabled(brokenArea) && enabled(usableArea),
           QStringLiteral("S15.46: the broken tile's click area is %1 and the usable one's %2")
               .arg(enabled(brokenArea) ? QStringLiteral("enabled") : QStringLiteral("disabled or missing"),
                    enabled(usableArea) ? QStringLiteral("enabled") : QStringLiteral("disabled or missing")));

    // GOAL: S15.46 -- clicking a broken tile opens nothing; clicking a usable one opens it.
    // WHY-THIS-SHAPE: the usable click comes second and must select, which proves clicks reach
    //       the tiles at all, so the broken click selecting nothing is not a click that went nowhere.
    // CONTROL: app/qml/LibraryView.qml `enabled: true` together with app/library/library_manager.cpp
    //       select()'s `|| !it->usable()` deleted, in one patch (either alone still refuses) -> red,
    //       "S15.46: clicking the broken tile opened "…/broken""; restored.
    QTest::mouseClick(&window, Qt::LeftButton, {}, centreOf(broken));
    settleEvents(window);
    expect(library.selectedPath().isEmpty(), QStringLiteral("S15.46: clicking the broken tile opened \"%1\"").arg(library.selectedPath()));
    QTest::mouseClick(&window, Qt::LeftButton, {}, centreOf(usable));
    settleEvents(window);
    expect(library.selectedPath() == ocean, QStringLiteral("S15.46: clicking the usable tile opened \"%1\", not %2").arg(library.selectedPath(), ocean));
    library.clearSelection();

    // GOAL: S15.45 -- a usable tile's preview loads through QML's own image loader from a wallpaper
    //       folder whose path holds a space, '#', '?', '%' and a non-ASCII letter.
    // WHY-THIS-SHAPE: this is the only check that runs the real URL-to-provider path; the package
    //       test's round trip imitates how Qt splits the URL, this one does not have to.
    // CONTROL: app/library/wpx_package.cpp, previewImageId's folder part made the raw `folderPath`
    //       instead of its base64 -> red, "S15.45: the usable tile's preview ended with status 3"; restored.
    QQuickItem* image = firstInheriting(usable, "QQuickImage");
    // Image.Ready is 1 and Image.Error 3; Null (0) and Loading (2) are not an outcome yet.
    const bool loaded = image && waitUntil([&] { const int s = image->property("status").toInt(); return s == 1 || s == 3; }, kWaitMs);
    expect(loaded && image->property("status").toInt() == 1,
           QStringLiteral("S15.45: the usable tile's preview ended with status %1 (1 is Ready) for %2")
               .arg(image ? image->property("status").toInt() : -1)
               .arg(image ? image->property("source").toUrl().toString() : QString()));
}

// S15.46: the detail view.
void detail(QQmlEngine& engine, QQuickWindow& window, LibraryManager& library, ApplyController& applier, const QString& ocean) {
    if (!library.select(ocean)) {
        expect(false, QStringLiteral("S15.46: ocean could not be opened for the detail view"));
        return;
    }
    const auto view = load(engine, window, "WallpaperDetail", library, nullptr, &applier);
    if (!view) {
        return;
    }
    if (!settleEvents(window)) {
        notVerified("S15.46 (the detail view)", "the offscreen window never finished laying the detail view out");
        return;
    }

    // GOAL: S15.46 -- with no monitor chosen, Apply is shown and cannot be pressed, and the view
    //       says why.
    // WHY-THIS-SHAPE: "Reset to defaults" is found by the same search and must be enabled, so a
    //       lookup that returned some disabled button is not mistaken for Apply. No KScreen answers
    //       here, so nothing is selected; Apply's enabled side is graded in app_main_window.
    // CONTROL: app/qml/WallpaperDetail.qml, Apply's `enabled: root.applier.blockedReason === "" && ...`
    //       made `enabled: root.applier.applyState !== ApplyController.Working` (control_batch) ->
    //       red, "S15.46: Apply is enabled, Reset is enabled, the reason shown is ...".
    QQuickItem* apply = nullptr;
    QQuickItem* reset = nullptr;
    for (QQuickItem* button : allInheriting(view.get(), "QQuickAbstractButton")) {
        const QString text = button->property("text").toString();
        (text == u"Apply" ? apply : (text == u"Reset to defaults" ? reset : button)) = button;
    }
    const QString reason = QStringLiteral("Choose a monitor above to apply it to.");
    QQuickItem* reasonLabel = nullptr;
    for (QQuickItem* item : allInheriting(view.get(), "QQuickText")) {
        if (item->isVisible() && item->property("text").toString() == reason) {
            reasonLabel = item;
        }
    }
    expect(apply && reset && apply->isVisible() && !apply->isEnabled() && reset->isEnabled() && reasonLabel != nullptr,
           QStringLiteral("S15.46: Apply is %1, Reset is %2, the reason shown is %3")
               .arg(apply ? (apply->isEnabled() ? QStringLiteral("enabled") : QStringLiteral("disabled")) : QStringLiteral("missing"),
                    reset ? (reset->isEnabled() ? QStringLiteral("enabled") : QStringLiteral("disabled")) : QStringLiteral("missing"),
                    reasonLabel ? reason : QStringLiteral("none")));

    QList<QQuickItem*> rows;
    collect<WallpaperProperty>(view.get(), rows);
    std::ranges::sort(rows, {}, [](const QQuickItem* row) { return row->mapToScene(QPointF(0, 0)).y(); });
    QStringList order;
    QQuickItem* slider = nullptr;
    QQuickItem* toggle = nullptr;
    for (QQuickItem* row : rows) {
        const QString key = row->property("modelData").value<WallpaperProperty>().key;
        order.append(key);
        if (key == u"speed") {
            slider = firstInheriting(row, "QQuickSlider");
        } else if (key == u"mute") {
            toggle = firstInheriting(row, "QQuickSwitch");
        }
    }
    // GOAL: S15.46 -- the detail view lists the settings top to bottom in wallpaper.json's order.
    // CONTROL: app/qml/WallpaperDetail.qml, the Repeater's `model: root.chosen.properties` made
    //       `root.chosen.properties.slice().sort((a, b) => a.key.localeCompare(b.key))` -> red,
    //       "S15.46: the settings are shown as mute,speed,tint"; restored.
    expect(order.join(u',') == u"speed,mute,tint", QStringLiteral("S15.46: the settings are shown as %1, not speed,mute,tint").arg(order.join(u',')));
    if (!slider || !toggle) {
        expect(false, QStringLiteral("S15.46: no slider for speed or no switch for mute"));
        return;
    }
    const auto sliderValue = [&] { return slider->property("value").toDouble(); };
    const auto speed = [&] { return library.propertyValues().value(QStringLiteral("speed")).toDouble(); };

    // GOAL: S15.46 -- the speed slider shows the library's value: an edit made in C++ moves it.
    // CONTROL: app/qml/WallpaperDetail.qml, the Slider's `value: row.modelData.type === "float" ? row.current : 0`
    //       made `? row.modelData.defaultValue : 0` -> red, "S15.46: after speed was set to 2 in the library the slider shows 1"; restored.
    library.setPropertyValue(QStringLiteral("speed"), 2.0);
    settleEvents(window);
    expect(sliderValue() == 2.0, QStringLiteral("S15.46: after speed was set to 2 in the library the slider shows %1").arg(sliderValue()));

    // GOAL: S15.46 -- dragging the slider writes the library's value, and "Reset to defaults"
    //       afterwards moves the dragged slider back to the default: user input does not cut the
    //       slider loose from the library.
    // WHY-THIS-SHAPE: a real press-move-release on the track, since only user input can break the
    //       binding; resetting without a drag first would pass a slider whose binding breaks on drag.
    // CONTROL: app/qml/WallpaperDetail.qml, the Slider's `onMoved: root.library.setPropertyValue(row.modelData.key, value)`
    //       made `onMoved: { root.library.setPropertyValue(row.modelData.key, value); value = value; }`
    //       -> red, "S15.46: after a drag and a reset the slider shows 1.85878, not 1" (control_batch,
    //       2026-10-02); restored.
    const QPointF left = slider->mapToScene(QPointF(slider->width() * 0.5, slider->height() / 2));
    const QPointF right = slider->mapToScene(QPointF(slider->width() * 0.9, slider->height() / 2));
    QTest::mousePress(&window, Qt::LeftButton, {}, left.toPoint());
    for (int step = 1; step <= 5; ++step) {
        QTest::mouseMove(&window, (left + (right - left) * step / 5.0).toPoint());
    }
    QTest::mouseRelease(&window, Qt::LeftButton, {}, right.toPoint());
    settleEvents(window);
    // Dragged to 90% of a 0.1-2 track, from 2 (its end): strictly between proves the drag wrote it.
    const double dragged = speed();
    expect(dragged > 1.5 && dragged < 2.0 && std::abs(dragged - sliderValue()) < 1e-9,
           QStringLiteral("S15.46: after dragging the slider right the library holds speed %1 and the slider shows %2").arg(dragged).arg(sliderValue()));
    library.resetPropertyValues();
    settleEvents(window);
    expect(sliderValue() == 1.0, QStringLiteral("S15.46: after a drag and a reset the slider shows %1, not 1").arg(sliderValue()));

    // GOAL: S15.46 -- the mute switch likewise: a click turns the library's mute off, and a
    //       reset afterwards turns the switch back on.
    // CONTROL: app/qml/WallpaperDetail.qml, the Switch's `onToggled: root.library.setPropertyValue(row.modelData.key, checked)`
    //       made `onToggled: { root.library.setPropertyValue(row.modelData.key, checked); checked = checked; }`
    //       -> red, "S15.46: after a click and a reset the switch is off"; restored.
    QTest::mouseClick(&window, Qt::LeftButton, {}, centreOf(toggle));
    settleEvents(window);
    const QVariant muted = library.propertyValues().value(QStringLiteral("mute"));
    expect(muted == QVariant(false) && !toggle->property("checked").toBool(),
           QStringLiteral("S15.46: after clicking the switch the library holds mute %1 and the switch is %2")
               .arg(muted.toString(), toggle->property("checked").toBool() ? QStringLiteral("on") : QStringLiteral("off")));
    library.resetPropertyValues();
    settleEvents(window);
    expect(toggle->property("checked").toBool(), QStringLiteral("S15.46: after a click and a reset the switch is off"));
}

// S15.52, S15.55: a library too long to fit, scrolled to its end.
void busyGrid(QQmlEngine& engine, QQuickWindow& window, const QString& root) {
    constexpr int kPackages = 40;
    const QString dir = root + QStringLiteral("/busy");
    const QString badName = QStringLiteral("bad %2 %1");
    bool built = writeFolder(dir + QLatin1Char('/') + badName, {{QStringLiteral("wallpaper.json"), "hello"}}).isEmpty();
    for (int i = 0; i < kPackages && built; ++i) {
        OrderedJson json = planManifest();
        json["title"] = QStringLiteral("Tile %1").arg(i, 2, 10, QLatin1Char('0')).toStdString();
        const Entries entries{{QStringLiteral("wallpaper.json"), jsonBytes(json)},
                              {QStringLiteral("preview.png"), pngBytes(64, 36)},
                              {QStringLiteral("assets/video.mp4"), QByteArrayLiteral("not really a video")}};
        built = writeFolder(dir + QStringLiteral("/tile%1").arg(i, 2, 10, QLatin1Char('0')), entries).isEmpty();
    }
    if (!built) {
        harnessFailure("S15.52: the library of forty wallpapers could not be built");
        return;
    }
    LibraryManager library(dir, 30);
    Importer importer(root + QStringLiteral("/busy-drop"), dir, 30);
    library.rescan();
    if (!waitUntil([&] { return library.state() == LibraryManager::State::Ready && library.wallpapers().size() == kPackages + 1; }, kWaitMs)) {
        expect(false, QStringLiteral("S15.52: the library of forty wallpapers never settled; state %1").arg(int(library.state())));
        return;
    }
    const auto view = load(engine, window, "LibraryView", library, &importer);
    if (!view) {
        return;
    }
    QQuickItem* gridView = firstInheriting(view.get(), "QQuickGridView");
    if (!gridView || !settleEvents(window) || !QMetaObject::invokeMethod(gridView, "positionViewAtEnd") || !settleEvents(window)) {
        notVerified("S15.52 (the grid keeps its place)", "the grid could not be found, laid out, or scrolled to its end");
        return;
    }
    const auto delegates = [&] {
        QList<QQuickItem*> tiles;
        collect<Wallpaper>(view.get(), tiles);
        return tiles;
    };
    const auto pathOf = [](const QQuickItem* tile) { return tile->property("modelData").value<Wallpaper>().filePath; };
    QList<QPointer<QQuickItem>> kept;
    QStringList keptPaths;
    QQuickItem* broken = nullptr;
    for (QQuickItem* tile : delegates()) {
        kept.append(tile);
        keptPaths.append(pathOf(tile));
        if (tile->property("modelData").value<Wallpaper>().fileName == badName) {
            broken = tile;
        }
    }
    const double scrolled = gridView->property("contentY").toDouble();
    // The first row with no tile; row 0 does not qualify, a GridView keeps its current item alive.
    qsizetype changed = -1;
    for (qsizetype row = 0; row < kPackages && changed < 0; ++row) {
        if (!keptPaths.contains(library.wallpapers().value(row).filePath)) {
            changed = row;
        }
    }

    // GOAL: S15.55 -- a broken tile's accessible name quotes its folder name as it is, "%1" and
    //       "%2" included, and then the reason.
    // WHY-THIS-SHAPE: read through a QML expression on the tile, since the attached Accessible
    //       object has no public C++ accessor; the folder name holds both markers, so a chained
    //       substitution changes it whichever order it runs in.
    //       The whole name is compared with the reason the tile's row carries, since the reason
    //       holds a byte offset; that reason is then matched on its own against the literal text
    //       for the fixture's "hello", so a wrong or empty reason in the row cannot pass by being
    //       compared with itself.
    // CONTROL: app/qml/LibraryView.qml, the one-pass `.replace(/%[12]/g, ...)` made
    //       `.arg(tile.modelData.fileName).arg(tile.modelData.problem)` -> red, "S15.55: the
    //       broken tile's accessible name is "bad %2 wallpaper.json is not valid JSON ..."; restored.
    //       app/library/wpx_package.cpp, the invalid-JSON reason "wallpaper.json is not valid JSON
    //       (at byte %1)." made "... is not valid (at byte %1)." -> red, "S15.55: ... the row's
    //       reason is "wallpaper.json is not valid (at byte 1)."" (and S15.46's shown reason), though
    //       the name equals the row's reason exactly; restored.
    if (!broken) {
        expect(false, QStringLiteral("S15.55: the broken tile is not among the delegates at the grid's end: [%1]").arg(keptPaths.join(u", ")));
    } else {
        QQmlExpression name(qmlContext(broken), broken, QStringLiteral("Accessible.name"));
        const QString got = name.evaluate().toString();
        const QString problem = broken->property("modelData").value<Wallpaper>().problem;
        const QString want = badName + QStringLiteral(", cannot be used: ") + problem;
        const bool reasonIsJson = QRegularExpression(QStringLiteral("^wallpaper\\.json is not valid JSON \\(at byte \\d+\\)\\.$")).match(problem).hasMatch();
        expect(!name.hasError() && got == want && reasonIsJson,
               QStringLiteral("S15.55: the broken tile's accessible name is \"%1\"%2, not \"%3\"; the row's reason is \"%4\"")
                   .arg(got, name.hasError() ? QStringLiteral(" (") + name.error().toString() + u')' : QString(), want, problem));
    }

    // GOAL: S15.52 -- when a wallpaper far above the visible rows changes, the grid stays where the
    //       user scrolled it and every tile it holds is kept, not rebuilt.
    // WHY-THIS-SHAPE: the changed wallpaper is one whose tile is not instantiated at the grid's
    //       end, so nothing held has a reason to change; each held tile is kept by a QPointer, so
    //       a rebuilt one reads as destroyed even if a new one took its place.
    // CONTROL: app/library/library_manager.cpp, WallpaperListModel::assign() made a reset
    //       (`beginResetModel(); m_rows = std::move(wallpapers); endResetModel(); return;` first)
    //       -> red, "S15.52: after tile01 changed (reread 1) the grid's contentY went 1350 -> 0 and
    //       0 of 17 tiles survived"; app/qml/LibraryView.qml's `model: root.library.wallpaperModel`
    //       made `root.library.wallpapers` -> red, the same line; IN_CLOSE_WRITE deleted from
    //       app/library/folder_watcher.cpp's kInsideEvents, so the rewrite is never re-read -> red,
    //       "(reread 0)"; restored.
    // COVERAGE-ASSERT: the grid was scrolled away from the top, a wallpaper with no tile was found,
    //       and at least a screenful of tiles was held.
    expect(scrolled > 0 && changed >= 0 && kept.size() >= 8,
           QStringLiteral("S15.52: the grid was not scrolled away from its top (contentY %1, %2 tiles held, a row with no tile %3)")
               .arg(scrolled).arg(kept.size()).arg(changed));
    if (changed < 0) {
        return;
    }
    // The same bytes, so the new stamp must come from the modification time: set two seconds on,
    // before the close, rather than hoped for from the clock. A file system that keeps the old one
    // leaves nothing the library could see, so that is not verified rather than graded.
    // CONTROL: by hand, test-side, the time set to `target.stamp.modifiedNs` itself -> "NOT VERIFIED
    //       -- S15.52 (the grid keeps its place) ... kept the modification time"; restored.
    const Wallpaper target = library.wallpapers().value(changed);
    const QString manifest = target.filePath + QStringLiteral("/wallpaper.json");
    const auto bytes = readBytes(manifest);
    const auto modified = bytes ? rewriteWithTime(manifest, *bytes, target.stamp.modifiedNs + 2'000'000'000) : std::nullopt;
    if (!modified) {
        harnessFailure("S15.52: " + target.fileName.toStdString() + " could not be rewritten");
        return;
    }
    if (*modified == target.stamp.modifiedNs) {
        notVerified("S15.52 (the grid keeps its place)",
                    "the file system kept the modification time of " + target.fileName.toStdString() + ", so its rewrite is not a change");
        return;
    }
    const bool reread = waitUntil([&] { return library.wallpapers().value(changed).stamp != target.stamp; }, kWaitMs);
    settleEvents(window);
    int survived = 0;
    for (qsizetype i = 0; i < kept.size(); ++i) {
        survived += kept[i] && pathOf(kept[i]) == keptPaths[i];
    }
    const double now = gridView->property("contentY").toDouble();
    expect(reread && std::abs(now - scrolled) < 0.5 && survived == kept.size(),
           QStringLiteral("S15.52: after %1 changed (reread %2) the grid's contentY went %3 -> %4 and %5 of %6 tiles survived")
               .arg(target.fileName).arg(reread).arg(scrolled).arg(now).arg(survived).arg(kept.size()));
}

QQuickItem* buttonNamed(QQuickItem* view, const QString& text) {
    for (QQuickItem* button : allInheriting(view, "QQuickAbstractButton")) {
        if (button->property("text").toString() == text) {
            return button;
        }
    }
    return nullptr;
}

// The banner (a Frame) whose label starts with `prefix`, shown or not.
QQuickItem* bannerStarting(QQuickItem* view, const QString& prefix) {
    for (QQuickItem* frame : allInheriting(view, "QQuickFrame")) {
        for (QQuickItem* label : allInheriting(frame, "QQuickLabel")) {
            if (label->property("text").toString().startsWith(prefix)) {
                return frame;
            }
        }
    }
    return nullptr;
}

bool click(QQuickWindow& window, QQuickItem* item) {
    if (item == nullptr || !item->isVisible() || !settleEvents(window)) {
        return false;
    }
    QTest::mouseClick(&window, Qt::LeftButton, {}, centreOf(item));
    return settleEvents(window);
}

// S15.82: the importer as the library view shows it.
void importerViews(QQmlEngine& engine, QQuickWindow& window, const QString& root) {
    if (::geteuid() == 0) {
        notVerified("S15.82", "running as root, which can write into a read-only folder");
        return;
    }
    const QString drop = root + QStringLiteral("/view-drop");
    const QString wallpapers = drop + QStringLiteral("/wallpapers");
    const auto package = zipBytes(root + QStringLiteral("/view.scratch"), planEntries(titled("Dropped")));
    if (!package || !QDir().mkpath(wallpapers) || !setMode(wallpapers, 0555)) {
        harnessFailure("S15.82: the drop folder with a read-only wallpapers/ could not be built");
        return;
    }
    LibraryManager library(wallpapers, 30);
    Importer importer(drop, wallpapers, 30);
    library.rescan();
    importer.rescan();
    const auto view = load(engine, window, "LibraryView", library, &importer);
    const bool ready = view && waitUntil([&] { return library.state() == LibraryManager::State::Ready && !importer.busy(); }, kWaitMs) &&
                       settleEvents(window);
    if (!ready) {
        static_cast<void>(setMode(wallpapers, 0755));
        expect(!view, QStringLiteral("S15.82: the empty library never settled, or the view was never laid out"));
        return;
    }

    // GOAL: S15.82 -- an empty library says where a .wpx can be put -- the importer's drop
    //       folder -- and the header offers Import… and Rescan.
    // CONTROL: app/qml/LibraryView.qml, the empty sentence's `.arg(root.importer.dropDirectory)` made
    //       `.arg(root.library.directory)` -> red, "S15.82: the empty library reads [...…/view-drop/wallpapers.|...]";
    //       the Import button given `visible: false` -> red, "S15.82: the header's Import… button is
    //       missing or hidden"; restored.
    const QString sentence = QStringLiteral("No wallpapers yet. Import a .wpx file, or put one in %1.").arg(importer.dropDirectory());
    expect(textsUnder(view.get()).contains(sentence), QStringLiteral("S15.82: the empty library reads [%1], not \"%2\"").arg(textsUnder(view.get()), sentence));
    QQuickItem* importButton = buttonNamed(view.get(), QStringLiteral("Import…"));
    QQuickItem* rescanButton = buttonNamed(view.get(), QStringLiteral("Rescan"));
    expect(importButton && importButton->isVisible(), QStringLiteral("S15.82: the header's Import… button is missing or hidden"));

    // GOAL: S15.82 -- an import that fails is shown as its sentence in a banner with a Dismiss
    //       button, and Dismiss clears it; Rescan then re-reads the drop folder, so a file whose
    //       import failed is imported once the reason is gone, and the success is shown as its
    //       own banner, "Imported <title>.", whose Dismiss clears it too.
    // WHY-THIS-SHAPE: the file is dropped while wallpapers/ is read-only, so its import fails and
    //       is not recorded; the folder is made writable with no event the drop folder's watch
    //       reports, so only the Rescan button can bring the retry. Buttons are pressed with real
    //       clicks, which also shows each is visible and enabled; each banner is found by its
    //       label, shown or not, so a hidden banner is told apart from a missing one.
    // CONTROL: app/qml/LibraryView.qml, one per run: the problems Banner given `visible: false` ->
    //       red, "the problems banner is hidden" (and its Dismiss line, as a hidden button cannot be
    //       clicked); its `onDismissed: root.importer.dismissProblems()` made `onDismissed: {}` ->
    //       red, "after Dismiss the problems are [x.wpx could not be imported. ...]" only; Rescan's
    //       `root.importer.rescan();` deleted -> red, "after Rescan the notices are [] and the
    //       library lists []"; the notices Banner given `visible: false` -> red, "the notices banner
    //       is hidden" (and its Dismiss line); its `onDismissed: root.importer.dismissNotices()` made
    //       `onDismissed: {}` -> red, "after Dismiss the notices are [Imported Dropped.]" only; the
    //       shared Banner's `visible: banner.text.length > 0` made `visible: false` -> red, all four
    //       banner lines; restored.
    const QString staged = drop + QStringLiteral("/.x.wpx");
    if (!writeBytes(staged, *package) || ::rename(QFile::encodeName(staged).constData(), QFile::encodeName(drop + QStringLiteral("/x.wpx")).constData()) != 0) {
        static_cast<void>(setMode(wallpapers, 0755));
        harnessFailure("S15.82: x.wpx could not be dropped in");
        return;
    }
    const bool failed = waitUntil([&] { return !importer.problems().isEmpty() && !importer.busy(); }, kWaitMs) && settleEvents(window);
    if (!setMode(wallpapers, 0755)) {
        harnessFailure("S15.82: wallpapers/ could not be made writable again");
        return;
    }
    if (!failed || !importer.problems().constFirst().startsWith(QStringLiteral("x.wpx could not be imported."))) {
        expect(false, QStringLiteral("S15.82: the import into a read-only wallpapers/ reported [%1]").arg(importer.problems().join(QStringLiteral(" | "))));
        return;
    }
    QQuickItem* problems = bannerStarting(view.get(), QStringLiteral("x.wpx could not be imported."));
    expect(problems && problems->isVisible(), QStringLiteral("S15.82: the problems banner is %1").arg(problems ? QStringLiteral("hidden") : QStringLiteral("missing")));
    const bool dismissedProblems = problems && click(window, buttonNamed(problems, QStringLiteral("Dismiss")));
    expect(dismissedProblems && importer.problems().isEmpty() && !problems->isVisible(),
           QStringLiteral("S15.82: after Dismiss the problems are [%1]").arg(importer.problems().join(QStringLiteral(" | "))));

    const bool rescanned = click(window, rescanButton);
    const bool imported = rescanned && waitUntil([&] { return !importer.notices().isEmpty() && library.wallpapers().size() == 1; }, kWaitMs) &&
                          settleEvents(window);
    QStringList listed;
    for (const Wallpaper& w : library.wallpapers()) {
        listed.append(w.displayTitle());
    }
    expect(imported && importer.notices() == QStringList{QStringLiteral("Imported Dropped.")},
           QStringLiteral("S15.82: after Rescan the notices are [%1] and the library lists [%2]").arg(importer.notices().join(QStringLiteral(" | ")), listed.join(u", ")));
    if (!imported) {
        return;
    }
    QQuickItem* notices = bannerStarting(view.get(), QStringLiteral("Imported Dropped."));
    expect(notices && notices->isVisible(), QStringLiteral("S15.82: the notices banner is %1").arg(notices ? QStringLiteral("hidden") : QStringLiteral("missing")));
    const bool dismissedNotices = notices && click(window, buttonNamed(notices, QStringLiteral("Dismiss")));
    expect(dismissedNotices && importer.notices().isEmpty() && !notices->isVisible(),
           QStringLiteral("S15.82: after Dismiss the notices are [%1]").arg(importer.notices().join(QStringLiteral(" | "))));
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "library_view";
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

    const QString dir = scratch.path() + QStringLiteral("/lib #?% é");
    const QString ocean = dir + QStringLiteral("/ocean");
    if (!writeFolder(ocean, planEntries(QByteArray(kPlanManifest))).isEmpty() ||
        !writeFolder(dir + QStringLiteral("/broken"), {{QStringLiteral("wallpaper.json"), "hello"}}).isEmpty()) {
        harnessFailure("the library folder could not be built");
        static_cast<void>(wallpaperx::testing::reportHarnessFaults("in this test file's fixtures, not in the app"));
        return 1;
    }
    auto library = std::make_unique<LibraryManager>(dir, 30);
    auto importer = std::make_unique<Importer>(scratch.path() + QStringLiteral("/drop"), dir, 30);
    // WallpaperDetail requires an ApplyController. Its clients name services nobody owns on this
    // bus and are never asked, so no monitor is selected and Apply stays blocked.
    KScreenClient kscreen(QDBusConnection::sessionBus(), QStringLiteral("io.wallpaperx.test.KScreen.unowned"));
    DaemonClient daemon(QDBusConnection::sessionBus(), QStringLiteral("io.wallpaperx.test.Daemon.unowned"));
    PlasmaController plasma(QDBusConnection::sessionBus(), QStringLiteral("io.wallpaperx.test.PlasmaShell.unowned"));
    auto applier = std::make_unique<ApplyController>(kscreen, *library, daemon, plasma);
    library->rescan();
    if (!waitUntil([&] { return library->state() == LibraryManager::State::Ready && library->wallpapers().size() == 2; }, kWaitMs)) {
        expect(false, QStringLiteral("the library never listed its two wallpapers; state %1").arg(int(library->state())));
    } else {
        QQuickWindow window;
        window.resize(1000, 700);
        QQmlEngine engine;
        engine.addImageProvider(QString::fromLatin1(kPreviewProviderId), new PreviewImageProvider);
        window.show();
        if (!QTest::qWaitForWindowExposed(&window)) {
            notVerified(kGoals, "the offscreen window was never exposed, so no click can reach it");
        } else {
            grid(engine, window, *library, *importer, ocean);
            detail(engine, window, *library, *applier, ocean);
            busyGrid(engine, window, scratch.path());
            importerViews(engine, window, scratch.path());
        }
    }
    applier.reset();
    importer.reset();
    library.reset();
    // No import may still be writing into the scratch folder when it is removed.
    static_cast<void>(QThreadPool::globalInstance()->waitForDone(30000));

    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file or its fixtures, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
