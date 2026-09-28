// SPDX-License-Identifier: GPL-2.0-or-later

// app/library/library_manager.cpp against real folders: its states, its scans, the open wallpaper
// and its edited values, and the watch that re-reads the folder by itself. No bus.
// Goals S15.36-S15.44, S15.48, S15.49, S15.57 in GOALS.md.
//
// Scans are counted inside this process by open_counter.cpp: each scan opens the folder once
// (opendir), and nothing else here opens it while the folder is watched with inotify.

#include "library_manager.h"
#include "open_counter.h"
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

void expect(bool cond, const QString& msg) {
    check(cond, msg.toStdString().c_str());
}

void fault(const char* goal, const QString& what) {
    harnessFailure(std::string(goal) + ": " + what.toStdString());
}

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

QByteArray titled(const char* title) {
    OrderedJson json = planManifest();
    json["title"] = title;
    return jsonBytes(json);
}

// A folder with the given archives; empty string on success.
[[nodiscard]] QString makeFolder(const QString& dir, const QList<QPair<QString, QByteArray>>& manifests) {
    if (!QDir().mkpath(dir)) {
        return QStringLiteral("could not make %1").arg(dir);
    }
    for (const auto& [name, manifest] : manifests) {
        if (const QString why = writeZip(dir + QLatin1Char('/') + name, planEntries(manifest)); !why.isEmpty()) {
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
    if (const QString why = makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
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
        expect(false, QStringLiteral("S15.36: the first scan of a folder with one package never settled Ready with it; state %1, [%2]")
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
        harnessFailure("S15.36: the folder could not be replaced by a file");
        return;
    }
    m.rescan();
    const bool unavailable = waitUntil([&] { return m.state() == LibraryManager::State::Unavailable; }, kWaitMs);
    expect(unavailable && m.unavailableReason() == dir + QStringLiteral(" is not a folder.") && m.wallpapers().isEmpty(),
           QStringLiteral("S15.36: a path that is a file read as state %1, \"%2\", with %3 wallpaper(s)")
               .arg(int(m.state())).arg(m.unavailableReason()).arg(m.wallpapers().size()));

    if (!QFile::remove(dir) || !QDir().rename(moved, dir)) {
        harnessFailure("S15.36: the folder could not be put back");
        return;
    }
    m.rescan();
    // Each wait or call is made before its expect(): the message argument may be built first.
    const bool cameBack = readyWith(m, 1);
    expect(cameBack && m.unavailableReason().isEmpty(),
           QStringLiteral("S15.36: after the folder came back the library is state %1, \"%2\", [%3]")
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

    // GOAL: S15.36 -- a folder that does not exist is Ready and empty, not Unavailable.
    // CONTROL: app/library/wpx_package.cpp, scanLibrary's `if (error == ENOENT)` made `if (false)`
    //       -> red, "S15.36: a missing folder read as state 2, "…/no/such/folder cannot be read: No
    //       such file or directory.""; restored, green.
    LibraryManager missing(root + QStringLiteral("/no/such/folder"), kSettleMs);
    missing.rescan();
    const bool missingReady = readyWith(missing, 0);
    expect(missingReady && missing.unavailableReason().isEmpty(),
           QStringLiteral("S15.36: a missing folder read as state %1, \"%2\"").arg(int(missing.state())).arg(missing.unavailableReason()));

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
    expect(deniedSettled && denied.state() == LibraryManager::State::Unavailable &&
               denied.unavailableReason() == closed + QStringLiteral(" cannot be read: Permission denied."),
           QStringLiteral("S15.36: a folder that cannot be listed read as state %1, \"%2\"").arg(int(denied.state())).arg(denied.unavailableReason()));
}

// S15.48
void overflowingManifest(const QString& root) {
    const QString dir = root + QStringLiteral("/overflow");
    QByteArray overflow(kPlanManifest);
    overflow.replace("\"default\": 1.0", "\"default\": 1e999");
    if (const QString why = makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)},
                                             {QStringLiteral("overflow.wpx"), overflow}});
        !why.isEmpty() || !overflow.contains("1e999")) {
        fault("S15.48", why.isEmpty() ? QStringLiteral("the 1e999 manifest could not be made") : why);
        return;
    }
    // GOAL: S15.48 -- a library holding a manifest with a number too large for a double (1e999)
    //       settles Ready: the package is listed broken with its reason beside the usable one, and
    //       the state never stays Loading.
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
    const Wallpaper* bad = find(m.wallpapers(), QStringLiteral("overflow.wpx"));
    expect(settled && m.state() == LibraryManager::State::Ready && titles(m.wallpapers()) == u"Ocean Waves,overflow.wpx(broken)" && bad &&
               bad->problem == u"wallpaper.json holds a number too large to read.",
           QStringLiteral("S15.48: a folder holding a 1e999 manifest is state %1 (\"%2\") listing [%3]; the bad one's reason is \"%4\"")
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
    // CONTROL: app/library/library_manager.cpp, IN_ATTRIB deleted from kAncestorEvents -> red,
    //       "S15.49: an unreadable folder made readable still reads state 2"; restored.
    {
        const QString dir = root + QStringLiteral("/opened");
        if (const QString why = makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)}}); !why.isEmpty() || !setMode(dir, 0)) {
            fault("S15.49", why.isEmpty() ? QStringLiteral("the folder could not be made mode 000") : why);
            return;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        const bool refused = waitUntil([&] { return m.state() == LibraryManager::State::Unavailable; }, kWaitMs);
        if (!setMode(dir, 0755)) {
            fault("S15.49", QStringLiteral("the folder could not be made readable"));
            return;
        }
        if (!refused) {
            expect(false, QStringLiteral("S15.49: a mode-000 folder read as state %1, not Unavailable").arg(int(m.state())));
        } else {
            const bool read = readyWith(m, 1);
            expect(read, QStringLiteral("S15.49: an unreadable folder made readable still reads state %1 [%2]").arg(int(m.state())).arg(titles(m.wallpapers())));
        }
    }

    // GOAL: S15.49 -- when the folder is missing and its nearest existing ancestor cannot be read,
    //       that ancestor being made readable, and then the folder appearing, are both noticed.
    // WHY-THIS-SHAPE: the unreadable ancestor refuses a watch too, so only a watch on the next one
    //       up sees the mode change; the folder is created afterwards to show the watch moved down.
    // CONTROL: app/library/library_manager.cpp, placeWatch()'s `if (at == "/") {` made
    //       `if (!awaited.isEmpty()) {` (it stops after one ancestor) -> red, "S15.49: after its
    //       unreadable parent was made readable the missing folder reads state 2"; restored.
    const QString parent = root + QStringLiteral("/sealed");
    const QString dir = parent + QStringLiteral("/wallpaperx");
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
        expect(false, QStringLiteral("S15.49: a folder under a mode-000 parent read as state %1, not Unavailable").arg(int(m.state())));
        return;
    }
    const bool readable = readyWith(m, 0);
    expect(readable && m.unavailableReason().isEmpty(),
           QStringLiteral("S15.49: after its unreadable parent was made readable the missing folder reads state %1, \"%2\"")
               .arg(int(m.state())).arg(m.unavailableReason()));
    if (readable) {
        if (const QString why = makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
            fault("S15.49", why);
            return;
        }
        const bool listed = readyWith(m, 1);
        expect(listed, QStringLiteral("S15.49: the folder created under the parent made readable was not read; [%1]").arg(titles(m.wallpapers())));
    }
}

// S15.37
void queuedScans(const QString& root) {
    const QString dir = root + QStringLiteral("/queued");
    if (const QString why = makeFolder(dir, {{QStringLiteral("old.wpx"), titled("Old")}}); !why.isEmpty()) {
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
    // WHY-THIS-SHAPE: scan A's work is finished (the pool drained) before the folder is changed and
    //       before the event loop runs, so A provably read "Mid" and three rescan() calls land while
    //       its result is unhandled. Scans are counted by this process opening the folder: the
    //       lists alone cannot tell one follow-up from three, since every later scan reads the same
    //       folder and an unchanged list is not published again.
    // CONTROL: app/library/library_manager.cpp, rescan()'s `if (m_scanning != Scanning::No) {...}`
    //       block deleted, so scans overlap -> red, "S15.37: 4 scan(s) read the folder, not 2";
    //       the continuation's `== Scanning::YesThenAgain) { rescan(); }` follow-up made never to run
    //       -> red, "S15.37: the library ended at [Mid,Old], not [New,Old]"; the continuation made to
    //       throw a scan away when another was asked for (`if (m_scanning == Scanning::YesThenAgain)
    //       { m_scanning = Scanning::No; rescan(); return; }` first) -> red, "S15.37: the lists
    //       published were [New,Old], not [Mid,Old | New,Old]"; rescan() given `m_model.assign({});`
    //       at its start -> red, "the list shown while the scan ran was []"; restored.
    // COVERAGE-ASSERT: the counter saw the first scan.
    const int first = scans.take().folderOpens;
    if (!writeZip(dir + QStringLiteral("/mid.wpx"), planEntries(titled("Mid"))).isEmpty()) {
        harnessFailure("S15.37: mid.wpx could not be written");
        return;
    }
    m.rescan();
    if (!QThreadPool::globalInstance()->waitForDone(kWaitMs)) {
        notVerified("S15.37", "the first scan did not finish within the bound, so what it read could not be fixed");
        return;
    }
    if (!QFile::remove(dir + QStringLiteral("/mid.wpx")) || !writeZip(dir + QStringLiteral("/new.wpx"), planEntries(titled("New"))).isEmpty()) {
        harnessFailure("S15.37: the folder could not be changed between the scans");
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
    const int ran = scans.take().folderOpens;
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
    const QString ocean = dir + QStringLiteral("/ocean.wpx");
    const QString still = dir + QStringLiteral("/still.wpx");
    const QString broken = dir + QStringLiteral("/broken.wpx");
    OrderedJson stillJson = planManifest();
    stillJson["title"] = "A Still";
    stillJson.erase("properties");
    if (const QString why = makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)},
                                             {QStringLiteral("still.wpx"), jsonBytes(stillJson)}});
        !why.isEmpty() || !writeBytes(broken, "not a zip")) {
        fault("S15.38", QStringLiteral("the folder could not be built: ") + why);
        return;
    }
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!readyWith(m, 3)) {
        expect(false, QStringLiteral("S15.38: the folder never settled with three entries; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    Counts c;
    countSignals(m, c);
    const QVariantMap defaults{{QStringLiteral("speed"), 1.0}, {QStringLiteral("mute"), true}, {QStringLiteral("tint"), QStringLiteral("#ffffff")}};

    // GOAL: S15.38 -- only a usable wallpaper in the list can be opened: select() of a broken or
    //       unknown path returns false and changes and emits nothing.
    // CONTROL: app/library/library_manager.cpp, select()'s `|| !it->usable()` deleted -> red,
    //       "S15.38: selecting a broken file or an unknown path ... selected "…/broken.wpx""; restored.
    const bool refusedBroken = !m.select(broken);
    const bool refusedUnknown = !m.select(dir + QStringLiteral("/nope.wpx"));
    expect(refusedBroken && refusedUnknown && m.selectedPath().isEmpty() && c.selection == 0 && c.values == 0,
           QStringLiteral("S15.38: selecting a broken file or an unknown path returned %1/%2 and selected \"%3\" (%4 selection signal(s))")
               .arg(!refusedBroken).arg(!refusedUnknown).arg(m.selectedPath()).arg(c.selection));
    const bool opened = m.select(ocean);
    expect(opened && m.selectedPath() == ocean && m.selectedWallpaper().title == u"Ocean Waves" && c.selection == 1,
           QStringLiteral("S15.38: selecting ocean.wpx gave \"%1\" with %2 selection signal(s)").arg(m.selectedPath()).arg(c.selection));

    // GOAL: S15.40 -- propertyValues holds every property of the open wallpaper at its default
    //       before any edit.
    // CONTROL: app/library/library_manager.cpp, propertyValues()'s `m_edits.value(property.key, property.defaultValue)`
    //       made `m_edits.value(property.key, QVariant())` -> red, "S15.40: the defaults read {mute=,
    //       speed=, tint=}"; restored.
    expect(m.propertyValues() == defaults, QStringLiteral("S15.40: the defaults read %1").arg(describe(m.propertyValues())));

    c.values = 0;
    const bool tookSpeed = m.setPropertyValue(QStringLiteral("speed"), 2.5);
    expect(tookSpeed && m.propertyValues().value(QStringLiteral("speed")) == QVariant(2.5) && c.values == 1,
           QStringLiteral("S15.40: an accepted edit of speed to 2.5 read %1 with %2 signal(s)").arg(describe(m.propertyValues())).arg(c.values));

    // GOAL: S15.40 -- an edit the property cannot hold is refused: false, no value changed, no
    //       propertyValuesChanged.
    // CONTROL: app/library/library_manager.cpp, setPropertyValue()'s `if (!accepted) { return false; }`
    //       made `if (!accepted) { changeSelection([&] { m_edits.insert(key, value); }); return true; }`
    //       -> red, "S15.40: the refused edit speed = 9 returned 1"; restored.
    const QVariantMap before = m.propertyValues();
    const std::pair<const char*, QVariant> refused[] = {
        {"speed", 9.0}, {"speed", QStringLiteral("1")}, {"speed", std::numeric_limits<double>::quiet_NaN()},
        {"nope", 1.0},  {"mute", 1},                     {"tint", QStringLiteral("red")},
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
    //       speed=2.5, tint=#ffffff}"; restored.
    const bool tookColour = m.setPropertyValue(QStringLiteral("tint"), QColor(255, 0, 0));
    const bool tookMute = m.setPropertyValue(QStringLiteral("mute"), false);
    expect(tookColour && tookMute && m.propertyValues().value(QStringLiteral("tint")) == QVariant(QStringLiteral("#ff0000")) &&
               m.propertyValues().value(QStringLiteral("mute")) == QVariant(false),
           QStringLiteral("S15.40: a QColor tint and mute=false read %1").arg(describe(m.propertyValues())));

    // GOAL: S15.40 -- resetPropertyValues() puts every value back to its default, and says so.
    // CONTROL: app/library/library_manager.cpp, resetPropertyValues()'s `m_edits.clear();` made
    //       `m_edits.remove(QStringLiteral("speed"));` -> red, "S15.40: after a reset the values read
    //       {mute=false, speed=1, tint=#ff0000}"; restored.
    c.values = 0;
    m.resetPropertyValues();
    expect(m.propertyValues() == defaults && c.values == 1,
           QStringLiteral("S15.40: after a reset the values read %1 with %2 signal(s)").arg(describe(m.propertyValues())).arg(c.values));

    // GOAL: S15.38 -- opening another wallpaper drops the edits made on the previous one;
    //       re-selecting the open one keeps them.
    // CONTROL: app/library/library_manager.cpp, the `m_edits.clear();` inside select()'s
    //       changeSelection deleted -> red, "S15.38: re-select kept edits 1, the other wallpaper had
    //       none 1; after coming back speed read 2.5"; restored.
    const bool setSpeed = m.setPropertyValue(QStringLiteral("speed"), 2.5);
    const bool reselect = setSpeed && m.select(ocean) && m.propertyValues().value(QStringLiteral("speed")) == QVariant(2.5);
    const bool other = m.select(still) && m.propertyValues().isEmpty();
    const bool back = m.select(ocean);
    expect(reselect && other && back && m.propertyValues() == defaults,
           QStringLiteral("S15.38: re-select kept edits %1, the other wallpaper had none %2; after coming back speed read %3")
               .arg(reselect).arg(other).arg(m.propertyValues().value(QStringLiteral("speed")).toString()));

    // GOAL: S15.40 -- after the open file is rewritten, an edit the new version cannot hold falls
    //       back to the default, a property it dropped is gone, a new one is at its default, and an
    //       edit still valid is kept -- with no further setPropertyValue, and no emission ever
    //       exposing an out-of-range value.
    // WHY-THIS-SHAPE: every propertyValuesChanged is checked against the wallpaper as read at that
    //       moment, not only the end state, since the first emission after the rescan is the one a
    //       binding (and Apply) would read.
    // CONTROL: app/library/library_manager.cpp, settle()'s pruning `property.accept(m_edits.value(property.key))`
    //       made `std::optional<QVariant>(m_edits.value(property.key))` -> red, "S15.40: after the
    //       rewrite ... the values read {speed=2.5, tint=#00ff00, zoom=2}" and "an emission exposed
    //       speed=2.5"; settle()'s `m_edits = std::move(edits);` made `m_edits.clear();` -> red,
    //       "...the values read {speed=1, tint=#ffffff, zoom=2}"; restored.
    // COVERAGE-ASSERT: at least one emission was checked.
    if (!m.setPropertyValue(QStringLiteral("speed"), 2.5) || !m.setPropertyValue(QStringLiteral("mute"), false) ||
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
    narrowed["properties"] = OrderedJson::parse(R"({"speed": {"type": "float", "default": 1.0, "min": 0.1, "max": 2.0},
                                                   "tint": {"type": "color", "default": "#ffffff"},
                                                   "zoom": {"type": "float", "default": 2.0, "min": 0.0, "max": 4.0}})");
    if (!writeZip(ocean, planEntries(jsonBytes(narrowed))).isEmpty()) {
        harnessFailure("S15.40: the narrowed ocean.wpx could not be written");
        return;
    }
    const bool reread = waitUntil([&] { return m.selectedWallpaper().properties.size() == 3 && m.selectedWallpaper().properties[2].key == u"zoom"; }, kWaitMs);
    const QVariantMap want{{QStringLiteral("speed"), 1.0}, {QStringLiteral("tint"), QStringLiteral("#00ff00")}, {QStringLiteral("zoom"), 2.0}};
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
    //       (speed 2.5, mute off) are exactly the ones the narrowing refused or dropped.
    // CONTROL: app/library/library_manager.cpp, settle()'s `m_edits = std::move(edits);` deleted
    //       and propertyValues() made to filter instead (`m_edits.value(property.key, property.defaultValue)`
    //       wrapped in `property.accept(...).value_or(property.defaultValue)`), one patch, the
    //       behaviour before decision B -> red, "S15.57: after the file was widened again the
    //       values read {mute=false, speed=2.5, tint=#00ff00}"; restored.
    if (reread) {
        if (!writeZip(ocean, planEntries(QByteArray(kPlanManifest))).isEmpty()) {
            harnessFailure("S15.57: the widened ocean.wpx could not be written");
            return;
        }
        const bool widened = waitUntil([&] { return m.selectedWallpaper().properties.size() == 3 && m.selectedWallpaper().properties[1].key == u"mute"; }, kWaitMs);
        const QVariantMap wantBack{{QStringLiteral("speed"), 1.0}, {QStringLiteral("mute"), true}, {QStringLiteral("tint"), QStringLiteral("#00ff00")}};
        expect(widened && m.selectedPath() == ocean && m.propertyValues() == wantBack,
               QStringLiteral("S15.57: after the file was widened again (reread %1) the values read %2, not %3")
                   .arg(widened).arg(describe(m.propertyValues()), describe(wantBack)));
    }

    // GOAL: S15.39 -- when the open wallpaper's file is removed, the selection and its edits are
    //       dropped; when the same file comes back it is listed but not reopened.
    // WHY-THIS-SHAPE: the return is waited for by its listing, so "not reopened" is judged on a
    //       scan that saw the file, not on one that had not happened yet.
    // CONTROL: app/library/library_manager.cpp, settle()'s `m_selection.clear();` deleted -> red,
    //       "S15.39: with its file removed the open wallpaper is still "…/ocean.wpx"" and "after
    //       the file came back ... "…/ocean.wpx" is open"; restored.
    if (!m.setPropertyValue(QStringLiteral("speed"), 1.5)) {
        expect(false, QStringLiteral("S15.39: speed 1.5 was refused on the open wallpaper; values %1").arg(describe(m.propertyValues())));
        return;
    }
    c = Counts{};
    const QString keep = dir + QStringLiteral(".ocean");
    if (!QFile::rename(ocean, keep)) {
        harnessFailure("S15.39: ocean.wpx could not be moved away");
        return;
    }
    const bool gone = waitUntil([&] { return find(m.wallpapers(), QStringLiteral("ocean.wpx")) == nullptr; }, kWaitMs);
    expect(gone && m.selectedPath().isEmpty() && m.propertyValues().isEmpty() && !m.selectedWallpaper().usable() && c.selection >= 1,
           QStringLiteral("S15.39: with its file removed (seen %1) the open wallpaper is still \"%2\", values %3, %4 selection signal(s)")
               .arg(gone).arg(m.selectedPath(), describe(m.propertyValues())).arg(c.selection));
    if (!QFile::rename(keep, ocean)) {
        harnessFailure("S15.39: ocean.wpx could not be put back");
        return;
    }
    const bool returned = waitUntil([&] { const Wallpaper* w = find(m.wallpapers(), QStringLiteral("ocean.wpx")); return w && w->usable(); }, kWaitMs);
    expect(returned && m.selectedPath().isEmpty() && m.propertyValues().isEmpty(),
           QStringLiteral("S15.39: after the file came back (listed %1) \"%2\" is open with values %3").arg(returned).arg(m.selectedPath(), describe(m.propertyValues())));

    // GOAL: S15.39 -- when the open wallpaper's file turns broken, the selection and its edits are
    //       dropped; when it is repaired it is not reopened.
    // CONTROL: app/library/library_manager.cpp, settle()'s `|| !kept->usable()` deleted -> red,
    //       "S15.39: with its file broken the open wallpaper is still "…/ocean.wpx""; restored.
    if (!m.select(ocean) || !m.setPropertyValue(QStringLiteral("speed"), 1.5)) {
        expect(false, QStringLiteral("S15.39: ocean.wpx could not be reopened before breaking it"));
        return;
    }
    const auto goodBytes = readBytes(ocean);
    if (!goodBytes || !writeBytes(ocean, "no longer a zip")) {
        harnessFailure("S15.39: ocean.wpx could not be broken");
        return;
    }
    const bool broke = waitUntil([&] { const Wallpaper* w = find(m.wallpapers(), QStringLiteral("ocean.wpx")); return w && !w->usable(); }, kWaitMs);
    expect(broke && m.selectedPath().isEmpty() && m.propertyValues().isEmpty(),
           QStringLiteral("S15.39: with its file broken (seen %1) the open wallpaper is still \"%2\", values %3").arg(broke).arg(m.selectedPath(), describe(m.propertyValues())));
    if (!writeBytes(ocean, *goodBytes)) {
        harnessFailure("S15.39: ocean.wpx could not be repaired");
        return;
    }
    const bool repaired = waitUntil([&] { const Wallpaper* w = find(m.wallpapers(), QStringLiteral("ocean.wpx")); return w && w->usable(); }, kWaitMs);
    expect(repaired && m.selectedPath().isEmpty(),
           QStringLiteral("S15.39: after the file was repaired (seen %1) \"%2\" is open").arg(repaired).arg(m.selectedPath()));
}

// S15.41, S15.42
void watching(const QString& root) {
    // GOAL: S15.41 -- a package added to the folder is listed without rescan() being called.
    // CONTROL: app/library/library_manager.cpp, the constructor's `connect(m_inotifyReady.get(),
    //       &QSocketNotifier::activated, ...)` deleted -> red, "S15.41: a package added to the
    //       folder was not listed"; restored.
    {
        const QString dir = root + QStringLiteral("/added");
        if (!QDir().mkpath(dir)) {
            fault("S15.41", QStringLiteral("could not make ") + dir);
            return;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        if (!readyWith(m, 0)) {
            expect(false, QStringLiteral("S15.41: an empty folder never settled Ready"));
        } else if (const QString why = writeZip(dir + QStringLiteral("/new.wpx"), planEntries(QByteArray(kPlanManifest))); !why.isEmpty()) {
            fault("S15.41", why);
        } else {
            const bool listed = waitUntil([&] { return m.wallpapers().size() == 1 && m.wallpapers()[0].usable(); }, kWaitMs);
            expect(listed, QStringLiteral("S15.41: a package added to the folder was not listed; [%1]").arg(titles(m.wallpapers())));
        }
    }

    // GOAL: S15.41 -- a library folder created after the library started is noticed, and a
    //       package put in it is listed, without rescan().
    // CONTROL: app/library/library_manager.cpp, handle()'s `return name == ancestor->awaited;` made
    //       `return false;` -> red, "S15.41: a folder created after start was not read"; restored.
    {
        const QString parent = root + QStringLiteral("/later");
        const QString dir = parent + QStringLiteral("/wallpaperx");
        if (!QDir().mkpath(parent)) {
            fault("S15.41", QStringLiteral("could not make ") + parent);
            return;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        if (!readyWith(m, 0)) {
            expect(false, QStringLiteral("S15.41: a missing folder never settled Ready"));
        } else if (const QString why = makeFolder(dir, {{QStringLiteral("new.wpx"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
            fault("S15.41", why);
        } else {
            const bool listed = waitUntil([&] { return m.wallpapers().size() == 1 && m.wallpapers()[0].usable(); }, kWaitMs);
            expect(listed, QStringLiteral("S15.41: a folder created after start was not read; [%1]").arg(titles(m.wallpapers())));
        }
    }

    // A listed package's own close, as the folder's watch reports it, is what the next two controls
    // remove: handle()'s final `return !empty || ...;` made `return !std::ranges::contains(
    // m_model.wallpapers(), decodeFileName(name), &Wallpaper::fileName);`, so only a name with no row
    // still scans.

    // GOAL: S15.41 -- a package rewritten in place (same name, same inode) is read again without rescan().
    // WHY-THIS-SHAPE: the rewrite truncates and writes the existing file rather than replacing it,
    //       so no entry appears or goes: only the folder watch's events for a listed name -- its
    //       writes, then its close -- report it.
    // CONTROL: app/library/library_manager.cpp, the handle() patch above -> red, "S15.41: a package
    //       rewritten in place still reads "Ocean Waves""; restored.
    {
        const QString dir = root + QStringLiteral("/inplace");
        const QString path = dir + QStringLiteral("/ocean.wpx");
        const QString why = makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)}});
        const auto bytes = zipBytes(dir + QStringLiteral(".scratch"), planEntries(titled("Rewritten")));
        if (!why.isEmpty() || !bytes) {
            fault("S15.41", why.isEmpty() ? QStringLiteral("the rewritten bytes could not be built") : why);
        } else {
            LibraryManager m(dir, kSettleMs);
            m.rescan();
            if (!readyWith(m, 1)) {
                expect(false, QStringLiteral("S15.41: the in-place folder never settled; [%1]").arg(titles(m.wallpapers())));
            } else if (!writeBytes(path, *bytes)) {
                fault("S15.41", QStringLiteral("ocean.wpx could not be rewritten"));
            } else {
                const bool reread = waitUntil([&] { return titles(m.wallpapers()) == u"Rewritten"; }, kWaitMs);
                expect(reread, QStringLiteral("S15.41: a package rewritten in place still reads \"%1\"").arg(titles(m.wallpapers())));
            }
        }
    }

    // GOAL: S15.41 -- an unreadable package made readable (chmod) is re-read and becomes usable
    //       without rescan().
    // CONTROL: app/library/library_manager.cpp, IN_ATTRIB deleted from kFolderEvents -> red,
    //       "S15.41: a package made readable still reads locked.wpx(broken)"; restored.
    if (::geteuid() == 0) {
        notVerified("S15.41 (a package made readable)", "running as root, which can read a mode-000 file");
    } else {
        const QString dir = root + QStringLiteral("/chmod");
        const QString path = dir + QStringLiteral("/locked.wpx");
        if (const QString why = makeFolder(dir, {{QStringLiteral("locked.wpx"), QByteArray(kPlanManifest)}}); !why.isEmpty() || !setMode(path, 0)) {
            fault("S15.41", why.isEmpty() ? QStringLiteral("locked.wpx could not be made mode 000") : why);
        } else {
            LibraryManager m(dir, kSettleMs);
            m.rescan();
            const bool brokenFirst = readyWith(m, 1) && !m.wallpapers()[0].usable();
            if (!setMode(path, 0644)) {
                fault("S15.41", QStringLiteral("locked.wpx could not be made readable"));
            } else if (!brokenFirst) {
                expect(false, QStringLiteral("S15.41: the mode-000 package was not listed broken first: [%1]").arg(titles(m.wallpapers())));
            } else {
                const bool usable = waitUntil([&] { return m.wallpapers().size() == 1 && m.wallpapers()[0].usable(); }, kWaitMs);
                expect(usable, QStringLiteral("S15.41: a package made readable still reads %1").arg(titles(m.wallpapers())));
            }
        }
    }

    // GOAL: S15.42 -- a package still being written when first read (half a ZIP, listed broken)
    //       ends up usable once the rest is written; it is never left stuck broken.
    // WHY-THIS-SHAPE: the second half is written only after the broken listing is seen, so the
    //       first read provably saw half a file; the append is then a write to a listed name,
    //       which only the folder watch's write events report.
    // CONTROL: app/library/library_manager.cpp, the handle() patch above -> red, "S15.42: the
    //       package stayed half.wpx(broken) after the rest was written"; restored.
    {
        const QString dir = root + QStringLiteral("/halves");
        const QString path = dir + QStringLiteral("/half.wpx");
        const auto bytes = zipBytes(dir + QStringLiteral(".scratch"), planEntries(QByteArray(kPlanManifest)));
        if (!QDir().mkpath(dir) || !bytes) {
            fault("S15.42", QStringLiteral("the halves folder or its package bytes could not be built"));
            return;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        const qsizetype half = bytes->size() / 2;
        if (!readyWith(m, 0)) {
            expect(false, QStringLiteral("S15.42: the empty folder never settled"));
        } else if (!writeBytes(path, bytes->left(half))) {
            fault("S15.42", QStringLiteral("the first half could not be written"));
        } else if (!waitUntil([&] { return m.wallpapers().size() == 1 && !m.wallpapers()[0].usable(); }, kWaitMs)) {
            expect(false, QStringLiteral("S15.42: half a package was never listed; [%1]").arg(titles(m.wallpapers())));
        } else if (!writeBytes(path, bytes->mid(half), QIODevice::WriteOnly | QIODevice::Append)) {
            fault("S15.42", QStringLiteral("the second half could not be written"));
        } else {
            const bool usable = waitUntil([&] { return m.wallpapers().size() == 1 && m.wallpapers()[0].usable(); }, kWaitMs);
            expect(usable, QStringLiteral("S15.42: the package stayed %1 after the rest was written").arg(titles(m.wallpapers())));
        }
    }
}

// S15.42
void quietPeriod(const QString& root) {
    constexpr int settle = 150;
    constexpr int gapMs = 20;
    constexpr int chunks = 8;
    const QString dir = root + QStringLiteral("/burst");
    const QString path = dir + QStringLiteral("/ocean.wpx");
    const QString why = makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)}});
    const auto bytes = zipBytes(dir + QStringLiteral(".scratch"), planEntries(titled("Burst")));
    if (!why.isEmpty() || !bytes) {
        fault("S15.42", why.isEmpty() ? QStringLiteral("the burst's package bytes could not be built") : why);
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
        expect(false, QStringLiteral("S15.42: the burst folder never settled"));
        return;
    }
    // GOAL: S15.42 -- writes to a watched package that keep coming sooner than the quiet period
    //       apart cost no scan while they last and exactly one after the last, which reads the
    //       finished file.
    // WHY-THIS-SHAPE: the event loop runs between writes (a test that slept instead would starve
    //       the manager's timer and pass whatever it did); scans are counted by this process
    //       opening the folder. If the test itself stalled so that two writes were more than half
    //       the quiet period apart, the run is abandoned as not verified rather than graded, since
    //       a correct manager may then legitimately scan mid-burst. Two defences keep the burst
    //       scan-free -- a write holds the name until its close, and a change is read only after
    //       the quiet period -- and either alone does, so only breaking both turns "during" red.
    // CONTROL: app/library/library_manager.cpp, `m_settle.setInterval(settleMs);` made
    //       `m_settle.setInterval(0);` together with handle()'s IN_MODIFY branch made to return true
    //       without holding the name, one patch -> red, "S15.42: 7 scan(s) ran during the burst";
    //       either alone left it green; the continuation's `} else if (linksChanged) {` made
    //       `} else if (true) {` -> red, "reads [Burst] after 5 scan(s), not [Burst] after exactly
    //       one"; restored.
    // COVERAGE-ASSERT: the counter must have seen the first scan, so a dead counter cannot read 0.
    const int initialScans = scans.take().folderOpens;
    expect(initialScans >= 1, QStringLiteral("S15.42: the scan counter saw %1 scan(s) of the first read; it is not counting").arg(initialScans));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        harnessFailure("S15.42: ocean.wpx could not be opened for the burst");
        return;
    }
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    qint64 widestGap = 0;
    const qsizetype piece = bytes->size() / chunks + 1;
    for (int i = 0; i < chunks; ++i) {
        const QByteArray chunk = bytes->mid(i * piece, piece);
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
    const int duringBurst = scans.take().folderOpens;
    if (widestGap >= settle / 2) {
        notVerified("S15.42 (the quiet period)", "this test stalled " + std::to_string(widestGap) + " ms between two writes, over half the quiet period");
        return;
    }
    const bool reread = waitUntil([&] { return titles(m.wallpapers()) == u"Burst"; }, kWaitMs);
    pump(4 * settle);
    const int afterBurst = scans.take().folderOpens;
    expect(duringBurst == 0, QStringLiteral("S15.42: %1 scan(s) ran during the burst (widest gap %2 ms, quiet period %3 ms)").arg(duringBurst).arg(widestGap).arg(settle));
    expect(reread && afterBurst == 1,
           QStringLiteral("S15.42: after the burst the package reads [%1] after %2 scan(s), not [Burst] after exactly one")
               .arg(titles(m.wallpapers())).arg(afterBurst));
}

// S15.43
void betweenReadAndHandled(const QString& root) {
    // Each case lets a scan finish its reading and changes the folder while the scan's result is
    // still queued, before the event loop runs. The watch placed before the scan is what must
    // report it; a watch placed only when the result is handled would come too late.
    const auto readThenChange = [](LibraryManager& m, const std::function<bool()>& change) {
        m.rescan();
        if (!QThreadPool::globalInstance()->waitForDone(kWaitMs)) {
            return QStringLiteral("the scan did not finish within the bound");
        }
        return change() ? QString() : QStringLiteral("the change could not be made");
    };
    // The one control for the four folder cases below: app/library/library_manager.cpp,
    // rescan()'s `placeWatch();` moved from before the scan into the result's continuation.

    // GOAL: S15.43 -- a package rewritten after a scan read it but before its result was handled
    //       is read again.
    // WHY-THIS-SHAPE: the pool is drained and the event loop not yet run, so the rewrite provably
    //       falls after the read; a new manager's first scan is used, so no earlier watch exists.
    // CONTROL: placeWatch() moved after the scan, as above -> red, "S15.43: a package rewritten
    //       after it was read still reads "Ocean Waves""; restored.
    {
        const QString dir = root + QStringLiteral("/gap-file");
        const QString path = dir + QStringLiteral("/ocean.wpx");
        const QString why = makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)}});
        const auto bytes = zipBytes(dir + QStringLiteral(".scratch"), planEntries(titled("Rewritten in the gap")));
        if (!why.isEmpty() || !bytes) {
            fault("S15.43", why.isEmpty() ? QStringLiteral("the rewritten bytes could not be built") : why);
        } else {
            LibraryManager m(dir, kSettleMs);
            const QString failed = readThenChange(m, [&] { return writeBytes(path, *bytes); });
            if (!failed.isEmpty()) {
                notVerified("S15.43 (a package rewritten in the gap)", failed.toStdString());
            } else {
                const bool reread = waitUntil([&] { return titles(m.wallpapers()) == u"Rewritten in the gap"; }, kWaitMs);
                expect(reread, QStringLiteral("S15.43: a package rewritten after it was read still reads \"%1\"").arg(titles(m.wallpapers())));
            }
        }
    }

    // GOAL: S15.43 -- a package added to the folder in the same gap is listed, and a library
    //       folder created in the gap, while it was missing, is read.
    // CONTROL: placeWatch() moved after the scan, as above -> red, "S15.43: a package added after
    //       the folder was read was not listed" and "a folder created after it was read was not
    //       read"; restored.
    {
        const QString dir = root + QStringLiteral("/gap-dir");
        if (const QString why = makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)}}); !why.isEmpty()) {
            fault("S15.43", why);
        } else {
            LibraryManager m(dir, kSettleMs);
            const QString failed = readThenChange(m, [&] { return writeZip(dir + QStringLiteral("/added.wpx"), planEntries(titled("Added"))).isEmpty(); });
            if (!failed.isEmpty()) {
                notVerified("S15.43 (a package added in the gap)", failed.toStdString());
            } else {
                const bool listed = waitUntil([&] { return titles(m.wallpapers()) == u"Added,Ocean Waves"; }, kWaitMs);
                expect(listed, QStringLiteral("S15.43: a package added after the folder was read was not listed; [%1]").arg(titles(m.wallpapers())));
            }
        }
    }

    {
        const QString parent = root + QStringLiteral("/gap-missing");
        const QString dir = parent + QStringLiteral("/wallpaperx");
        if (!QDir().mkpath(parent)) {
            fault("S15.43", QStringLiteral("could not make ") + parent);
        } else {
            LibraryManager m(dir, kSettleMs);
            const QString failed = readThenChange(m, [&] { return makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)}}).isEmpty(); });
            if (!failed.isEmpty()) {
                notVerified("S15.43 (a folder created in the gap)", failed.toStdString());
            } else {
                const bool listed = waitUntil([&] { return titles(m.wallpapers()) == u"Ocean Waves"; }, kWaitMs);
                expect(listed, QStringLiteral("S15.43: a folder created after it was read was not read; [%1]").arg(titles(m.wallpapers())));
            }
        }
    }

    // GOAL: S15.43 -- a plain file put where the missing folder should be, in the same gap, is
    //       noticed: the library ends Unavailable instead of staying an empty Ready.
    // CONTROL: placeWatch() moved after the scan, as above -> red, "S15.43: a file put at the
    //       missing folder's path after it was read left the library state 1"; restored.
    {
        const QString parent = root + QStringLiteral("/gap-file-at-path");
        const QString dir = parent + QStringLiteral("/wallpaperx");
        if (!QDir().mkpath(parent)) {
            fault("S15.43", QStringLiteral("could not make ") + parent);
        } else {
            LibraryManager m(dir, kSettleMs);
            const QString failed = readThenChange(m, [&] { return writeBytes(dir, "a file"); });
            if (!failed.isEmpty()) {
                notVerified("S15.43 (a file put at the folder's path in the gap)", failed.toStdString());
            } else {
                const bool noticed = waitUntil([&] { return m.state() == LibraryManager::State::Unavailable; }, kWaitMs);
                expect(noticed, QStringLiteral("S15.43: a file put at the missing folder's path after it was read left the library state %1")
                                    .arg(int(m.state())));
            }
        }
    }

    // GOAL: S15.43 -- a linked package whose target, outside the folder, is rewritten after the
    //       scan read it but before the link's own watch is placed is read again.
    // WHY-THIS-SHAPE: the folder's watch cannot see a file in another folder, and the link's
    //       watch is placed only when the result is handled, so only watchLinks() comparing the
    //       stamp it read with the file now can notice.
    // CONTROL: app/library/library_manager.cpp, watchLinks()'s `changed = changed ||
    //       stampOf(wallpaper.filePath).value_or(FileStamp{}) != wallpaper.stamp;` deleted -> red,
    //       "S15.43: a linked package rewritten after it was read still reads "Linked""; restored.
    {
        const QString dir = root + QStringLiteral("/gap-link");
        const QString elsewhere = root + QStringLiteral("/gap-link-target");
        const QString target = elsewhere + QStringLiteral("/target.wpx");
        const QString why = makeFolder(elsewhere, {{QStringLiteral("target.wpx"), titled("Linked")}});
        const auto bytes = zipBytes(elsewhere + QStringLiteral(".scratch"), planEntries(titled("Linked, rewritten")));
        if (!why.isEmpty() || !bytes || !QDir().mkpath(dir) || !QFile::link(target, dir + QStringLiteral("/link.wpx"))) {
            fault("S15.43", why.isEmpty() ? QStringLiteral("the linked package could not be set up") : why);
        } else {
            LibraryManager m(dir, kSettleMs);
            const QString failed = readThenChange(m, [&] { return writeBytes(target, *bytes); });
            if (!failed.isEmpty()) {
                notVerified("S15.43 (a linked package rewritten in the gap)", failed.toStdString());
            } else {
                const bool reread = waitUntil([&] { return titles(m.wallpapers()) == u"Linked, rewritten"; }, kWaitMs);
                expect(reread, QStringLiteral("S15.43: a linked package rewritten after it was read still reads \"%1\"").arg(titles(m.wallpapers())));
            }
        }
    }
}

// S15.44, S15.50
void idleLibrary(const QString& root) {
    constexpr int settle = 20;
    constexpr int window = 40 * settle;
    const QString dir = root + QStringLiteral("/idle");
    const QString locked = dir + QStringLiteral("/locked.wpx");
    const auto package = zipBytes(dir + QStringLiteral(".scratch"), planEntries(QByteArray(kPlanManifest)));
    const QString why = makeFolder(dir, {{QStringLiteral("ocean.wpx"), QByteArray(kPlanManifest)}, {QStringLiteral("locked.wpx"), QByteArray(kPlanManifest)}});
    const RawNamesRemover removeRawNames(dir, {"caf\xe8.wpx", "caf\xe9.wpx"});
    if (!why.isEmpty() || !package || !writeBytes(dir + QStringLiteral("/notzip.wpx"), "hello") ||
        !QFile::link(QStringLiteral("locked.wpx"), dir + QStringLiteral("/lockedlink.wpx")) ||
        !QFile::link(QStringLiteral("nothing-here.wpx"), dir + QStringLiteral("/dangling.wpx")) ||
        !writeRawName(dir, "caf\xe8.wpx", *package) || !writeRawName(dir, "caf\xe9.wpx", *package) || !setMode(locked, 0)) {
        fault("S15.44", QStringLiteral("the idle folder could not be built: ") + why);
        static_cast<void>(setMode(locked, 0644));
        return;
    }
    const bool asRoot = ::geteuid() == 0;
    OpenCounter scans(dir);
    if (!scans.ok()) {
        harnessFailure("S15.44: the open counter was already counting another folder");
        static_cast<void>(setMode(locked, 0644));
        return;
    }
    LibraryManager m(dir, settle);
    m.rescan();
    const bool ready = readyWith(m, 7);
    // GOAL: S15.44, S15.50 -- a library left alone, holding a broken package, an unreadable one, a
    //       link to the unreadable one, a dangling link and two names that are not UTF-8, stops
    //       reading its folder: no scan over forty quiet periods after it settles.
    // WHY-THIS-SHAPE: counted by this process opening the folder. The link to the unreadable
    //       package is the row whose recorded stamp cannot come from a read, so it is the one a
    //       wrong stamp would re-scan forever; the non-UTF-8 names are the rows whose path text
    //       must still name the real file for stat.
    // CONTROL: app/library/wpx_package.cpp, scanLibrary's `wallpaper.stamp = stampFrom(st);`
    //       deleted -> red, "S15.44: an idle library scanned its folder 38 time(s) in 800 ms";
    //       app/library/library_manager.cpp, `IN_OPEN |` added to kFolderEvents, so the scan's own
    //       opens of the packages look like changes -> red, "... scanned its folder 38 time(s)" (39
    //       in another run); restored, green each time.
    // COVERAGE-ASSERT: the counter saw the first scan.
    const int first = scans.take().folderOpens;
    if (!ready) {
        static_cast<void>(setMode(locked, 0644));
        expect(false, QStringLiteral("S15.44: the idle folder never settled with seven entries; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    pump(window);
    const int idle = scans.take().folderOpens;
    if (!setMode(locked, 0644)) {
        harnessFailure("S15.44: locked.wpx could not be made readable again");
    }
    expect(first >= 1, QStringLiteral("S15.44: the scan counter saw %1 scan(s) of the first read; it is not counting").arg(first));
    expect(idle == 0, QStringLiteral("S15.44: an idle library scanned its folder %1 time(s) in %2 ms").arg(idle).arg(window));
    if (asRoot) {
        notVerified("S15.44 (with an unreadable package)", "running as root, which can read a mode-000 file");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "library_manager";
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified("S15.36-S15.44, S15.48, S15.49, S15.57", "no temporary folder could be made");
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

    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file's fixtures, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
