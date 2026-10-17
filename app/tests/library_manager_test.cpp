// SPDX-License-Identifier: GPL-2.0-or-later

// app/library/library_manager.cpp and app/library/folder_watcher.cpp against real library
// folders, each wallpaper a folder of its own: the library's states, its scans, the open wallpaper
// and its edited values, reveal(), and the watch that re-reads the library by itself. No bus.
// Goals S15.36-S15.44, S15.48, S15.49, S15.57, S15.69 in GOALS.md.
//
// Scans are counted inside this process by open_counter.cpp: each scan opens the library folder
// once off the main thread (opendir).

#include "library_manager.h"
#include "open_counter.h"
#include "openat2_shim.h"
#include "wpx_fixtures.h"
#include "wpx_package.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThreadPool>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>

#include <sys/stat.h>
#include <unistd.h>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

namespace {

constexpr int kSettleMs = 30;
// Every wait below ends as soon as its condition holds; this is only the failure bound.
constexpr int kWaitMs = 3000;

struct Counts {
    int state = 0;
    int list = 0;
    int selection = 0;
    int values = 0;
};

void countSignals(LibraryManager& m, Counts& c) {
    QObject::connect(&m, &LibraryManager::stateChanged, [&c] { ++c.state; });
    QObject::connect(&m, &LibraryManager::wallpapersChanged, [&c] { ++c.list; });
    QObject::connect(&m, &LibraryManager::selectionChanged, [&c] { ++c.selection; });
    QObject::connect(&m, &LibraryManager::propertyValuesChanged, [&c] { ++c.values; });
}

QString titles(const QList<Wallpaper>& list) {
    QStringList out;
    for (const Wallpaper& w : list) {
        out.append(w.usable() ? w.title : w.fileName + QStringLiteral("(broken)"));
    }
    return out.join(u',');
}

QString describe(const QVariantMap& values) {
    QStringList out;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        out.append(it.key() + QLatin1Char('=') + it.value().toString());
    }
    return QLatin1Char('{') + out.join(u", ") + QLatin1Char('}');
}

const Wallpaper* find(const QList<Wallpaper>& list, const QString& fileName) {
    const auto it = std::ranges::find(list, fileName, &Wallpaper::fileName);
    return it == list.end() ? nullptr : &*it;
}

// A library folder holding one wallpaper folder per (name, manifest), with PLAN's other files.
// Empty string on success.
[[nodiscard]] QString makeLibrary(const QString& dir, const QList<QPair<QString, QByteArray>>& wallpapers) {
    if (!QDir().mkpath(dir)) {
        return QStringLiteral("could not make %1").arg(dir);
    }
    for (const auto& [name, manifest] : wallpapers) {
        if (const QString why = writeFolder(dir + QLatin1Char('/') + name, planEntries(manifest)); !why.isEmpty()) {
            return why;
        }
    }
    return {};
}

bool readyWith(LibraryManager& m, qsizetype count) {
    return waitUntil([&] { return m.state() == LibraryManager::State::Ready && m.wallpapers().size() == count; }, kWaitMs);
}

// S15.36
void states(const QString& root) {
    const QString dir = root + QStringLiteral("/states");
    if (const QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
        fault("S15.36", why);
        return;
    }
    LibraryManager m(dir, kSettleMs);
    // GOAL: S15.36 -- the library is Loading, with an empty list, until its first scan has been
    //       taken, and only then Ready.
    // WHY-THIS-SHAPE: checked right after rescan() too, before the event loop runs, which is when a
    //       manager that settled Ready eagerly (before the scan came back) would show it.
    // CONTROL: app/library/library_manager.h, `State m_state = State::Loading;` made
    //       `State::Ready` -> red, "S15.36: before its first scan the library is 1 with 0
    //       wallpaper(s)" (1 is Ready); restored.
    const bool loadingBefore = m.state() == LibraryManager::State::Loading && m.wallpapers().isEmpty();
    m.rescan();
    const bool loadingDuring = m.state() == LibraryManager::State::Loading;
    expect(loadingBefore && loadingDuring,
           QStringLiteral("S15.36: before its first scan the library is %1 with %2 wallpaper(s)").arg(int(m.state())).arg(m.wallpapers().size()));
    if (!readyWith(m, 1)) {
        expect(false, QStringLiteral("S15.36: the first scan of a library with one wallpaper never settled Ready with it; state %1, [%2]")
                          .arg(int(m.state())).arg(titles(m.wallpapers())));
        return;
    }

    // GOAL: S15.36 -- whenever stateChanged or wallpapersChanged is emitted, state, reason and list
    //       already agree: Unavailable has a reason and an empty list, Ready has no reason.
    // WHY-THIS-SHAPE: each emission is recorded with the three fields as a handler reads them, and
    //       each record is checked against the invariant, so an emission between two of the writes
    //       is caught whichever order they come in.
    // CONTROL: app/library/library_manager.cpp, settle()'s `m_model.assign(std::move(wallpapers));`
    //       moved after `emit stateChanged();` -> red, "S15.36: a handler saw Unavailable with 1
    //       wallpaper(s)"; rescan()'s Ready settle given `m_unavailableReason` instead of `{}` -> red,
    //       "a handler saw state 1 with 1 wallpaper(s) and reason "…/states is not a folder.""; restored.
    // COVERAGE-ASSERT: both signals must have been seen in both directions.
    struct Seen {
        LibraryManager::State state;
        qsizetype count;
        QString reason;
    };
    QList<Seen> seenState;
    QList<Seen> seenList;
    QObject::connect(&m, &LibraryManager::stateChanged, [&] { seenState.append({m.state(), m.wallpapers().size(), m.unavailableReason()}); });
    QObject::connect(&m, &LibraryManager::wallpapersChanged, [&] { seenList.append({m.state(), m.wallpapers().size(), m.unavailableReason()}); });

    // GOAL: S15.36 -- a path that exists but is a file is Unavailable, saying so, with an empty list.
    // CONTROL: app/library/library_manager.cpp, rescan()'s `settle(State::Unavailable, ...)` made
    //       `settle(State::Ready, {}, {})` -> red, "S15.36: a path that is a file read as state 1"; restored.
    const QString moved = dir + QStringLiteral(".moved");
    if (!QDir().rename(dir, moved) || !writeBytes(dir, "a file")) {
        harnessFailure("S15.36: the library folder could not be replaced by a file");
        return;
    }
    m.rescan();
    const bool unavailable = waitUntil([&] { return m.state() == LibraryManager::State::Unavailable; }, kWaitMs);
    expect(unavailable && m.unavailableReason() == dir + QStringLiteral(" is not a folder.") && m.wallpapers().isEmpty(),
           QStringLiteral("S15.36: a path that is a file read as state %1, \"%2\", with %3 wallpaper(s)")
               .arg(int(m.state())).arg(m.unavailableReason()).arg(m.wallpapers().size()));

    if (!QFile::remove(dir) || !QDir().rename(moved, dir)) {
        harnessFailure("S15.36: the library folder could not be put back");
        return;
    }
    m.rescan();
    // Each wait or call is made before its expect(): the message argument may be built first.
    const bool cameBack = readyWith(m, 1);
    // CONTROL: app/library/library_manager.cpp:132, rescan's Ready settle given `m_unavailableReason`,
    //       control_batch 2026-10-15, app_library_manager '^states$' -> "FAIL: S15.36: after the library folder
    //       came back it is state 1, "…/states is not a folder.", [Ocean Waves]", with the handler-record line;
    //       restored, green.
    expect(cameBack && m.unavailableReason().isEmpty(),
           QStringLiteral("S15.36: after the library folder came back it is state %1, \"%2\", [%3]")
               .arg(int(m.state())).arg(m.unavailableReason(), titles(m.wallpapers())));

    int unavailableSeen = 0;
    int readySeen = 0;
    for (const QList<Seen>* records : {&seenState, &seenList}) {
        for (const Seen& s : *records) {
            if (s.state == LibraryManager::State::Unavailable) {
                ++unavailableSeen;
                expect(s.count == 0 && !s.reason.isEmpty(),
                       QStringLiteral("S15.36: a handler saw Unavailable with %1 wallpaper(s) and reason \"%2\"").arg(s.count).arg(s.reason));
            } else {
                ++readySeen;
                expect(s.state == LibraryManager::State::Ready && s.reason.isEmpty() && s.count == 1,
                       QStringLiteral("S15.36: a handler saw state %1 with %2 wallpaper(s) and reason \"%3\"")
                           .arg(int(s.state)).arg(s.count).arg(s.reason));
            }
        }
    }
    expect(unavailableSeen == 2 && readySeen == 2,
           QStringLiteral("S15.36: expected each signal once each way; saw %1 Unavailable and %2 Ready record(s)").arg(unavailableSeen).arg(readySeen));

    // GOAL: S15.36 -- a library folder that does not exist is Ready and empty, not Unavailable.
    // CONTROL: app/library/wpx_package.cpp, scanLibrary's `if (error == ENOENT)` made `if (false)`
    //       -> red, "S15.36: a missing library folder read as state 2, "…/no/such/folder cannot be
    //       read: No such file or directory.""; restored, green.
    LibraryManager missing(root + QStringLiteral("/no/such/folder"), kSettleMs);
    missing.rescan();
    const bool missingReady = readyWith(missing, 0);
    expect(missingReady && missing.unavailableReason().isEmpty(),
           QStringLiteral("S15.36: a missing library folder read as state %1, \"%2\"").arg(int(missing.state())).arg(missing.unavailableReason()));

    if (::geteuid() == 0) {
        notVerified("S15.36 (a folder that cannot be listed)", "running as root, which can list a mode-000 folder");
        return;
    }
    const QString closed = root + QStringLiteral("/closed");
    if (!QDir().mkpath(closed) || !setMode(closed, 0)) {
        harnessFailure("S15.36: the mode-000 folder could not be made");
        return;
    }
    LibraryManager denied(closed, kSettleMs);
    denied.rescan();
    const bool deniedSettled = waitUntil([&] { return denied.state() != LibraryManager::State::Loading; }, kWaitMs);
    if (!setMode(closed, 0755)) {
        harnessFailure("S15.36: the mode-000 folder could not be made readable again");
    }
    // CONTROL: app/library/wpx_package.cpp:795 and :837, scanLibrary's `systemError(error)` made `QString()`,
    //       control_batch 2026-10-15, app_library_manager '^states$' -> "FAIL: S15.36: a folder that cannot be
    //       listed read as state 2, "…/closed cannot be read: .""; restored, green.
    expect(deniedSettled && denied.state() == LibraryManager::State::Unavailable &&
               denied.unavailableReason() == closed + QStringLiteral(" cannot be read: Permission denied."),
           QStringLiteral("S15.36: a folder that cannot be listed read as state %1, \"%2\"").arg(int(denied.state())).arg(denied.unavailableReason()));
}

// S15.48
void overflowingManifest(const QString& root) {
    const QString dir = root + QStringLiteral("/overflow");
    QByteArray overflow(kPlanManifest);
    overflow.replace("\"default\": 1.0", "\"default\": 1e999");
    if (const QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}, {QStringLiteral("overflow"), overflow}});
        !why.isEmpty() || !overflow.contains("1e999")) {
        fault("S15.48", why.isEmpty() ? QStringLiteral("the 1e999 manifest could not be made") : why);
        return;
    }
    // GOAL: S15.48 -- a library holding a wallpaper whose manifest has a number too large for a
    //       double (1e999) settles Ready: that wallpaper is listed broken with its reason beside the
    //       usable one, and the state never stays Loading.
    // WHY-THIS-SHAPE: the wait is for any settled state, then the state and reason are printed, so
    //       a scan that threw is told apart from one stuck in Loading. The scan-failure handler it
    //       would reach is not reachable by any input today (see CONTROL).
    // CONTROL: app/library/wpx_package.cpp, parseManifest's `catch (const Json::out_of_range&)`
    //       clause deleted (the generic `Json::exception` catch takes it) -> red, "S15.48: ... reason
    //       ... wallpaper.json cannot be read: [json.exception.out_of_range.406] ..."; both that and
    //       the `Json::exception` clause deleted, so the throw leaves the scan -> red, "state 2
    //       ... Reading …/overflow failed: [json.exception.out_of_range.406] number overflow ...",
    //       which is library_manager.cpp's onFailed handler settling Unavailable; those two plus
    //       rescan()'s `.onFailed(...)` block deleted -> red, "state 0" (stuck Loading); restored.
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    const bool settled = waitUntil([&] { return m.state() != LibraryManager::State::Loading; }, kWaitMs);
    const Wallpaper* bad = find(m.wallpapers(), QStringLiteral("overflow"));
    expect(settled && m.state() == LibraryManager::State::Ready && titles(m.wallpapers()) == u"Ocean Waves,overflow(broken)" && bad &&
               bad->problem == u"wallpaper.json holds a number too large to read.",
           QStringLiteral("S15.48: a library holding a 1e999 manifest is state %1 (\"%2\") listing [%3]; the bad one's reason is \"%4\"")
               .arg(int(m.state())).arg(m.unavailableReason(), titles(m.wallpapers()), bad ? bad->problem : QString()));
}

// S15.49
void folderMadeReadable(const QString& root) {
    if (::geteuid() == 0) {
        notVerified("S15.49", "running as root, which can list a mode-000 folder");
        return;
    }
    // GOAL: S15.49 -- a library folder that could not be listed and is then made readable is
    //       read again without rescan().
    // WHY-THIS-SHAPE: a mode change leaves no trace but an attribute event, which the kernel
    //       reports to the parent's watch; waiting for Unavailable first proves the first read
    //       was refused.
    // CONTROL: app/library/folder_watcher.cpp, IN_ATTRIB deleted from kAncestorEvents -> red,
    //       "S15.49: an unreadable library folder made readable still reads state 2"; restored.
    {
        const QString dir = root + QStringLiteral("/opened");
        if (const QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}}); !why.isEmpty() || !setMode(dir, 0)) {
            fault("S15.49", why.isEmpty() ? QStringLiteral("the library folder could not be made mode 000") : why);
            return;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        const bool refused = waitUntil([&] { return m.state() == LibraryManager::State::Unavailable; }, kWaitMs);
        if (!setMode(dir, 0755)) {
            fault("S15.49", QStringLiteral("the library folder could not be made readable"));
            return;
        }
        if (!refused) {
            expect(false, QStringLiteral("S15.49: a mode-000 library folder read as state %1, not Unavailable").arg(int(m.state())));
        } else {
            const bool read = readyWith(m, 1);
            expect(read, QStringLiteral("S15.49: an unreadable library folder made readable still reads state %1 [%2]").arg(int(m.state())).arg(titles(m.wallpapers())));
        }
    }

    // GOAL: S15.49 -- when the library folder is missing and its nearest existing ancestor cannot
    //       be read, that ancestor being made readable, and then the folder appearing, are both
    //       noticed.
    // WHY-THIS-SHAPE: the unreadable ancestor refuses a watch too, so only a watch on the next one
    //       up sees the mode change; the folder is created afterwards to show the watch moved down.
    // CONTROL: app/library/folder_watcher.cpp, placeWatch()'s `if (at == "/") {` made
    //       `if (!awaited.isEmpty()) {` (it stops after one ancestor) -> red, "S15.49: after its
    //       unreadable parent was made readable the missing library folder reads state 2"; restored.
    const QString parent = root + QStringLiteral("/sealed");
    const QString dir = parent + QStringLiteral("/wallpapers");
    if (!QDir().mkpath(parent) || !setMode(parent, 0)) {
        fault("S15.49", QStringLiteral("the mode-000 parent could not be made"));
        return;
    }
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    const bool refused = waitUntil([&] { return m.state() == LibraryManager::State::Unavailable; }, kWaitMs);
    if (!setMode(parent, 0755)) {
        fault("S15.49", QStringLiteral("the parent could not be made readable"));
        return;
    }
    if (!refused) {
        expect(false, QStringLiteral("S15.49: a library folder under a mode-000 parent read as state %1, not Unavailable").arg(int(m.state())));
        return;
    }
    const bool readable = readyWith(m, 0);
    expect(readable && m.unavailableReason().isEmpty(),
           QStringLiteral("S15.49: after its unreadable parent was made readable the missing library folder reads state %1, \"%2\"")
               .arg(int(m.state())).arg(m.unavailableReason()));
    if (readable) {
        if (const QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
            fault("S15.49", why);
            return;
        }
        const bool listed = readyWith(m, 1);
        expect(listed, QStringLiteral("S15.49: the library folder created under the parent made readable was not read; [%1]").arg(titles(m.wallpapers())));
    }
}

// S15.37
void queuedScans(const QString& root) {
    const QString dir = root + QStringLiteral("/queued");
    if (const QString why = makeLibrary(dir, {{QStringLiteral("old"), titled("Old")}}); !why.isEmpty()) {
        fault("S15.37", why);
        return;
    }
    OpenCounter scans(dir);
    if (!scans.ok()) {
        harnessFailure("S15.37: the open counter was already counting another folder");
        return;
    }
    // A quiet period no test outlasts, so the folder's own watch cannot start a scan of its own.
    LibraryManager m(dir, 600000);
    m.rescan();
    if (!readyWith(m, 1)) {
        expect(false, QStringLiteral("S15.37: the first scan never settled; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    QStringList history;
    QObject::connect(&m, &LibraryManager::wallpapersChanged, [&] { history.append(titles(m.wallpapers())); });

    // GOAL: S15.37 -- a scan running when rescan() is called again, any number of times, is
    //       finished and published, and exactly one more scan follows it, whose result is shown
    //       next; until the first result is handled the previous list stays shown, not emptied.
    // WHY-THIS-SHAPE: scan A's work is finished (the pool drained) before the library is changed
    //       and before the event loop runs, so A provably read "Mid" and three rescan() calls land
    //       while its result is unhandled. Scans are counted by the scan thread opening the folder:
    //       the lists alone cannot tell one follow-up from three, since every later scan reads the
    //       same folder and an unchanged list is not published again.
    // CONTROL: app/library/library_manager.cpp, rescan()'s `if (m_scanning != Scanning::No) {...}`
    //       block deleted, so scans overlap -> red, "S15.37: 4 scan(s) read the folder, not 2";
    //       the continuation's `== Scanning::YesThenAgain) { rescan(); }` follow-up made never to run
    //       -> red, "S15.37: the library ended at [Mid,Old], not [New,Old]"; the continuation made to
    //       throw a scan away when another was asked for (`if (m_scanning == Scanning::YesThenAgain)
    //       { m_scanning = Scanning::No; rescan(); return; }` first) -> red, "S15.37: the lists
    //       published were [New,Old], not [Mid,Old | New,Old]"; rescan() given `m_model.assign({});`
    //       at its start -> red, "the list shown while the scan ran was []"; restored.
    // COVERAGE-ASSERT: the counter saw the first scan.
    const int first = scans.take().scans;
    if (!writeFolder(dir + QStringLiteral("/mid"), planEntries(titled("Mid"))).isEmpty()) {
        harnessFailure("S15.37: mid could not be written");
        return;
    }
    m.rescan();
    if (!QThreadPool::globalInstance()->waitForDone(kWaitMs)) {
        notVerified("S15.37", "the first scan did not finish within the bound, so what it read could not be fixed");
        return;
    }
    if (!QDir(dir + QStringLiteral("/mid")).removeRecursively() || !writeFolder(dir + QStringLiteral("/new"), planEntries(titled("New"))).isEmpty()) {
        harnessFailure("S15.37: the library could not be changed between the scans");
        return;
    }
    m.rescan();
    m.rescan();
    m.rescan();
    expect(titles(m.wallpapers()) == u"Old" && m.state() == LibraryManager::State::Ready,
           QStringLiteral("S15.37: the list shown while the scan ran was [%1], state %2").arg(titles(m.wallpapers())).arg(int(m.state())));
    const bool settled = waitUntil([&] { return titles(m.wallpapers()) == u"New,Old"; }, kWaitMs);
    // Anything still queued behind the latest result is delivered before the history is read.
    pump(100);
    const int ran = scans.take().scans;
    expect(first >= 1, QStringLiteral("S15.37: the scan counter saw %1 scan(s) of the first read; it is not counting").arg(first));
    expect(settled && titles(m.wallpapers()) == u"New,Old",
           QStringLiteral("S15.37: the library ended at [%1], not [New,Old]").arg(titles(m.wallpapers())));
    expect(history == QStringList{QStringLiteral("Mid,Old"), QStringLiteral("New,Old")},
           QStringLiteral("S15.37: the lists published were [%1], not [Mid,Old | New,Old]").arg(history.join(QStringLiteral(" | "))));
    expect(ran == 2, QStringLiteral("S15.37: %1 scan(s) read the folder, not 2: the one running and one follow-up").arg(ran));
}

// S15.38, S15.39, S15.40, S15.57
void selectionAndValues(const QString& root) {
    const QString dir = root + QStringLiteral("/selection");
    const QString ocean = dir + QStringLiteral("/ocean");
    const QString oceanManifest = ocean + QStringLiteral("/wallpaper.json");
    const QString still = dir + QStringLiteral("/still");
    const QString broken = dir + QStringLiteral("/broken");
    OrderedJson stillJson = planManifest();
    stillJson["title"] = "A Still";
    stillJson.erase("properties");
    if (const QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}, {QStringLiteral("still"), jsonBytes(stillJson)},
                                              {QStringLiteral("broken"), QByteArray("not json")}});
        !why.isEmpty()) {
        fault("S15.38", QStringLiteral("the library could not be built: ") + why);
        return;
    }
    // A video with sound, so the declared mute is offered and volume added after it (S15.473).
    if (!writeBytes(ocean + QStringLiteral("/assets/video.mp4"), soundVideo())) {
        fault("S15.38", QStringLiteral("ocean's video could not be written"));
        return;
    }
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!readyWith(m, 3)) {
        expect(false, QStringLiteral("S15.38: the library never settled with three wallpapers; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    Counts c;
    countSignals(m, c);
    const QVariantMap defaults{{QStringLiteral("speed"), 1.0}, {QStringLiteral("mute"), true}, {QStringLiteral("tint"), QStringLiteral("#ffffff")},
                               {QStringLiteral("volume"), 1.0}};

    // GOAL: S15.38 -- only a usable wallpaper in the list can be opened: select() of a broken or
    //       unknown path returns false and changes and emits nothing.
    // CONTROL: app/library/library_manager.cpp, select()'s `|| !it->usable()` deleted -> red,
    //       "S15.38: selecting a broken folder or an unknown path ... selected "…/broken""; restored.
    const bool refusedBroken = !m.select(broken);
    const bool refusedUnknown = !m.select(dir + QStringLiteral("/nope"));
    expect(refusedBroken && refusedUnknown && m.selectedPath().isEmpty() && c.selection == 0 && c.values == 0,
           QStringLiteral("S15.38: selecting a broken folder or an unknown path returned %1/%2 and selected \"%3\" (%4 selection signal(s))")
               .arg(!refusedBroken).arg(!refusedUnknown).arg(m.selectedPath()).arg(c.selection));
    const bool opened = m.select(ocean);
    expect(opened && m.selectedPath() == ocean && m.selectedWallpaper().title == u"Ocean Waves" && c.selection == 1,
           QStringLiteral("S15.38: selecting ocean gave \"%1\" with %2 selection signal(s)").arg(m.selectedPath()).arg(c.selection));

    // GOAL: S15.40 -- propertyValues holds every property of the open wallpaper at its default
    //       before any edit.
    // CONTROL: app/library/library_manager.cpp, propertyValues()'s `m_edits.value(property.key, property.defaultValue)`
    //       made `m_edits.value(property.key, QVariant())` -> red, "S15.40: the defaults read {mute=,
    //       speed=, tint=, volume=}" (control_batch, re-run 2026-10-11 on a video with sound); restored.
    expect(m.propertyValues() == defaults, QStringLiteral("S15.40: the defaults read %1").arg(describe(m.propertyValues())));

    c.values = 0;
    // CONTROL: app/library/wpx_package.cpp, control_batch 2026-10-15: the speed choices' `>=`/`<=` range
    //       test made `>`/`<` (2, the range's max, no longer offered) -> red, "S15.40: an accepted edit of
    //       speed to 2 read {mute=true, speed=1, tint=#ffffff, volume=1} with 0 signal(s)"; restored.
    // CONTROL: app/library/library_manager.cpp:262, setPropertyValue emitting propertyValuesChanged a second time,
    //       control_batch 2026-10-15, app_library_manager '^selectionAndValues$' -> "FAIL: S15.40: an accepted edit
    //       of speed to 2 read {mute=true, speed=2, tint=#ffffff, volume=1} with 2 signal(s)"; restored, green.
    const bool tookSpeed = m.setPropertyValue(QStringLiteral("speed"), 2.0);
    expect(tookSpeed && m.propertyValues().value(QStringLiteral("speed")) == QVariant(2.0) && c.values == 1,
           QStringLiteral("S15.40: an accepted edit of speed to 2 read %1 with %2 signal(s)").arg(describe(m.propertyValues())).arg(c.values));

    // GOAL: S15.40 -- an edit the property cannot hold is refused: false, no value changed, no
    //       propertyValuesChanged. Speed 0.75 is inside the declared range but is not a playback speed.
    // CONTROL: app/library/library_manager.cpp, setPropertyValue()'s `if (!accepted) { return false; }`
    //       made `if (!accepted) { changeSelection([&] { m_edits.insert(key, value); }); return true; }`
    //       -> red, "S15.40: the refused edit speed = 9 returned 1", and each row after it, the two volume
    //       rows included (control_batch, re-run 2026-10-11); restored. The 0.75 row alone, control_batch
    //       2026-10-14: accept()'s `!offered.contains(number)` (app/library/wpx_package.cpp) made `false` ->
    //       red, "S15.40: the refused edit speed = 0.75 returned 1".
    const QVariantMap before = m.propertyValues();
    const std::pair<const char*, QVariant> refused[] = {
        {"speed", 9.0}, {"speed", 0.75}, {"speed", QStringLiteral("1")}, {"speed", std::numeric_limits<double>::quiet_NaN()},
        {"nope", 1.0},  {"mute", 1},                     {"tint", QStringLiteral("red")},
        {"volume", 1.5}, {"volume", -0.1},
    };
    c.values = 0;
    for (const auto& [key, value] : refused) {
        const bool took = m.setPropertyValue(QString::fromLatin1(key), value);
        expect(!took && m.propertyValues() == before && c.values == 0,
               QStringLiteral("S15.40: the refused edit %1 = %2 returned %3; values now %4, %5 signal(s)")
                   .arg(QLatin1String(key), value.toString()).arg(took).arg(describe(m.propertyValues())).arg(c.values));
    }
    // CONTROL: app/library/wpx_package.cpp, accept()'s `if (value.typeId() == QMetaType::QColor) {`
    //       made `if (false) {` -> red, "S15.40: a QColor tint and mute=false read {mute=false,
    //       speed=2, tint=#ffffff, volume=1}" (control_batch, re-run 2026-10-11); restored.
    const bool tookColour = m.setPropertyValue(QStringLiteral("tint"), QColor(255, 0, 0));
    const bool tookMute = m.setPropertyValue(QStringLiteral("mute"), false);
    expect(tookColour && tookMute && m.propertyValues().value(QStringLiteral("tint")) == QVariant(QStringLiteral("#ff0000")) &&
               m.propertyValues().value(QStringLiteral("mute")) == QVariant(false),
           QStringLiteral("S15.40: a QColor tint and mute=false read %1").arg(describe(m.propertyValues())));

    // GOAL: S15.40 -- resetPropertyValues() puts every value back to its default, and says so.
    // CONTROL: app/library/library_manager.cpp, resetPropertyValues()'s `m_edits.clear();` made
    //       `m_edits.remove(QStringLiteral("speed"));` -> red, "S15.40: after a reset the values read
    //       {mute=false, speed=1, tint=#ff0000, volume=1}" (control_batch, re-run 2026-10-11); restored.
    c.values = 0;
    m.resetPropertyValues();
    expect(m.propertyValues() == defaults && c.values == 1,
           QStringLiteral("S15.40: after a reset the values read %1 with %2 signal(s)").arg(describe(m.propertyValues())).arg(c.values));

    // GOAL: S15.38 -- opening another wallpaper drops the edits made on the previous one;
    //       re-selecting the open one keeps them.
    // CONTROL: app/library/library_manager.cpp, the `m_edits.clear();` inside select()'s
    //       changeSelection deleted -> red, "S15.38: re-select kept edits 1, the other wallpaper had
    //       none 1; after coming back speed read 2" (control_batch, re-run 2026-10-15); restored.
    const bool setSpeed = m.setPropertyValue(QStringLiteral("speed"), 2.0);
    const bool reselect = setSpeed && m.select(ocean) && m.propertyValues().value(QStringLiteral("speed")) == QVariant(2.0);
    const bool other = m.select(still) && m.propertyValues().isEmpty();
    const bool back = m.select(ocean);
    expect(reselect && other && back && m.propertyValues() == defaults,
           QStringLiteral("S15.38: re-select kept edits %1, the other wallpaper had none %2; after coming back speed read %3")
               .arg(reselect).arg(other).arg(m.propertyValues().value(QStringLiteral("speed")).toString()));

    // GOAL: S15.40 -- after the open wallpaper's wallpaper.json is rewritten, an edit the new
    //       version cannot hold falls back to the default, a property it dropped is gone, a new one
    //       is at its default, and an edit still valid is kept -- with no further setPropertyValue,
    //       and no emission ever exposing an out-of-range value.
    // WHY-THIS-SHAPE: every propertyValuesChanged is checked against the wallpaper as read at that
    //       moment, not only the end state, since the first emission after the rescan is the one a
    //       binding (and Apply) would read. The rewrite drops tint, not mute: a video with sound
    //       always offers mute (S15.473), so only a property it does not add can be dropped.
    // CONTROL: app/library/library_manager.cpp, control_batch, re-run 2026-10-15: settle()'s pruning
    //       `property.accept(m_edits.value(property.key))` made `std::optional<QVariant>(m_edits.value(property.key))`
    //       -> red, "S15.40: after the rewrite (reread 1) ... the values read {mute=false, speed=2, volume=1,
    //       zoom=2}, not {mute=false, speed=1, volume=1, zoom=2}" and "1 emission(s) checked; an emission
    //       exposed speed=2"; settle()'s `m_edits = std::move(edits);` made `m_edits.clear();` -> red,
    //       "...the values read {mute=true, speed=1, volume=1, zoom=2}, not ..."; restored.
    // COVERAGE-ASSERT: at least one emission was checked.
    if (!m.setPropertyValue(QStringLiteral("speed"), 2.0) || !m.setPropertyValue(QStringLiteral("mute"), false) ||
        !m.setPropertyValue(QStringLiteral("tint"), QStringLiteral("#00ff00"))) {
        expect(false, QStringLiteral("S15.40: the edits before the rewrite were refused; values %1").arg(describe(m.propertyValues())));
        return;
    }
    int emissionsChecked = 0;
    QStringList exposed;
    const auto guard = QObject::connect(&m, &LibraryManager::propertyValuesChanged, [&] {
        ++emissionsChecked;
        const QVariantMap values = m.propertyValues();
        for (const WallpaperProperty& p : m.selectedWallpaper().properties) {
            if (!p.accept(values.value(p.key))) {
                exposed.append(p.key + QLatin1Char('=') + values.value(p.key).toString());
            }
        }
    });
    OrderedJson narrowed = planManifest();
    narrowed["properties"] = OrderedJson::parse(R"({"speed": {"type": "float", "default": 1.0, "min": 0.1, "max": 1.5},
                                                   "mute": {"type": "bool", "default": true},
                                                   "zoom": {"type": "float", "default": 2.0, "min": 0.0, "max": 4.0}})");
    if (!writeBytes(oceanManifest, jsonBytes(narrowed))) {
        harnessFailure("S15.40: the narrowed wallpaper.json could not be written");
        return;
    }
    const bool reread = waitUntil([&] { return m.selectedWallpaper().properties.size() == 4 && m.selectedWallpaper().properties[2].key == u"zoom"; }, kWaitMs);
    const QVariantMap want{{QStringLiteral("speed"), 1.0}, {QStringLiteral("mute"), false}, {QStringLiteral("zoom"), 2.0},
                           {QStringLiteral("volume"), 1.0}};
    expect(reread && m.selectedPath() == ocean && m.propertyValues() == want,
           QStringLiteral("S15.40: after the rewrite (reread %1) the open wallpaper is \"%2\" and the values read %3, not %4")
               .arg(reread).arg(m.selectedPath(), describe(m.propertyValues()), describe(want)));
    expect(exposed.isEmpty() && emissionsChecked >= 1,
           QStringLiteral("S15.40: %1 emission(s) checked; an emission exposed %2").arg(emissionsChecked).arg(exposed.join(u", ")));
    QObject::disconnect(guard);

    // GOAL: S15.57 -- an edit the file update made invalid is forgotten for good: when the file
    //       is rewritten again so the old edit would fit, the value stays at the default, and a
    //       property dropped and restored comes back at its default, not at the old edit.
    // WHY-THIS-SHAPE: follows the S15.40 rewrite on the same open wallpaper, so the edits it made
    //       (speed 2, tint green) are exactly the ones the narrowing refused or dropped; mute's edit,
    //       still valid throughout, stays.
    // CONTROL: app/library/library_manager.cpp, control_batch, re-run 2026-10-15: settle()'s pruning made to
    //       keep every edit, as for S15.40 -> red, "S15.57: after the file was widened again (reread 1) the
    //       values read {mute=false, speed=2, tint=#ffffff, volume=1}, not {mute=false, speed=1, ...}"; restored. (The 2026-10-02 control, filtering
    //       in propertyValues() instead of pruning in settle(), was not re-run.)
    if (reread) {
        if (!writeBytes(oceanManifest, QByteArray(kPlanManifest))) {
            harnessFailure("S15.57: the widened wallpaper.json could not be written");
            return;
        }
        const bool widened = waitUntil([&] { return m.selectedWallpaper().properties.size() == 4 && m.selectedWallpaper().properties[2].key == u"tint"; }, kWaitMs);
        const QVariantMap wantBack{{QStringLiteral("speed"), 1.0}, {QStringLiteral("mute"), false}, {QStringLiteral("tint"), QStringLiteral("#ffffff")},
                                   {QStringLiteral("volume"), 1.0}};
        expect(widened && m.selectedPath() == ocean && m.propertyValues() == wantBack,
               QStringLiteral("S15.57: after the file was widened again (reread %1) the values read %2, not %3")
                   .arg(widened).arg(describe(m.propertyValues()), describe(wantBack)));
    }

    // GOAL: S15.39 -- when the open wallpaper's folder is moved out of the library, the selection
    //       and its edits are dropped; when the same folder comes back it is listed but not reopened.
    // WHY-THIS-SHAPE: the return is waited for by its listing, so "not reopened" is judged on a
    //       scan that saw the folder, not on one that had not happened yet.
    // CONTROL: app/library/library_manager.cpp, settle()'s `m_selection.clear();` deleted (control_batch,
    //       2026-10-15) -> red, "S15.39: with its folder moved away (seen 1) the open wallpaper is still
    //       "…/ocean", values {}, 1 selection signal(s)" and "after the folder came back (listed 1)
    //       "…/ocean" is open with values {mute=true, speed=1, tint=#ffffff, volume=1}"; restored.
    if (!m.setPropertyValue(QStringLiteral("speed"), 0.5)) {
        expect(false, QStringLiteral("S15.39: speed 0.5 was refused on the open wallpaper; values %1").arg(describe(m.propertyValues())));
        return;
    }
    c = Counts{};
    const QString keep = root + QStringLiteral("/selection-away");
    if (!QDir().rename(ocean, keep)) {
        harnessFailure("S15.39: ocean could not be moved away");
        return;
    }
    const bool gone = waitUntil([&] { return find(m.wallpapers(), QStringLiteral("ocean")) == nullptr; }, kWaitMs);
    expect(gone && m.selectedPath().isEmpty() && m.propertyValues().isEmpty() && !m.selectedWallpaper().usable() && c.selection >= 1,
           QStringLiteral("S15.39: with its folder moved away (seen %1) the open wallpaper is still \"%2\", values %3, %4 selection signal(s)")
               .arg(gone).arg(m.selectedPath(), describe(m.propertyValues())).arg(c.selection));
    if (!QDir().rename(keep, ocean)) {
        harnessFailure("S15.39: ocean could not be put back");
        return;
    }
    const bool returned = waitUntil([&] { const Wallpaper* w = find(m.wallpapers(), QStringLiteral("ocean")); return w && w->usable(); }, kWaitMs);
    expect(returned && m.selectedPath().isEmpty() && m.propertyValues().isEmpty(),
           QStringLiteral("S15.39: after the folder came back (listed %1) \"%2\" is open with values %3").arg(returned).arg(m.selectedPath(), describe(m.propertyValues())));

    // GOAL: S15.39 -- when the open wallpaper turns broken, the selection and its edits are
    //       dropped; when it is repaired it is not reopened.
    // CONTROL: app/library/library_manager.cpp, settle()'s `|| !kept->usable()` deleted (control_batch,
    //       2026-10-15) -> red, "S15.39: with its wallpaper.json broken (seen 1) the open wallpaper is still
    //       "…/ocean", values {}" and "after the wallpaper was repaired (seen 1) "…/ocean" is open"; restored.
    if (!m.select(ocean) || !m.setPropertyValue(QStringLiteral("speed"), 0.5)) {
        expect(false, QStringLiteral("S15.39: ocean could not be reopened before breaking it"));
        return;
    }
    const auto goodBytes = readBytes(oceanManifest);
    if (!goodBytes || !writeBytes(oceanManifest, "no longer json")) {
        harnessFailure("S15.39: ocean's wallpaper.json could not be broken");
        return;
    }
    const bool broke = waitUntil([&] { const Wallpaper* w = find(m.wallpapers(), QStringLiteral("ocean")); return w && !w->usable(); }, kWaitMs);
    expect(broke && m.selectedPath().isEmpty() && m.propertyValues().isEmpty(),
           QStringLiteral("S15.39: with its wallpaper.json broken (seen %1) the open wallpaper is still \"%2\", values %3").arg(broke).arg(m.selectedPath(), describe(m.propertyValues())));
    if (!writeBytes(oceanManifest, *goodBytes)) {
        harnessFailure("S15.39: ocean's wallpaper.json could not be repaired");
        return;
    }
    const bool repaired = waitUntil([&] { const Wallpaper* w = find(m.wallpapers(), QStringLiteral("ocean")); return w && w->usable(); }, kWaitMs);
    expect(repaired && m.selectedPath().isEmpty(),
           QStringLiteral("S15.39: after the wallpaper was repaired (seen %1) \"%2\" is open").arg(repaired).arg(m.selectedPath()));
}

// S15.41, S15.42
void watching(const QString& root) {
    // GOAL: S15.41 -- a wallpaper folder moved into the library, as an import renames one in, is
    //       listed without rescan() being called.
    // CONTROL: app/library/folder_watcher.cpp, the constructor's `connect(m_inotifyReady.get(),
    //       &QSocketNotifier::activated, ...)` deleted -> red, "S15.41: a wallpaper moved into the
    //       library was not listed"; restored.
    {
        const QString dir = root + QStringLiteral("/added");
        if (!QDir().mkpath(dir)) {
            fault("S15.41", QStringLiteral("could not make ") + dir);
            return;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        if (!readyWith(m, 0)) {
            expect(false, QStringLiteral("S15.41: an empty library never settled Ready"));
        } else if (const QString why = moveInFolder(root + QStringLiteral("/added-staging"), dir, QStringLiteral("new"), planEntries(QByteArray(kPlanManifest)));
                   !why.isEmpty()) {
            fault("S15.41", why);
        } else {
            const bool listed = waitUntil([&] { return m.wallpapers().size() == 1 && m.wallpapers()[0].usable(); }, kWaitMs);
            expect(listed, QStringLiteral("S15.41: a wallpaper moved into the library was not listed; [%1]").arg(titles(m.wallpapers())));
        }
    }

    // GOAL: S15.41 -- a library folder created after the library started is noticed, and a
    //       wallpaper put in it is listed, without rescan().
    // CONTROL: app/library/folder_watcher.cpp, handle()'s `return QByteArray(event.name) == ancestor->awaited;`
    //       made `return false;` -> red, "S15.41: a library folder created after start was not read"; restored.
    {
        const QString parent = root + QStringLiteral("/later");
        const QString dir = parent + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(parent)) {
            fault("S15.41", QStringLiteral("could not make ") + parent);
            return;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        if (!readyWith(m, 0)) {
            expect(false, QStringLiteral("S15.41: a missing library folder never settled Ready"));
        } else if (const QString why = makeLibrary(dir, {{QStringLiteral("new"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
            fault("S15.41", why);
        } else {
            const bool listed = waitUntil([&] { return m.wallpapers().size() == 1 && m.wallpapers()[0].usable(); }, kWaitMs);
            expect(listed, QStringLiteral("S15.41: a library folder created after start was not read; [%1]").arg(titles(m.wallpapers())));
        }
    }

    // A listed wallpaper's own files closing, as the watch inside its folder reports them, is what
    // the next two controls remove: IN_CLOSE_WRITE deleted from folder_watcher.cpp's kInsideEvents,
    // so a write holds the wallpaper until the 30 s hold lapses, past every wait here.

    // GOAL: S15.41 -- a wallpaper.json rewritten in place (same name, same inode) is read again
    //       without rescan().
    // WHY-THIS-SHAPE: the rewrite truncates and writes the existing file rather than replacing it,
    //       so no entry appears or goes: only the watch inside the wallpaper's folder -- its writes,
    //       then its close -- reports it.
    // CONTROL: the kInsideEvents patch above -> red, "S15.41: a wallpaper.json rewritten in place
    //       still reads "Ocean Waves""; restored.
    {
        const QString dir = root + QStringLiteral("/inplace");
        if (const QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
            fault("S15.41", why);
        } else {
            LibraryManager m(dir, kSettleMs);
            m.rescan();
            if (!readyWith(m, 1)) {
                expect(false, QStringLiteral("S15.41: the in-place library never settled; [%1]").arg(titles(m.wallpapers())));
            } else if (!writeBytes(dir + QStringLiteral("/ocean/wallpaper.json"), titled("Rewritten"))) {
                fault("S15.41", QStringLiteral("wallpaper.json could not be rewritten"));
            } else {
                const bool reread = waitUntil([&] { return titles(m.wallpapers()) == u"Rewritten"; }, kWaitMs);
                expect(reread, QStringLiteral("S15.41: a wallpaper.json rewritten in place still reads \"%1\"").arg(titles(m.wallpapers())));
            }
        }
    }

    // GOAL: S15.41 -- an unreadable wallpaper.json made readable (chmod) is re-read and the
    //       wallpaper becomes usable without rescan().
    // CONTROL: app/library/folder_watcher.cpp, IN_ATTRIB deleted from kInsideEvents -> red,
    //       "S15.41: a wallpaper.json made readable still reads locked(broken)"; restored.
    if (::geteuid() == 0) {
        notVerified("S15.41 (a file made readable)", "running as root, which can read a mode-000 file");
    } else {
        const QString dir = root + QStringLiteral("/chmod");
        const QString path = dir + QStringLiteral("/locked/wallpaper.json");
        if (const QString why = makeLibrary(dir, {{QStringLiteral("locked"), QByteArray(kPlanManifest)}}); !why.isEmpty() || !setMode(path, 0)) {
            fault("S15.41", why.isEmpty() ? QStringLiteral("locked's wallpaper.json could not be made mode 000") : why);
        } else {
            LibraryManager m(dir, kSettleMs);
            m.rescan();
            const bool brokenFirst = readyWith(m, 1) && !m.wallpapers()[0].usable();
            if (!setMode(path, 0644)) {
                fault("S15.41", QStringLiteral("locked's wallpaper.json could not be made readable"));
            } else if (!brokenFirst) {
                expect(false, QStringLiteral("S15.41: the wallpaper with a mode-000 wallpaper.json was not listed broken first: [%1]").arg(titles(m.wallpapers())));
            } else {
                const bool usable = waitUntil([&] { return m.wallpapers().size() == 1 && m.wallpapers()[0].usable(); }, kWaitMs);
                expect(usable, QStringLiteral("S15.41: a wallpaper.json made readable still reads %1").arg(titles(m.wallpapers())));
            }
        }
    }

    // GOAL: S15.41 -- an editor's hidden file written beside a listed wallpaper's files (a swap
    //       file, say) costs no scan: hidden names are no part of a wallpaper.
    // WHY-THIS-SHAPE: scans are counted by the scan thread opening the library; the swap file is
    //       written and closed several times, each close an event a non-hidden file would turn
    //       into a scan.
    // CONTROL: app/library/folder_watcher.cpp, handle()'s `if (name.startsWith('.') || (dir.isEmpty() && !isItemName(name))) {`
    //       made `if (dir.isEmpty() && !isItemName(name)) {` -> red, "S15.41: an editor's hidden
    //       swap file written 5 time(s) cost 5 scan(s)"; restored.
    // COVERAGE-ASSERT: the counter saw the first scan.
    {
        const QString dir = root + QStringLiteral("/swap");
        if (const QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
            fault("S15.41", why);
        } else {
            OpenCounter scans(dir);
            LibraryManager m(dir, kSettleMs);
            m.rescan();
            const bool ready = readyWith(m, 1);
            pump(4 * kSettleMs);
            const int first = scans.take().scans;
            bool written = true;
            for (int i = 0; i < 5 && written; ++i) {
                written = writeBytes(dir + QStringLiteral("/ocean/.wallpaper.json.swp"), QByteArray(64 * (i + 1), 'x'));
                pump(3 * kSettleMs);
            }
            pump(4 * kSettleMs);
            const int during = scans.take().scans;
            if (!scans.ok() || !written) {
                fault("S15.41", QStringLiteral("the swap file could not be written, or the counter was busy"));
            } else {
                expect(ready && first >= 1, QStringLiteral("S15.41: the swap library settled %1 and the counter saw %2 first scan(s)").arg(ready).arg(first));
                expect(during == 0, QStringLiteral("S15.41: an editor's hidden swap file written 5 time(s) cost %1 scan(s)").arg(during));
            }
        }
    }

    // GOAL: S15.42 -- a wallpaper whose wallpaper.json was half written when first read (listed
    //       broken) ends up usable once the rest is written; it is never left stuck broken.
    // WHY-THIS-SHAPE: the folder arrives by rename with half its manifest, so its broken listing
    //       depends on no write or close event; the second half is written only after that listing
    //       is seen, and the append is then a write to a listed wallpaper, which only the watch
    //       inside its folder reports.
    // CONTROL: the kInsideEvents patch above -> red, "S15.42: the wallpaper stayed half(broken)
    //       after the rest was written"; restored.
    {
        const QString dir = root + QStringLiteral("/halves");
        const QString path = dir + QStringLiteral("/half/wallpaper.json");
        const QByteArray manifest(kPlanManifest);
        const qsizetype half = manifest.size() / 2;
        Entries halfEntries = planEntries(manifest);
        halfEntries.first().second = manifest.left(half);
        if (!QDir().mkpath(dir)) {
            fault("S15.42", QStringLiteral("the halves library could not be built"));
            return;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        if (!readyWith(m, 0)) {
            expect(false, QStringLiteral("S15.42: the halves library never settled"));
        } else if (!moveInFolder(root + QStringLiteral("/halves-staging"), dir, QStringLiteral("half"), halfEntries).isEmpty()) {
            fault("S15.42", QStringLiteral("the half wallpaper could not be moved in"));
        } else if (!waitUntil([&] { return titles(m.wallpapers()) == u"half(broken)" && m.wallpapers()[0].problem.contains(QLatin1String("not valid JSON")); }, kWaitMs)) {
            expect(false, QStringLiteral("S15.42: half a wallpaper.json was never listed broken; [%1]").arg(titles(m.wallpapers())));
        } else if (!writeBytes(path, manifest.mid(half), QIODevice::WriteOnly | QIODevice::Append)) {
            fault("S15.42", QStringLiteral("the second half could not be written"));
        } else {
            const bool usable = waitUntil([&] { return m.wallpapers().size() == 1 && m.wallpapers()[0].usable(); }, kWaitMs);
            expect(usable, QStringLiteral("S15.42: the wallpaper stayed %1 after the rest was written").arg(titles(m.wallpapers())));
        }
    }
}

// S15.42
void quietPeriod(const QString& root) {
    constexpr int settle = 150;
    constexpr int gapMs = 20;
    constexpr int chunks = 8;
    const QString dir = root + QStringLiteral("/burst");
    const QString path = dir + QStringLiteral("/ocean/wallpaper.json");
    const QByteArray bytes = titled("Burst");
    if (const QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
        fault("S15.42", why);
        return;
    }
    OpenCounter scans(dir);
    if (!scans.ok()) {
        harnessFailure("S15.42: the open counter was already counting another folder");
        return;
    }
    LibraryManager m(dir, settle);
    m.rescan();
    if (!readyWith(m, 1)) {
        expect(false, QStringLiteral("S15.42: the burst library never settled"));
        return;
    }
    // GOAL: S15.42 -- writes to a listed wallpaper's wallpaper.json that keep coming sooner than
    //       the quiet period apart cost no scan while they last and exactly one after the last,
    //       which reads the finished file.
    // WHY-THIS-SHAPE: the event loop runs between writes (a test that slept instead would starve
    //       the manager's timer and pass whatever it did); scans are counted by the scan thread
    //       opening the library. If the test itself stalled so that two writes were more than half
    //       the quiet period apart, the run is abandoned as not verified rather than graded, since
    //       a correct manager may then legitimately scan mid-burst. Two defences keep the burst
    //       scan-free -- a write holds the wallpaper until its close, and a change is read only
    //       after the quiet period -- and either alone does, so only breaking both turns "during" red.
    // CONTROL: app/library/folder_watcher.cpp, `m_settle.setInterval(settleMs);` made
    //       `m_settle.setInterval(0);` together with handle()'s IN_MODIFY branch made to return true
    //       without holding, one patch -> red, "S15.42: 7 scan(s) ran during the burst"; either
    //       alone left it green; app/library/library_manager.cpp, the continuation's
    //       `== Scanning::YesThenAgain) {` made `!= Scanning::No) {`, so every scan asks for another
    //       -> red, "after the burst the wallpaper reads [Burst] after 9605 scan(s), not [Burst]
    //       after exactly one"; restored.
    // COVERAGE-ASSERT: the counter must have seen the first scan, so a dead counter cannot read 0.
    const int initialScans = scans.take().scans;
    expect(initialScans >= 1, QStringLiteral("S15.42: the scan counter saw %1 scan(s) of the first read; it is not counting").arg(initialScans));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        harnessFailure("S15.42: wallpaper.json could not be opened for the burst");
        return;
    }
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    qint64 widestGap = 0;
    const qsizetype piece = bytes.size() / chunks + 1;
    for (int i = 0; i < chunks; ++i) {
        const QByteArray chunk = bytes.mid(i * piece, piece);
        if (file.write(chunk) != chunk.size() || !file.flush()) {
            harnessFailure("S15.42: a burst write failed");
            return;
        }
        const qint64 now = clock.elapsed();
        widestGap = std::max(widestGap, now - last);
        last = now;
        if (i + 1 < chunks) {
            pump(gapMs);
        }
    }
    file.close();
    const int duringBurst = scans.take().scans;
    if (widestGap >= settle / 2) {
        notVerified("S15.42 (the quiet period)", "this test stalled " + std::to_string(widestGap) + " ms between two writes, over half the quiet period");
        return;
    }
    const bool reread = waitUntil([&] { return titles(m.wallpapers()) == u"Burst"; }, kWaitMs);
    pump(4 * settle);
    const int afterBurst = scans.take().scans;
    expect(duringBurst == 0, QStringLiteral("S15.42: %1 scan(s) ran during the burst (widest gap %2 ms, quiet period %3 ms)").arg(duringBurst).arg(widestGap).arg(settle));
    expect(reread && afterBurst == 1,
           QStringLiteral("S15.42: after the burst the wallpaper reads [%1] after %2 scan(s), not [Burst] after exactly one")
               .arg(titles(m.wallpapers())).arg(afterBurst));
}

// S15.43
void betweenReadAndHandled(const QString& root) {
    // Each case lets a scan finish its reading and changes the library while the scan's result is
    // still queued, before the event loop runs. The watch placed before the scan is what must
    // report it; a watch placed only when the result is handled would come too late.
    const auto readThenChange = [](LibraryManager& m, const std::function<bool()>& change) {
        m.rescan();
        if (!QThreadPool::globalInstance()->waitForDone(kWaitMs)) {
            return QStringLiteral("the scan did not finish within the bound");
        }
        return change() ? QString() : QStringLiteral("the change could not be made");
    };
    // The one control for the four cases below: app/library/library_manager.cpp, rescan()'s
    // `m_watcher.scanStarting();` moved from before the scan to the start of the result's continuation.

    // GOAL: S15.43 -- a wallpaper.json rewritten after a scan read it but before its result was
    //       handled is read again.
    // WHY-THIS-SHAPE: the pool is drained and the event loop not yet run, so the rewrite provably
    //       falls after the read; a new manager's first scan is used, so no earlier watch exists.
    // CONTROL: scanStarting() moved after the scan, as above -> red, "S15.43: a wallpaper.json
    //       rewritten after it was read still reads "Ocean Waves""; restored.
    {
        const QString dir = root + QStringLiteral("/gap-file");
        if (const QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
            fault("S15.43", why);
        } else {
            LibraryManager m(dir, kSettleMs);
            const QString failed = readThenChange(m, [&] { return writeBytes(dir + QStringLiteral("/ocean/wallpaper.json"), titled("Rewritten in the gap")); });
            if (!failed.isEmpty()) {
                notVerified("S15.43 (a wallpaper.json rewritten in the gap)", failed.toStdString());
            } else {
                const bool reread = waitUntil([&] { return titles(m.wallpapers()) == u"Rewritten in the gap"; }, kWaitMs);
                expect(reread, QStringLiteral("S15.43: a wallpaper.json rewritten after it was read still reads \"%1\"").arg(titles(m.wallpapers())));
            }
        }
    }

    // GOAL: S15.43 -- a wallpaper folder added to the library in the same gap is listed, and a
    //       library folder created in the gap, while it was missing, is read.
    // CONTROL: scanStarting() moved after the scan, as above -> red, "S15.43: a wallpaper added
    //       after the library was read was not listed" and "a library folder created after it was
    //       read was not read"; restored.
    {
        const QString dir = root + QStringLiteral("/gap-dir");
        if (const QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
            fault("S15.43", why);
        } else {
            LibraryManager m(dir, kSettleMs);
            const QString failed = readThenChange(m, [&] {
                return moveInFolder(root + QStringLiteral("/gap-dir-staging"), dir, QStringLiteral("added"), planEntries(titled("Added"))).isEmpty();
            });
            if (!failed.isEmpty()) {
                notVerified("S15.43 (a wallpaper added in the gap)", failed.toStdString());
            } else {
                const bool listed = waitUntil([&] { return titles(m.wallpapers()) == u"Added,Ocean Waves"; }, kWaitMs);
                expect(listed, QStringLiteral("S15.43: a wallpaper added after the library was read was not listed; [%1]").arg(titles(m.wallpapers())));
            }
        }
    }

    {
        const QString parent = root + QStringLiteral("/gap-missing");
        const QString dir = parent + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(parent)) {
            fault("S15.43", QStringLiteral("could not make ") + parent);
        } else {
            LibraryManager m(dir, kSettleMs);
            const QString failed = readThenChange(m, [&] { return makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}}).isEmpty(); });
            if (!failed.isEmpty()) {
                notVerified("S15.43 (a library folder created in the gap)", failed.toStdString());
            } else {
                const bool listed = waitUntil([&] { return titles(m.wallpapers()) == u"Ocean Waves"; }, kWaitMs);
                expect(listed, QStringLiteral("S15.43: a library folder created after it was read was not read; [%1]").arg(titles(m.wallpapers())));
            }
        }
    }

    // GOAL: S15.43 -- a plain file put where the missing library folder should be, in the same
    //       gap, is noticed: the library ends Unavailable instead of staying an empty Ready.
    // CONTROL: scanStarting() moved after the scan, as above -> red, "S15.43: a file put at the
    //       missing library folder's path after it was read left the library state 1"; restored.
    {
        const QString parent = root + QStringLiteral("/gap-file-at-path");
        const QString dir = parent + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(parent)) {
            fault("S15.43", QStringLiteral("could not make ") + parent);
        } else {
            LibraryManager m(dir, kSettleMs);
            const QString failed = readThenChange(m, [&] { return writeBytes(dir, "a file"); });
            if (!failed.isEmpty()) {
                notVerified("S15.43 (a file put at the library folder's path in the gap)", failed.toStdString());
            } else {
                const bool noticed = waitUntil([&] { return m.state() == LibraryManager::State::Unavailable; }, kWaitMs);
                expect(noticed, QStringLiteral("S15.43: a file put at the missing library folder's path after it was read left the library state %1")
                                    .arg(int(m.state())));
            }
        }
    }
}

// S15.44, S15.50
void idleLibrary(const QString& root) {
    constexpr int settle = 20;
    constexpr int window = 40 * settle;
    const QString dir = root + QStringLiteral("/idle");
    const QString staging = root + QStringLiteral("/idle-staging");
    const QString lockedFolder = dir + QStringLiteral("/locked");
    const QString lockedFile = dir + QStringLiteral("/lockedfile/wallpaper.json");
    const RawNamesRemover removeRawNames(dir, {"caf\xe8", "caf\xe9"});
    QString why = makeLibrary(dir, {{QStringLiteral("ocean"), QByteArray(kPlanManifest)}, {QStringLiteral("badjson"), QByteArray("{ nope")},
                                    {QStringLiteral("locked"), QByteArray(kPlanManifest)}, {QStringLiteral("lockedfile"), QByteArray(kPlanManifest)},
                                    {QStringLiteral(".hidden"), QByteArray(kPlanManifest)}});
    for (const QByteArray& name : {QByteArray("caf\xe8"), QByteArray("caf\xe9")}) {
        if (why.isEmpty() && (!writeFolder(staging + QStringLiteral("/w"), planEntries(QByteArray(kPlanManifest))).isEmpty() ||
                              ::rename(QFile::encodeName(staging + QStringLiteral("/w")).constData(), (QFile::encodeName(dir) + '/' + name).constData()) != 0)) {
            why = QStringLiteral("a folder whose name is not UTF-8 could not be made");
        }
    }
    if (!why.isEmpty() || !writeFolder(root + QStringLiteral("/idle-elsewhere/linked"), planEntries(QByteArray(kPlanManifest))).isEmpty() ||
        !QFile::link(QStringLiteral("../idle-elsewhere/linked"), dir + QStringLiteral("/linked")) ||
        !QFile::link(QStringLiteral("nothing-here"), dir + QStringLiteral("/dangling")) || !setMode(lockedFile, 0) || !setMode(lockedFolder, 0)) {
        fault("S15.44", QStringLiteral("the idle library could not be built: ") + why);
        static_cast<void>(setMode(lockedFolder, 0755));
        static_cast<void>(setMode(lockedFile, 0644));
        return;
    }
    const auto unlock = [&] {
        if (!setMode(lockedFolder, 0755) || !setMode(lockedFile, 0644)) {
            harnessFailure("S15.44: the locked folder and file could not be made readable again");
        }
    };
    const bool asRoot = ::geteuid() == 0;
    OpenCounter scans(dir);
    // The scratch root, so the files reached through the link to idle-elsewhere count too.
    FileOpens files(root);
    if (!scans.ok() || !files.ok()) {
        harnessFailure("S15.44: an open counter was already counting another folder, or the root could not be resolved");
        unlock();
        return;
    }
    LibraryManager m(dir, settle);
    m.rescan();
    const bool ready = readyWith(m, 7);
    // GOAL: S15.44, S15.50 -- a library left alone, holding a usable wallpaper, a broken one, an
    //       unreadable folder, a folder whose wallpaper.json is unreadable, a link to a folder, a
    //       dangling link, a hidden folder and two folders whose names are not UTF-8, stops reading
    //       it: no scan, no walk of the folder, and no wallpaper file opened, by any open call, over
    //       forty quiet periods after it settles.
    // WHY-THIS-SHAPE: counted by this process opening the library (a scan off the main thread, the
    //       watcher's walk on it), and by every file below the scratch root opened through open,
    //       openat or openat2, O_PATH included (FileOpens, openat2_shim.cpp), on any thread; openat2
    //       alone would miss a lease probe or the reader's fallback. The library holds one of each
    //       entry the scan or the watcher treats specially -- an unreadable folder refusing its
    //       watch at every walk, links, a hidden folder, names whose text must still name the real
    //       folder -- so a special case that asks for another scan is caught.
    // CONTROL: app/library/library_manager.cpp, the continuation's `== Scanning::YesThenAgain) {`
    //       made `!= Scanning::No) {`, so every scan asks for another -> red, "S15.44: an idle
    //       library was scanned 5540 time(s), walked 5540 time(s) and opened 22160 file(s) in 800
    //       ms"; restored.
    // COVERAGE-ASSERT: the first read was seen by all three counters -- a scan, the watcher's walk
    //       on the main thread, and file opens -- so none can read as "none" while idle.
    // CONTROL: app/library/folder_watcher.cpp, watchTree()'s `const Listing listing(::opendir(path.constData()), ::closedir);`
    //       made to open through `::fdopendir(::open(...))`, so the walk is invisible to the walk
    //       counter -> red, "S15.44: the counters saw 1 scan(s), 0 walk(s) and 18 file open(s) in the
    //       first read", the only line red; restored.
    const OpenCounts firstOpens = scans.take();
    const int firstOpened = files.take();
    if (!ready) {
        unlock();
        expect(false, QStringLiteral("S15.44: the idle library never settled with seven entries; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    pump(window);
    const OpenCounts idle = scans.take();
    const int idleOpened = files.take();
    unlock();
    expect(firstOpens.scans >= 1 && firstOpens.watcherOpens >= 1 && firstOpened >= 1,
           QStringLiteral("S15.44: the counters saw %1 scan(s), %2 walk(s) and %3 file open(s) in the first read; one is not counting")
               .arg(firstOpens.scans).arg(firstOpens.watcherOpens).arg(firstOpened));
    expect(idle.scans == 0 && idle.watcherOpens == 0 && idleOpened == 0,
           QStringLiteral("S15.44: an idle library was scanned %1 time(s), walked %2 time(s) and opened %3 file(s) in %4 ms")
               .arg(idle.scans).arg(idle.watcherOpens).arg(idleOpened).arg(window));
    if (asRoot) {
        notVerified("S15.44 (with an unreadable folder)", "running as root, which can read a mode-000 folder");
    }
}

// S15.69
void revealed(const QString& root) {
    const QString dir = root + QStringLiteral("/reveal");
    const QString staging = root + QStringLiteral("/reveal-staging");
    if (const QString why = makeLibrary(dir, {{QStringLiteral("old"), titled("Old")}}); !why.isEmpty()) {
        fault("S15.69", why);
        return;
    }
    // A quiet period no test outlasts: only reveal() and rescan() start scans here.
    LibraryManager m(dir, 600000);
    m.rescan();
    if (!readyWith(m, 1)) {
        expect(false, QStringLiteral("S15.69: the reveal library never settled; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    const QString old = dir + QStringLiteral("/old");

    // GOAL: S15.69 -- reveal() of a wallpaper already listed usable selects it at once, with no
    //       scan to wait for.
    // CONTROL: app/library/library_manager.cpp, reveal()'s `if (select(filePath)) {` made
    //       `if (false) {` -> red, "S15.69: revealing a listed wallpaper selected "" at once"; restored.
    m.reveal(old);
    expect(m.selectedPath() == old, QStringLiteral("S15.69: revealing a listed wallpaper selected \"%1\" at once").arg(m.selectedPath()));
    m.clearSelection();

    // GOAL: S15.69 -- a folder moved in and revealed before the library has seen it (as after an
    //       import) is selected by the scan reveal() starts.
    // WHY-THIS-SHAPE: reveal() is called before the event loop runs, so the watch has not reported
    //       the folder and only reveal's own scan can list it.
    // CONTROL: app/library/library_manager.cpp, settle()'s `m_selection = shown->filePath;` deleted ->
    //       red, "S15.69: a folder revealed right after it was moved in ... selected """; restored.
    if (!moveInFolder(staging, dir, QStringLiteral("first"), planEntries(titled("First"))).isEmpty()) {
        fault("S15.69", QStringLiteral("first could not be moved in"));
        return;
    }
    m.reveal(dir + QStringLiteral("/first"));
    const bool firstSelected = waitUntil([&] { return m.selectedPath() == dir + QStringLiteral("/first"); }, kWaitMs);
    expect(firstSelected, QStringLiteral("S15.69: a folder revealed right after it was moved in is listed [%1] and selected \"%2\"")
                              .arg(titles(m.wallpapers()), m.selectedPath()));

    // GOAL: S15.69 -- a reveal made while an earlier scan's result is pending is not given up by
    //       that scan, which may have read the library before the folder existed; the next scan
    //       selects it.
    // WHY-THIS-SHAPE: the earlier scan is started and drained before the folder is moved in, so it
    //       provably read a library without it; reveal() is then called with that result still
    //       queued, so the result it would wrongly act on is the one that cannot list the folder.
    // CONTROL: app/library/library_manager.cpp, settle()'s `if (m_reveal && m_reveal->scanBegun) {`
    //       made `if (m_reveal) {` -> red, "S15.69: a reveal made while an earlier scan was pending
    //       ended with "…/first" selected"; restored.
    m.rescan();
    if (!QThreadPool::globalInstance()->waitForDone(kWaitMs) ||
        !moveInFolder(staging, dir, QStringLiteral("second"), planEntries(titled("Second"))).isEmpty()) {
        fault("S15.69", QStringLiteral("the earlier scan did not finish, or second could not be moved in"));
        return;
    }
    m.reveal(dir + QStringLiteral("/second"));
    const bool secondSelected = waitUntil([&] { return m.selectedPath() == dir + QStringLiteral("/second"); }, kWaitMs);
    expect(secondSelected, QStringLiteral("S15.69: a reveal made while an earlier scan was pending ended with \"%1\" selected, listing [%2]")
                               .arg(m.selectedPath(), titles(m.wallpapers())));

    // GOAL: S15.69 -- select() and clearSelection() made while a reveal is pending cancel it: the
    //       scan that then lists the revealed folder does not take the user's choice away.
    // WHY-THIS-SHAPE: the pool is held so the reveal's scan cannot run before the user's call.
    // CONTROL: app/library/library_manager.cpp, select()'s `m_reveal.reset();` deleted -> red,
    //       "S15.69: select() after reveal() ended with "…/third" selected"; clearSelection()'s
    //       `m_reveal.reset();` deleted -> red, "S15.69: clearSelection() after reveal() ended with
    //       "…/fourth" selected"; restored.
    for (const bool clear : {false, true}) {
        const QString name = clear ? QStringLiteral("fourth") : QStringLiteral("third");
        QString after;
        {
            PoolHold hold(kWaitMs);
            if (!hold.ok() || !moveInFolder(staging, dir, name, planEntries(titled("Later"))).isEmpty()) {
                fault("S15.69", QStringLiteral("the pool could not be held, or %1 could not be moved in").arg(name));
                return;
            }
            m.reveal(dir + QLatin1Char('/') + name);
            if (clear) {
                m.clearSelection();
            } else if (!m.select(old)) {
                expect(false, QStringLiteral("S15.69: old could not be selected while a reveal was pending"));
            }
        }
        const bool listed = waitUntil([&] { return find(m.wallpapers(), name) != nullptr; }, kWaitMs);
        pump(50);
        expect(listed && m.selectedPath() == (clear ? QString() : old),
               QStringLiteral("S15.69: %1 after reveal() ended with \"%2\" selected (listed %3)")
                   .arg(clear ? QStringLiteral("clearSelection()") : QStringLiteral("select()"), m.selectedPath()).arg(listed));
    }

    // GOAL: S15.69 -- a revealed path that the scan does not list is given up: a later scan that
    //       lists a folder by that name does not select it.
    // CONTROL: app/library/library_manager.cpp, settle()'s `m_reveal.reset();` deleted -> red,
    //       "S15.69: a path revealed before it existed was selected when it appeared later"; restored.
    m.clearSelection();
    const QString never = dir + QStringLiteral("/fifth");
    m.reveal(never);
    const bool scanned = waitUntil([&] { return !QThreadPool::globalInstance()->activeThreadCount(); }, kWaitMs);
    pump(50);
    if (!scanned) {
        expect(false, QStringLiteral("S15.69: the scan reveal() started was still running after %1 ms").arg(kWaitMs));
        return;
    }
    if (!moveInFolder(staging, dir, QStringLiteral("fifth"), planEntries(titled("Fifth"))).isEmpty()) {
        fault("S15.69", QStringLiteral("fifth could not be moved in"));
        return;
    }
    m.rescan();
    const bool appeared = waitUntil([&] { return find(m.wallpapers(), QStringLiteral("fifth")) != nullptr; }, kWaitMs);
    pump(50);
    expect(appeared && m.selectedPath().isEmpty(),
           QStringLiteral("S15.69: a path revealed before it existed was selected when it appeared later: \"%1\" (listed %2)").arg(m.selectedPath()).arg(appeared));

    // GOAL: S15.69 -- a folder moved in with a file still open for writing and revealed is not
    //       given up by the scan that holds it back: once the file is closed, the next scan lists
    //       and selects it.
    // WHY-THIS-SHAPE: the folder is renamed in with its asset open, so the walk before reveal()'s
    //       scan finds the writer and holds it; the result is taken with the pool drained and
    //       posted events delivered, and the folder shown to be absent from it, before the close.
    //       The quiet period here outlasts the test, so the scan after the close is rescan()'s.
    // CONTROL: app/library/library_manager.cpp, rescan()'s continuation block
    //       `if (m_reveal && held.contains(...)) { m_reveal->scanBegun = false; }` deleted -> red,
    //       "S15.69: a folder revealed while a file in it was open for writing ended listed 1 and
    //       selected """; restored.
    m.clearSelection();
    Entries sixth = planEntries(titled("Sixth"));
    const QByteArray asset = sixth.takeLast().second;
    QFile open(staging + QStringLiteral("/sixth/assets/video.mp4"));
    if (!writeFolder(staging + QStringLiteral("/sixth"), sixth).isEmpty() || !QDir().mkpath(staging + QStringLiteral("/sixth/assets")) ||
        !open.open(QIODevice::WriteOnly) || open.write(asset.left(4)) != 4 || !open.flush() ||
        !QDir().rename(staging + QStringLiteral("/sixth"), dir + QStringLiteral("/sixth"))) {
        fault("S15.69", QStringLiteral("sixth could not be moved in with its asset open"));
        return;
    }
    m.reveal(dir + QStringLiteral("/sixth"));
    const bool heldScanDone = QThreadPool::globalInstance()->waitForDone(kWaitMs);
    QCoreApplication::sendPostedEvents();
    const bool heldBack = heldScanDone && find(m.wallpapers(), QStringLiteral("sixth")) == nullptr && m.state() == LibraryManager::State::Ready;
    const bool closed = open.write(asset.mid(4)) == asset.size() - 4 && open.flush();
    open.close();
    if (!heldBack || !closed) {
        notVerified("S15.69 (a reveal held back)", heldBack ? "the rest of the asset could not be written" : "the scan reveal() started did not hold the open folder back");
        return;
    }
    m.rescan();
    const bool listed = waitUntil([&] { return find(m.wallpapers(), QStringLiteral("sixth")) != nullptr; }, kWaitMs);
    pump(50);
    expect(listed && m.selectedPath() == dir + QStringLiteral("/sixth"),
           QStringLiteral("S15.69: a folder revealed while a file in it was open for writing ended listed %1 and selected \"%2\"").arg(listed).arg(m.selectedPath()));
}

// S15.490
void probeReused(const QString& root) {
    const QString dir = root + QStringLiteral("/reused");
    const QString asset = dir + QStringLiteral("/ocean/assets/video.mp4");
    if (!writeFolder(dir + QStringLiteral("/ocean"), planEntriesWith(QByteArray(kPlanManifest), soundVideo())).isEmpty()) {
        fault("S15.490", QStringLiteral("the library could not be written"));
        return;
    }
    // A quiet period no test outlasts, so only rescan() starts a scan.
    LibraryManager m(dir, 600000);
    m.rescan();
    const auto oceanAudio = [&] {
        const Wallpaper* ocean = find(m.wallpapers(), QStringLiteral("ocean"));
        return ocean != nullptr && ocean->usable() ? int(ocean->audio) : -1;
    };
    if (!readyWith(m, 1) || oceanAudio() != int(AudioStreams::Present)) {
        notVerified("S15.490 (a rescan reuses the probe's answer)", "the first scan did not list ocean with sound");
        return;
    }
    // Non-media bytes of the same size in place, the modification time put back, so the stamp is unchanged.
    const QByteArray path = QFile::encodeName(asset);
    struct stat before {};
    const int fd = ::stat(path.constData(), &before) == 0 ? ::open(path.constData(), O_WRONLY | O_CLOEXEC) : -1;
    const QByteArray junk(qsizetype(before.st_size), 'x');
    const timespec times[2] = {{0, UTIME_OMIT}, before.st_mtim};
    const bool rewritten = fd >= 0 && ::write(fd, junk.constData(), size_t(junk.size())) == junk.size() && ::futimens(fd, times) == 0;
    if (fd >= 0) {
        ::close(fd);
    }
    // A second folder, so the rescan's result always differs and its arrival can be waited for.
    if (!rewritten || !writeFolder(dir + QStringLiteral("/marker"), planEntries(titled("Marker"))).isEmpty()) {
        fault("S15.490", QStringLiteral("the asset could not be rewritten, or the marker folder written"));
        return;
    }
    // GOAL: S15.490 at the manager -- rescan() hands the rows it shows to the scan, so an asset whose
    //       stamp is unchanged keeps the answer it was probed with.
    // WHY-THIS-SHAPE: the asset is no longer media, so a scan handed nothing reads it as Unknown; the marker
    //       folder makes the rescan's result a change, whose arrival is the barrier.
    // CONTROL: via control_batch.sh, --phase '^probeReused$', 2026-10-11, LibraryManager::rescan
    //       (app/library/library_manager.cpp) calling `scanLibrary(directory)` with no rows -> "FAIL: S15.490:
    //       after a rescan with ocean's stamp unchanged, ocean's audio reads 0 (Present is 2; rescanned 1)".
    //       Reverted.
    m.rescan();
    const bool rescanned = readyWith(m, 2);
    expect(rescanned && oceanAudio() == int(AudioStreams::Present),
           QStringLiteral("S15.490: after a rescan with ocean's stamp unchanged, ocean's audio reads %1 (Present is %2; rescanned %3)")
               .arg(oceanAudio()).arg(int(AudioStreams::Present)).arg(rescanned));
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "library_manager";
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified("S15.36-S15.44, S15.48, S15.49, S15.57, S15.69", "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    QCoreApplication app(argc, argv);

    const QString root = scratch.path();
    runPhase("states", [&] { states(root); });
    runPhase("overflowingManifest", [&] { overflowingManifest(root); });
    runPhase("folderMadeReadable", [&] { folderMadeReadable(root); });
    runPhase("queuedScans", [&] { queuedScans(root); });
    runPhase("selectionAndValues", [&] { selectionAndValues(root); });
    runPhase("watching", [&] { watching(root); });
    runPhase("quietPeriod", [&] { quietPeriod(root); });
    runPhase("betweenReadAndHandled", [&] { betweenReadAndHandled(root); });
    runPhase("idleLibrary", [&] { idleLibrary(root); });
    runPhase("revealed", [&] { revealed(root); });
    runPhase("probeReused", [&] { probeReused(root); });
    return verdict();
}
