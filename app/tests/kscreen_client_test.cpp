// SPDX-License-Identifier: GPL-2.0-or-later

// KScreenClient (app/kde/kscreen_client.cpp) against a fake KScreen on a private bus. Goals
// S15.6-S15.12, S15.14, S15.16, S15.18-S15.20, S15.23, S15.25, S15.26 and S15.28 in GOALS.md.
//
// Every phase stands up its own fake under its own name, never org.kde.KScreen, on a bus that
// can start nothing (renderer/tests/support/isolated_session.conf; refuseUnlessPrivateBus proves both). The
// barrier used after a reply is released is the client's count of calls in flight: each call it
// makes is a QDBusPendingCallWatcher child of it, deleted once its reply has been handled, so when
// only the calls the fake still holds remain, every reply sent so far has been acted on.

#include "fake_kscreen.h"
#include "kscreen_client.h"
#include "kscreen_config.h"
#include "kscreen_fixtures.h"
#include "tests/test_report.h"

#include <QCoreApplication>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QElapsedTimer>

#include <memory>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;
using State = KScreenClient::State;

namespace {

constexpr int kWaitMs = 5000;

// The real reply, read once by main() before any phase runs; main() stops on a fixture it cannot read.
QVariantMap g_fixture;

void expect(bool cond, const QString& msg) {
    check(cond, msg.toStdString().c_str());
}

QString names(const QList<Monitor>& monitors) {
    QStringList out;
    for (const Monitor& m : monitors) {
        out.append(m.outputName + QLatin1Char('/') + m.displayName);
    }
    return QLatin1Char('[') + out.join(QStringLiteral(", ")) + QLatin1Char(']');
}

QString summary(const KScreenClient& c) {
    return QStringLiteral("state %1, reason \"%2\", monitors %3, selected \"%4\"")
        .arg(stateName(c.state()), c.unavailableReason(), names(c.monitors()), c.selectedOutput());
}

const Monitor* byName(const QList<Monitor>& monitors, const QString& name) {
    for (const Monitor& m : monitors) {
        if (m.outputName == name) {
            return &m;
        }
    }
    return nullptr;
}

qsizetype inFlight(const KScreenClient& c) {
    return c.findChildren<QDBusPendingCallWatcher*>().size();
}

// A fake and a client pointed at it, or a named reason the phase could not start.
struct Rig {
    std::unique_ptr<FakeKScreen> fake;
    std::unique_ptr<KScreenClient> client;

    static std::optional<Rig> make(const char* phase, const char* goals, KScreenTimeouts timeouts = {}) {
        Rig rig;
        rig.fake = std::make_unique<FakeKScreen>(QStringLiteral("io.wallpaperx.test.FakeKScreen.") + QLatin1String(phase));
        if (const QString fault = rig.fake->start(); !fault.isEmpty()) {
            notVerified(goals, std::string(phase) + ": the fake KScreen could not stand up: " + fault.toStdString());
            return std::nullopt;
        }
        rig.client = std::make_unique<KScreenClient>(dbusSource(rig.fake->service(), timeouts));
        rig.releaseWithinMs = std::min(timeouts.backendMs, timeouts.callMs) * 3 / 5;
        return rig;
    }

    ~Rig() {
        client.reset(); // before the fake, so no held call is answered to a dead client
    }
    Rig() = default;
    Rig(Rig&&) = default;
    Rig& operator=(Rig&&) = default;

    // Refreshes and waits until the answer has been handled: out of Loading, and nothing in flight
    // but the `held` calls the fake is keeping. Waiting only for "not Loading" would return at once
    // from a client that was already Ready, and grade the previous answer.
    bool refreshAndSettle(const char* phase, int timeoutMs = kWaitMs, qsizetype held = 0) {
        client->refresh();
        const bool answered =
            waitFor([&] { return client->state() != State::Loading && inFlight(*client) == held; }, timeoutMs);
        expect(answered, QStringLiteral("%1: the refresh was not answered within %2 ms (%3, %4 call(s) in flight, expected %5)")
                             .arg(QLatin1String(phase))
                             .arg(timeoutMs)
                             .arg(stateName(client->state()))
                             .arg(inFlight(*client))
                             .arg(held));
        return answered;
    }
    bool settle(const char* phase, int timeoutMs = kWaitMs) {
        const bool answered = waitFor([&] { return client->state() != State::Loading; }, timeoutMs);
        expect(answered, QStringLiteral("%1: still Loading after %2 ms").arg(QLatin1String(phase)).arg(timeoutMs));
        return answered;
    }
    // Waits until exactly `held` calls are in flight: the ones the fake is deliberately holding.
    bool quiesce(const char* phase, qsizetype held = 0) {
        const bool done = waitFor([&] { return inFlight(*client) == held; }, kWaitMs);
        expect(done, QStringLiteral("%1: %2 call(s) in flight after %3 ms, expected %4")
                         .arg(QLatin1String(phase))
                         .arg(inFlight(*client))
                         .arg(kWaitMs)
                         .arg(held));
        return done;
    }

    // A held call times out on the client's side, and a reply released after that is dropped by
    // Qt whatever the client would have done with it: a check that released one late would pass
    // on the strength of the timeout. So a release must come well inside the shorter of this
    // client's timeouts, or inside `boundMs` for a phase that holds only the longer call.
    QElapsedTimer sinceHeld;
    int releaseWithinMs = 0;
    bool inTime(const char* goals, int boundMs = 0) {
        const int bound = boundMs > 0 ? boundMs : releaseWithinMs;
        if (sinceHeld.isValid() && sinceHeld.elapsed() < bound) {
            return true;
        }
        notVerified(goals, "a held call could not be released within " + std::to_string(bound) +
                               " ms of being sent, so the client may already have timed it out");
        return false;
    }
    // Every call the fake saw came from the client's connection.
    void expectOnlyClientCalled(const char* phase) const {
        const QSet<QString> want{QDBusConnection::sessionBus().baseService()};
        expect(fake->callers == want, QStringLiteral("%1: the fake was called by %2, expected only the client (%3)")
                                          .arg(QLatin1String(phase), QStringList(fake->callers.values()).join(u','),
                                               QDBusConnection::sessionBus().baseService()));
    }
};

struct Emissions {
    int state = 0;
    int monitors = 0;
    int selection = 0;
};

void count(KScreenClient& c, Emissions& e) {
    QObject::connect(&c, &KScreenClient::stateChanged, [&e] { ++e.state; });
    QObject::connect(&c, &KScreenClient::monitorsChanged, [&e] { ++e.monitors; });
    QObject::connect(&c, &KScreenClient::selectionChanged, [&e] { ++e.selection; });
}

QVariantMap screen(const QString& name, qlonglong id, qlonglong x, bool enabled = true) {
    return output({.name = name, .id = id, .x = x, .enabled = enabled});
}

// S15.6
void wireRoundTrip() {
    auto rig = Rig::make("roundtrip", "S15.6");
    if (!rig) {
        return;
    }
    // DP-4 at 125%, so a double travels as `d` beside the fixture's `x` integers.
    QVariantList outputs = g_fixture.value(QStringLiteral("outputs")).toList();
    QVariantMap dp4 = outputs[0].toMap();
    dp4.insert(QStringLiteral("scale"), 1.25);
    outputs[0] = dp4;
    const QVariantMap config = withOutputs(g_fixture, outputs);
    rig->fake->config = config;
    // getEdid answers an empty block (as KScreen does for an id it does not know), so names stay
    // the connector's, as the JSON path has them.
    rig->fake->edidMode = FakeKScreen::Mode::Answer;

    // GOAL: S15.6 -- a reply that crossed the bus as nested a{sv}/av parses to exactly what the
    //       same map parses to without the bus: plainVariant unpacks every level.
    // WHY-THIS-SHAPE: compared against parseKScreenConfig on the JSON-shaped map, not against
    //       hand-written values, so every field KScreen sends is covered; the count check keeps an
    //       empty list on both sides from comparing equal.
    // CONTROL: app/kde/kscreen_config.cpp, plainVariant's QDBusVariant unwrap removed -> red,
    //       Unavailable "KScreen's reply has no list of outputs."; the ArrayType branch returning
    //       an empty list -> red, "differs"; restored, green.
    if (!rig->refreshAndSettle("S15.6")) {
        return;
    }
    const auto viaJson = parseKScreenConfig(config);
    expect(viaJson.has_value() && viaJson->size() == 2, QStringLiteral("S15.6: the JSON path did not give two monitors"));
    expect(rig->client->state() == State::Ready,
           QStringLiteral("S15.6: the fixture over the bus did not reach Ready: ") + summary(*rig->client));
    if (viaJson) {
        expect(rig->client->monitors() == *viaJson,
               QStringLiteral("S15.6: the monitors read over the bus %1 differ from the JSON path's %2")
                   .arg(names(rig->client->monitors()), names(*viaJson)));
        const Monitor* wireDp4 = byName(rig->client->monitors(), QStringLiteral("DP-4"));
        expect(wireDp4 && wireDp4->scale == 1.25 && wireDp4->geometry.width() == 2048,
               QStringLiteral("S15.6: DP-4's 1.25 scale did not survive the bus"));
    }

    // GOAL: S15.14 -- the answer came from the fake: one getConfig from the client's connection.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, requestConfig called twice per read -> red, "the fake
    //       saw 2 getConfig call(s)"; restored, green (re-run via control_batch.sh 2026-10-26).
    expect(rig->fake->configCalls == 1, QStringLiteral("S15.14: the fake saw %1 getConfig call(s), expected 1").arg(rig->fake->configCalls));
    rig->expectOnlyClientCalled("S15.14");

    // GOAL: S15.7 -- the backend is requested with an empty name and empty arguments, which lets
    //       KScreen's launcher pick by its own platform; a file name ("KSC_KWayland") makes it
    //       answer false.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, requestBackend's name set to "KSC_KWayland" -> red,
    //       "requestBackend was asked for (KSC_KWayland, ...)"; restored, green.
    const QVariantList args = rig->fake->backendArguments.value(0);
    expect(args.size() == 2 && args[0].toString().isEmpty() && args[0].metaType().id() == QMetaType::QString,
           QStringLiteral("S15.7: requestBackend was asked for (%1, ...), expected an empty name")
               .arg(args.value(0).toString()));
}

// Checks an Unavailable client reports nothing: no monitor, no selection, and select() refusing.
// A choice may still be remembered behind the accessors (S15.16); what is read is what counts.
void expectUnavailable(Rig& rig, const char* phase, const QString& reasonMentions) {
    KScreenClient& c = *rig.client;
    expect(c.state() == State::Unavailable, QStringLiteral("%1: expected Unavailable; ").arg(QLatin1String(phase)) + summary(c));
    expect(c.unavailableReason().contains(reasonMentions),
           QStringLiteral("%1: the reason should say \"%2\"; ").arg(QLatin1String(phase), reasonMentions) + summary(c));
    expect(c.monitors().isEmpty() && c.selectedOutput().isEmpty() && c.selectedMonitor().outputName.isEmpty(),
           QStringLiteral("%1: Unavailable must report no monitor and no selection; ").arg(QLatin1String(phase)) + summary(c));
    expect(!c.select(QStringLiteral("DP-3")) && !c.select(QStringLiteral("DP-4")) && c.selectedOutput().isEmpty(),
           QStringLiteral("%1: select() succeeded while Unavailable").arg(QLatin1String(phase)));
}

// S15.7
void unavailableStates() {
    // GOAL: S15.7 -- a service that is not on the bus reads Unavailable, saying KScreen is not running.
    // WHY-THIS-SHAPE: a name nobody owns on a bus that can activate nothing; the bus is asked, so
    //       the premise is proven rather than assumed.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, describe()'s `if (isUnreachable(error))` made `if (false)`
    //       (control_batch, 2026-10-26) -> red, "S15.7 absent: the reason should say "is not running";
    //       ... reason "KScreen refused the request: The name ... was not provided by any .service files"".
    {
        const QString nobody = QStringLiteral("io.wallpaperx.test.NobodyHome");
        if (QDBusConnection::sessionBus().interface()->isServiceRegistered(nobody).value()) {
            notVerified("S15.7 (absent service)", "the name meant to be absent is owned");
        } else {
            Rig rig;
            rig.client = std::make_unique<KScreenClient>(dbusSource(nobody));
            if (rig.refreshAndSettle("S15.7 absent")) {
                expectUnavailable(rig, "S15.7 absent", QStringLiteral("is not running"));
            }
        }
    }

    // GOAL: S15.7 -- a launcher that answers requestBackend with false reads Unavailable, and the
    //       client does not go on to read a configuration.
    // WHY-THIS-SHAPE: the fake still serves a valid configuration, so a client that ignored the
    //       false would reach Ready here rather than fail somewhere else.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, `|| !args.front().toBool()` removed -> red, Ready with
    //       DP-3 and DP-4; restored, green.
    if (auto rig = Rig::make("backendfalse", "S15.7 (backend refused)")) {
        rig->fake->backendAnswer = false;
        rig->fake->config = g_fixture;
        if (rig->refreshAndSettle("S15.7 backend false")) {
            expectUnavailable(*rig, "S15.7 backend false", QStringLiteral("could not load a display backend"));
            expect(rig->fake->configCalls == 0, QStringLiteral("S15.7 backend false: getConfig was called anyway"));
            expect(rig->fake->backendCalls == 1, QStringLiteral("S15.7 backend false: the fake saw no requestBackend"));
        }
    }

    // GOAL: S15.7 -- a getConfig error reads Unavailable with KScreen's own message.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, requestConfig's ErrorMessage branch removed -> red, the
    //       reason became "KScreen answered with something other than a configuration."; restored.
    if (auto rig = Rig::make("configerror", "S15.7 (getConfig error)")) {
        rig->fake->configMode = FakeKScreen::Mode::Error;
        rig->fake->configError = QStringLiteral("backend exploded");
        if (rig->refreshAndSettle("S15.7 getConfig error")) {
            expectUnavailable(*rig, "S15.7 getConfig error", QStringLiteral("KScreen refused the request: backend exploded"));
        }
    }

    // GOAL: S15.261 -- a NameHasNoOwner error from KScreen reads as KScreen not running, not as a
    //       refusal.
    // WHY-THIS-SHAPE: the private bus answers an absent name with ServiceUnknown (the case above),
    //       so the fake sends NameHasNoOwner itself; the reason is compared whole.
    // CONTROL: app/kde/dbus_reply.h, `|| error.name() == u"org.freedesktop.DBus.Error.NameHasNoOwner"`
    //       deleted from isUnreachable (control_batch, 2026-10-04) -> red, "S15.261: KScreen's
    //       NameHasNoOwner reads "KScreen refused the request: gone"".
    if (auto rig = Rig::make("noowner", "S15.261 (KScreen)")) {
        rig->fake->configMode = FakeKScreen::Mode::Error;
        rig->fake->errorName = QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner");
        rig->fake->configError = QStringLiteral("gone");
        if (rig->refreshAndSettle("S15.261")) {
            const QString want = QStringLiteral("KDE's display configuration service (KScreen) is not running.");
            expect(rig->client->state() == State::Unavailable && rig->client->unavailableReason() == want,
                   QStringLiteral("S15.261: KScreen's NameHasNoOwner reads \"%1\"").arg(rig->client->unavailableReason()));
        }
    }

    // GOAL: S15.262 -- KScreen's own message in a refusal is cut at 300 characters with "…", so a
    //       long one cannot fill the window.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, describe()'s `clamped(error.message())` made
    //       `error.message()` (control_batch, 2026-10-26) -> red, "S15.262: a 400-character refusal
    //       reads as 429 characters".
    if (auto rig = Rig::make("longerror", "S15.262 (KScreen)")) {
        rig->fake->configMode = FakeKScreen::Mode::Error;
        rig->fake->configError = QString(400, u'e');
        if (rig->refreshAndSettle("S15.262")) {
            const QString want = QStringLiteral("KScreen refused the request: ") + QString(300, u'e') + QStringLiteral("…");
            expect(rig->client->state() == State::Unavailable && rig->client->unavailableReason() == want,
                   QStringLiteral("S15.262: a 400-character refusal reads as %1 characters").arg(rig->client->unavailableReason().size()));
        }
    }

    // GOAL: S15.7, S15.20 -- a getConfig that is never answered ends in Unavailable within the
    //       client's call timeout, not in Loading forever, and not after the longer backend bound.
    // WHY-THIS-SHAPE: the wait is bounded strictly between the injected call and backend timeouts
    //       (static_assert), so reaching the bound is itself the failure and the check waits
    //       milliseconds; the fake proves it held the call, so the client's silence is not
    //       mistaken for a client that never asked.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, requestConfig's asyncCall given `m_timeouts.backendMs`
    //       -> red, "the refresh was not answered within 2000 ms (Loading, 1 call(s) in flight...)"
    //       (re-run via control_batch.sh 2026-10-26); given a literal 60000 (the injected value
    //       ignored) -> the same red (re-run via control_batch.sh 2026-10-26); restored, green.
    static constexpr KScreenTimeouts hang{.backendMs = 5000, .callMs = 200};
    static constexpr int hangWaitMs = hang.backendMs * 2 / 5;
    static_assert(hang.callMs < hangWaitMs && hangWaitMs < hang.backendMs);
    if (auto rig = Rig::make("confighang", "S15.7 (getConfig never answers)", hang)) {
        rig->fake->configMode = FakeKScreen::Mode::Hold;
        if (rig->refreshAndSettle("S15.7 getConfig never answers", hangWaitMs)) {
            expectUnavailable(*rig, "S15.7 getConfig never answers", QStringLiteral("did not answer"));
        }
        expect(rig->fake->heldConfig.size() == 1,
               QStringLiteral("S15.7 getConfig never answers: the fake held %1 getConfig call(s), expected 1")
                   .arg(rig->fake->heldConfig.size()));
    }

    // GOAL: S15.7 -- a malformed reply reads Unavailable with the parser's sentence, and a reply
    //       that is not a map at all is refused by the client.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, a refused parse answered as no monitors -> red,
    //       "S15.7 malformed: expected Unavailable; state Ready"; the not-a-map branch removed -> red, the reason became
    //       "KScreen's reply has no list of outputs."; restored, green.
    if (auto rig = Rig::make("malformed", "S15.7 (malformed reply)")) {
        rig->fake->config = configOf({screen(QStringLiteral("DP-3"), 1, 0), screen(QStringLiteral("DP-3"), 2, 1920)});
        if (rig->refreshAndSettle("S15.7 malformed")) {
            expectUnavailable(*rig, "S15.7 malformed", QStringLiteral("two outputs named DP-3"));
        }
        rig->fake->config = QStringLiteral("not a configuration");
        if (rig->refreshAndSettle("S15.7 not a map")) {
            expectUnavailable(*rig, "S15.7 not a map", QStringLiteral("something other than a configuration"));
        }
        rig->expectOnlyClientCalled("S15.14 malformed");
    }
}

// S15.8
void selection() {
    const QVariantMap dp3 = screen(QStringLiteral("DP-3"), 2, 0);
    const QVariantMap dp4 = screen(QStringLiteral("DP-4"), 1, 1920);
    const QVariantMap hdmiOff = screen(QStringLiteral("HDMI-A-1"), 3, 3840, false);

    // GOAL: S15.8 -- select() of a screen that is absent or not selectable returns false and
    //       changes nothing; with two selectable screens nothing is chosen for the user.
    // CONTROL: app/kde/kscreen_client.cpp, `|| !it->selectable()` removed from select() -> red,
    //       "select(HDMI-A-1), a disabled screen, returned true"; settled()'s auto-select
    //       `count_if(...) == 1` made `>= 1` -> red, "chose DP-3 on its own" (re-run via
    //       control_batch.sh 2026-10-05); restored, green.
    auto rig = Rig::make("selection", "S15.8");
    if (!rig) {
        return;
    }
    KScreenClient& c = *rig->client;
    Emissions e;
    count(c, e);
    rig->fake->config = configOf({dp3, dp4, hdmiOff});
    if (!rig->refreshAndSettle("S15.8") || !rig->quiesce("S15.8")) {
        return;
    }
    expect(c.state() == State::Ready && c.monitors().size() == 3, QStringLiteral("S15.8: not Ready with three screens: ") + summary(c));
    expect(c.selectedOutput().isEmpty(), QStringLiteral("S15.8: with two selectable screens the client chose %1 on its own").arg(c.selectedOutput()));
    const int before = e.selection;
    expect(!c.select(QStringLiteral("DP-9")), QStringLiteral("S15.8: select(DP-9), a screen KScreen never listed, returned true"));
    expect(!c.select(QStringLiteral("HDMI-A-1")), QStringLiteral("S15.8: select(HDMI-A-1), a disabled screen, returned true"));
    expect(!c.select(QString()), QStringLiteral("S15.8: select(\"\") returned true"));
    expect(c.selectedOutput().isEmpty() && e.selection == before,
           QStringLiteral("S15.8: a refused select() changed the selection to \"%1\" or signalled").arg(c.selectedOutput()));
    expect(c.select(QStringLiteral("DP-3")) && c.selectedOutput() == QStringLiteral("DP-3") &&
               c.selectedMonitor().outputName == QStringLiteral("DP-3") && e.selection == before + 1,
           QStringLiteral("S15.8: select(DP-3) did not select it with one selectionChanged; ") + summary(c));

    // GOAL: S15.8 -- a chosen screen survives a re-read that still lists it.
    // CONTROL: app/kde/kscreen_client.cpp, settled()'s `if (selectedMonitor().outputName.isEmpty())`
    //       made `if (true)`, so every Ready re-read clears the choice -> red, "a re-read listing DP-3
    //       lost the selection" (with S15.16/18/19/334 lines); control_batch.sh 2026-10-14; restored.
    const int reads = rig->fake->configCalls;
    rig->fake->emitConfigChanged();
    if (!waitFor([&] { return rig->fake->configCalls > reads; }, kWaitMs) || !rig->quiesce("S15.8 re-read")) {
        expect(false, QStringLiteral("S15.8: configChanged led to no re-read"));
        return;
    }
    expect(c.selectedOutput() == QStringLiteral("DP-3"), QStringLiteral("S15.8: a re-read listing DP-3 lost the selection; ") + summary(c));

    // GOAL: S15.8 -- when the chosen screen disappears the selection empties; it never moves to
    //       the one remaining screen, even though a fresh client would choose that screen itself.
    // WHY-THIS-SHAPE: exactly one selectable screen remains, which is the auto-select condition,
    //       so this is the case where a move is most tempting and hardest to see.
    // CONTROL: app/kde/kscreen_client.cpp, settled()'s `} else if (std::ranges::count_if` made
    //       `} if ((!m_selection || m_selection->isEmpty()) && std::ranges::count_if`, so auto-select
    //       also runs from the lost state -> red, "the selection moved to DP-4"; re-run via
    //       control_batch.sh 2026-10-05; restored, green.
    rig->fake->config = configOf({dp4, hdmiOff});
    const int reads2 = rig->fake->configCalls;
    rig->fake->emitConfigChanged();
    if (!waitFor([&] { return rig->fake->configCalls > reads2; }, kWaitMs) || !rig->quiesce("S15.8 unplug")) {
        expect(false, QStringLiteral("S15.8: configChanged led to no re-read after DP-3 went away"));
        return;
    }
    expect(c.monitors().size() == 2 && byName(c.monitors(), QStringLiteral("DP-3")) == nullptr,
           QStringLiteral("S15.8: the re-read did not drop DP-3; ") + summary(c));
    expect(c.selectedOutput().isEmpty() && c.selectedMonitor().outputName.isEmpty(),
           QStringLiteral("S15.8: DP-3 went away and the selection moved to %1").arg(c.selectedOutput()));

    // GOAL: S15.8 -- the selection does not move one re-read later either: after the chosen screen
    //       went away, any further configChanged (a mode change, a second hotplug) leaves the
    //       selection empty until the user picks.
    // WHY-THIS-SHAPE: the check above sees only the re-read in which DP-3 vanished; a guard that
    //       compares with the previous selection alone is empty-handed on the next re-read, where
    //       "nothing selected, one selectable screen" looks the same as a first start.
    // CONTROL: app/kde/kscreen_client.cpp, settled()'s `m_selection->clear()` made
    //       `m_selection.reset()`, so a lost selection reads as never-selected -> red, "moved to
    //       DP-4 one re-read later", while the immediate-move check above stays green; re-run via
    //       control_batch.sh 2026-10-05; restored, green.
    const int reads2b = rig->fake->configCalls;
    rig->fake->emitConfigChanged();
    if (!waitFor([&] { return rig->fake->configCalls > reads2b; }, kWaitMs) || !rig->quiesce("S15.8 second re-read")) {
        expect(false, QStringLiteral("S15.8: configChanged led to no second re-read after DP-3 went away"));
        return;
    }
    expect(c.selectedOutput().isEmpty(),
           QStringLiteral("S15.8: DP-3 went away and the selection moved to %1 one re-read later").arg(c.selectedOutput()));

    // GOAL: S15.16 -- a choice a Ready answer lost stays lost when its screen is listed again:
    //       only the user brings it back.
    // WHY-THIS-SHAPE: two selectable screens are listed, so auto-select cannot be what fills the
    //       selection here; a non-empty selection can only be the lost choice returning.
    // CONTROL: app/kde/kscreen_client.cpp, settled()'s `m_selection->clear();` deleted, so a lost
    //       choice is only hidden -> red, "DP-3 came back", while every S15.8 check stays green;
    //       re-run via control_batch.sh 2026-10-05; restored, green.
    rig->fake->config = configOf({dp3, dp4});
    const int reads3 = rig->fake->configCalls;
    rig->fake->emitConfigChanged();
    if (!waitFor([&] { return rig->fake->configCalls > reads3; }, kWaitMs) || !rig->quiesce("S15.8 replug")) {
        expect(false, QStringLiteral("S15.8: configChanged led to no re-read after replug"));
        return;
    }
    expect(c.selectedOutput().isEmpty(),
           QStringLiteral("S15.16: DP-3 came back after its selection was lost, and was selected again as %1").arg(c.selectedOutput()));

    // GOAL: S15.8 -- a chosen screen that is still listed but no longer selectable (disabled) is
    //       deselected, again without moving to the other screen.
    // CONTROL: app/kde/kscreen_client.cpp, `|| !it->selectable()` removed from selectedMonitor(),
    //       so a listed but disabled choice is still reported -> red, "still selected while
    //       disabled"; restored, green.
    if (!c.select(QStringLiteral("DP-3"))) {
        expect(false, QStringLiteral("S15.8: could not select DP-3 after replug; ") + summary(c));
        return;
    }
    rig->fake->config = configOf({screen(QStringLiteral("DP-3"), 2, 0, false), dp4});
    const int reads4 = rig->fake->configCalls;
    rig->fake->emitConfigChanged();
    if (!waitFor([&] { return rig->fake->configCalls > reads4; }, kWaitMs) || !rig->quiesce("S15.8 disable")) {
        expect(false, QStringLiteral("S15.8: configChanged led to no re-read after disabling DP-3"));
        return;
    }
    expect(c.selectedOutput().isEmpty(),
           QStringLiteral("S15.8: DP-3 was disabled and is %1").arg(c.selectedOutput() == QStringLiteral("DP-3")
                                                                         ? QStringLiteral("still selected while disabled")
                                                                         : QStringLiteral("replaced by ") + c.selectedOutput()));
}

// S15.8, the auto-select half
void autoSelectOnlyScreen() {
    // GOAL: S15.8 -- with nothing chosen and exactly one selectable screen, that screen is selected,
    //       announced by one selectionChanged.
    // WHY-THIS-SHAPE: a disabled screen sits beside it, so "the only screen" and "the only
    //       selectable screen" differ and the check is on the second.
    // CONTROL: app/kde/kscreen_client.cpp, settled()'s auto-select branch made `} else if (false &&
    //       std::ranges::count_if(...) == 1)` -> red, "selected \"\", 0 selectionChanged"; re-run via
    //       control_batch.sh 2026-10-05; restored, green.
    auto rig = Rig::make("autoselect", "S15.8 (auto-select)");
    if (!rig) {
        return;
    }
    Emissions e;
    count(*rig->client, e);
    rig->fake->config = configOf({screen(QStringLiteral("HDMI-A-1"), 3, 0, false), screen(QStringLiteral("DP-4"), 1, 1920)});
    if (!rig->refreshAndSettle("S15.8 auto-select")) {
        return;
    }
    expect(rig->client->selectedOutput() == QStringLiteral("DP-4") && e.selection == 1,
           QStringLiteral("S15.8: one selectable screen should be selected with one signal; %1, %2 selectionChanged")
               .arg(summary(*rig->client))
               .arg(e.selection));
}

// Emits configChanged and waits until the re-read it causes has been handled.
bool reread(Rig& rig, const char* phase, qsizetype held = 0) {
    const int reads = rig.fake->configCalls;
    rig.fake->emitConfigChanged();
    const bool done = waitFor([&] { return rig.fake->configCalls > reads; }, kWaitMs) && rig.quiesce(phase, held);
    expect(done, QStringLiteral("%1: configChanged led to no re-read that was handled").arg(QLatin1String(phase)));
    return done;
}

// S15.16
void choiceSurvivesOutage() {
    const QVariantMap dp3 = screen(QStringLiteral("DP-3"), 2, 0);
    const QVariantMap dp4 = screen(QStringLiteral("DP-4"), 1, 1920);

    // GOAL: S15.16 -- a failed read and the Loading that "Try again" passes through are not
    //       answers: the choice is reported as nothing while they last (one selectionChanged), and
    //       returns, with a selectionChanged, on the next Ready answer that lists it.
    // WHY-THIS-SHAPE: two selectable screens, so auto-select cannot put DP-3 back; only the
    //       remembered choice can. refresh() from Unavailable settles Loading before asking, so
    //       the Loading step is exercised by the same sequence.
    // CONTROL: app/kde/kscreen_client.cpp, settled()'s reconciling `if (m_state == State::Ready) {`
    //       made `m_state != State::Loading`, so an Unavailable read reconciles -> red, "did not come
    //       back"; made `m_state != State::Unavailable`, so the Loading step reconciles -> the
    //       same red; both re-run via control_batch.sh 2026-10-05; restored, green.
    if (auto rig = Rig::make("outage", "S15.16")) {
        KScreenClient& c = *rig->client;
        Emissions e;
        count(c, e);
        rig->fake->config = configOf({dp3, dp4});
        if (!rig->refreshAndSettle("S15.16") || !rig->quiesce("S15.16") || !c.select(QStringLiteral("DP-3"))) {
            expect(false, QStringLiteral("S15.16: could not reach Ready with DP-3 selected; ") + summary(c));
        } else {
            const int chosen = e.selection;
            rig->fake->configMode = FakeKScreen::Mode::Error;
            rig->fake->emitConfigChanged();
            if (!waitFor([&] { return c.state() == State::Unavailable; }, kWaitMs) || !rig->quiesce("S15.16 outage")) {
                expect(false, QStringLiteral("S15.16: a failing re-read did not reach Unavailable; ") + summary(c));
            } else {
                expect(c.selectedOutput().isEmpty() && e.selection == chosen + 1,
                       QStringLiteral("S15.16: Unavailable should report no selection after one selectionChanged; %1, %2 signal(s)")
                           .arg(summary(c))
                           .arg(e.selection - chosen));
                rig->fake->configMode = FakeKScreen::Mode::Answer;
                if (rig->refreshAndSettle("S15.16 try again") && rig->quiesce("S15.16 try again")) {
                    expect(c.state() == State::Ready && c.selectedOutput() == QStringLiteral("DP-3") && e.selection == chosen + 2,
                           QStringLiteral("S15.16: the choice of DP-3 did not come back with one selectionChanged after the outage; %1, %2 signal(s)")
                               .arg(summary(c))
                               .arg(e.selection - chosen));
                }
            }
        }
    }

    // GOAL: S15.16 -- a client that never had a choice still auto-selects the only selectable
    //       screen when its first read failed and a later one succeeds: an outage is not a loss.
    // CONTROL: app/kde/kscreen_client.cpp, settled() recording an Unavailable read as a lost
    //       choice (`if (m_state == State::Unavailable && !m_selection) m_selection.emplace();`)
    //       -> red, "should be auto-selected after the outage"; re-run via control_batch.sh
    //       2026-10-05; restored, green.
    if (auto rig = Rig::make("outagefirst", "S15.16 (never chosen)")) {
        rig->fake->backendAnswer = false;
        rig->fake->config = configOf({screen(QStringLiteral("HDMI-A-1"), 3, 0, false), screen(QStringLiteral("DP-4"), 1, 1920)});
        if (!rig->refreshAndSettle("S15.16 first read fails") || rig->client->state() != State::Unavailable) {
            expect(false, QStringLiteral("S15.16: the first read did not fail as arranged; ") + summary(*rig->client));
        } else {
            rig->fake->backendAnswer = true;
            if (rig->refreshAndSettle("S15.16 second read")) {
                expect(rig->client->selectedOutput() == QStringLiteral("DP-4"),
                       QStringLiteral("S15.16: DP-4, the only selectable screen, should be auto-selected after the outage; ") +
                           summary(*rig->client));
            }
        }
    }
}

// S15.20
void separateTimeouts() {
    const QVariantMap dp4 = screen(QStringLiteral("DP-4"), 1, 0);

    // GOAL: S15.20 -- requestBackend is bounded by the backend timeout, not the call timeout: an
    //       answer that comes after the call timeout but inside the backend timeout is taken.
    // WHY-THIS-SHAPE: the answer is held for four call timeouts, so the client has provably
    //       outlived the call bound, and released inside 3/5 of the backend bound (static_assert
    //       keeps the two apart). The timing is confirmed before the state is graded: a harness
    //       that ran late would otherwise blame the client's correct backend timeout on it.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, read()'s requestBackend asyncCall given
    //       `m_timeouts.callMs` -> red, "gave up on requestBackend at the call timeout, after 1xx ms";
    //       restored, green.
    static constexpr KScreenTimeouts slow{.backendMs = 3000, .callMs = 150};
    static constexpr int slowHoldMs = slow.callMs * 4;
    static constexpr int slowReleaseMs = slow.backendMs * 3 / 5;
    static_assert(slowHoldMs < slowReleaseMs);
    if (auto rig = Rig::make("backendslow", "S15.20 (slow requestBackend)", slow)) {
        rig->fake->backendMode = FakeKScreen::Mode::Hold;
        rig->fake->config = configOf({dp4});
        rig->sinceHeld.start();
        rig->client->refresh();
        if (!waitFor([&] { return rig->fake->heldBackend.size() == 1; }, kWaitMs)) {
            notVerified("S15.20 (slow requestBackend)", "the fake did not receive the requestBackend call");
        } else {
            waitFor([&] { return rig->sinceHeld.elapsed() >= slowHoldMs || rig->client->state() != State::Loading; }, kWaitMs);
            if (rig->inTime("S15.20 (slow requestBackend)", slowReleaseMs)) {
                expect(rig->client->state() == State::Loading,
                       QStringLiteral("S15.20: the client gave up on requestBackend at the call timeout, after %1 ms; ")
                               .arg(rig->sinceHeld.elapsed()) + summary(*rig->client));
                if (rig->client->state() == State::Loading) {
                    rig->fake->reply(rig->fake->heldBackend[0], true);
                    if (rig->settle("S15.20 slow requestBackend") && rig->quiesce("S15.20 slow requestBackend")) {
                        expect(rig->client->state() == State::Ready && rig->client->monitors().size() == 1,
                               QStringLiteral("S15.20: a requestBackend answered inside the backend timeout was not taken; ") +
                                   summary(*rig->client));
                    }
                }
            }
        }
    }

    // GOAL: S15.20 -- a requestBackend that is never answered ends in Unavailable at the backend
    //       timeout, which applies even when it is shorter than the call timeout.
    // WHY-THIS-SHAPE: the wait is bounded between the short injected backend timeout and the long
    //       call timeout (static_assert): only a client that times requestBackend by the backend
    //       bound gets there.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, read()'s requestBackend asyncCall given
    //       `m_timeouts.callMs` -> red, "not answered within 3000 ms"; restored, green.
    static constexpr KScreenTimeouts backendHang{.backendMs = 200, .callMs = 5000};
    static constexpr int backendHangWaitMs = backendHang.callMs * 3 / 5;
    static_assert(backendHang.backendMs < backendHangWaitMs && backendHangWaitMs < backendHang.callMs);
    if (auto rig = Rig::make("backendhang", "S15.20 (requestBackend never answers)", backendHang)) {
        rig->fake->backendMode = FakeKScreen::Mode::Hold;
        if (rig->refreshAndSettle("S15.20 requestBackend never answers", backendHangWaitMs)) {
            expectUnavailable(*rig, "S15.20 requestBackend never answers", QStringLiteral("did not answer"));
        }
        expect(rig->fake->heldBackend.size() == 1,
               QStringLiteral("S15.20 requestBackend never answers: the fake held %1 requestBackend call(s), expected 1")
                   .arg(rig->fake->heldBackend.size()));
    }
}

// S15.23
void mirrorSelection() {
    const QVariantMap dp3 = screen(QStringLiteral("DP-3"), 2, 0);
    const QVariantMap dp4 = screen(QStringLiteral("DP-4"), 1, 1920);
    // DP-3 copying DP-4, at DP-4's position as KScreen reports a replica.
    const QVariantMap dp3Mirror =
        output({.name = QStringLiteral("DP-3"), .id = 2, .x = qlonglong(1920), .replicationSource = qlonglong(1)});

    auto rig = Rig::make("mirrors", "S15.23");
    if (!rig) {
        return;
    }
    KScreenClient& c = *rig->client;
    Emissions e;
    count(c, e);

    // GOAL: S15.23 -- with one real screen and one replica, the real screen is the one auto-selected,
    //       and select() of the replica returns false and changes nothing.
    // WHY-THIS-SHAPE: the replica is listed first and is enabled, connected and sized, so only its
    //       replica status separates it from a second selectable screen, which would block
    //       auto-select altogether.
    // CONTROL: app/kde/kscreen_config.h, `&& mirrorOf.isEmpty()` removed from selectable() -> red,
    //       "should be auto-selected"; app/kde/kscreen_client.cpp, `|| !it->selectable()` removed
    //       from select() -> red, "select(DP-3), a replica"; restored, green.
    rig->fake->config = configOf({dp3Mirror, dp4});
    if (!rig->refreshAndSettle("S15.23") || !rig->quiesce("S15.23")) {
        return;
    }
    expect(c.selectedOutput() == QStringLiteral("DP-4"),
           QStringLiteral("S15.23: DP-4, the only screen that is not a replica, should be auto-selected; ") + summary(c));
    const int before = e.selection;
    const bool accepted = c.select(QStringLiteral("DP-3"));
    expect(!accepted && c.selectedOutput() == QStringLiteral("DP-4") && e.selection == before,
           QStringLiteral("S15.23: select(DP-3), a replica, returned %1 and left %2 with %3 selectionChanged")
               .arg(accepted)
               .arg(summary(c))
               .arg(e.selection - before));

    // GOAL: S15.23 -- a chosen screen that becomes a replica on a Ready answer loses the choice,
    //       which does not move to the remaining screen, and does not come back when mirroring is
    //       switched off.
    // WHY-THIS-SHAPE: after mirroring starts DP-4 is the only selectable screen, the auto-select
    //       condition, so a lost choice treated as "never chosen" would move there.
    // CONTROL: app/kde/kscreen_config.h, `&& mirrorOf.isEmpty()` removed from selectable() -> red,
    //       "still selected as a replica"; app/kde/kscreen_client.cpp, settled()'s
    //       `m_selection->clear();` deleted -> red, "came back when mirroring stopped" (re-run via
    //       control_batch.sh 2026-10-05); restored, green.
    rig->fake->config = configOf({dp3, dp4});
    if (!reread(*rig, "S15.23 two screens") || !c.select(QStringLiteral("DP-3"))) {
        expect(false, QStringLiteral("S15.23: could not select DP-3 with mirroring off; ") + summary(c));
        return;
    }
    rig->fake->config = configOf({dp3Mirror, dp4});
    if (!reread(*rig, "S15.23 mirroring on")) {
        return;
    }
    expect(c.selectedOutput().isEmpty(),
           QStringLiteral("S15.23: DP-3 became a replica of DP-4 and is %1")
               .arg(c.selectedOutput() == QStringLiteral("DP-3") ? QStringLiteral("still selected as a replica")
                                                                  : QStringLiteral("replaced by \"%1\"").arg(c.selectedOutput())));
    rig->fake->config = configOf({dp3, dp4});
    if (reread(*rig, "S15.23 mirroring off")) {
        expect(c.selectedOutput().isEmpty(),
               QStringLiteral("S15.23: the choice of DP-3 came back when mirroring stopped, as \"%1\"").arg(c.selectedOutput()));
    }
}

// S15.9
void staleRepliesDropped() {
    const QVariantMap configA = configOf({screen(QStringLiteral("DP-3"), 2, 0), screen(QStringLiteral("DP-4"), 1, 1920)});
    const QVariantMap configB = configOf({screen(QStringLiteral("DP-4"), 1, 0)});

    // GOAL: S15.9 -- a getConfig reply to an older refresh, arriving after the newer refresh has
    //       been answered, does not replace the newer answer.
    // WHY-THIS-SHAPE: both getConfig calls are held by the fake, so the order the replies arrive
    //       in is the test's choice and not the scheduler's; the "no call in flight" barrier is
    //       what proves the late reply was handled before the check reads the list.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, `if (!isCurrent(generation)) return;` removed from
    //       requestConfig's reply handler -> red, monitors became [DP-3, DP-4]; restored, green.
    if (auto rig = Rig::make("staleconfig", "S15.9 (stale getConfig)")) {
        rig->fake->configMode = FakeKScreen::Mode::Hold;
        rig->fake->edidMode = FakeKScreen::Mode::Answer; // empty EDIDs, answered at once
        // The second refresh only once the first has reached getConfig: issued back to back, the
        // first refresh's requestBackend answer is already stale and it never asks for a config.
        rig->sinceHeld.start();
        rig->client->refresh();
        const bool first = waitFor([&] { return rig->fake->heldConfig.size() == 1; }, kWaitMs);
        if (first) {
            rig->client->refresh();
        }
        if (!first || !waitFor([&] { return rig->fake->heldConfig.size() == 2; }, kWaitMs)) {
            notVerified("S15.9 (stale getConfig)", "the fake did not receive two getConfig calls");
        } else {
            rig->fake->reply(rig->fake->heldConfig[1], configB);
            if (rig->settle("S15.9 newer reply") && rig->quiesce("S15.9 newer reply", 1) &&
                rig->inTime("S15.9 (stale getConfig)")) {
                rig->fake->reply(rig->fake->heldConfig[0], configA);
                if (rig->quiesce("S15.9 older reply")) {
                    const auto& m = rig->client->monitors();
                    expect(rig->client->state() == State::Ready && m.size() == 1 && m.front().outputName == QStringLiteral("DP-4"),
                           QStringLiteral("S15.9: the older getConfig reply overwrote the newer one; ") + summary(*rig->client));
                }
            }
        }
    }

    // GOAL: S15.9 -- a requestBackend refusal belonging to an older refresh does not turn a
    //       client that the newer refresh made Ready into Unavailable.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, `if (!isCurrent(generation)) return;` removed from
    //       read()'s requestBackend reply handler -> red, Unavailable "could not load a display backend";
    //       restored, green.
    if (auto rig = Rig::make("stalebackend", "S15.9 (stale requestBackend)")) {
        rig->fake->backendMode = FakeKScreen::Mode::Hold;
        rig->fake->config = configB;
        rig->sinceHeld.start();
        rig->client->refresh();
        rig->client->refresh();
        if (!waitFor([&] { return rig->fake->heldBackend.size() == 2; }, kWaitMs)) {
            notVerified("S15.9 (stale requestBackend)", "the fake did not receive two requestBackend calls");
        } else {
            rig->fake->reply(rig->fake->heldBackend[1], true);
            if (rig->settle("S15.9 newer backend") && rig->quiesce("S15.9 newer backend", 1) &&
                rig->inTime("S15.9 (stale requestBackend)")) {
                rig->fake->reply(rig->fake->heldBackend[0], false);
                if (rig->quiesce("S15.9 older backend")) {
                    expect(rig->client->state() == State::Ready && rig->client->monitors().size() == 1,
                           QStringLiteral("S15.9: an older requestBackend refusal replaced the newer answer; ") + summary(*rig->client));
                }
            }
        }
    }
}

// S15.10
void configChangedReReads() {
    auto rig = Rig::make("configchanged", "S15.10");
    if (!rig) {
        return;
    }
    KScreenClient& c = *rig->client;
    rig->fake->config = configOf({screen(QStringLiteral("DP-3"), 2, 0), screen(QStringLiteral("DP-4"), 1, 1920)});
    if (!rig->refreshAndSettle("S15.10") || !rig->quiesce("S15.10")) {
        return;
    }

    // GOAL: S15.10 -- KScreen's configChanged signal makes the client read the configuration
    //       again with no call from the app, and while that read is outstanding the client stays
    //       Ready showing the previous answer.
    // WHY-THIS-SHAPE: the fake holds the re-read, so "stays Ready" is observed while the new
    //       answer provably has not arrived, not after it may already have; and only inside the
    //       call timeout, since past it the client's correct timeout would read as this failure.
    // CONTROL: app/kde/kscreen_dbus_source.h, onBackendConfigChanged's body emptied -> red,
    //       "configChanged led to no re-read"; app/kde/kscreen_client.cpp, refresh() made to settle
    //       Loading unconditionally -> red, "Loading during the re-read"; restored, green.
    rig->fake->configMode = FakeKScreen::Mode::Hold;
    rig->sinceHeld.start();
    rig->fake->emitConfigChanged();
    if (!waitFor([&] { return rig->fake->heldConfig.size() == 1; }, kWaitMs)) {
        expect(false, QStringLiteral("S15.10: configChanged led to no re-read within %1 ms").arg(kWaitMs));
        return;
    }
    if (!rig->inTime("S15.10 (the re-read's answer)")) {
        return;
    }
    expect(c.state() == State::Ready && c.monitors().size() == 2,
           QStringLiteral("S15.10: Ready with the old answer expected during the re-read, but %1").arg(
               c.state() == State::Loading ? QStringLiteral("Loading during the re-read") : summary(c)));
    rig->fake->reply(rig->fake->heldConfig[0], configOf({screen(QStringLiteral("DP-4"), 1, 0)}));
    if (rig->quiesce("S15.10 re-read")) {
        expect(c.state() == State::Ready && c.monitors().size() == 1 && c.monitors().front().outputName == QStringLiteral("DP-4"),
               QStringLiteral("S15.10: the re-read's answer was not taken; ") + summary(c));
    }
    expect(rig->fake->backendCalls == 2, QStringLiteral("S15.10: expected two requestBackend calls, the fake saw %1").arg(rig->fake->backendCalls));
}

// S15.11
void edidNames() {
    // GOAL: S15.11 -- a getEdid answer names the screen whose KScreen id was asked, not the screen
    //       at that position in the list.
    // WHY-THIS-SHAPE: the reply lists DP-3 (id 2) first, so id and list position disagree for
    //       both screens; in KScreen's own order they coincide and a positional mix-up is invisible.
    // CONTROL: app/kde/kscreen_client.cpp, rename() writing the name to the monitor whose list
    //       position + 1 is the kscreenId -> red, "expected DP-3/M27Q and DP-4/M27Q P"; restored, green (re-run via control_batch.sh 2026-10-26).
    if (auto rig = Rig::make("edid", "S15.11")) {
        rig->fake->config = withOutputs(g_fixture, reversed(g_fixture.value(QStringLiteral("outputs")).toList()));
        rig->fake->edids = {{1, edidNamed("M27Q P")}, {2, edidNamed("M27Q")}};
        if (rig->refreshAndSettle("S15.11") && rig->quiesce("S15.11")) {
            const Monitor* dp3 = byName(rig->client->monitors(), QStringLiteral("DP-3"));
            const Monitor* dp4 = byName(rig->client->monitors(), QStringLiteral("DP-4"));
            expect(dp3 && dp3->displayName == QStringLiteral("M27Q") && dp4 && dp4->displayName == QStringLiteral("M27Q P"),
                   QStringLiteral("S15.11: expected DP-3/M27Q and DP-4/M27Q P; got ") + names(rig->client->monitors()));
            expect(rig->fake->edidCalls == 2, QStringLiteral("S15.11: the fake saw %1 getEdid call(s), expected 2").arg(rig->fake->edidCalls));
        }
    }

    // GOAL: S15.11 -- when the selected screen's name arrives, selectedMonitor carries it and
    //       selectionChanged says so.
    // CONTROL: app/kde/kscreen_client.cpp, rename()'s `if (it->outputName == selectedOutput())
    //       { emit selectionChanged(); }` removed -> red, "0 selectionChanged"; restored, green (re-run via control_batch.sh 2026-10-26).
    if (auto rig = Rig::make("edidselected", "S15.11 (selected name)")) {
        Emissions e;
        count(*rig->client, e);
        rig->fake->config = configOf({screen(QStringLiteral("DP-4"), 1, 0)});
        rig->fake->edidMode = FakeKScreen::Mode::Hold;
        rig->sinceHeld.start();
        const bool ready = rig->refreshAndSettle("S15.11 selected", kWaitMs, 1) &&
                           waitFor([&] { return rig->fake->heldEdid.size() == 1; }, kWaitMs) &&
                           rig->client->selectedOutput() == QStringLiteral("DP-4");
        if (ready && !rig->inTime("S15.11 (selected name)")) {
            // reported as not verified
        } else if (ready) {
            const int before = e.selection;
            rig->fake->reply(rig->fake->heldEdid[0], edidNamed("M27Q P"));
            if (rig->quiesce("S15.11 selected")) {
                expect(rig->client->selectedMonitor().displayName == QStringLiteral("M27Q P") && e.selection == before + 1,
                       QStringLiteral("S15.11: selected name \"%1\" with %2 selectionChanged after its EDID arrived")
                           .arg(rig->client->selectedMonitor().displayName)
                           .arg(e.selection - before));
            }
        } else {
            expect(false, QStringLiteral("S15.11 selected: DP-4 was not auto-selected with its getEdid held; ") +
                              summary(*rig->client));
        }
    }

    // GOAL: S15.11 -- a getEdid answer for an older read does not rename a screen after the newer
    //       read's answer.
    // WHY-THIS-SHAPE: all four getEdid calls are held and released newest first, which is the
    //       order a slow first answer produces; the barrier proves the old ones were handled.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, `!isCurrent(generation) ||` removed from the getEdid
    //       handler -> red, names became OLD-1/OLD-2; restored, green (re-run via control_batch.sh 2026-10-26).
    if (auto rig = Rig::make("edidstale", "S15.11 (stale name)")) {
        rig->fake->config = g_fixture;
        rig->fake->edidMode = FakeKScreen::Mode::Hold;
        rig->sinceHeld.start();
        const bool setUp = rig->refreshAndSettle("S15.11 stale", kWaitMs, 2) &&
                           waitFor([&] { return rig->fake->heldEdid.size() == 2; }, kWaitMs);
        if (setUp) {
            rig->fake->emitConfigChanged();
        }
        if (!setUp || !waitFor([&] { return rig->fake->heldEdid.size() == 4; }, kWaitMs)) {
            notVerified("S15.11 (stale name)", "the fake did not receive two rounds of getEdid calls");
        } else {
            auto& held = rig->fake->heldEdid;
            for (int i = 2; i < 4; ++i) {
                rig->fake->reply(held[i], edidNamed("NEW-" + QByteArray::number(rig->fake->edidIds[i])));
            }
            if (rig->quiesce("S15.11 newer names", 2) && rig->inTime("S15.11 (stale name)")) {
                for (int i = 0; i < 2; ++i) {
                    rig->fake->reply(held[i], edidNamed("OLD-" + QByteArray::number(rig->fake->edidIds[i])));
                }
                if (rig->quiesce("S15.11 older names")) {
                    const Monitor* dp3 = byName(rig->client->monitors(), QStringLiteral("DP-3"));
                    const Monitor* dp4 = byName(rig->client->monitors(), QStringLiteral("DP-4"));
                    expect(dp3 && dp3->displayName == QStringLiteral("NEW-2") && dp4 && dp4->displayName == QStringLiteral("NEW-1"),
                           QStringLiteral("S15.11: expected NEW-2 on DP-3 and NEW-1 on DP-4; got ") + names(rig->client->monitors()));
                }
            }
        }
    }

    // GOAL: S15.11 -- a getEdid that fails costs only the friendly name: the client stays Ready
    //       with every screen, named by connector.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, a getEdid error reply emitted as unavailable -> red,
    //       "a failed getEdid should leave Ready"; restored, green (re-run via control_batch.sh 2026-10-26).
    if (auto rig = Rig::make("ediderror", "S15.11 (failed getEdid)")) {
        rig->fake->config = g_fixture;
        rig->fake->edidMode = FakeKScreen::Mode::Error;
        if (rig->refreshAndSettle("S15.11 failed") && rig->quiesce("S15.11 failed")) {
            expect(rig->client->state() == State::Ready && rig->fake->edidCalls == 2 &&
                       names(rig->client->monitors()) == QStringLiteral("[DP-4/DP-4, DP-3/DP-3]"),
                   QStringLiteral("S15.11: a failed getEdid should leave Ready with connector names; ") + summary(*rig->client));
        }
    }
}

// S15.18
void edidNamesCarried() {
    auto rig = Rig::make("edidcarry", "S15.18");
    if (!rig) {
        return;
    }
    KScreenClient& c = *rig->client;
    FakeKScreen& fake = *rig->fake;
    fake.edidMode = FakeKScreen::Mode::Hold;

    // Every read here holds its two getEdid calls; `read` returns once both are held and the
    // getConfig answer has been handled, and `answer` releases them by KScreen id.
    const auto read = [&](const QVariantMap& config, const char* phase) {
        fake.config = config;
        const qsizetype heldBefore = fake.heldEdid.size();
        rig->sinceHeld.start();
        if (c.state() == State::Ready) {
            fake.emitConfigChanged();
        } else {
            c.refresh();
        }
        const bool done = waitFor([&] { return fake.heldEdid.size() == heldBefore + 2; }, kWaitMs) && rig->quiesce(phase, 2);
        expect(done, QStringLiteral("%1: the read's two getEdid calls were not both held; ").arg(QLatin1String(phase)) + summary(c));
        return done;
    };
    const auto answer = [&](const QMap<int, std::optional<QByteArray>>& byId, const char* phase) {
        if (!rig->inTime("S15.18")) {
            return false;
        }
        const qsizetype first = fake.heldEdid.size() - 2;
        for (qsizetype i = first; i < fake.heldEdid.size(); ++i) {
            const auto edid = byId.value(fake.edidIds[i]);
            if (edid) {
                fake.reply(fake.heldEdid[i], *edid);
            } else {
                fake.replyError(fake.heldEdid[i], QStringLiteral("no EDID"));
            }
        }
        return rig->quiesce(phase);
    };
    const QVariantMap real = g_fixture;
    const QMap<int, std::optional<QByteArray>> realNames{{1, edidNamed("M27Q P")}, {2, edidNamed("M27Q")}};
    // DP-3 (id 2, listed second) with one field of its panel replaced.
    const auto dp3With = [&](const QString& key, const QVariant& value) {
        QVariantList outputs = real.value(QStringLiteral("outputs")).toList();
        QVariantMap dp3 = outputs[1].toMap();
        dp3.insert(key, value);
        outputs[1] = dp3;
        return withOutputs(real, outputs);
    };

    if (!read(real, "S15.18 first read") || !answer(realNames, "S15.18 first read") ||
        names(c.monitors()) != QStringLiteral("[DP-4/M27Q P, DP-3/M27Q]") || !c.select(QStringLiteral("DP-3"))) {
        expect(false, QStringLiteral("S15.18: could not reach both EDID names with DP-3 selected; ") + summary(c));
        return;
    }
    Emissions e;
    count(c, e);

    // GOAL: S15.18 -- a re-read that reports the same panels keeps the EDID names before getEdid
    //       answers again, and so changes nothing a QML binding sees: no monitorsChanged, no
    //       selectionChanged.
    // WHY-THIS-SHAPE: getEdid is held, so the names seen can only have been carried from the
    //       previous answer, not re-read.
    // CONTROL: app/kde/kscreen_client.cpp, settleReady()'s `keepKnownNames(answer, m_answer);`
    //       deleted -> red, "[DP-4/DP-4, DP-3/DP-3]" with 1 monitorsChanged; re-run via
    //       control_batch.sh 2026-10-26; restored, green.
    if (!read(real, "S15.18 unchanged")) {
        return;
    }
    expect(names(c.monitors()) == QStringLiteral("[DP-4/M27Q P, DP-3/M27Q]") && e.monitors == 0 && e.selection == 0,
           QStringLiteral("S15.18: an unchanged re-read showed %1 with %2 monitorsChanged and %3 selectionChanged before getEdid answered")
               .arg(names(c.monitors()))
               .arg(e.monitors)
               .arg(e.selection));

    // GOAL: S15.18, S15.965 -- a getEdid answer is authoritative: an EDID with no name (named() with
    //       an empty name) turns a carried name back into the connector name, while a getEdid that
    //       fails keeps the name shown.
    // WHY-THIS-SHAPE: both arrive on the same re-read, one per screen, and each screen had a
    //       carried name, so each branch decides its own screen's name.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, the getEdid handler returning on an empty name -> red,
    //       "DP-3 should be named DP-3"; the handler taking an error reply as an EDID (the
    //       ErrorMessage test removed) -> red, "DP-4 should keep M27Q P"; app/kde/kscreen_client.cpp,
    //       rename()'s connector-name fallback dropped (`shown = modelName`) -> red, "DP-3 should be
    //       named DP-3"; each re-run via control_batch.sh 2026-10-26, restored, green.
    QByteArray nameless = edidNamed("X");
    nameless[54 + 18 + 3] = '\x10'; // a dummy descriptor: a valid EDID with no name in it
    if (answer({{2, nameless}, {1, std::nullopt}}, "S15.18 nameless and failed")) {
        const Monitor* dp3 = byName(c.monitors(), QStringLiteral("DP-3"));
        const Monitor* dp4 = byName(c.monitors(), QStringLiteral("DP-4"));
        expect(dp3 && dp3->displayName == QStringLiteral("DP-3"),
               QStringLiteral("S15.18: DP-3's EDID has no name, so DP-3 should be named DP-3; got ") + names(c.monitors()));
        expect(dp4 && dp4->displayName == QStringLiteral("M27Q P"),
               QStringLiteral("S15.18: DP-4's getEdid failed, so DP-4 should keep M27Q P; got ") + names(c.monitors()));
    }

    // GOAL: S15.18 -- a different panel on the same connector does not inherit the old model name:
    //       a change of physical size alone, or of the mode list alone, shows the connector name
    //       until getEdid answers, while the unchanged screen keeps its name.
    // WHY-THIS-SHAPE: size and modes are changed in separate reads, each against a panel that
    //       matches in the other field, so each field is proven to be part of the comparison.
    // CONTROL: app/kde/kscreen_config.cpp, describePanel ignoring sizeMM -> red on "size"; ignoring
    //       the mode list -> red on "modes"; restored, green.
    if (!read(real, "S15.18 names back") || !answer(realNames, "S15.18 names back")) {
        return;
    }
    const QVariantMap otherSize = dp3With(QStringLiteral("sizeMM"), QVariantMap{{QStringLiteral("width"), qlonglong(700)},
                                                                                {QStringLiteral("height"), qlonglong(400)}});
    if (read(otherSize, "S15.18 size")) {
        expect(names(c.monitors()) == QStringLiteral("[DP-4/M27Q P, DP-3/DP-3]"),
               QStringLiteral("S15.18: a different physical size on DP-3 should show DP-3 until getEdid answers; got ") +
                   names(c.monitors()));
        if (!answer({{1, edidNamed("M27Q P")}, {2, edidNamed("OTHER")}}, "S15.18 size")) {
            return;
        }
    }
    QVariantList modes = otherSize.value(QStringLiteral("outputs")).toList()[1].toMap().value(QStringLiteral("modes")).toList();
    QVariantMap mode = modes[0].toMap();
    mode.insert(QStringLiteral("refreshRate"), 143.9);
    modes[0] = mode;
    QVariantList outputs = otherSize.value(QStringLiteral("outputs")).toList();
    QVariantMap dp3 = outputs[1].toMap();
    dp3.insert(QStringLiteral("modes"), modes);
    outputs[1] = dp3;
    if (read(withOutputs(real, outputs), "S15.18 modes")) {
        expect(names(c.monitors()) == QStringLiteral("[DP-4/M27Q P, DP-3/DP-3]"),
               QStringLiteral("S15.18: a different mode list on DP-3 should show DP-3 until getEdid answers; got ") +
                   names(c.monitors()));
        if (!answer(realNames, "S15.18 modes")) {
            return;
        }
    }

    // GOAL: S15.18 -- a reply that describes no panel (neither sizeMM nor modes) carries no name,
    //       since nothing shows the panel is the same one.
    // CONTROL: app/kde/kscreen_client.cpp, keepKnownNames without `!monitor.panel.isEmpty() &&`
    //       -> red, "with no panel described ... got [DP-4/M27Q P, DP-3/M27Q]"; restored, green (re-run via control_batch.sh 2026-10-26).
    const QVariantMap bare = configOf({screen(QStringLiteral("DP-4"), 1, 1440), screen(QStringLiteral("DP-3"), 2, 0)});
    if (read(bare, "S15.18 no panel") && answer(realNames, "S15.18 no panel") &&
        names(c.monitors()) == QStringLiteral("[DP-4/M27Q P, DP-3/M27Q]") && read(bare, "S15.18 no panel again")) {
        expect(names(c.monitors()) == QStringLiteral("[DP-4/DP-4, DP-3/DP-3]"),
               QStringLiteral("S15.18: with no panel described, a re-read should show connector names until getEdid answers; got ") +
                   names(c.monitors()));
    } else {
        expect(false, QStringLiteral("S15.18: the panel-less reads did not reach both EDID names; ") + summary(c));
    }
}

// S15.19
void launcherRestart() {
    const QVariantMap dp3 = screen(QStringLiteral("DP-3"), 2, 0);
    const QVariantMap dp4 = screen(QStringLiteral("DP-4"), 1, 1920);
    auto rig = Rig::make("restart", "S15.19");
    if (!rig) {
        return;
    }
    KScreenClient& c = *rig->client;
    const QString service = rig->fake->service();
    rig->fake->config = configOf({dp3, dp4});
    if (!rig->refreshAndSettle("S15.19") || !rig->quiesce("S15.19") || !c.select(QStringLiteral("DP-3"))) {
        expect(false, QStringLiteral("S15.19: could not reach Ready with DP-3 selected; ") + summary(c));
        return;
    }

    // GOAL: S15.19, S15.12 -- KScreen's launcher leaving the bus is noticed with no configChanged:
    //       the client drops the old list at once (Loading, no monitor, no selection) and asks
    //       once more; that read failing, it reads Unavailable "not running", reporting nothing.
    // WHY-THIS-SHAPE: the fake is destroyed, which releases the name and sends nothing else; the
    //       bus can start nothing, so the one retry must fail. The Loading step lasts
    //       milliseconds, so it is seen by snapshotting at each stateChanged, not by polling.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, the `connect(launcher, ...serviceOwnerChanged...)`
    //       deleted -> red, "the launcher left the bus and the client stayed state Ready";
    //       app/kde/kscreen_client.h, monitors() returning `m_answer` whatever the state (a non-Ready
    //       client cannot otherwise be made to show a list) -> red, "went Loading with 2 monitor(s)";
    //       app/kde/kscreen_dbus_source.cpp, the loss branch never retrying (`if (m_launcherRetrySpent
    //       && newOwner.isEmpty())` made `if (true)`) -> red, "the reason should say "is not running"".
    //       All re-run via control_batch.sh 2026-10-26; restored.
    struct Seen {
        State state;
        qsizetype monitors;
        QString selected;
    };
    QList<Seen> seen;
    QObject::connect(&c, &KScreenClient::stateChanged, [&] { seen.append({c.state(), c.monitors().size(), c.selectedOutput()}); });
    rig->fake.reset();
    if (!waitFor([&] { return c.state() == State::Unavailable; }, kWaitMs) || !rig->quiesce("S15.19 launcher gone")) {
        expect(false, QStringLiteral("S15.19: the launcher left the bus and the client stayed ") + summary(c));
        return;
    }
    expectUnavailable(*rig, "S15.19 launcher gone", QStringLiteral("is not running"));
    expect(!seen.isEmpty() && seen.front().state == State::Loading && seen.front().monitors == 0 && seen.front().selected.isEmpty(),
           seen.isEmpty() ? QStringLiteral("S15.19: no stateChanged after the launcher left")
                          : QStringLiteral("S15.19: after the launcher left, the client first went %1 with %2 monitor(s), selection \"%3\"; "
                                           "expected Loading with none")
                                .arg(stateName(seen.front().state))
                                .arg(seen.front().monitors)
                                .arg(seen.front().selected));

    // GOAL: S15.19 -- a new launcher taking the name is read exactly once (one requestBackend, one
    //       getConfig) with no configChanged, the client is Ready with that launcher's list, and
    //       the remembered choice is back since that list holds it.
    // WHY-THIS-SHAPE: the new fake lists a third screen, so its list is told apart from the old
    //       one; two selectable screens rule out auto-select putting DP-3 back. Counted after the
    //       quiesce barrier, so a second read would already be in the counts.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, the watcher's `WatchForOwnerChange` made
    //       `WatchForUnregistration` -> red, "the new launcher was never read";
    //       app/kde/kscreen_client.cpp, the `returned` handler's `refresh();` doubled -> red,
    //       "DP-3 should be selected again"; restored, green (both re-run 2026-10-26).
    auto second = std::make_unique<FakeKScreen>(service);
    second->config = configOf({dp3, dp4, screen(QStringLiteral("HDMI-A-2"), 3, 3840)});
    if (const QString fault = second->start(); !fault.isEmpty()) {
        notVerified("S15.19 (new launcher)", "the second fake KScreen could not stand up: " + fault.toStdString());
        return;
    }
    rig->fake = std::move(second);
    if (!waitFor([&] { return c.state() == State::Ready && c.monitors().size() == 3; }, kWaitMs) || !rig->quiesce("S15.19 new launcher")) {
        expect(false, QStringLiteral("S15.19: the new launcher was never read (%1 requestBackend, %2 getConfig); ")
                              .arg(rig->fake->backendCalls)
                              .arg(rig->fake->configCalls) +
                          summary(c));
        return;
    }
    expect(rig->fake->backendCalls == 1 && rig->fake->configCalls == 1 && c.selectedOutput() == QStringLiteral("DP-3"),
           QStringLiteral("S15.19: after the new launcher (%1 requestBackend, %2 getConfig) DP-3 should be selected again; ")
                   .arg(rig->fake->backendCalls)
                   .arg(rig->fake->configCalls) +
               summary(c));
    rig->expectOnlyClientCalled("S15.19 new launcher");
}

// S15.28
void launcherReplaced() {
    const QVariantMap dp3 = screen(QStringLiteral("DP-3"), 2, 0);
    const QVariantMap dp4 = screen(QStringLiteral("DP-4"), 1, 1920);
    // The default stay-up period (60 s) outlasts this phase, so the retry the first replacement
    // spends is still spent at the second.
    auto rig = Rig::make("replace", "S15.28", {.launcherStayUpMs = 60000});
    if (!rig) {
        return;
    }
    KScreenClient& c = *rig->client;
    const QString service = rig->fake->service();
    rig->fake->config = configOf({dp3, dp4});
    if (!rig->refreshAndSettle("S15.28") || !rig->quiesce("S15.28") || c.state() != State::Ready) {
        expect(false, QStringLiteral("S15.28: could not reach Ready; ") + summary(c));
        return;
    }
    // The premise: each hand-over reaches the client as one signal naming both owners.
    QList<std::pair<QString, QString>> changes;
    QDBusServiceWatcher watcher(service, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange);
    QObject::connect(&watcher, &QDBusServiceWatcher::serviceOwnerChanged,
                     [&](const QString&, const QString& oldOwner, const QString& newOwner) { changes.append({oldOwner, newOwner}); });

    // Queues `next` behind the current owner, releases the current one, and waits until `next`
    // owns the name and the client has read `screens` monitors. False, reported, otherwise.
    const auto handOver = [&](std::unique_ptr<FakeKScreen> next, qsizetype screens, const char* phase) {
        if (const QString fault = next->startQueued(); !fault.isEmpty()) {
            notVerified("S15.28", std::string(phase) + ": the next fake could not queue: " + fault.toStdString());
            return false;
        }
        const qsizetype before = changes.size();
        rig->fake = std::move(next);
        const bool handed = waitFor([&] { return changes.size() > before && rig->fake->owns(); }, kWaitMs);
        if (!handed || changes.size() != before + 1 || changes.back().first.isEmpty() || changes.back().second.isEmpty()) {
            notVerified("S15.28", std::string(phase) + ": the name did not change hands in one NameOwnerChanged naming both owners");
            return false;
        }
        const bool read = waitFor([&] { return c.state() == State::Ready && c.monitors().size() == screens; }, kWaitMs) &&
                          rig->quiesce(phase);
        expect(read, QStringLiteral("%1: the new owner was never read (%2 requestBackend, %3 getConfig); ")
                         .arg(QLatin1String(phase))
                         .arg(rig->fake->backendCalls)
                         .arg(rig->fake->configCalls) +
                         summary(c));
        return read;
    };

    // Every state the client passes through, snapshotted as it is emitted: Unavailable on a
    // hand-over lasts only until the new owner's answer.
    QStringList states;
    QObject::connect(&c, &KScreenClient::stateChanged, [&] {
        states.append(stateName(c.state()) + (c.unavailableReason().isEmpty() ? QString() : QLatin1Char(' ') + c.unavailableReason()));
    });

    // GOAL: S15.28 -- an owner replaced in one NameOwnerChanged (old and new owner both set) is
    //       read exactly once and never reads Unavailable, whether or not the loss retry was
    //       already spent; and the hand-over spends no retry, so a real loss after it is still
    //       retried ("is not running" once that read fails, not "keeps stopping").
    // WHY-THIS-SHAPE: the next fake queues for the name, so releasing the old one hands it over
    //       atomically; a watcher of the test's own proves that premise. Each new fake lists one
    //       more screen, so its answer is told apart; counts are read after the quiesce barrier.
    //       The retry is spent by a real loss between the two hand-overs, inside the 60 s stay-up.
    // CONTROL: via control_batch.sh, 2026-10-26, each reverted: app/kde/kscreen_client.cpp, the
    //       `returned` handler's `if (m_state == State::Unavailable)` made `if (true)` -> "FAIL:
    //       S15.28 retry unspent: the new owner saw 2 requestBackend";
    //       app/kde/kscreen_dbus_source.cpp, onLauncherOwnerChanged's `if (m_launcherRetrySpent &&
    //       newOwner.isEmpty())` made `if (m_launcherRetrySpent)` -> "FAIL: S15.28 retry spent: the
    //       hand-over passed through [... Unavailable ... keeps stopping ...]";
    //       `m_launcherRetrySpent = m_launcherRetrySpent || newOwner.isEmpty();` made `= true;` ->
    //       "FAIL: S15.28: the loss after a hand-over read [...keeps stopping...]".
    auto second = std::make_unique<FakeKScreen>(service);
    second->config = configOf({dp3, dp4, screen(QStringLiteral("HDMI-A-2"), 3, 3840)});
    states.clear();
    if (!handOver(std::move(second), 3, "S15.28 retry unspent")) {
        return;
    }
    expect(rig->fake->backendCalls == 1 && rig->fake->configCalls == 1 && !states.join(u'|').contains(QLatin1String("Unavailable")),
           QStringLiteral("S15.28 retry unspent: the new owner saw %1 requestBackend and %2 getConfig, expected 1 each; the hand-over passed through [%3]")
               .arg(rig->fake->backendCalls)
               .arg(rig->fake->configCalls)
               .arg(states.join(QStringLiteral(" -> "))));

    // A real loss, which spends the retry: the retry's read fails with nobody to answer it.
    states.clear();
    rig->fake.reset();
    if (!waitFor([&] { return c.state() == State::Unavailable; }, kWaitMs) || !rig->quiesce("S15.28 loss")) {
        expect(false, QStringLiteral("S15.28: the launcher left and the client stayed ") + summary(c));
        return;
    }
    expect(c.unavailableReason().contains(QLatin1String("is not running")),
           QStringLiteral("S15.28: the loss after a hand-over read [%1]; a hand-over that spent no retry is retried once").arg(states.join(QStringLiteral(" -> "))));
    auto back = std::make_unique<FakeKScreen>(service);
    back->config = configOf({dp3, dp4, screen(QStringLiteral("HDMI-A-2"), 3, 3840)});
    if (const QString fault = back->start(); !fault.isEmpty()) {
        notVerified("S15.28 (retry spent)", "the fake could not come back: " + fault.toStdString());
        return;
    }
    rig->fake = std::move(back);
    if (!waitFor([&] { return c.state() == State::Ready && c.monitors().size() == 3; }, kWaitMs) || !rig->quiesce("S15.28 back")) {
        expect(false, QStringLiteral("S15.28: the launcher that came back was never read; ") + summary(c));
        return;
    }

    auto third = std::make_unique<FakeKScreen>(service);
    third->config = configOf({dp3, dp4, screen(QStringLiteral("HDMI-A-2"), 3, 3840), screen(QStringLiteral("HDMI-A-3"), 4, 5760)});
    states.clear();
    if (!handOver(std::move(third), 4, "S15.28 retry spent")) {
        return;
    }
    expect(!states.join(u'|').contains(QLatin1String("Unavailable")),
           QStringLiteral("S15.28 retry spent: the hand-over passed through [%1]").arg(states.join(QStringLiteral(" -> "))));
    expect(rig->fake->backendCalls == 1 && rig->fake->configCalls == 1,
           QStringLiteral("S15.28 retry spent: the new owner saw %1 requestBackend and %2 getConfig, expected 1 each")
               .arg(rig->fake->backendCalls)
               .arg(rig->fake->configCalls));
}

// S15.334
void namesSurviveLauncherLoss() {
    auto rig = Rig::make("namesloss", "S15.334");
    if (!rig) {
        return;
    }
    KScreenClient& c = *rig->client;
    const QString service = rig->fake->service();
    rig->fake->config = g_fixture;
    rig->fake->edids = {{1, edidNamed("M27Q P")}, {2, edidNamed("M27Q")}};
    if (!rig->refreshAndSettle("S15.334") || !rig->quiesce("S15.334") || names(c.monitors()) != QStringLiteral("[DP-4/M27Q P, DP-3/M27Q]") ||
        !c.select(QStringLiteral("DP-3"))) {
        expect(false, QStringLiteral("S15.334: could not reach both EDID names with DP-3 selected; ") + summary(c));
        return;
    }
    // GOAL: S15.334 -- the model names a Ready answer showed survive the launcher leaving and a new
    //       one being read: the new launcher's answer for the same panels shows them before its own
    //       getEdid answers. While not Ready (Loading after the loss, Unavailable once the retry
    //       failed) the client reports no monitor and no selection.
    // WHY-THIS-SHAPE: the new launcher holds every getEdid, so a name seen can only have been
    //       carried across the non-Ready states; the non-Ready states are snapshotted as emitted.
    //       The fixture's panels carry sizeMM and modes, which carrying a name requires.
    // CONTROL: app/kde/kscreen_client.cpp, settleReady() carrying names only when
    //       `before.state == State::Ready`, via control_batch.sh, 2026-10-26 -> "FAIL: S15.334: after the
    //       launcher came back the names read [DP-4/DP-4, DP-3/DP-3]". Reverted.
    QStringList notReady;
    QObject::connect(&c, &KScreenClient::stateChanged, [&] {
        if (c.state() != State::Ready && (!c.monitors().isEmpty() || !c.selectedOutput().isEmpty())) {
            notReady.append(stateName(c.state()) + QStringLiteral(" with %1 monitor(s), selection \"%2\"").arg(c.monitors().size()).arg(c.selectedOutput()));
        }
    });
    rig->fake.reset();
    if (!waitFor([&] { return c.state() == State::Unavailable; }, kWaitMs) || !rig->quiesce("S15.334 loss")) {
        expect(false, QStringLiteral("S15.334: the launcher left and the client stayed ") + summary(c));
        return;
    }
    auto next = std::make_unique<FakeKScreen>(service);
    next->config = g_fixture;
    next->edidMode = FakeKScreen::Mode::Hold;
    if (const QString fault = next->start(); !fault.isEmpty()) {
        notVerified("S15.334", "the new fake KScreen could not stand up: " + fault.toStdString());
        return;
    }
    rig->fake = std::move(next);
    rig->sinceHeld.start();
    const bool read = waitFor([&] { return c.state() == State::Ready && rig->fake->heldEdid.size() == 2; }, kWaitMs) && rig->quiesce("S15.334 back", 2);
    expect(read && names(c.monitors()) == QStringLiteral("[DP-4/M27Q P, DP-3/M27Q]") && c.selectedOutput() == QStringLiteral("DP-3"),
           QStringLiteral("S15.334: after the launcher came back the names read %1 with getEdid held; ").arg(names(c.monitors())) + summary(c));
    expect(notReady.isEmpty(), QStringLiteral("S15.334: while not Ready the client reported [%1]").arg(notReady.join(QStringLiteral(" | "))));
}

// S15.25 and S15.26, the client's side
void idsAndEmptyConnectors() {
    // GOAL: S15.26 -- getEdid is asked only for connected outputs: an empty connector has no
    //       panel to name, while a connected but disabled monitor still has one.
    // WHY-THIS-SHAPE: the empty connector has an id, so the id test alone would ask for it; the
    //       disabled monitor sits beside it, so "connected" and "selectable" are told apart.
    // CONTROL: app/kde/kscreen_dbus_source.cpp, requestMonitorNames' `|| !monitor.connected` removed
    //       -> red, "getEdid was asked for ids [1, 3, 4]"; restored, green (re-run via control_batch.sh 2026-10-26).
    if (auto rig = Rig::make("emptyconnector", "S15.26 (no getEdid for an empty connector)")) {
        rig->fake->config = configOf({screen(QStringLiteral("DP-1"), 1, 0),
                                      output({.name = QStringLiteral("HDMI-A-1"), .id = 3, .enabled = false, .connected = false,
                                              .withPos = false, .withSize = false}),
                                      output({.name = QStringLiteral("DP-3"), .id = 4, .enabled = false, .withPos = false, .withSize = false})});
        if (rig->refreshAndSettle("S15.26") && rig->quiesce("S15.26")) {
            QList<int> asked = rig->fake->edidIds;
            std::ranges::sort(asked);
            QStringList shown;
            for (const int id : asked) {
                shown.append(QString::number(id));
            }
            expect(rig->client->state() == State::Ready && asked == QList<int>{1, 4},
                   QStringLiteral("S15.26: getEdid was asked for ids [%1], expected [1, 4] (not the empty connector's 3); ")
                           .arg(shown.join(QStringLiteral(", "))) +
                       summary(*rig->client));
        }
    }

    // GOAL: S15.25 -- a reply giving two outputs one id reads Unavailable with the parser's
    //       sentence, and no getEdid is sent, since its answer could name either screen.
    // CONTROL: app/kde/kscreen_config.cpp, the shared-id `return std::unexpected` made a no-op ->
    //       red, "expected Unavailable; state Ready" and "2 getEdid"; restored, green.
    if (auto rig = Rig::make("sharedid", "S15.25 (a shared id over the bus)")) {
        rig->fake->config = configOf({screen(QStringLiteral("DP-1"), 5, 0), screen(QStringLiteral("DP-2"), 5, 1920)});
        if (rig->refreshAndSettle("S15.25 shared id") && rig->quiesce("S15.25 shared id")) {
            expectUnavailable(*rig, "S15.25 shared id", QStringLiteral("KScreen gave DP-1 and DP-2 the same id"));
            expect(rig->fake->edidCalls == 0,
                   QStringLiteral("S15.25 shared id: the client sent %1 getEdid for a reply it should refuse").arg(rig->fake->edidCalls));
        }
    }
}

// S15.12
void readyToUnavailable() {
    struct Seen {
        State state;
        qsizetype monitors;
        QString selected;
    };

    // GOAL: S15.12 -- a client that was Ready with a selection and then fails a re-read reports no
    //       monitor and no selection (the choice is remembered, S15.16, but not reported), and
    //       every signal it emits is emitted after all of them are written: a handler of any of
    //       the three already reads the new state, list and selection.
    // WHY-THIS-SHAPE: the handlers snapshot all three fields at emission time; checking the
    //       fields after the wait instead passes whatever order the signals fire in.
    // CONTROL: app/kde/kscreen_client.h, monitors() returning `m_answer` whatever the state -> red,
    //       "stateChanged saw state Unavailable, 1 monitor(s)" on both paths, via control_batch.sh
    //       2026-10-26; restored, green. A non-Ready client's list is derived from its state in
    //       monitors(), and every signal is emitted from settled() after the fields are written, so
    //       monitors() is the place left where an ordering break can be written.
    // COVERAGE-ASSERT: app/kde/kscreen_client.cpp, settled()'s `emit monitorsChanged()` removed -> red, "expected all three
    //       signals, saw stateChanged,selectionChanged" on both paths (re-run via control_batch.sh 2026-10-26).
    for (const bool viaBackend : {false, true}) {
        const char* phase = viaBackend ? "S15.12 (backend refused)" : "S15.12 (getConfig error)";
        auto rig = Rig::make(viaBackend ? "unavailable2" : "unavailable1", phase);
        if (!rig) {
            continue;
        }
        KScreenClient& c = *rig->client;
        rig->fake->config = configOf({screen(QStringLiteral("DP-4"), 1, 0)});
        if (!rig->refreshAndSettle(phase) || !rig->quiesce(phase) || c.selectedOutput() != QStringLiteral("DP-4")) {
            expect(false, QStringLiteral("%1: not Ready with DP-4 selected before the failure; ").arg(QLatin1String(phase)) + summary(c));
            continue;
        }
        QList<std::pair<const char*, Seen>> seen;
        const auto snap = [&](const char* signal) { seen.append({signal, {c.state(), c.monitors().size(), c.selectedOutput()}}); };
        QObject::connect(&c, &KScreenClient::stateChanged, [&] { snap("stateChanged"); });
        QObject::connect(&c, &KScreenClient::monitorsChanged, [&] { snap("monitorsChanged"); });
        QObject::connect(&c, &KScreenClient::selectionChanged, [&] { snap("selectionChanged"); });
        if (viaBackend) {
            rig->fake->backendAnswer = false;
        } else {
            rig->fake->configMode = FakeKScreen::Mode::Error;
        }
        rig->fake->emitConfigChanged();
        if (!waitFor([&] { return c.state() == State::Unavailable; }, kWaitMs)) {
            expect(false, QStringLiteral("%1: never became Unavailable; ").arg(QLatin1String(phase)) + summary(c));
            continue;
        }
        expectUnavailable(*rig, phase, viaBackend ? QStringLiteral("could not load") : QStringLiteral("refused the request"));
        QStringList signalNames;
        for (const auto& [signal, s] : seen) {
            signalNames.append(QLatin1String(signal));
            expect(s.state == State::Unavailable && s.monitors == 0 && s.selected.isEmpty(),
                   QStringLiteral("%1: %2 saw state %3, %4 monitor(s), selection \"%5\"")
                       .arg(QLatin1String(phase), QLatin1String(signal), stateName(s.state))
                       .arg(s.monitors)
                       .arg(s.selected));
        }
        // COVERAGE-ASSERT: all three signals fired, so the loop above compared three snapshots.
        expect(signalNames.contains(QStringLiteral("stateChanged")) && signalNames.contains(QStringLiteral("monitorsChanged")) &&
                   signalNames.contains(QStringLiteral("selectionChanged")),
               QStringLiteral("%1: expected all three signals, saw %2").arg(QLatin1String(phase), signalNames.join(u',')));
    }

    // GOAL: S15.12 -- "Try again" works: refresh() from Unavailable reaches Ready and clears the reason.
    // CONTROL: app/kde/kscreen_client.cpp, refresh() returning early when Unavailable -> red, "did
    //       not recover"; restored, green.
    if (auto rig = Rig::make("recover", "S15.12 (recovery)")) {
        rig->fake->backendAnswer = false;
        rig->fake->config = configOf({screen(QStringLiteral("DP-4"), 1, 0)});
        if (rig->refreshAndSettle("S15.12 recovery, first") && rig->client->state() == State::Unavailable) {
            rig->fake->backendAnswer = true;
            if (rig->refreshAndSettle("S15.12 recovery, again")) {
                expect(rig->client->state() == State::Ready && rig->client->unavailableReason().isEmpty() &&
                           rig->client->monitors().size() == 1,
                       QStringLiteral("S15.12: refresh() from Unavailable did not recover; ") + summary(*rig->client));
            }
        } else {
            expect(false, QStringLiteral("S15.12 recovery: the first refresh did not read Unavailable"));
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "kscreen_client";
    QCoreApplication app(argc, argv);
    if (const auto refused = refuseUnlessPrivateBus()) {
        return *refused;
    }
    if (const auto fixture = realFixture()) {
        g_fixture = *fixture;
    } else {
        wallpaperx::testing::harnessFailure("the KScreen fixture " WALLPAPERX_APP_TEST_DATA "/kscreen_getconfig.json could not be read");
        static_cast<void>(wallpaperx::testing::reportHarnessFaults("in this test file, not in the app"));
        return 1;
    }

    wireRoundTrip();
    unavailableStates();
    selection();
    autoSelectOnlyScreen();
    choiceSurvivesOutage();
    separateTimeouts();
    mirrorSelection();
    staleRepliesDropped();
    configChangedReReads();
    edidNames();
    edidNamesCarried();
    launcherRestart();
    launcherReplaced();
    namesSurviveLauncherLoss();
    idsAndEmptyConnectors();
    readyToUnavailable();

    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file or its fake KScreen, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
