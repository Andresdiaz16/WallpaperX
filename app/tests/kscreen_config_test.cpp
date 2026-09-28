// SPDX-License-Identifier: GPL-2.0-or-later

// Reading KScreen's getConfig reply and a monitor's EDID: app/kde/kscreen_config.cpp, with no bus.
// Goals S15.1-S15.5, S15.21, S15.22 and S15.25 in GOALS.md.

#include "kscreen_config.h"
#include "kscreen_fixtures.h"
#include "tests/test_report.h"

#include <QCoreApplication>
#include <QString>

#include <string>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;

namespace {

void expect(bool cond, const QString& msg) {
    check(cond, msg.toStdString().c_str());
}

const Monitor* byName(const QList<Monitor>& monitors, const QString& name) {
    for (const Monitor& m : monitors) {
        if (m.outputName == name) {
            return &m;
        }
    }
    return nullptr;
}

QString describe(const Monitor& m) {
    return QStringLiteral("%1 id=%2 geometry=(%3,%4 %5x%6) pixels=%7x%8 rotation=%9 flipped=%10 primary=%11")
        .arg(m.outputName)
        .arg(m.kscreenId)
        .arg(m.geometry.x())
        .arg(m.geometry.y())
        .arg(m.geometry.width())
        .arg(m.geometry.height())
        .arg(m.pixelSize.width())
        .arg(m.pixelSize.height())
        .arg(m.rotation)
        .arg(m.flipped)
        .arg(m.primary);
}

// The fields the real fixture must produce for one screen, checked by connector name.
void expectRealScreens(const QList<Monitor>& monitors, const char* which) {
    const QString tag = QString::fromLatin1(which);
    expect(monitors.size() == 2, tag + QStringLiteral(": expected exactly two monitors, got %1").arg(monitors.size()));
    const Monitor* dp3 = byName(monitors, QStringLiteral("DP-3"));
    const Monitor* dp4 = byName(monitors, QStringLiteral("DP-4"));
    expect(dp3 != nullptr && dp4 != nullptr, tag + QStringLiteral(": DP-3 and DP-4 not both present"));
    if (dp3 == nullptr || dp4 == nullptr) {
        return;
    }
    expect(dp3->kscreenId == 2 && dp3->geometry == QRectF(0, 0, 1440, 2560) && dp3->pixelSize == QSize(1440, 2560) &&
               dp3->rotation == 90 && !dp3->flipped && !dp3->primary && dp3->selectable(),
           tag + QStringLiteral(": DP-3 should be id 2, portrait 1440x2560 at the origin, rotated 90, not primary; got ") +
               describe(*dp3));
    expect(dp4->kscreenId == 1 && dp4->geometry == QRectF(1440, 761, 2560, 1440) && dp4->pixelSize == QSize(2560, 1440) &&
               dp4->rotation == 0 && !dp4->flipped && dp4->primary && dp4->selectable(),
           tag + QStringLiteral(": DP-4 should be id 1, 2560x1440 at (1440,761), unrotated, primary; got ") +
               describe(*dp4));
    expect(dp3->displayName == QStringLiteral("DP-3") && dp4->displayName == QStringLiteral("DP-4"),
           tag + QStringLiteral(": before any EDID, a screen's display name should be its connector name"));
}

// S15.1
void realFixture_parsesByConnectorName() {
    const auto fixture = realFixture();
    if (!fixture) {
        wallpaperx::testing::harnessFailure("the KScreen fixture " WALLPAPERX_APP_TEST_DATA
                                            "/kscreen_getconfig.json could not be read as a JSON object");
        return;
    }
    const auto parsed = parseKScreenConfig(*fixture);
    expect(parsed.has_value(), QStringLiteral("S15.1: the real reply was refused: ") + parsed.error_or(QString()));
    if (parsed) {
        expectRealScreens(*parsed, "S15.1 (listed DP-4 first, as KScreen sent it)");
    }

    // GOAL: S15.1 -- a screen's identity is its connector name; reordering KScreen's list changes
    //       nothing about any screen, including which KScreen id it carries.
    // WHY-THIS-SHAPE: the fixture alone cannot catch an id taken from list position: DP-4 is first
    //       and id 1, DP-3 second and id 2, so position-derived ids happen to be right. Only the
    //       same reply reversed separates "by name" from "by position".
    // CONTROL: app/kde/kscreen_config.cpp, kscreenId set to monitors.size() + 1 -> the fixture
    //       order passed and this reversed copy went red naming DP-3 with id=1; restored, green.
    const QVariantList outputs = fixture->value(QStringLiteral("outputs")).toList();
    const auto reversedParse = parseKScreenConfig(withOutputs(*fixture, reversed(outputs)));
    expect(reversedParse.has_value(), QStringLiteral("S15.1: the reversed reply was refused"));
    if (reversedParse) {
        expectRealScreens(*reversedParse, "S15.1 (the same reply listed DP-3 first)");
    }
}

// S15.2
void numbers_intOrDouble_andFractionalScale() {
    // GOAL: S15.2 -- a number is read whether it arrives as an integer (qlonglong on the wire, int
    //       from a hand-built map) or as a double, per value, not per field.
    // WHY-THIS-SHAPE: one output per numeric type, all in one reply: the fixture only ever holds
    //       qlonglong, so it passes with the double case deleted from number().
    // CONTROL: app/kde/kscreen_config.cpp, `case QMetaType::Double:` removed from number() ->
    //       red: the reply was refused ("did not say where D-1 is"); `case QMetaType::Int:` removed
    //       -> red naming I-1; `case QMetaType::LongLong:` removed -> red naming L-1; restored, green.
    OutputSpec asInt{.name = QStringLiteral("I-1"), .id = 1, .x = 0, .y = 0, .width = 800, .height = 600};
    OutputSpec asLong{.name = QStringLiteral("L-1"), .id = 2, .x = qlonglong(800), .y = qlonglong(0),
                      .width = qlonglong(800), .height = qlonglong(600)};
    OutputSpec asDouble{.name = QStringLiteral("D-1"), .id = 3, .x = 1600.0, .y = 0.0, .width = 800.0, .height = 600.0};
    const auto parsed = parseKScreenConfig(configOf({output(asInt), output(asLong), output(asDouble)}));
    expect(parsed.has_value(), QStringLiteral("S15.2: a reply mixing int, qlonglong and double was refused: ") +
                                   parsed.error_or(QString()));
    if (parsed) {
        for (const auto& [name, x] : {std::pair{"I-1", 0}, std::pair{"L-1", 800}, std::pair{"D-1", 1600}}) {
            const Monitor* m = byName(*parsed, QString::fromLatin1(name));
            expect(m != nullptr && m->geometry == QRectF(x, 0, 800, 600),
                   QStringLiteral("S15.2: %1 should be 800x600 at (%2,0); got %3")
                       .arg(QString::fromLatin1(name))
                       .arg(x)
                       .arg(m ? describe(*m) : QStringLiteral("nothing")));
        }
    }

    // GOAL: S15.2 -- a fractional scale divides the pixel size into the logical size; the position
    //       stays as KScreen reports it (already logical); the pixel size is kept.
    // WHY-THIS-SHAPE: 1.25 arrives as a double, 2 as an integer: a scale read only from one type
    //       would leave that screen at scale 1 and its logical size equal to its pixels.
    // CONTROL: app/kde/kscreen_config.cpp, `/ monitor.scale` removed from the geometry -> red on
    //       both screens; restored, green.
    OutputSpec fractional{.name = QStringLiteral("DP-1"), .id = 1, .x = qlonglong(0), .y = qlonglong(0),
                          .width = qlonglong(2560), .height = qlonglong(1440), .scale = 1.25};
    OutputSpec doubled{.name = QStringLiteral("DP-2"), .id = 2, .x = qlonglong(2048), .y = qlonglong(0),
                       .width = qlonglong(3840), .height = qlonglong(2160), .scale = qlonglong(2)};
    const auto scaled = parseKScreenConfig(configOf({output(fractional), output(doubled)}));
    expect(scaled.has_value(), QStringLiteral("S15.2: a scaled reply was refused"));
    if (scaled) {
        const Monitor* a = byName(*scaled, QStringLiteral("DP-1"));
        const Monitor* b = byName(*scaled, QStringLiteral("DP-2"));
        expect(a && a->scale == 1.25 && a->geometry == QRectF(0, 0, 2048, 1152) && a->pixelSize == QSize(2560, 1440),
               QStringLiteral("S15.2: DP-1 at scale 1.25 should cover 2048x1152 at the origin with 2560x1440 pixels; got ") +
                   (a ? describe(*a) : QStringLiteral("nothing")));
        expect(b && b->scale == 2.0 && b->geometry == QRectF(2048, 0, 1920, 1080) && b->pixelSize == QSize(3840, 2160),
               QStringLiteral("S15.2: DP-2 at scale 2 should cover 1920x1080 at (2048,0) with 3840x2160 pixels; got ") +
                   (b ? describe(*b) : QStringLiteral("nothing")));
    }
}

// S15.3
void untrustworthyReply_isRefusedWhole() {
    const QVariantMap good = output({.name = QStringLiteral("DP-1"), .id = 1});

    struct Case {
        const char* what;
        QVariantMap config;
        const char* reasonMentions;
    };
    // GOAL: S15.3 -- a reply that cannot name each screen exactly once is refused as a whole,
    //       with a sentence for the user, and never as a partial list.
    // WHY-THIS-SHAPE: each case holds a valid DP-1 beside the fault, so a parser that skipped the
    //       bad output instead of refusing would return one monitor and still go red here; the
    //       reason is matched so each case proves its own branch, not a neighbour's.
    // CONTROL: app/kde/kscreen_config.cpp, one run per branch, each branch's `return
    //       std::unexpected(...)` deleted: the duplicate-name case went red ("was accepted with 2
    //       monitor(s)"), the empty-name case red, the missing-position and missing-size cases red
    //       (made `else if (false)`), and the no-outputs case red with its check made to accept a
    //       missing list. Restored, green.
    const Case cases[] = {
        {"no outputs list", QVariantMap{{QStringLiteral("screen"), QVariantMap{}}}, "no list of outputs"},
        {"outputs that is not a list", QVariantMap{{QStringLiteral("outputs"), QStringLiteral("DP-1")}},
         "no list of outputs"},
        {"an output with an empty name", configOf({good, output({.name = QString(), .id = 2})}), "without a name"},
        {"two outputs named DP-1", configOf({good, output({.name = QStringLiteral("DP-1"), .id = 2, .x = 1920})}),
         "two outputs named DP-1"},
        {"an enabled, connected output with no position",
         configOf({good, output({.name = QStringLiteral("DP-2"), .id = 2, .withPos = false})}), "where DP-2 is"},
        {"an enabled, connected output with no size",
         configOf({good, output({.name = QStringLiteral("DP-2"), .id = 2, .withSize = false})}), "where DP-2 is"},
        {"an enabled, connected output with a 0x0 size",
         configOf({good, output({.name = QStringLiteral("DP-2"), .id = 2, .width = qlonglong(0), .height = qlonglong(0)})}),
         "where DP-2 is"},
        {"an enabled, connected output whose position is text",
         configOf({good, output({.name = QStringLiteral("DP-2"), .id = 2, .x = QStringLiteral("0")})}), "where DP-2 is"},
    };
    for (const Case& c : cases) {
        const auto parsed = parseKScreenConfig(c.config);
        if (parsed) {
            expect(false, QStringLiteral("S15.3: a reply with %1 was accepted with %2 monitor(s)")
                              .arg(QString::fromLatin1(c.what))
                              .arg(parsed->size()));
            continue;
        }
        expect(parsed.error().contains(QString::fromLatin1(c.reasonMentions)),
               QStringLiteral("S15.3: a reply with %1 was refused, but the reason \"%2\" does not say \"%3\"")
                   .arg(QString::fromLatin1(c.what), parsed.error(), QString::fromLatin1(c.reasonMentions)));
    }

    // GOAL: S15.3 -- a screen that is not showing a desktop may lack a position and size; it is
    //       listed, and is not selectable.
    // CONTROL: app/kde/kscreen_config.cpp, the refusal's `else if (monitor.enabled && monitor.connected)`
    //       made `else` -> red, "a disabled output without geometry was refused"; restored, green.
    const auto disabled = parseKScreenConfig(configOf(
        {good, output({.name = QStringLiteral("HDMI-A-1"), .id = 2, .enabled = false, .withPos = false, .withSize = false}),
         output({.name = QStringLiteral("DP-9"), .id = 3, .connected = false, .withPos = false, .withSize = false})}));
    expect(disabled.has_value(), QStringLiteral("S15.3: a disabled output without geometry was refused: ") +
                                     disabled.error_or(QString()));
    if (disabled) {
        const Monitor* off = byName(*disabled, QStringLiteral("HDMI-A-1"));
        const Monitor* gone = byName(*disabled, QStringLiteral("DP-9"));
        expect(disabled->size() == 3 && off && gone && !off->selectable() && !gone->selectable(),
               QStringLiteral("S15.3: a disabled and a disconnected output should both be listed and unselectable"));
    }
}

// S15.3, the selectable() half
void selectable_needsEnabledConnectedAndSize() {
    // GOAL: S15.3 -- a screen that is disabled or disconnected is not selectable, even when KScreen
    //       still reports a position and size for it (a replica is not selectable either: S15.22).
    // WHY-THIS-SHAPE: geometry is present on every case here, so the enabled and connected terms
    //       are the only thing that can make the two unselectable ones unselectable.
    // CONTROL: app/kde/kscreen_config.h, `enabled &&` removed from selectable() -> red on
    //       "HDMI-A-1 (disabled)"; `connected &&` removed -> red on "DP-9 (disconnected)"; restored.
    const auto parsed = parseKScreenConfig(configOf({
        output({.name = QStringLiteral("DP-1"), .id = 1}),
        output({.name = QStringLiteral("HDMI-A-1"), .id = 2, .x = 1920, .enabled = false}),
        output({.name = QStringLiteral("DP-9"), .id = 3, .x = 3840, .connected = false}),
    }));
    expect(parsed.has_value(), QStringLiteral("S15.3: reply with disabled and disconnected screens was refused"));
    if (!parsed) {
        return;
    }
    const Monitor* on = byName(*parsed, QStringLiteral("DP-1"));
    const Monitor* off = byName(*parsed, QStringLiteral("HDMI-A-1"));
    const Monitor* gone = byName(*parsed, QStringLiteral("DP-9"));
    expect(on && on->selectable(), QStringLiteral("S15.3: DP-1 (enabled, connected, sized) should be selectable"));
    expect(off && !off->selectable(), QStringLiteral("S15.3: HDMI-A-1 (disabled) should not be selectable"));
    expect(gone && !gone->selectable(), QStringLiteral("S15.3: DP-9 (disconnected) should not be selectable"));
}

// S15.4
void rotationFlags() {
    struct Case {
        qlonglong flags;
        int degrees;
        bool flipped;
    };
    // GOAL: S15.4 -- every KScreen rotation flag maps to its degrees and mirror state, and a value
    //       that is not exactly one flag maps to unrotated and unmirrored.
    // WHY-THIS-SHAPE: the whole table, since the fixture only exercises 1 and 2; 3 is two flags at
    //       once, which a bit-test reading would take for 90 degrees.
    // CONTROL: app/kde/kscreen_config.cpp, `case 1 << 3:` mapped to 90 -> red on flags 8;
    //       `case 1 << 1: case 3:` -> red on flags 3; restored, green.
    const Case cases[] = {{1, 0, false},   {2, 90, false},   {4, 180, false}, {8, 270, false},
                          {16, 0, true},   {32, 90, true},   {64, 180, true}, {128, 270, true},
                          {0, 0, false},   {3, 0, false},    {256, 0, false}, {-1, 0, false}};
    for (const Case& c : cases) {
        const auto parsed = parseKScreenConfig(configOf({output({.name = QStringLiteral("DP-1"), .rotation = c.flags})}));
        if (!parsed || parsed->size() != 1) {
            expect(false, QStringLiteral("S15.4: a reply with rotation %1 did not parse to one monitor").arg(c.flags));
            continue;
        }
        const Monitor& m = parsed->front();
        expect(m.rotation == c.degrees && m.flipped == c.flipped,
               QStringLiteral("S15.4: rotation flags %1 should read %2 degrees%3; got %4 degrees%5")
                   .arg(c.flags)
                   .arg(c.degrees)
                   .arg(c.flipped ? QStringLiteral(" flipped") : QString())
                   .arg(m.rotation)
                   .arg(m.flipped ? QStringLiteral(" flipped") : QString()));
    }
}

// S15.5
void edidNames() {
    const auto expectName = [](const QByteArray& edid, const QString& want, const char* what) {
        const QString got = edidMonitorName(edid);
        expect(got == want, QStringLiteral("S15.5: %1: expected \"%2\", got \"%3\"").arg(QString::fromLatin1(what), want, got));
    };

    // GOAL: S15.5 -- the monitor-name descriptor is found in any of the four descriptor slots.
    // CONTROL: app/kde/kscreen_config.cpp, the slot list cut to {54, 72, 90} -> red on "name in the
    //       fourth slot"; restored, green.
    expectName(edidNamed("M27Q P"), QStringLiteral("M27Q P"), "name in the second slot");
    expectName(edidNamed("M27Q", 3), QStringLiteral("M27Q"), "name in the fourth slot");
    expectName(edidNamed("THIRTEENCHARS"), QStringLiteral("THIRTEENCHARS"), "a 13-byte name with no newline");

    // GOAL: S15.5 -- a block that is not an EDID base block names nothing.
    // WHY-THIS-SHAPE: the 127-byte block keeps its name descriptor in slot 3 (bytes 108-125),
    //       inside the data, so dropping the length check returns the name rather than reading
    //       past the end: the check is decided by the code, not by what lies beyond the buffer.
    // CONTROL: app/kde/kscreen_config.cpp, `edid.size() < 128 ||` removed -> red on "127-byte
    //       block"; the header comparison removed -> red on "no header"; restored, green.
    QByteArray noHeader = edidNamed("M27Q");
    noHeader[1] = 0;
    expectName(noHeader, QString(), "no header");
    expectName(edidNamed("M27Q", 3).left(127), QString(), "127-byte block");
    expectName(QByteArray(), QString(), "empty block");

    // GOAL: S15.5 -- only a display descriptor tagged 0xFC is a name: not the serial (0xFF) or
    //       free text (0xFE), and not a detailed timing whose byte 3 happens to be 0xFC.
    // CONTROL: app/kde/kscreen_config.cpp, the tag test accepting 0xFE too -> red on "0xFE text only";
    //       the zero-pixel-clock test removed -> red on "timing descriptor"; restored, green.
    QByteArray textOnly = edidNamed("NOTANAME");
    textOnly[54 + 18 + 3] = '\xfe';
    expectName(textOnly, QString(), "0xFE text only");
    QByteArray timing = edidNamed("NOTANAME");
    timing[54 + 18] = '\x01'; // a non-zero pixel clock makes this a detailed timing descriptor
    expectName(timing, QString(), "timing descriptor");

    // GOAL: S15.5 -- the name stops at its newline, drops bytes that are not printable ASCII, and
    //       loses its space padding.
    // CONTROL: app/kde/kscreen_config.cpp, the newline `break` removed -> red on "text after the
    //       newline"; the printable-range filter removed -> red on "control and high bytes";
    //       restored, green.
    QByteArray afterNewline = edidNamed("AB");
    afterNewline.replace(54 + 18 + 5, 13, QByteArray("AB\nXYZ       ", 13));
    expectName(afterNewline, QStringLiteral("AB"), "text after the newline");
    QByteArray dirty = edidNamed("x");
    dirty.replace(54 + 18 + 5, 13, QByteArray("M2\x01" "7\xc3\xa9Q\x7f\n     ", 13));
    expectName(dirty, QStringLiteral("M27Q"), "control and high bytes");
}

// S15.22
void mirrors() {
    // DP-1 (id 1) is the source; DP-2 (id 2) carries the replicationSource under test.
    const auto withSource = [](const QVariant& source) {
        return parseKScreenConfig(configOf({output({.name = QStringLiteral("DP-1"), .id = 1}),
                                            output({.name = QStringLiteral("DP-2"), .id = 2, .replicationSource = source})}));
    };

    // GOAL: S15.22 -- a replicationSource naming another output, as an integer or as a double,
    //       makes this output a replica of that output's connector name, and not selectable; the
    //       source itself stays a selectable screen.
    // WHY-THIS-SHAPE: mirrorOf is compared with the connector name, not the id, since the
    //       picker and Apply read the name; the double case exists because the fixture only
    //       holds integers and number() is shared with every other field.
    // CONTROL: app/kde/kscreen_config.cpp, `mirrorOf = it->outputName` made `QString::number(id)`
    //       -> red on both cases, mirrorOf="1"; the replicationSource read refusing a Double -> red
    //       on "as a double" only; app/kde/kscreen_config.h, `&& mirrorOf.isEmpty()` removed from
    //       selectable() -> red, "selectable=1"; restored, green.
    for (const auto& [what, source] : {std::pair{"as an integer", QVariant(qlonglong(1))}, std::pair{"as a double", QVariant(1.0)}}) {
        const auto parsed = withSource(source);
        if (!parsed) {
            expect(false, QStringLiteral("S15.22: replicationSource 1 %1 was refused: %2").arg(QLatin1String(what), parsed.error()));
            continue;
        }
        const Monitor* src = byName(*parsed, QStringLiteral("DP-1"));
        const Monitor* copy = byName(*parsed, QStringLiteral("DP-2"));
        expect(copy && copy->mirrorOf == QStringLiteral("DP-1") && !copy->selectable(),
               QStringLiteral("S15.22: replicationSource 1 %1 should make DP-2 an unselectable replica of DP-1; got mirrorOf=\"%2\" selectable=%3")
                   .arg(QLatin1String(what), copy ? copy->mirrorOf : QStringLiteral("(none)"))
                   .arg(copy && copy->selectable()));
        expect(src && src->mirrorOf.isEmpty() && src->selectable(),
               QStringLiteral("S15.22: DP-1, the source, should stay a selectable screen (%1)").arg(QLatin1String(what)));
    }

    // GOAL: S15.22 -- no replicationSource field, or 0, means the output is not a replica.
    // WHY-THIS-SHAPE: a KScreen older than mirroring sends no field at all, and 0 is what every
    //       other output sends (the real fixture has 0 on both), so each must read as "none".
    // CONTROL: app/kde/kscreen_config.cpp, the absent-field default `std::optional<double>(0)`
    //       made `std::nullopt` -> red on "absent" (and on every hand-built reply, none of which
    //       has the field); `if (id == 0) continue;` removed -> red on "0" (and on the real
    //       reply, which sends 0); restored, green.
    for (const auto& [what, source] : {std::pair{"absent", QVariant()}, std::pair{"0", QVariant(qlonglong(0))}}) {
        const auto parsed = withSource(source);
        const Monitor* copy = parsed ? byName(*parsed, QStringLiteral("DP-2")) : nullptr;
        expect(copy && copy->mirrorOf.isEmpty() && copy->selectable(),
               QStringLiteral("S15.22: replicationSource %1 should leave DP-2 a selectable screen; got %2")
                   .arg(QLatin1String(what), parsed ? QStringLiteral("mirrorOf=\"%1\"").arg(copy ? copy->mirrorOf : QString())
                                                    : QStringLiteral("refused: ") + parsed.error()));
    }

    // GOAL: S15.22 -- a replicationSource the reply cannot resolve to another output (no such id,
    //       the output itself) or cannot read as a number refuses the whole reply with a sentence
    //       naming the output; never a list. (Two outputs sharing an id is refused earlier: S15.25.)
    // WHY-THIS-SHAPE: each case beside a valid source, so a parser that dropped the bad field and
    //       read the output as a plain screen returns a list and goes red; the sentence is
    //       matched per case so each proves its own branch.
    // CONTROL: app/kde/kscreen_config.cpp, one run each: an unknown source skipped (`if (it ==
    //       monitors.end()) continue;` before the refusal) -> red on "no such id"; `it ==
    //       monitors.end() ||` kept and `|| it == monitors.begin() + i` removed -> red on "itself";
    //       a non-numeric value read as 0 -> red on "text"; restored, green.
    struct Refusal {
        const char* what;
        QVariantMap config;
        const char* reasonMentions;
    };
    const QVariantMap dp1 = output({.name = QStringLiteral("DP-1"), .id = 1});
    const Refusal refusals[] = {
        {"no such id", configOf({dp1, output({.name = QStringLiteral("DP-2"), .id = 2, .replicationSource = qlonglong(9)})}),
         "DP-2 mirrors a screen, but not which one"},
        {"itself", configOf({dp1, output({.name = QStringLiteral("DP-2"), .id = 2, .replicationSource = qlonglong(2)})}),
         "DP-2 mirrors a screen, but not which one"},
        {"text", configOf({dp1, output({.name = QStringLiteral("DP-2"), .id = 2, .replicationSource = QStringLiteral("1")})}),
         "did not say whether DP-2 mirrors another screen"},
    };
    for (const Refusal& r : refusals) {
        const auto parsed = parseKScreenConfig(r.config);
        if (parsed) {
            expect(false, QStringLiteral("S15.22: a replicationSource naming %1 was accepted with %2 monitor(s)")
                              .arg(QLatin1String(r.what))
                              .arg(parsed->size()));
            continue;
        }
        expect(parsed.error().contains(QLatin1String(r.reasonMentions)),
               QStringLiteral("S15.22: a replicationSource naming %1 was refused, but the reason \"%2\" does not say \"%3\"")
                   .arg(QLatin1String(r.what), parsed.error(), QLatin1String(r.reasonMentions)));
    }
}

// S15.25
void ids_wholeAndUnique() {
    const QVariantMap dp1 = output({.name = QStringLiteral("DP-1"), .id = 1});
    const auto dp2WithId = [](const QVariant& id) {
        QVariantMap dp2 = output({.name = QStringLiteral("DP-2"), .x = qlonglong(1920)});
        dp2.insert(QStringLiteral("id"), id);
        return dp2;
    };
    const char* notWhole = "KScreen gave DP-2 an id that is not a whole number";

    // GOAL: S15.25 -- an id that is present must be a whole number from 0 to INT_MAX, and no two
    //       outputs may share one, or the whole reply is refused with a sentence naming the output
    //       (both outputs, for a shared id): the id alone routes a getEdid answer to a screen.
    // WHY-THIS-SHAPE: each case beside a valid DP-1, so a parser that skipped the bad output or
    //       truncated its id returns a list and goes red; the sentence is matched per case. The
    //       mirror case is the one S15.22 used to refuse as an unresolvable source: it must now
    //       fail on the shared id itself, before any source is looked up.
    // CONTROL: app/kde/kscreen_config.cpp, one run each: the shared-id `return std::unexpected`
    //       made a no-op -> red, "two outputs with id 1 ... was accepted"; `*value !=
    //       std::trunc(*value)` removed -> red on "2.5"; `*value < 0 ||` removed -> red on "-1";
    //       `number(id)` made `std::optional<double>(id.toDouble())` -> red on "text"; `*value >
    //       std::numeric_limits<int>::max() ||` removed -> red on "2^31"; restored, green.
    struct Refusal {
        const char* what;
        QVariantMap config;
        const char* reasonMentions;
    };
    const Refusal refusals[] = {
        {"two outputs with id 1", configOf({dp1, dp2WithId(qlonglong(2)), output({.name = QStringLiteral("DP-3"), .id = 1, .x = 3840})}),
         "KScreen gave DP-1 and DP-3 the same id"},
        {"a replica whose source id two outputs share",
         configOf({dp1, output({.name = QStringLiteral("DP-2"), .id = 2, .replicationSource = qlonglong(1)}),
                   output({.name = QStringLiteral("DP-3"), .id = 1, .x = 3840})}),
         "KScreen gave DP-1 and DP-3 the same id"},
        // 2.5, not 1.5: truncated, 1.5 would collide with DP-1 and be refused as a shared id.
        {"id 2.5", configOf({dp1, dp2WithId(2.5)}), notWhole},
        {"id -1", configOf({dp1, dp2WithId(qlonglong(-1))}), notWhole},
        {"id text", configOf({dp1, dp2WithId(QStringLiteral("2"))}), notWhole},
        {"id 2^31", configOf({dp1, dp2WithId(qlonglong(1) << 31)}), notWhole},
    };
    for (const Refusal& r : refusals) {
        const auto parsed = parseKScreenConfig(r.config);
        if (parsed) {
            expect(false, QStringLiteral("S15.25: a reply with %1 was accepted with %2 monitor(s)").arg(QLatin1String(r.what)).arg(parsed->size()));
            continue;
        }
        expect(parsed.error().contains(QLatin1String(r.reasonMentions)),
               QStringLiteral("S15.25: a reply with %1 was refused, but the reason \"%2\" does not say \"%3\"")
                   .arg(QLatin1String(r.what), parsed.error(), QLatin1String(r.reasonMentions)));
    }

    // GOAL: S15.25 -- an output with no id is listed with kscreenId -1, and two such outputs are
    //       not a shared id.
    // CONTROL: app/kde/kscreen_config.cpp, the id block's `id.isValid()` made `true`, so a missing
    //       id is read as a number -> red, "an output without an id was refused"; restored, green.
    QVariantMap noId1 = output({.name = QStringLiteral("DP-2"), .x = qlonglong(1920)});
    QVariantMap noId2 = output({.name = QStringLiteral("DP-3"), .x = qlonglong(3840)});
    noId1.remove(QStringLiteral("id"));
    noId2.remove(QStringLiteral("id"));
    const auto missing = parseKScreenConfig(configOf({dp1, noId1, noId2}));
    expect(missing.has_value(), QStringLiteral("S15.25: an output without an id was refused: ") + missing.error_or(QString()));
    if (missing) {
        const Monitor* a = byName(*missing, QStringLiteral("DP-1"));
        const Monitor* b = byName(*missing, QStringLiteral("DP-2"));
        const Monitor* c = byName(*missing, QStringLiteral("DP-3"));
        expect(a && a->kscreenId == 1 && b && b->kscreenId == -1 && c && c->kscreenId == -1,
               QStringLiteral("S15.25: DP-1 should keep id 1 and the two outputs without one read -1; got %1, %2, %3")
                   .arg(a ? describe(*a) : QStringLiteral("no DP-1"), b ? describe(*b) : QStringLiteral("no DP-2"),
                        c ? describe(*c) : QStringLiteral("no DP-3")));
    }
}

// S15.21
void internalFieldsHiddenFromQml() {
    // GOAL: S15.21 -- KScreen's own output id and the panel description are not properties QML
    //       can read: the id is valid only for one backend instance, and nothing in QML may key on
    //       it.
    // WHY-THIS-SHAPE: asked of the meta-object QML reads; a lookup of two properties QML must see
    //       rules out a meta-object where every lookup fails.
    // CONTROL: app/kde/kscreen_config.h, `Q_PROPERTY(int kscreenId MEMBER kscreenId CONSTANT)`
    //       added -> red on kscreenId; `Q_PROPERTY(QString panel MEMBER panel CONSTANT)` added ->
    //       red on panel; restored, green.
    const QMetaObject& meta = Monitor::staticMetaObject;
    expect(meta.indexOfProperty("outputName") >= 0 && meta.indexOfProperty("mirrorOf") >= 0,
           QStringLiteral("S15.21: Monitor's meta-object does not list outputName and mirrorOf, so the lookups below prove nothing"));
    expect(meta.indexOfProperty("kscreenId") < 0, QStringLiteral("S15.21: kscreenId is a property QML can read"));
    expect(meta.indexOfProperty("panel") < 0, QStringLiteral("S15.21: panel is a property QML can read"));
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    wallpaperx::testing::g_testName = "kscreen_config";

    realFixture_parsesByConnectorName();
    numbers_intOrDouble_andFractionalScale();
    untrustworthyReply_isRefusedWhole();
    selectable_needsEnabledConnectedAndSize();
    rotationFlags();
    edidNames();
    mirrors();
    ids_wholeAndUnique();
    internalFieldsHiddenFromQml();

    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file, not in app/kde/kscreen_config.cpp");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
