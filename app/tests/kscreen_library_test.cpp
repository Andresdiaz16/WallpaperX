// SPDX-License-Identifier: GPL-2.0-or-later

// The monitor list read through libKF6Screen in this process (app/kde/kscreen_library_source.cpp),
// and the run-time choice between it and KScreen's D-Bus service (app/kde/kscreen_source.cpp).
// Goals S15.959-S15.965, S15.967 and S15.969 in GOALS.md. Three modes, one ctest entry each:
//   display   -- under a nested KWin with two virtual outputs, changed with kscreen-doctor;
//   offscreen -- no Wayland connection at all, and hand-built KScreen outputs read with no compositor;
//   oldlib    -- libKF6Screen 6.7.5 loaded in place of the installed one, on a bus that can start
//                only a stand-in for org.kde.KScreen that records each start (--stand-in).

#include "kscreen_client.h"
#include "kscreen_config.h"
#include "kscreen_dbus_source.h"
#include "kscreen_library_source.h"
#include "kscreen_source.h"
#include "fake_kscreen.h"
#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <KScreen/Config>
#include <KScreen/EDID>
#include <KScreen/GetConfigOperation>
#include <KScreen/Mode>
#include <KScreen/Output>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QScreen>

#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <memory>
#include <optional>

#include <link.h>

using namespace wallpaperx;
using wallpaperx::testing::check;
using wallpaperx::testing::guardedPhase;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::phaseSelected;
using State = KScreenClient::State;

namespace {

constexpr int kWaitMs = 10000;

void expect(bool cond, const QString& msg) { check(cond, msg.toStdString().c_str()); }

using wallpaperx::apptest::refuseUnlessPrivateBus;
using wallpaperx::apptest::refuseUnlessUnderDbusRunSession;
using wallpaperx::apptest::stateName;
using wallpaperx::apptest::waitFor;

// Services events for `ms` whatever happens, for checks that nothing further arrives.
void spin(int ms) { static_cast<void>(waitFor([] { return false; }, ms)); }

QString describe(const Monitor& m) {
    return QStringLiteral("%1(id %2) geom %3,%4 %5x%6 px %7x%8 rot %9 flip %10 scale %11 en %12 conn %13 prim %14 rate %15 mirror '%16'")
        .arg(m.outputName).arg(m.kscreenId).arg(m.geometry.x()).arg(m.geometry.y()).arg(m.geometry.width())
        .arg(m.geometry.height()).arg(m.pixelSize.width()).arg(m.pixelSize.height()).arg(m.rotation).arg(m.flipped)
        .arg(m.scale).arg(m.enabled).arg(m.connected).arg(m.primary).arg(m.refreshRate).arg(m.mirrorOf);
}

// kscreen-doctor (the installed one, so the installed libKF6Screen) against the nested KWin. False,
// with a harness fault, when it does not run or exits non-zero.
bool doctor(const QStringList& args) {
    QProcess p;
    p.start(QStringLiteral("/usr/bin/kscreen-doctor"), args);
    if (!p.waitForFinished(kWaitMs) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        harnessFailure("kscreen-doctor " + args.join(QLatin1Char(' ')).toStdString() + " failed: " +
                       p.readAllStandardError().toStdString() + p.readAllStandardOutput().toStdString());
        return false;
    }
    return true;
}

bool baseline() {
    return doctor({QStringLiteral("output.Virtual-0.enable"), QStringLiteral("output.Virtual-1.enable"),
                   QStringLiteral("output.Virtual-0.mirror.none"), QStringLiteral("output.Virtual-1.mirror.none"),
                   QStringLiteral("output.Virtual-0.scale.1"), QStringLiteral("output.Virtual-1.scale.1"),
                   QStringLiteral("output.Virtual-0.rotation.none"), QStringLiteral("output.Virtual-1.rotation.none"),
                   QStringLiteral("output.Virtual-0.position.0,0"), QStringLiteral("output.Virtual-1.position.1024,0"),
                   QStringLiteral("output.Virtual-0.priority.1"), QStringLiteral("output.Virtual-1.priority.2")});
}

// Puts the nested KWin's outputs back on scope exit, so a phase that returns early does not leave
// the next phase a scaled, rotated or mirrored Virtual-1.
struct RestoreBaseline {
    ~RestoreBaseline() { static_cast<void>(baseline()); }
};

// The fields every monitor should carry, worked out here from libkscreen's own getters on a read
// of the test's own -- not through the source's map or parseKScreenConfig. nullopt (with a harness
// fault) when that read fails, so a failed read is never graded as "libkscreen lists no monitors".
std::optional<QList<Monitor>> oracle() {
    KScreen::GetConfigOperation op;
    if (!op.exec() || !op.config()) {
        harnessFailure("the test's own GetConfigOperation failed: " + op.errorString().toStdString());
        return std::nullopt;
    }
    const KScreen::ConfigPtr config = op.config();
    QList<Monitor> out;
    for (const KScreen::OutputPtr& o : config->outputs()) {
        Monitor m;
        m.kscreenId = o->id();
        m.outputName = o->name();
        m.displayName = o->name();
        m.enabled = o->isEnabled();
        m.connected = o->isConnected();
        m.primary = o->priority() == 1;
        m.scale = o->scale();
        const int r = int(o->rotation());
        m.rotation = (r & (2 | 32)) ? 90 : (r & (4 | 64)) ? 180 : (r & (8 | 128)) ? 270 : 0;
        m.flipped = r >= 16;
        const KScreen::ModePtr mode = o->currentMode();
        m.refreshRate = mode ? mode->refreshRate() : 0;
        QSize px = mode ? mode->size() : o->size();
        if (m.rotation == 90 || m.rotation == 270) {
            px.transpose();
        }
        m.pixelSize = px;
        m.geometry = QRectF(o->pos(), QSizeF(px) / m.scale);
        if (o->replicationSource() != 0) {
            const KScreen::OutputPtr src = config->output(o->replicationSource());
            m.mirrorOf = src ? src->name() : QStringLiteral("?");
        }
        out.append(m);
    }
    return out;
}

// The one definition of "agrees with libkscreen" for S15.960, used by the wait and by the grade.
// Not Monitor's operator==: that also compares `panel`, which parseKScreenConfig derives and oracle()
// does not rebuild, so whole-value equality never holds against oracle() output.
constexpr int kOracleFields = 12;
bool sameAsOracle(const Monitor& g, const Monitor& w) {
    return g.kscreenId == w.kscreenId && g.displayName == w.displayName && g.geometry == w.geometry &&
           g.pixelSize == w.pixelSize && g.scale == w.scale && g.rotation == w.rotation && g.flipped == w.flipped &&
           g.primary == w.primary && g.refreshRate == w.refreshRate && g.enabled == w.enabled &&
           g.connected == w.connected && g.mirrorOf == w.mirrorOf;
}

bool agreesWithOracle(const QList<Monitor>& got, const QList<Monitor>& want) {
    return got.size() == want.size() && std::ranges::all_of(want, [&](const Monitor& w) {
               const auto it = std::ranges::find(got, w.outputName, &Monitor::outputName);
               return it != got.end() && sameAsOracle(*it, w);
           });
}

// Compares every field S15.960 names; returns how many fields were compared.
int compareFields(const QList<Monitor>& got, const QList<Monitor>& want, const QString& when) {
    int compared = 0;
    expect(got.size() == want.size(), when + QStringLiteral(": the library path listed %1 monitor(s), libkscreen %2")
                                                  .arg(got.size()).arg(want.size()));
    for (const Monitor& w : want) {
        const auto it = std::ranges::find(got, w.outputName, &Monitor::outputName);
        if (it == got.end()) {
            expect(false, when + QStringLiteral(": libkscreen lists %1, the library path does not").arg(w.outputName));
            continue;
        }
        const Monitor& g = *it;
        compared += kOracleFields;
        expect(sameAsOracle(g, w), when + QStringLiteral(": the library path's monitor differs from libkscreen's own getters:\n  got  ") +
                                       describe(g) + QStringLiteral("\n  want ") + describe(w));
    }
    return compared;
}

bool waitReady(KScreenClient& c, const char* what) {
    if (!waitFor([&] { return c.state() != State::Loading; }, kWaitMs)) {
        check(false, (std::string(what) + ": the client was still Loading after 10 s").c_str());
        return false;
    }
    if (c.state() != State::Ready) {
        check(false, (std::string(what) + ": the client is not Ready: " + c.unavailableReason().toStdString()).c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- display mode (nested KWin)

void libraryChosen() {
    // GOAL: S15.959 -- with libKF6Screen 6.8.1 loaded, systemKScreenSource gives the in-process
    //       library source. False if the probe of the loaded library picks the D-Bus source.
    // WHY-THIS-SHAPE: asked of the function main.cpp calls, by type, rather than by watching for a
    //       D-Bus call: the D-Bus source on this private bus would just report "not running", which
    //       an Unavailable from the library source cannot be told apart from.
    // CONTROL: app/kde/kscreen_library_source.cpp inProcess() returns false -> red, "systemKScreenSource
    //       did not give the library source" (re-run via control_batch.sh 2026-10-26); restored, green.
    auto source = systemKScreenSource(QDBusConnection::sessionBus());
    check(dynamic_cast<KScreenLibrarySource*>(source.get()) != nullptr,
          "S15.959: with the installed libKF6Screen (no BackendManager::method) loaded, systemKScreenSource did not give the library source");
}

void nestedListing() {
    // GOAL: S15.967 -- against the installed libKF6Screen and a real nested KWin, the client the app
    //       builds lists exactly the compositor's screens, each selectable. Covers only the KDE
    //       version installed on the machine that runs it; an upgrade that breaks listing turns it red.
    // WHY-THIS-SHAPE: the expected names come from Qt's own Wayland screens, not from libkscreen, so
    //       a libkscreen that answers but answers wrong (or nothing) is caught.
    // CONTROL: app/kde/kscreen_library_source.cpp, the loop over config->outputs() skipped (an empty
    //       list) -> red, "S15.967: the client lists [] ..." (re-run via control_batch.sh 2026-10-26).
    QStringList want;
    for (QScreen* s : QGuiApplication::screens()) {
        want.append(s->name());
    }
    want.sort();
    if (want.size() != 2) {
        harnessFailure("the nested KWin shows " + std::to_string(want.size()) + " screen(s), not the 2 it was started with");
        return;
    }
    KScreenClient client(systemKScreenSource(QDBusConnection::sessionBus()));
    client.refresh();  // As main.cpp does once everything is wired.
    if (!waitReady(client, "S15.967")) {
        return;
    }
    QStringList got;
    bool allSelectable = true;
    for (const Monitor& m : client.monitors()) {
        got.append(m.outputName);
        allSelectable = allSelectable && m.selectable();
    }
    got.sort();
    expect(got == want && allSelectable,
           QStringLiteral("S15.967: the client lists [%1] (all selectable: %2); the nested KWin shows [%3]")
               .arg(got.join(u", "), allSelectable ? u"yes" : u"no", want.join(u", ")));
}

void sameFields() {
    // GOAL: S15.960 -- for the same screens, the library path yields the id, name, geometry, pixel
    //       size, scale, rotation, primary, refresh, enabled and mirror fields libkscreen itself reports.
    // WHY-THIS-SHAPE: the expectation is built in oracle() from libkscreen's getters, so a key the
    //       source's describe() writes wrong or drops shows up; comparing against parseKScreenConfig
    //       of the source's own map would compare the code with itself. Each state is reached with
    //       kscreen-doctor in a separate process, as a user's change would be.
    //       A disabled output is covered in parsedFields: KWin 6.6.80's virtual outputs ignore a disable.
    // CONTROL: app/kde/kscreen_library_source.cpp describe() writing "scale" as 1.0 -> red, "S15.960
    //       scaled and rotated: the library path's monitor differs" (re-run via control_batch.sh 2026-10-26).
    // COVERAGE-ASSERT: counts the compared fields and which kinds of state were compared (scaled,
    //       rotated, mirrored), so a doctor step that silently does nothing cannot pass for coverage.
    if (!baseline()) {
        return;
    }
    const RestoreBaseline restore;
    KScreenClient client(systemKScreenSource(QDBusConnection::sessionBus()));
    client.refresh();  // As main.cpp does once everything is wired.
    if (!waitReady(client, "S15.960 baseline")) {
        return;
    }
    int compared = 0;
    bool sawScaled = false, sawRotated = false, sawMirror = false;
    const auto step = [&](const QStringList& args, const QString& when) {
        if (!doctor(args)) {
            return false;
        }
        const auto want = oracle();
        if (!want) {
            return false;
        }
        // GOAL: S15.960 -- the change notice brings the client into agreement with libkscreen
        //       within 10 s of a change made outside the app.
        // WHY-THIS-SHAPE: the wait and compareFields share sameAsOracle(); a whole-Monitor `contains`
        //       also compares `panel`, which oracle() does not fill, so it never matched and the wait
        //       was a fixed 10 s sleep. An expired wait is graded once: compareFields names a field
        //       that still differs, and "only after the wait" covers a client that caught up late.
        // CONTROL: app/kde/kscreen_library_source.cpp, the ConfigMonitor connection skipped -> red,
        //       "S15.960 scaled and rotated: the library path's monitor differs" (control_batch.sh
        //       2026-10-26). Test-side, by hand: the predicate made `got.contains(w)` again -> red, "S15.960
        //       scaled and rotated: the client agreed with libkscreen only after the 10 s wait", and the
        //       same for mirrored (2026-10-26).
        const bool matched = waitFor([&] {
            return client.state() == State::Ready && agreesWithOracle(client.monitors(), *want);
        }, kWaitMs);
        if (!matched && agreesWithOracle(client.monitors(), *want)) {
            expect(false, when + QStringLiteral(": the client agreed with libkscreen only after the 10 s wait"));
        }
        compared += compareFields(client.monitors(), *want, when);
        for (const Monitor& w : *want) {
            sawScaled = sawScaled || w.scale != 1.0;
            sawRotated = sawRotated || w.rotation != 0;
            sawMirror = sawMirror || !w.mirrorOf.isEmpty();
        }
        return true;
    };
    // GOAL (harness): a failed read of the test's own libkscreen is reported as a harness fault only,
    //       never as the library path listing monitors libkscreen does not.
    // CONTROL: test-side, by hand: oracle() made to return nullopt after its harnessFailure on every
    //       call -> "FAIL (harness): the test's own GetConfigOperation failed", no S15.960 line (2026-10-26).
    const auto base = oracle();
    if (!base) {
        return;
    }
    compared += compareFields(client.monitors(), *base, QStringLiteral("S15.960 baseline"));
    if (!step({QStringLiteral("output.Virtual-1.scale.2"), QStringLiteral("output.Virtual-1.rotation.left")},
              QStringLiteral("S15.960 scaled and rotated"))) {
        return;
    }
    if (!step({QStringLiteral("output.Virtual-1.mirror.Virtual-0")}, QStringLiteral("S15.960 mirrored"))) {
        return;
    }
    expect(sawScaled && sawRotated && sawMirror && compared >= 3 * 2 * kOracleFields,
           QStringLiteral("S15.960 coverage: compared %1 field(s); scaled %2, rotated %3, mirrored %4")
               .arg(compared).arg(sawScaled).arg(sawRotated).arg(sawMirror));
}

void changeNotice() {
    // GOAL: S15.961 -- a change made outside the app reaches the client as monitorsChanged with the
    //       new values, and a selected output that stops being selectable clears the selection.
    // WHY-THIS-SHAPE: no refresh() is called by the test; only the library's change notice can
    //       produce the new answer. Every wait has a failure branch that names what never came.
    // CONTROL: app/kde/kscreen_library_source.cpp, each re-run via control_batch.sh 2026-10-26: the
    //       constructor's ConfigMonitor connection removed -> red, "S15.961: no monitorsChanged carried
    //       ..."; the finished handler emitting unavailable() for any config holding a replica (the
    //       mirror re-read fails) -> red, "... no Ready list showed the mirror with the selection
    //       cleared (state 2 ..., selection "" ...)" -- the selection was empty, so a wait on the
    //       empty selection alone passed this break. Restored, green.
    if (!baseline()) {
        return;
    }
    const RestoreBaseline restore;
    KScreenClient client(systemKScreenSource(QDBusConnection::sessionBus()));
    client.refresh();  // As main.cpp does once everything is wired.
    // waitReady reports its own failure; a refused select() is a separate one, reported once.
    // CONTROL: app/kde/kscreen_library_source.cpp, the finished handler emitting unavailable("control")
    //       for every answer -> one line only, "S15.961: the client is not Ready: control"
    //       (control_batch.sh 2026-10-26). Test-side, by hand: "Virtual-9" selected instead -> red, one line, "S15.961: setup --
    //       the Ready client refused to select Virtual-9 (it lists [Virtual-1, Virtual-0])" (2026-10-26).
    if (!waitReady(client, "S15.961")) {
        return;
    }
    if (const QString target = QStringLiteral("Virtual-1"); !client.select(target)) {
        QStringList listed;
        for (const Monitor& m : client.monitors()) listed.append(m.outputName);
        expect(false, QStringLiteral("S15.961: setup -- the Ready client refused to select %1 (it lists [%2])")
                          .arg(target, listed.join(u", ")));
        return;
    }
    int changes = 0;
    QObject::connect(&client, &KScreenClient::monitorsChanged, [&] { ++changes; });
    if (!doctor({QStringLiteral("output.Virtual-1.scale.2"), QStringLiteral("output.Virtual-1.position.0,768")})) {
        return;
    }
    const auto v1 = [&] {
        const auto ms = client.monitors();
        const auto it = std::ranges::find(ms, QStringLiteral("Virtual-1"), &Monitor::outputName);
        return it == ms.end() ? Monitor{} : *it;
    };
    expect(waitFor([&] { return changes > 0 && v1().scale == 2.0 && v1().geometry == QRectF(0, 768, 512, 384); }, kWaitMs),
           QStringLiteral("S15.961: no monitorsChanged carried Virtual-1 at scale 2, 0,768 512x384 within 10 s (%1 change(s); now %2)")
               .arg(changes).arg(describe(v1())));
    // A replica shows its source's desktop, so it is not selectable (disabling cannot be produced
    // on KWin's virtual outputs).
    if (!doctor({QStringLiteral("output.Virtual-1.mirror.Virtual-0")})) {
        return;
    }
    // GOAL: S15.961 -- on a Ready re-read that lists Virtual-1 as a mirror, the selection is cleared.
    // WHY-THIS-SHAPE: an empty selection alone also holds for any client that is not Ready (the
    //       selection reads empty then), so a re-read that failed would pass; Ready and the mirror
    //       actually listed are required in the same wait.
    // CONTROL: the second break in changeNotice's CONTROL above is the one for this wait.
    const bool cleared = waitFor([&] {
        return client.state() == State::Ready && v1().mirrorOf == u"Virtual-0" && client.selectedOutput().isEmpty();
    }, kWaitMs);
    expect(cleared, QStringLiteral("S15.961: within 10 s of Virtual-1 becoming a mirror of Virtual-0 outside the app, no Ready list "
                                   "showed the mirror with the selection cleared (state %1 \"%2\", selection \"%3\", Virtual-1 now %4)")
                        .arg(stateName(client.state()), client.unavailableReason(), client.selectedOutput(), describe(v1())));
}

void latestRead() {
    // GOAL: S15.963 -- when two reads overlap, the superseded one reports nothing: one answered()
    //       for two read() calls.
    // WHY-THIS-SHAPE: both reads return the same screens, so the shown list cannot tell them apart;
    //       the answer count can. The barrier is a GetConfigOperation of the test's own started after
    //       both: the library finishes operations in the order they were started on this thread.
    // CONTROL: app/kde/kscreen_library_source.cpp, the generation test at the top of the finished
    //       handler removed -> red, "S15.963: 2 answer(s) and 0 failure(s) ..." (re-run via
    //       control_batch.sh 2026-10-26).
    auto owned = systemKScreenSource(QDBusConnection::sessionBus());
    KScreenSource* source = owned.get();
    int answers = 0, failures = 0;
    QObject::connect(source, &KScreenSource::answered, [&] { ++answers; });
    QObject::connect(source, &KScreenSource::unavailable, [&] { ++failures; });
    source->read();
    source->read();
    KScreenLibrarySource* lib = dynamic_cast<KScreenLibrarySource*>(source);
    if (lib == nullptr) {
        harnessFailure("S15.963: the library source is not in use, so overlapping library reads cannot be made");
        return;
    }
    const bool settled = waitFor([&] { return answers + failures > 0; }, kWaitMs);
    KScreen::GetConfigOperation barrier;
    static_cast<void>(barrier.exec());
    spin(100);
    expect(settled && answers == 1 && failures == 0,
           QStringLiteral("S15.963: %1 answer(s) and %2 failure(s) for two overlapping reads; only the latest may report")
               .arg(answers).arg(failures));
}

void answeredInTime() {
    // GOAL: S15.969 -- a library read that answers within its time limit is never turned Unavailable
    //       afterwards by that limit running out.
    // WHY-THIS-SHAPE: the source is built with a 1.5 s limit, not the app's 25 s, and every state is
    //       recorded for 2.5 s after Ready, past the limit; checking only the final state would miss a
    //       brief Unavailable that a later read repaired. The library is warmed first, so the 1.5 s
    //       measures a read, not the one-off loading of its backend plugin.
    // CONTROL: app/kde/kscreen_library_source.cpp, `m_deadline.stop();` deleted from the finished
    //       handler -> red, "S15.969: after answering in time the client passed through [Ready,
    //       Unavailable]" (control_batch.sh 2026-10-26).
    KScreen::GetConfigOperation warm;
    if (!warm.exec()) {
        harnessFailure("S15.969: setup -- a warm-up read of the library did not complete: " + warm.errorString().toStdString());
        return;
    }
    KScreenClient client(std::make_unique<KScreenLibrarySource>(1500));
    QStringList states;
    QObject::connect(&client, &KScreenClient::stateChanged, [&] {
        states.append(stateName(client.state()) +
                      (client.state() == State::Unavailable ? QStringLiteral(" \"") + client.unavailableReason() + u'"' : QString()));
    });
    client.refresh();
    if (!waitFor([&] { return client.state() != State::Loading; }, kWaitMs) || client.state() != State::Ready) {
        expect(false, QStringLiteral("S15.969: the first read did not end Ready (states [%1])").arg(states.join(u", ")));
        return;
    }
    states.clear();
    states.append(QStringLiteral("Ready"));
    spin(2500);
    expect(states == QStringList{QStringLiteral("Ready")} && client.state() == State::Ready,
           QStringLiteral("S15.969: after answering in time the client passed through [%1]").arg(states.join(u", ")));
}

void destroyMidRead() {
    // GOAL: S15.964 -- a client destroyed while its library read is outstanding leaves nothing that
    //       runs into freed memory when the read completes.
    // WHY-THIS-SHAPE: the binary is built with AddressSanitizer, which ends the run on the first
    //       touch of freed memory; the read is made to complete afterwards by a GetConfigOperation of
    //       the test's own, so "the read never finished" cannot pass for "the read finished cleanly".
    // CONTROL: app/kde/kscreen_library_source.cpp, the finished-handler's context object changed from
    //       `this` to `operation` -> "AddressSanitizer: heap-use-after-free" (re-run via control_batch.sh
    //       2026-10-26).
    {
        auto client = std::make_unique<KScreenClient>(systemKScreenSource(QDBusConnection::sessionBus()));
        client->refresh();
    }
    KScreen::GetConfigOperation drain;
    check(drain.exec(), "S15.964: setup -- the read made after the client was destroyed did not complete");
    spin(200);
}

// ---------------------------------------------------------------- offscreen mode

void offWayland() {
    // GOAL: S15.962 -- with no Wayland connection the client ends Unavailable with the Wayland reason,
    //       is never Ready (with an empty list or otherwise), and refresh() emits nothing before it returns.
    // WHY-THIS-SHAPE: every state the client passes through is recorded, not only the final one, so a
    //       Ready that is later replaced still fails; emissions are counted across the refresh() call.
    // CONTROL: app/kde/kscreen_library_source.cpp, each re-run via control_batch.sh 2026-10-26:
    //       refusal(), which the constructor calls once, answering nullopt with no Wayland connection
    //       -> red, "S15.962: the client never
    //       became Unavailable off Wayland (state 1)"; read() emitting the refusal directly instead of
    //       through the zero timer -> red, "the first refresh() reported 1 outcome(s)" and "refresh() of
    //       an Unavailable client reported 1 outcome(s) before returning".
    KScreenClient client(systemKScreenSource(QDBusConnection::sessionBus()));
    QList<State> states;
    int emitted = 0;
    // Counts only a read's outcome: refresh() itself moves an Unavailable client to Loading, by design.
    QObject::connect(&client, &KScreenClient::stateChanged, [&] {
        states.append(client.state());
        emitted += client.state() != State::Loading;
    });
    QObject::connect(&client, &KScreenClient::monitorsChanged, [&] { ++emitted; });
    client.refresh();
    expect(emitted == 0, QStringLiteral("S15.962: the first refresh() reported %1 outcome(s) before returning").arg(emitted));
    if (!waitFor([&] { return client.state() == State::Unavailable; }, kWaitMs)) {
        expect(false, QStringLiteral("S15.962: the client never became Unavailable off Wayland (state %1)").arg(int(client.state())));
        return;
    }
    expect(client.unavailableReason() == u"Monitors can only be listed in a Wayland session.",
           QStringLiteral("S15.962: the reason off Wayland is \"%1\"").arg(client.unavailableReason()));
    emitted = 0;
    client.refresh();
    const int inside = emitted;
    expect(inside == 0, QStringLiteral("S15.962: refresh() of an Unavailable client reported %1 outcome(s) before returning").arg(inside));
    if (!waitFor([&] { return client.state() == State::Unavailable && emitted > 0; }, kWaitMs)) {
        expect(false, QStringLiteral("S15.962: after refresh() the client did not come back to Unavailable"));
    }
    spin(100);
    QStringList seen;
    for (State s : states) {
        seen.append(stateName(s));
    }
    expect(!states.contains(State::Ready) && client.state() == State::Unavailable,
           QStringLiteral("S15.962: off Wayland the client passed through states [%1]").arg(seen.join(u", ")));
}

// ---------------------------------------------------------------- hand-built outputs (no KWin)

// One output, given once and written two ways: as libkscreen's own objects for parseKScreenOutputs,
// and as the map KScreen's D-Bus getConfig returns, for parseKScreenConfig.
struct OutputSpec {
    int id;
    QString name;
    QSize px;
    float rate = 60.0f;
    QPoint pos;
    double scale = 1.0;
    int rotation = KScreen::Output::None;
    bool enabled = true;
    bool connected = true;
    uint priority = 0;
    int replicaOf = 0;
};

KScreen::OutputPtr libraryOutput(const OutputSpec& s) {
    KScreen::OutputPtr o(new KScreen::Output);
    o->setId(s.id);
    o->setName(s.name);
    KScreen::ModePtr mode(new KScreen::Mode);
    mode->setId(QStringLiteral("m%1").arg(s.id));
    mode->setSize(s.px);
    mode->setRefreshRate(s.rate);
    o->setModes({{mode->id(), mode}});
    o->setCurrentModeId(s.enabled ? mode->id() : QString());
    o->setSize(s.px);
    o->setPos(s.pos);
    o->setScale(s.scale);
    o->setRotation(KScreen::Output::Rotation(s.rotation));
    o->setConnected(s.connected);
    // libKF6Screen 6.8 ignores setEnabled() on an output without the Disable capability.
    o->setCapabilities(KScreen::Output::Capability::Disable);
    o->setEnabled(s.enabled);
    o->setPriority(s.priority);
    o->setSizeMm(QSize(600, 340));
    o->setReplicationSource(s.replicaOf);
    return o;
}

QVariantMap dbusOutput(const OutputSpec& s) {
    const auto size = [](QSize z) { return QVariantMap{{QStringLiteral("width"), z.width()}, {QStringLiteral("height"), z.height()}}; };
    const QString modeId = QStringLiteral("m%1").arg(s.id);
    return {{QStringLiteral("id"), s.id},
            {QStringLiteral("name"), s.name},
            {QStringLiteral("pos"), QVariantMap{{QStringLiteral("x"), s.pos.x()}, {QStringLiteral("y"), s.pos.y()}}},
            {QStringLiteral("scale"), s.scale},
            {QStringLiteral("size"), size(s.px)},
            {QStringLiteral("rotation"), s.rotation},
            {QStringLiteral("currentModeId"), s.enabled ? modeId : QString()},
            {QStringLiteral("connected"), s.connected},
            {QStringLiteral("enabled"), s.enabled},
            {QStringLiteral("priority"), s.priority},
            {QStringLiteral("sizeMM"), size(QSize(600, 340))},
            {QStringLiteral("replicationSource"), s.replicaOf},
            {QStringLiteral("modes"), QVariantList{QVariantMap{{QStringLiteral("id"), modeId},
                                                               {QStringLiteral("size"), size(s.px)},
                                                               {QStringLiteral("refreshRate"), double(s.rate)}}}}};
}

void parsedFields() {
    // GOAL: S15.960 -- for a disabled output (beside a scaled-and-rotated one and a replica),
    //       parseKScreenOutputs yields exactly the Monitor parseKScreenConfig yields on the equivalent
    //       D-Bus getConfig reply. False if app/kde/kscreen_library_source.cpp describe() drops or
    //       mistranslates a field, e.g. reports a disabled output as enabled.
    // WHY-THIS-SHAPE: hand-built KScreen::Output objects, because KWin 6.6.80's virtual outputs ignore
    //       kscreen-doctor's disable. The D-Bus map is written by the test from the same spec, never
    //       through describe(), so describe() is compared with the reply shape and not with itself.
    //       Each output carries the Disable capability, without which libKF6Screen 6.8 ignores setEnabled.
    // CONTROL: app/kde/kscreen_library_source.cpp describe() writing "enabled" as true -> red,
    //       "S15.960 hand-built HDMI-A-1: the library path gives ... en 1 ... the D-Bus reply gives
    //       ... en 0" (control_batch.sh, 2026-10-26).
    // COVERAGE-ASSERT: the D-Bus parse must itself show a disabled, a scaled-and-rotated and a
    //       mirrored monitor, or the comparison is a harness failure rather than a pass.
    const QList<OutputSpec> specs{
        {.id = 1, .name = QStringLiteral("DP-1"), .px = {2560, 1440}, .rate = 144.0f, .scale = 1.5,
         .rotation = KScreen::Output::Left, .priority = 1},
        {.id = 2, .name = QStringLiteral("HDMI-A-1"), .px = {1920, 1080}, .pos = {1440, 0}, .enabled = false},
        {.id = 3, .name = QStringLiteral("DP-2"), .px = {2560, 1440}, .priority = 2, .replicaOf = 1},
    };
    KScreen::OutputList outputs;
    QVariantList described;
    for (const OutputSpec& s : specs) {
        outputs.insert(s.id, libraryOutput(s));
        described.append(dbusOutput(s));
    }
    const auto want = parseKScreenConfig({{QStringLiteral("outputs"), described}});
    const auto got = parseKScreenOutputs(outputs);
    if (!want || want->size() != 3) {
        harnessFailure("S15.960: the test's D-Bus reply was refused: " + (want ? std::string("wrong size") : want.error().toStdString()));
        return;
    }
    const auto find = [](const QList<Monitor>& ms, const QString& name) {
        const auto it = std::ranges::find(ms, name, &Monitor::outputName);
        return it == ms.end() ? std::optional<Monitor>{} : std::optional<Monitor>{*it};
    };
    const auto hdmi = find(*want, QStringLiteral("HDMI-A-1")), dp1 = find(*want, QStringLiteral("DP-1")),
               dp2 = find(*want, QStringLiteral("DP-2"));
    if (!hdmi || hdmi->enabled || !dp1 || dp1->scale != 1.5 || dp1->rotation != 90 || !dp2 || dp2->mirrorOf != u"DP-1") {
        harnessFailure("S15.960: the test's D-Bus reply does not describe a disabled, a scaled-and-rotated and a mirrored monitor");
        return;
    }
    if (!got) {
        expect(false, QStringLiteral("S15.960 hand-built: parseKScreenOutputs refused outputs the D-Bus parser accepts: ") + got.error());
        return;
    }
    expect(got->monitors.size() == want->size(),
           QStringLiteral("S15.960 hand-built: the library path gives %1 monitor(s), the D-Bus reply %2").arg(got->monitors.size()).arg(want->size()));
    for (const Monitor& w : *want) {
        const auto g = find(got->monitors, w.outputName);
        expect(g && *g == w, QStringLiteral("S15.960 hand-built %1: the library path gives\n  %2 (panel '%3')\n  the D-Bus reply gives\n  %4 (panel '%5')")
                                 .arg(w.outputName, g ? describe(*g) : QStringLiteral("nothing"), g ? g->panel : QString(),
                                      describe(w), w.panel));
    }
}

// A base EDID block libkscreen accepts: header, manufacturer DEL, version 1.4, a detailed timing,
// then `name` as the monitor-name descriptor (0xFC) or, with `named` false, as free text (0xFE)
// that is not a name, and a checksum.
QByteArray realEdid(const QByteArray& name, bool named) {
    QByteArray e(128, '\0');
    e.replace(0, 8, QByteArray("\x00\xff\xff\xff\xff\xff\xff\x00", 8));
    e[8] = '\x10'; e[9] = '\xac';           // "DEL"
    e[10] = '\x2e'; e[11] = '\xa1';         // product code
    e[16] = 10; e[17] = 30;                 // week 10 of 2020
    e[18] = 1; e[19] = 4;                   // EDID 1.4
    e[20] = '\xa5'; e[21] = 60; e[22] = 34; // digital input, 60 x 34 cm
    e[54] = '\x56'; e[55] = '\x5e';         // descriptor 0: a detailed timing (non-zero pixel clock)
    const int at = 54 + 18;
    e[at + 3] = named ? '\xfc' : '\xfe';
    QByteArray text = name.left(13);
    if (text.size() < 13) text.append('\n');
    e.replace(at + 5, 13, text.leftJustified(13, ' '));
    for (int slot : {2, 3}) e[54 + 18 * slot + 3] = '\x10';  // dummy descriptors
    int sum = 0;
    for (int i = 0; i < 127; ++i) sum += quint8(e[i]);
    e[127] = char((256 - sum % 256) % 256);
    return e;
}

void edidNames() {
    // GOAL: S15.965 -- parseKScreenOutputs gives a connected output's EDID model name, an empty name
    //       entry for an EDID with none (the client, not the source, falls back to the connector), and
    //       no entry for a disconnected output or one with no EDID. False if
    //       app/kde/kscreen_library_source.cpp ignores the EDID, fills in a name, or names outputs it should skip.
    // WHY-THIS-SHAPE: one call over four hand-built outputs, compared with the exact expected list,
    //       so an extra entry fails as surely as a wrong one; the EDIDs are checked valid by
    //       libkscreen's own Edid first, so a malformed fixture is a harness failure, not a pass.
    // CONTROL: app/kde/kscreen_library_source.cpp, each control_batch.sh 2026-10-26, red on "S15.965: names
    //       [...]": the EDID read made QString() -> [1 '', 2 '']; the connector name put back in for an
    //       empty model name -> [..., 2 'HDMI-A-1']; `|| !monitor.connected` removed -> [..., 3 'DELL
    //       U2720Q']; a missing EDID read as an empty one -> [..., 4 '']. The client's fallback itself
    //       is checked in kscreen_client_test.cpp (S15.18, "DP-3 should be named DP-3").
    const QByteArray withName = realEdid("DELL U2720Q", true), withoutName = realEdid("NOT A NAME", false);
    if (!KScreen::Edid(withName).isValid() || !KScreen::Edid(withoutName).isValid() ||
        KScreen::Edid(withName).name() != u"DELL U2720Q") {
        harnessFailure("S15.965: libkscreen's Edid does not accept the test's EDID bytes as a valid block naming DELL U2720Q");
        return;
    }
    KScreen::OutputList outputs;
    const auto add = [&](int id, const QString& name, bool connected, const QByteArray* edid) {
        KScreen::OutputPtr o = libraryOutput({.id = id, .name = name, .px = {1920, 1080}, .pos = {1920 * (id - 1), 0},
                                              .enabled = connected, .connected = connected});
        if (edid != nullptr) o->setEdid(*edid);
        outputs.insert(id, o);
    };
    add(1, QStringLiteral("DP-1"), true, &withName);
    add(2, QStringLiteral("HDMI-A-1"), true, &withoutName);
    add(3, QStringLiteral("DP-2"), false, &withName);
    add(4, QStringLiteral("DP-3"), true, nullptr);
    const auto got = parseKScreenOutputs(outputs);
    if (!got) {
        expect(false, QStringLiteral("S15.965: parseKScreenOutputs refused the outputs: ") + got.error());
        return;
    }
    const QList<KScreenModelName> want{{1, QStringLiteral("DELL U2720Q")}, {2, QString()}};
    QStringList seen;
    for (const KScreenModelName& n : got->names) {
        seen.append(QStringLiteral("%1 '%2'").arg(n.kscreenId).arg(n.modelName));
    }
    expect(got->names == want,
           QStringLiteral("S15.965: names [%1]; want [1 'DELL U2720Q', 2 ''] (3 disconnected and 4 without an EDID get none)")
               .arg(seen.join(u", ")));
    bool connectorNames = got->monitors.size() == 4;
    for (const Monitor& m : got->monitors) connectorNames = connectorNames && m.displayName == m.outputName;
    expect(connectorNames, QStringLiteral("S15.965: the monitors answered() carries are not each named by their connector"));
}

// ---------------------------------------------------------------- oldlib mode (6.7.5 loaded)

QString startedMarker() { return qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/kscreen-launcher-started"); }

int countStarts() {
    QFile f(startedMarker());
    return f.open(QIODevice::ReadOnly) ? int(f.readAll().count('\n')) : 0;
}

void oldLibrary() {
    // GOAL: S15.959 -- with libKF6Screen 6.7.5 loaded, systemKScreenSource gives the D-Bus source.
    // WHY-THIS-SHAPE: asked by type and of inProcess(), not by counting launcher starts: constructing
    //       either source starts nothing, and 6.7.5 starts the launcher only for its X11 (XRandR)
    //       backend, never its Wayland one, so no wrong choice here is seen by the counter.
    // CONTROL: app/kde/kscreen_library_source.cpp inProcess() returns true -> red, "inProcess() says the
    //       backend runs in this process" and "did not give the D-Bus source" (re-run via control_batch.sh
    //       2026-10-26).
    check(!KScreenLibrarySource::inProcess(), "S15.959: with libKF6Screen 6.7.5 loaded, inProcess() says the backend runs in this process");
    {
        auto source = systemKScreenSource(QDBusConnection::sessionBus());
        check(dynamic_cast<KScreenDBusSource*>(source.get()) != nullptr,
              "S15.959: with libKF6Screen 6.7.5 loaded, systemKScreenSource did not give the D-Bus source");
        spin(1500);
    }
    // GOAL: choosing a source starts nothing on the bus; only a read may start KScreen's launcher.
    // WHY-THIS-SHAPE: the bus can start one thing, a stand-in for org.kde.KScreen that appends a line
    //       to a file when started, so a start is counted rather than inferred from a missing reply.
    // CONTROL: app/kde/kscreen_source.cpp, systemKScreenSource calling read() on the D-Bus source it
    //       returns -> red, "choosing a source ... started KScreen's launcher 1 time(s)" (re-run via
    //       control_batch.sh 2026-10-26).
    const int before = countStarts();
    expect(before == 0, QStringLiteral("S15.959: choosing a source with libKF6Screen 6.7.5 loaded started KScreen's launcher %1 time(s)").arg(before));
    // The recorder proves itself: a start the test asks for must leave its line. Not waited on: the
    // stand-in never takes the name, so the bus answers only after its own timeout.
    QDBusConnection::sessionBus().interface()->asyncCall(QStringLiteral("StartServiceByName"), QStringLiteral("org.kde.KScreen"), 0u);
    if (!waitFor([] { return countStarts() > 0; }, 5000)) {
        harnessFailure("the org.kde.KScreen stand-in did not record a start the test asked for, so its silence above proves nothing");
    }
}

// The canonical path of every libKF6Screen this process has loaded.
QStringList loadedKScreenLibraries() {
    QStringList found;
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* out) {
            const std::filesystem::path name(info->dlpi_name != nullptr ? info->dlpi_name : "");
            if (name.filename().string().starts_with("libKF6Screen.so")) {
                std::error_code ec;
                const auto real = std::filesystem::canonical(name, ec);
                static_cast<QStringList*>(out)->append(QString::fromStdString(ec ? name.string() : real.string()));
            }
            return 0;
        },
        &found);
    return found;
}

int standIn() {
    QFile f(startedMarker());
    if (f.open(QIODevice::Append)) {
        f.write("started\n");
    }
    return 0;  // Exits without taking the name: a start is all that is recorded.
}

}  // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "kscreen_library";
    const QByteArray mode = argc > 1 ? QByteArray(argv[1]) : QByteArray();
    if (mode == "--stand-in") {
        return standIn();
    }
    if (const auto refused = refuseUnlessUnderDbusRunSession()) {
        return *refused;
    }
    QGuiApplication app(argc, argv);
    // GOAL (harness): oldlib's bus can start nothing but this binary's own stand-in. The shared guard
    //       below proves only the activatable name, and org.kde.KScreen is also the real launcher's.
    // WHY-THIS-SHAPE: read from the service directory CMake generated for the entry: it must hold
    //       exactly one file, whose Name is org.kde.KScreen and whose Exec is this very binary with
    //       --stand-in. Refused (exit 1) before anything can ask the bus to start it.
    // CONTROL: test-side, by hand: a second file, other.service (Exec=/bin/false), dropped into
    //       kscreen_old_services -> exit 1, "... holds [org.kde.KScreen.service, other.service]"; the
    //       stand-in's Exec made /bin/false -> exit 1, "... Exec '/bin/false'" (both 2026-10-26).
    if (mode == "oldlib") {
        const QDir dir(qEnvironmentVariable("WALLPAPERX_KSCREEN_OLD_SERVICES"));
        const QStringList files = dir.entryList(QDir::Files | QDir::Hidden);
        QString name, exec;
        if (QFile f(dir.filePath(QStringLiteral("org.kde.KScreen.service"))); f.open(QIODevice::ReadOnly)) {
            for (const QString& line : QString::fromUtf8(f.readAll()).split(u'\n')) {
                if (line.startsWith(u"Name=")) name = line.mid(5);
                if (line.startsWith(u"Exec=")) exec = line.mid(5);
            }
        }
        const QString self = QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
        if (dir.path().isEmpty() || files != QStringList{QStringLiteral("org.kde.KScreen.service")} ||
            name != u"org.kde.KScreen" || exec != u'\'' + self + QStringLiteral("' --stand-in")) {
            harnessFailure("oldlib's service directory '" + dir.path().toStdString() + "' holds [" + files.join(u", ").toStdString() +
                           "] with Name '" + name.toStdString() + "' and Exec '" + exec.toStdString() +
                           "'; it may hold only org.kde.KScreen.service starting this binary with --stand-in");
            static_cast<void>(wallpaperx::testing::reportHarnessFaults("in the private bus this run was given, not in the app"));
            return 1;
        }
    }
    // oldlib's bus may start one thing, the org.kde.KScreen stand-in this binary serves as.
    if (const auto refused = refuseUnlessPrivateBus(mode == "oldlib" ? QStringList{QStringLiteral("org.kde.KScreen")} : QStringList{})) {
        return *refused;
    }
    if (mode == "display") {
        if (QGuiApplication::platformName() != u"wayland") {
            harnessFailure("the display entry is not on the nested KWin (platform " + QGuiApplication::platformName().toStdString() + ")");
        } else {
            if (phaseSelected("libraryChosen")) guardedPhase("libraryChosen", libraryChosen);
            if (phaseSelected("nestedListing")) guardedPhase("nestedListing", nestedListing);
            if (phaseSelected("sameFields")) guardedPhase("sameFields", sameFields);
            if (phaseSelected("changeNotice")) guardedPhase("changeNotice", changeNotice);
            if (phaseSelected("latestRead")) guardedPhase("latestRead", latestRead);
            if (phaseSelected("answeredInTime")) guardedPhase("answeredInTime", answeredInTime);
            if (phaseSelected("destroyMidRead")) guardedPhase("destroyMidRead", destroyMidRead);
        }
    } else if (mode == "offscreen") {
        if (QGuiApplication::platformName() != u"offscreen") {
            harnessFailure("the offscreen entry runs on " + QGuiApplication::platformName().toStdString());
        } else {
            if (phaseSelected("offWayland")) guardedPhase("offWayland", offWayland);
            if (phaseSelected("parsedFields")) guardedPhase("parsedFields", parsedFields);
            if (phaseSelected("edidNames")) guardedPhase("edidNames", edidNames);
        }
    } else if (mode == "oldlib") {
        // GOAL (harness): the oldlib checks run only with the extracted libKF6Screen 6.7.5 as the one
        //       copy loaded; any other copy is the harness's fault, never a failure of the app.
        // WHY-THIS-SHAPE: read from the loader's own list of loaded objects, canonical paths compared,
        //       not from LD_LIBRARY_PATH being set: a loader that ignored it would load the installed
        //       copy, whose correct answer the checks would then report as an app failure.
        // CONTROL: test-side, by hand 2026-10-26: the entry run with LD_LIBRARY_PATH emptied ->
        //       "HARNESS FAULT: ... libKF6Screen 6.7.5 is not the copy loaded ... /usr/lib/...", no
        //       S15.959 FAIL; restored, passes.
        const QString wanted = QString::fromStdString(
            std::filesystem::weakly_canonical(qEnvironmentVariable("LD_LIBRARY_PATH").toStdString() + "/libKF6Screen.so.6.7.5").string());
        const QStringList loaded = loadedKScreenLibraries();
        if (loaded != QStringList{wanted}) {
            harnessFailure("libKF6Screen 6.7.5 is not the copy loaded: this process holds [" + loaded.join(u", ").toStdString() +
                           "], the oldlib entry needs exactly " + wanted.toStdString());
        } else if (phaseSelected("oldLibrary")) {
            guardedPhase("oldLibrary", oldLibrary);
        }
    } else {
        harnessFailure("unknown mode '" + mode.toStdString() + "'");
    }
    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file or its nested KWin, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
