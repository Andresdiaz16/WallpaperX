// SPDX-License-Identifier: GPL-2.0-or-later

// app/qml/MonitorPicker.qml, driven offscreen by a real KScreenClient reading a fake KScreen on a
// private bus. Goals S15.13, S15.17, S15.24 and S15.26 in GOALS.md.
//
// Layout on the offscreen platform is the same arithmetic as on a display: item geometry is
// computed by QML bindings, not by the window system. What this cannot see is rendering.

#include "fake_kscreen.h"
#include "kscreen_client.h"
#include "kscreen_config.h"
#include "kscreen_fixtures.h"
#include "tests/test_report.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QJSValue>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;

namespace {

constexpr const char* kGoals = "S15.13, S15.17, S15.24, S15.26";
constexpr int kWaitMs = 5000;
// MonitorPicker.qml draws each tile 2 px inside its screen's rectangle on every side.
constexpr double kTileInset = 4;

void expect(bool cond, const QString& msg) {
    check(cond, msg.toStdString().c_str());
}

// Qt answers a platform plugin it cannot load with qFatal, which aborts before this file can
// report anything. That is a property of the Qt install, so it becomes a skip naming it.
// CONTROL: by hand, test-side: the binary run under ctest's own command line with `-platform
//       nonexistent` appended (the argument overrides the QT_QPA_PLATFORM this file sets) ->
//       "NOT VERIFIED -- ... Qt could not load the offscreen platform plugin", exit 77.
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

// The picker's tiles, by connector name: `screens` for the ones that select, `mirrors` for the
// replicas drawn with their source. Found by the Monitor each delegate is bound to.
struct Tiles {
    QMap<QString, QQuickItem*> screens;
    QMap<QString, QQuickItem*> mirrors;
    int duplicates = 0;
};

void collectTiles(QQuickItem* item, Tiles& tiles) {
    for (QQuickItem* child : item->childItems()) {
        const QVariant data = child->property("modelData");
        if (data.metaType() == QMetaType::fromType<Monitor>()) {
            const Monitor m = data.value<Monitor>();
            auto& into = m.mirrorOf.isEmpty() ? tiles.screens : tiles.mirrors;
            if (into.contains(m.outputName)) {
                ++tiles.duplicates;
            }
            into.insert(m.outputName, child);
        }
        collectTiles(child, tiles);
    }
}

QString textsUnder(QQuickItem* item) {
    QString out;
    for (QQuickItem* child : item->childItems()) {
        const QVariant text = child->property("text");
        if (child->isVisible() && text.metaType().id() == QMetaType::QString) {
            out += text.toString() + QLatin1Char('\n');
        }
        out += textsUnder(child);
    }
    return out;
}

QRectF sceneRect(const QQuickItem* t) {
    return QRectF(t->mapToScene(QPointF(0, 0)), QSizeF(t->width(), t->height()));
}

QString rectOf(const QQuickItem* t) {
    const QRectF r = sceneRect(t);
    return QStringLiteral("(%1,%2 %3x%4)").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

QString keys(const QMap<QString, QQuickItem*>& map) {
    return QLatin1Char('[') + map.keys().join(QStringLiteral(", ")) + QLatin1Char(']');
}

struct Picker {
    QQuickWindow& window;
    QQuickItem* root;
    KScreenClient& client;
    FakeKScreen& fake;

    // Serves `config`, waits until the client holds exactly that answer, and returns the tiles.
    // Compared with the parser's reading of the same map, since two scenarios can list the same
    // names; getEdid is held throughout, so no display name differs from it.
    std::optional<Tiles> show(const char* scenario, const QVariantMap& config, const QStringList& names) {
        const auto want = parseKScreenConfig(config);
        if (!want || want->size() != names.size()) {
            expect(false, QStringLiteral("%1: the scenario's reply does not parse to [%2]").arg(QLatin1String(scenario), names.join(u',')));
            return std::nullopt;
        }
        fake.config = config;
        client.refresh();
        if (!waitFor([&] { return client.state() == KScreenClient::State::Ready && client.monitors() == *want; }, kWaitMs)) {
            QStringList listed;
            for (const Monitor& m : client.monitors()) {
                listed.append(m.outputName);
            }
            expect(false, QStringLiteral("%1: the client never took the answer listing [%2]; it is %3 with [%4]: %5")
                              .arg(QLatin1String(scenario), names.join(u','))
                              .arg(int(client.state()))
                              .arg(listed.join(u','), client.unavailableReason()));
            return std::nullopt;
        }
        // Delegates of the previous model go through deleteLater; they must be gone before the
        // tiles are collected, or they count as duplicates.
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        Tiles tiles;
        collectTiles(root, tiles);
        return tiles;
    }

    void click(QQuickItem* tile) {
        const QPoint centre = tile->mapToScene(QPointF(tile->width() / 2, tile->height() / 2)).toPoint();
        QTest::mouseClick(&window, Qt::LeftButton, {}, centre);
        QCoreApplication::processEvents();
    }

    // Each named tile, clicked at its centre while another screen is selected, selects itself.
    void expectEachClickSelectsItself(const char* scenario, const Tiles& tiles, const QStringList& names) {
        int clicked = 0;
        for (qsizetype i = 0; i < names.size(); ++i) {
            QQuickItem* tile = tiles.screens.value(names[i]);
            const QString other = names[(i + 1) % names.size()];
            if (!tile || !client.select(other)) {
                expect(false, QStringLiteral("%1: no tile for %2, or %3 could not be selected first").arg(QLatin1String(scenario), names[i], other));
                continue;
            }
            click(tile);
            ++clicked;
            expect(client.selectedOutput() == names[i],
                   QStringLiteral("%1: clicking %2 at %3 selected \"%4\"").arg(QLatin1String(scenario), names[i], rectOf(tile), client.selectedOutput()));
        }
        // COVERAGE-ASSERT: every named tile was clicked, each from a different selection.
        expect(clicked == names.size() && names.size() >= 2,
               QStringLiteral("%1: %2 of %3 tiles were clicked").arg(QLatin1String(scenario)).arg(clicked).arg(names.size()));
    }

    // No two tiles cover one point, so no click can land on a tile other than the one drawn there.
    void expectNoOverlap(const char* scenario, const Tiles& tiles) {
        const QList<QQuickItem*> all = tiles.screens.values() + tiles.mirrors.values();
        for (qsizetype i = 0; i < all.size(); ++i) {
            for (qsizetype j = i + 1; j < all.size(); ++j) {
                const QRectF both = sceneRect(all[i]).intersected(sceneRect(all[j]));
                expect(both.width() * both.height() < 1,
                       QStringLiteral("%1: tiles %2 and %3 overlap").arg(QLatin1String(scenario), rectOf(all[i]), rectOf(all[j])));
            }
        }
    }
};

// S15.13: the real two-screen reply, with a disabled screen listed first.
void realLayout(Picker& p) {
    const QVariantMap hdmiOff = output({.name = QStringLiteral("HDMI-A-1"), .id = 3, .enabled = false, .withPos = false, .withSize = false});
    const QVariantMap real = *realFixture();
    const auto tiles = p.show("S15.13", withOutputs(real, QVariantList{hdmiOff} + real.value(QStringLiteral("outputs")).toList()),
                              {QStringLiteral("HDMI-A-1"), QStringLiteral("DP-4"), QStringLiteral("DP-3")});
    if (!tiles) {
        return;
    }

    // GOAL: S15.13 -- the picker draws one tile per selectable screen and none for a screen
    //       showing no desktop, which it lists by name instead.
    // CONTROL: app/qml/MonitorPicker.qml, the first Repeater's model made `root.kscreen.monitors`
    //       -> red, "got tiles [DP-3, DP-4, HDMI-A-1]"; the footer's visible made false -> red,
    //       "HDMI-A-1 is not listed"; restored, green.
    expect(tiles->screens.size() == 2 && tiles->mirrors.isEmpty() && tiles->duplicates == 0 &&
               tiles->screens.contains(QStringLiteral("DP-3")) && tiles->screens.contains(QStringLiteral("DP-4")),
           QStringLiteral("S15.13: expected tiles for DP-3 and DP-4 only, got tiles %1 and mirror tiles %2")
               .arg(keys(tiles->screens), keys(tiles->mirrors)));
    expect(textsUnder(p.root).contains(QStringLiteral("Not showing a desktop: HDMI-A-1")),
           QStringLiteral("S15.13: the disabled HDMI-A-1 is not listed under the picker"));

    QQuickItem* dp3 = tiles->screens.value(QStringLiteral("DP-3"));
    QQuickItem* dp4 = tiles->screens.value(QStringLiteral("DP-4"));
    if (!dp3 || !dp4) {
        expect(false, QStringLiteral("S15.13: no DP-3 and DP-4 tiles to measure"));
        return;
    }
    // GOAL: S15.13 -- tiles sit where KScreen places the screens, at one common scale: DP-3
    //       portrait at the left edge, DP-4 landscape to its right and 761 logical pixels lower,
    //       both sized in proportion.
    // WHY-THIS-SHAPE: relative positions and sizes, measured in the tiles' own scale (DP-3's
    //       height over 2560), so the check survives any window size or margin; kTileInset adds
    //       back the tiles' drawn inset and the 2 px tolerance absorbs rounding only.
    // CONTROL: app/qml/MonitorPicker.qml, the tile's y made `root.originY + 2` -> red, "DP-4
    //       should sit 761 lower"; its height taken from `tile.placement.area.width` -> red, "DP-3
    //       should be portrait"; its x made `root.originX + 2` -> red, "DP-4 should sit 1440
    //       right"; restored, green.
    const double s = (dp3->height() + kTileInset) / 2560.0;
    expect(s > 0.05, QStringLiteral("S15.13: the tiles are drawn at scale %1, too small to measure").arg(s));
    expect(dp3->height() > dp3->width() && std::abs((dp3->width() + kTileInset) - 1440 * s) < 2,
           QStringLiteral("S15.13: DP-3 should be portrait, 1440x2560 at scale %1; its tile is %2").arg(s).arg(rectOf(dp3)));
    expect(std::abs((dp4->width() + kTileInset) - 2560 * s) < 2 && std::abs((dp4->height() + kTileInset) - 1440 * s) < 2,
           QStringLiteral("S15.13: DP-4 should be 2560x1440 at scale %1; its tile is %2").arg(s).arg(rectOf(dp4)));
    expect(std::abs((dp4->x() - dp3->x()) - 1440 * s) < 2,
           QStringLiteral("S15.13: DP-4 should sit 1440 right of DP-3; tiles %1 and %2").arg(rectOf(dp3), rectOf(dp4)));
    expect(std::abs((dp4->y() - dp3->y()) - 761 * s) < 2,
           QStringLiteral("S15.13: DP-4 should sit 761 lower than DP-3; tiles %1 and %2").arg(rectOf(dp3), rectOf(dp4)));

    // GOAL: S15.13 -- clicking a tile selects that tile's screen, by connector name.
    // WHY-THIS-SHAPE: a real click at each tile's centre, so the MouseArea and its binding are
    //       what is graded; KScreen lists HDMI-A-1, DP-4, DP-3 while the tiles are DP-4, DP-3, so
    //       a tile selecting by its index into the client's list goes red.
    // CONTROL: app/qml/MonitorPicker.qml, the tile delegate given `required property int index`
    //       and onClicked selecting `root.kscreen.monitors[tile.index].outputName` -> red,
    //       "clicking DP-3 ... selected "DP-4""; restored, green.
    p.expectEachClickSelectsItself("S15.13", *tiles, {QStringLiteral("DP-3"), QStringLiteral("DP-4")});
}

// S15.17: screens whose areas overlap, none of them a replica.
void overlappingScreens(Picker& p) {
    const auto at = [](const char* name, qlonglong id, qlonglong x, qlonglong y) {
        return output({.name = QString::fromLatin1(name), .id = id, .x = x, .y = y});
    };

    // GOAL: S15.17 -- screens that overlap, identically, partly, or in a chain where only
    //       neighbours overlap, each get a tile of their own: no two tiles cover one point, and a
    //       click at each tile's centre selects that screen.
    // WHY-THIS-SHAPE: the no-overlap test is what makes "each click selects itself" meaningful
    //       for every point of a tile, not only its centre; the chain's first and last screens
    //       overlap nothing, so only merging the grown group again catches the last one.
    // CONTROL: app/qml/MonitorPicker.qml, `overlaps` returning false (no grouping) -> red, "tiles
    //       ... overlap" and "clicking DP-3 ... selected "HDMI-A-1"" in "identical"; the grouping
    //       test made on each group's first member instead of its grown box -> red, "tiles ...
    //       overlap" in "chain"; restored, green.
    struct Case {
        const char* scenario;
        QVariantList outputs;
        QStringList clickable;
    };
    const Case cases[] = {
        {"S15.17 identical",
         {at("DP-3", 2, 0, 0), at("HDMI-A-1", 3, 0, 0), at("DP-4", 1, 1920, 0)},
         {QStringLiteral("DP-3"), QStringLiteral("HDMI-A-1"), QStringLiteral("DP-4")}},
        {"S15.17 partial", {at("DP-1", 1, 0, 0), at("DP-2", 2, 960, 540)}, {QStringLiteral("DP-1"), QStringLiteral("DP-2")}},
        {"S15.17 chain",
         {at("DP-1", 1, 0, 0), at("DP-2", 2, 1800, 0), at("DP-3", 3, 3600, 0)},
         {QStringLiteral("DP-1"), QStringLiteral("DP-2"), QStringLiteral("DP-3")}},
    };
    for (const Case& c : cases) {
        const auto tiles = p.show(c.scenario, configOf(c.outputs), c.clickable);
        if (!tiles) {
            continue;
        }
        expect(tiles->screens.size() == c.clickable.size() && tiles->duplicates == 0,
               QStringLiteral("%1: expected one tile each for [%2], got %3").arg(QLatin1String(c.scenario), c.clickable.join(u','), keys(tiles->screens)));
        p.expectNoOverlap(c.scenario, *tiles);
        p.expectEachClickSelectsItself(c.scenario, *tiles, c.clickable);
    }

    // GOAL: S15.17 -- a tile sharing its area says so: "same area as" the other screen's name.
    // CONTROL: app/qml/MonitorPicker.qml, the `same area as` line not pushed -> red; restored, green.
    if (const auto tiles = p.show("S15.17 identical label", configOf(cases[0].outputs), cases[0].clickable)) {
        QQuickItem* dp3 = tiles->screens.value(QStringLiteral("DP-3"));
        expect(dp3 && textsUnder(dp3).contains(QStringLiteral("same area as HDMI-A-1")),
               QStringLiteral("S15.17: DP-3's tile does not say it shares its area with HDMI-A-1: %1")
                   .arg(dp3 ? textsUnder(dp3).replace(u'\n', u'|') : QStringLiteral("(no tile)")));
    }

    // GOAL: S15.17 -- neighbours at a fractional scale, whose logical edges miss by rounding (a
    //       2560-pixel panel at 1.5 is 1706.67 wide, its neighbour starts at 1706), are not
    //       merged: each keeps its own position and size.
    // WHY-THIS-SHAPE: sizes and offsets measured against the scale of the unscaled screen's
    //       tile, so a merge into two equal strips (1813 each) is a 28 px error, well past the
    //       2 px rounding tolerance.
    // CONTROL: app/qml/MonitorPicker.qml, `overlaps` counting anything over 0 px instead of 1 px
    //       -> red, "DP-1 should be 1706.67 wide"; restored, green.
    const QVariantMap scaled = output({.name = QStringLiteral("DP-1"), .id = 1, .width = qlonglong(2560),
                                       .height = qlonglong(1440), .scale = 1.5});
    if (const auto tiles = p.show("S15.17 fractional", configOf({scaled, at("DP-2", 2, 1706, 0)}),
                                  {QStringLiteral("DP-1"), QStringLiteral("DP-2")})) {
        QQuickItem* dp1 = tiles->screens.value(QStringLiteral("DP-1"));
        QQuickItem* dp2 = tiles->screens.value(QStringLiteral("DP-2"));
        if (!dp1 || !dp2) {
            expect(false, QStringLiteral("S15.17 fractional: no DP-1 and DP-2 tiles; got %1").arg(keys(tiles->screens)));
        } else {
            const double s = (dp2->height() + kTileInset) / 1080.0;
            expect(std::abs((dp1->width() + kTileInset) - (2560 / 1.5) * s) < 2 && std::abs((dp2->width() + kTileInset) - 1920 * s) < 2,
                   QStringLiteral("S15.17 fractional: DP-1 should be 1706.67 wide and DP-2 1920 at scale %1; tiles %2 and %3")
                       .arg(s)
                       .arg(rectOf(dp1), rectOf(dp2)));
            expect(std::abs((dp2->x() - dp1->x()) - 1706 * s) < 2,
                   QStringLiteral("S15.17 fractional: DP-2 should sit 1706 right of DP-1; tiles %1 and %2").arg(rectOf(dp1), rectOf(dp2)));
        }
    }
}

// S15.24: replicas.
void mirroredScreens(Picker& p) {
    // DP-4 at the origin, DP-3 copying it at its position, DP-1 beside them.
    const QVariantMap dp4 = output({.name = QStringLiteral("DP-4"), .id = 1, .width = qlonglong(2560), .height = qlonglong(1440)});
    const QVariantMap dp3 = output({.name = QStringLiteral("DP-3"), .id = 2, .width = qlonglong(2560), .height = qlonglong(1440),
                                    .replicationSource = qlonglong(1)});
    const QVariantMap dp1 = output({.name = QStringLiteral("DP-1"), .id = 3, .x = qlonglong(2560)});
    const auto tiles = p.show("S15.24", configOf({dp4, dp3, dp1}), {QStringLiteral("DP-4"), QStringLiteral("DP-3"), QStringLiteral("DP-1")});
    if (!tiles) {
        return;
    }

    // GOAL: S15.24 -- a replica is drawn as a tile of its own that is not a selectable one, and
    //       says what it copies: "DP-3 · mirrors DP-4".
    // CONTROL: app/qml/MonitorPicker.qml, `placed` filtering on `m.enabled && m.connected` instead
    //       of `m.selectable` -> red, "expected screen tiles [DP-1, DP-4] and mirror tile [DP-3]";
    //       the replica's second line made `copy.modelData.outputName` -> red, "does not say
    //       mirrors DP-4"; restored, green.
    expect(keys(tiles->screens) == QStringLiteral("[DP-1, DP-4]") && keys(tiles->mirrors) == QStringLiteral("[DP-3]") &&
               tiles->duplicates == 0,
           QStringLiteral("S15.24: expected screen tiles [DP-1, DP-4] and mirror tile [DP-3], got %1 and %2 (%3 duplicate(s))")
               .arg(keys(tiles->screens), keys(tiles->mirrors))
               .arg(tiles->duplicates));
    QQuickItem* copy = tiles->mirrors.value(QStringLiteral("DP-3"));
    QQuickItem* source = tiles->screens.value(QStringLiteral("DP-4"));
    if (!copy || !source) {
        return;
    }
    expect(textsUnder(copy).contains(QStringLiteral("DP-3 · mirrors DP-4")),
           QStringLiteral("S15.24: DP-3's tile does not say mirrors DP-4: %1").arg(textsUnder(copy).replace(u'\n', u'|')));

    // GOAL: S15.24 -- the replica sits in a band under its source, inside the source's area: below
    //       the source's tile and within its width.
    // CONTROL: app/qml/MonitorPicker.qml, the band's y made `a.y` (the top of the source's area) ->
    //       red, "should sit under DP-4"; restored, green.
    const QRectF c = sceneRect(copy);
    const QRectF s = sceneRect(source);
    expect(c.top() >= s.bottom() - 0.5 && c.left() >= s.left() - 0.5 && c.right() <= s.right() + 0.5 && c.height() > 1,
           QStringLiteral("S15.24: DP-3's tile %1 should sit under DP-4's %2, within its width").arg(rectOf(copy), rectOf(source)));

    // GOAL: S15.24 -- a click on the replica selects nothing: not the replica, and not its source
    //       drawn beside it.
    // WHY-THIS-SHAPE: DP-1 is selected first, so a click that reached the source's tile would
    //       change the selection to DP-4; a click on the replica itself is refused by select()
    //       whatever the QML does, so the source is what can go wrong here.
    // CONTROL: app/qml/MonitorPicker.qml, the source keeping its whole area (the line shrinking
    //       `result[source.outputName].area` deleted), so its tile lies under the band -> red,
    //       "clicking DP-3's band selected "DP-4""; restored, green.
    if (!p.client.select(QStringLiteral("DP-1"))) {
        expect(false, QStringLiteral("S15.24: DP-1 could not be selected before the click"));
    } else {
        p.click(copy);
        expect(p.client.selectedOutput() == QStringLiteral("DP-1"),
               QStringLiteral("S15.24: clicking DP-3's band selected \"%1\"; DP-1 should have stayed selected").arg(p.client.selectedOutput()));
    }
    p.expectNoOverlap("S15.24", *tiles);

    // GOAL: S15.24 -- a replica whose source has no tile (the source is disabled) gets no tile
    //       and is listed under the picker as "DP-5 (mirrors DP-6)".
    // CONTROL: app/qml/MonitorPicker.qml, the footer naming each screen by `m.outputName` alone ->
    //       red, "(mirrors DP-6)" missing; `mirrors` without its `placed.some(...)` condition -> red,
    //       "got mirror tiles [DP-5]"; restored, green.
    const QVariantMap dp6Off = output({.name = QStringLiteral("DP-6"), .id = 2, .enabled = false, .withPos = false, .withSize = false});
    const QVariantMap dp5 = output({.name = QStringLiteral("DP-5"), .id = 3, .x = qlonglong(1920), .replicationSource = qlonglong(2)});
    const auto orphan = p.show("S15.24 no source", configOf({output({.name = QStringLiteral("DP-4"), .id = 1}), dp6Off, dp5}),
                               {QStringLiteral("DP-4"), QStringLiteral("DP-6"), QStringLiteral("DP-5")});
    if (orphan) {
        expect(keys(orphan->screens) == QStringLiteral("[DP-4]") && orphan->mirrors.isEmpty(),
               QStringLiteral("S15.24: a replica of a disabled screen should have no tile; got screen tiles %1 and mirror tiles %2")
                   .arg(keys(orphan->screens), keys(orphan->mirrors)));
        expect(textsUnder(p.root).contains(QStringLiteral("DP-5 (mirrors DP-6)")),
               QStringLiteral("S15.24: the footer does not list \"DP-5 (mirrors DP-6)\": %1").arg(textsUnder(p.root).replace(u'\n', u'|')));
    }
}

// The connector names in one of the picker's `var` arrays of monitors.
QStringList outputNamesIn(const QVariant& value) {
    const QVariantList list = value.metaType() == QMetaType::fromType<QJSValue>() ? value.value<QJSValue>().toVariant().toList()
                                                                                  : value.toList();
    QStringList out;
    for (const QVariant& entry : list) {
        out.append(entry.value<Monitor>().outputName);
    }
    return out;
}

// S15.26: an empty connector is not a monitor.
void emptyConnector(Picker& p) {
    const QVariantMap dp1 = output({.name = QStringLiteral("DP-1"), .id = 1});
    const QVariantMap empty = output({.name = QStringLiteral("HDMI-A-1"), .id = 2, .enabled = false, .connected = false,
                                      .withPos = false, .withSize = false});
    const QVariantMap off = output({.name = QStringLiteral("DP-3"), .id = 3, .enabled = false, .withPos = false, .withSize = false});
    const auto tiles = p.show("S15.26", configOf({dp1, empty, off}), {QStringLiteral("DP-1"), QStringLiteral("HDMI-A-1"), QStringLiteral("DP-3")});
    if (!tiles) {
        return;
    }

    // GOAL: S15.26 -- an output that is not connected is neither in `unplaced` nor in the footer,
    //       while a connected but disabled monitor still is: "Not showing a desktop: DP-3".
    // WHY-THIS-SHAPE: both sit beside one selectable screen and neither has geometry, so
    //       `connected` is the only field separating them; the footer is matched as a whole line,
    //       so a stray name anywhere in it goes red.
    // CONTROL: app/qml/MonitorPicker.qml, `unplaced`'s `m.connected &&` removed -> red,
    //       "unplaced is [HDMI-A-1, DP-3]" and the footer "Not showing a desktop: HDMI-A-1, DP-3";
    //       restored, green.
    const QStringList unplaced = outputNamesIn(p.root->property("unplaced"));
    const QStringList lines = textsUnder(p.root).split(u'\n');
    expect(unplaced == QStringList{QStringLiteral("DP-3")} && tiles->screens.size() == 1,
           QStringLiteral("S15.26: unplaced is [%1] with %2 tile(s); expected [DP-3] beside DP-1's tile")
               .arg(unplaced.join(QStringLiteral(", ")))
               .arg(tiles->screens.size()));
    expect(lines.contains(QStringLiteral("Not showing a desktop: DP-3")),
           QStringLiteral("S15.26: the footer should read \"Not showing a desktop: DP-3\"; the picker shows: %1").arg(lines.join(u'|')));
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "monitor_picker";
    // Before the QGuiApplication, whose platform theme may connect to the session bus.
    if (const auto refused = refuseUnlessUnderDbusRunSession()) {
        return *refused;
    }
    // Set here, not only by ctest: a run by hand must never open a window on the desktop.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    g_qtHandler = qInstallMessageHandler(skipWithoutPlatform);
    QGuiApplication app(argc, argv);
    qInstallMessageHandler(g_qtHandler);
    if (const auto refused = refuseUnlessPrivateBus()) {
        return *refused;
    }

    // GOAL (harness): a fixture that cannot be read fails the run as the test's own fault; it is
    //       never a skip, which would stop grading the picker while the suite stays green.
    // CONTROL: by hand, test-side: app/tests/data/kscreen_getconfig.json renamed -> "FAIL
    //       (harness): the KScreen fixture ... could not be read", exit 1, ctest "Failed"; restored.
    if (!realFixture()) {
        wallpaperx::testing::harnessFailure("the KScreen fixture " WALLPAPERX_APP_TEST_DATA "/kscreen_getconfig.json could not be read");
        static_cast<void>(wallpaperx::testing::reportHarnessFaults("in this test file's data, not in the app"));
        return 1;
    }
    auto fake = std::make_unique<FakeKScreen>(QStringLiteral("io.wallpaperx.test.FakeKScreen.picker"));
    if (const QString fault = fake->start(); !fault.isEmpty()) {
        notVerified(kGoals, "the fake KScreen could not stand up: " + fault.toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    // Every getEdid is held, so each screen keeps its connector name as its display name.
    fake->edidMode = FakeKScreen::Mode::Hold;
    auto client = std::make_unique<KScreenClient>(QDBusConnection::sessionBus(), fake->service());

    QQuickWindow window;
    window.resize(1000, 700);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.loadFromModule("WallpaperX", "MonitorPicker");
    std::unique_ptr<QObject> root(component.createWithInitialProperties({{QStringLiteral("kscreen"), QVariant::fromValue(client.get())}}));
    auto* picker = qobject_cast<QQuickItem*>(root.get());
    if (picker == nullptr) {
        wallpaperx::testing::harnessFailure("MonitorPicker did not load: " + component.errorString().toStdString());
        static_cast<void>(wallpaperx::testing::reportHarnessFaults("loading app/qml/MonitorPicker.qml"));
        return 1;
    }
    picker->setParentItem(window.contentItem());
    picker->setSize(QSizeF(1000, 700));
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) {
        notVerified(kGoals, "the offscreen window was never exposed, so no click can reach it");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    Picker p{window, picker, *client, *fake};
    realLayout(p);
    overlappingScreens(p);
    mirroredScreens(p);
    emptyConnector(p);

    root.reset();
    client.reset();
    fake.reset();
    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file or its fake KScreen, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
