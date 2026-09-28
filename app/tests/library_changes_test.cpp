// SPDX-License-Identifier: GPL-2.0-or-later

// app/library/library_manager.cpp following a folder as it changes: a new package is read only
// once it is finished, a package being rewritten keeps its row, an empty file is never listed,
// holds lapse, the folder is polled when inotify is not used or runs out of watches, scans are
// never thrown away, and the list model changes row by row. Goals S15.52, S15.56, S15.58-S15.66
// in GOALS.md.
//
// Scans and package reads are counted inside this process by open_counter.cpp.

#include "library_manager.h"
#include "open_counter.h"
#include "wpx_fixtures.h"
#include "wpx_package.h"

#include <QAbstractItemModelTester>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QThreadPool>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

namespace {

constexpr int kSettleMs = 30;
// Longer than the quiet period, so a manager that read on quiet alone would read mid-write.
constexpr int kWriteGapMs = 60;
constexpr int kPollMs = 100;
// Every wait below ends as soon as its condition holds; this is only the failure bound.
constexpr int kWaitMs = 3000;

void expect(bool cond, const QString& msg) {
    check(cond, msg.toStdString().c_str());
}

void fault(const char* goal, const QString& what) {
    harnessFailure(std::string(goal) + ": " + what.toStdString());
}

QString titles(const QList<Wallpaper>& list) {
    QStringList out;
    for (const Wallpaper& w : list) {
        out.append(w.usable() ? w.title : w.fileName + QStringLiteral("(broken)"));
    }
    return out.join(u',');
}

QByteArray titled(const char* title) {
    OrderedJson json = planManifest();
    json["title"] = title;
    return jsonBytes(json);
}

std::optional<QByteArray> packageTitled(const QString& scratch, const char* title) {
    return zipBytes(scratch, planEntries(titled(title)));
}

bool readyWith(LibraryManager& m, const QString& list) {
    return waitUntil([&] { return m.state() == LibraryManager::State::Ready && titles(m.wallpapers()) == list; }, kWaitMs);
}

// Every list the manager published, in order.
class History {
public:
    explicit History(LibraryManager& m)
        : m_connection(QObject::connect(&m, &LibraryManager::wallpapersChanged, [this, &m] { m_lists.append(titles(m.wallpapers())); })) {}
    ~History() { QObject::disconnect(m_connection); }
    History(const History&) = delete;
    History& operator=(const History&) = delete;

    QStringList take() { return std::exchange(m_lists, {}); }

private:
    QStringList m_lists;
    QMetaObject::Connection m_connection;
};

// The lists published once the pool is drained and only posted events -- a scan's result among
// them -- are delivered, so no timer or socket notifier runs first. Empty when none was.
QStringList publishedByScan(History& history) {
    if (!QThreadPool::globalInstance()->waitForDone(kWaitMs)) {
        return {};
    }
    QCoreApplication::sendPostedEvents();
    return history.take();
}

// Writes `bytes` to `file` in `pieces`, running the event loop `gapMs` between them, so the
// manager sees every write as it happens. False when a write fails.
bool writeSlowly(QFile& file, const QByteArray& bytes, int pieces, int gapMs) {
    const qsizetype piece = bytes.size() / pieces + 1;
    for (int i = 0; i < pieces; ++i) {
        const QByteArray chunk = bytes.mid(i * piece, piece);
        if (file.write(chunk) != chunk.size() || !file.flush()) {
            return false;
        }
        if (i + 1 < pieces) {
            pump(gapMs);
        }
    }
    return true;
}

// S15.56: a new package is read only once it is finished.
void finishedFiles(const QString& root) {
    const QString dir = root + QStringLiteral("/finished");
    const QString staging = root + QStringLiteral("/staging");
    const auto copied = packageTitled(root + QStringLiteral("/copied.scratch"), "Copied");
    const auto heldOpen = packageTitled(root + QStringLiteral("/held.scratch"), "Held");
    const auto half = packageTitled(root + QStringLiteral("/half.scratch"), "Half");
    if (!copied || !heldOpen || !half || !QDir().mkpath(dir) || !QDir().mkpath(staging) ||
        !writeZip(dir + QStringLiteral("/good.wpx"), planEntries(titled("Good"))).isEmpty() ||
        !writeZip(staging + QStringLiteral("/moved.wpx"), planEntries(titled("Moved"))).isEmpty() ||
        !writeZip(staging + QStringLiteral("/linked.wpx"), planEntries(titled("Linked"))).isEmpty()) {
        fault("S15.56", QStringLiteral("the folder or its packages could not be built"));
        return;
    }
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.56: the folder never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);

    // GOAL: S15.56 -- a new package copied in slowly, with pauses longer than the quiet period,
    //       is published once, usable, after it is closed; never as a broken half.
    // WHY-THIS-SHAPE: the pauses outlast the quiet period, so only holding a new name back until
    //       its writer closes it keeps the halves out; every published list is recorded, not just
    //       the last one.
    // CONTROL: app/library/library_manager.cpp, handle()'s `folder.writing.insert(name, holdFromNow());`
    //       deleted -> red, "S15.56: a slow copy was published as [Good,copied.wpx(broken) |
    //       Copied,Good]" (its creation, read after the first write, scans it); restored.
    {
        QFile file(dir + QStringLiteral("/copied.wpx"));
        if (!file.open(QIODevice::WriteOnly) || !writeSlowly(file, *copied, 8, kWriteGapMs)) {
            fault("S15.56", QStringLiteral("copied.wpx could not be written"));
            return;
        }
        file.close();
        const bool listed = readyWith(m, QStringLiteral("Copied,Good"));
        pump(4 * kSettleMs);
        const QStringList published = history.take();
        expect(listed && published == QStringList{QStringLiteral("Copied,Good")},
               QStringLiteral("S15.56: a slow copy was published as [%1]").arg(published.join(QStringLiteral(" | "))));
    }

    // GOAL: S15.56 -- a new package written whole but still held open is not listed; it is
    //       listed as soon as its writer closes it.
    // CONTROL: app/library/library_manager.cpp, handle()'s `folder.writing.insert(name, holdFromNow());`
    //       deleted -> red, "S15.56: a package still held open was listed: [Copied,Good,Held]";
    //       IN_CLOSE_WRITE deleted from kFolderEvents -> red, "S15.56: a package held open was not
    //       listed when closed"; restored.
    {
        QFile file(dir + QStringLiteral("/held.wpx"));
        if (!file.open(QIODevice::WriteOnly) || file.write(*heldOpen) != heldOpen->size() || !file.flush()) {
            fault("S15.56", QStringLiteral("held.wpx could not be written"));
            return;
        }
        pump(10 * kSettleMs);
        const QString whileOpen = titles(m.wallpapers());
        file.close();
        const bool listed = readyWith(m, QStringLiteral("Copied,Good,Held"));
        static_cast<void>(history.take());
        expect(whileOpen == u"Copied,Good", QStringLiteral("S15.56: a package still held open was listed: [%1]").arg(whileOpen));
        expect(listed, QStringLiteral("S15.56: a package held open was not listed when closed; [%1]").arg(titles(m.wallpapers())));
    }

    // GOAL: S15.56 -- a package created and half-written just before rescan(), its creation still
    //       unread from inotify, is not shown by the scan that read it half-written.
    // WHY-THIS-SHAPE: no event loop runs between creating the file and rescan(), so the creation is
    //       provably unread; the pool is drained, so the scan read the half; then only posted events
    //       are delivered, so no socket notifier can read the creation before the result is handled
    //       (pumping the loop lets the notifier win or lose by the dispatcher's order). good.wpx,
    //       listed since the first scan, is deleted in the same window: a publish without it proves
    //       this scan's result was handled. Unproven premises skip this grade only, so the checks
    //       below stay reachable.
    // CONTROL: app/library/library_manager.cpp, both `readWatchEvents();` calls in rescan() and its
    //       continuation deleted -> red, "S15.56: a scan started while half.wpx was being written,
    //       its creation unread, published [Copied,Held,half.wpx(broken)]"; either one alone deleted
    //       leaves this check green (the other covers it); holdBack()'s `scan.removeIf(...)` line
    //       deleted -> red, the same line. By hand, test-side: half.wpx written whole -> NOT
    //       VERIFIED, "does not read as broken"; restored.
    {
        QFile file(dir + QStringLiteral("/half.wpx"));
        if (!file.open(QIODevice::WriteOnly) || file.write(half->left(half->size() / 2)) != half->size() / 2 || !file.flush() ||
            !QFile::remove(dir + QStringLiteral("/good.wpx"))) {
            fault("S15.56", QStringLiteral("half.wpx could not be written, or good.wpx removed"));
            return;
        }
        const auto direct = scanLibrary(dir);
        const bool readsBroken = direct && titles(*direct).contains(QStringLiteral("half.wpx(broken)"));
        m.rescan();
        const QStringList during = publishedByScan(history);
        if (!readsBroken) {
            notVerified("S15.56 (a scan during a write)", "the half-written half.wpx does not read as broken, so showing it could not be seen");
        } else if (during.isEmpty()) {
            notVerified("S15.56 (a scan during a write)", "the scan did not finish, or its result was not handled when the posted events were delivered");
        } else {
            expect(during == QStringList{QStringLiteral("Copied,Held")},
                   QStringLiteral("S15.56: a scan started while half.wpx was being written, its creation unread, published [%1]")
                       .arg(during.join(QStringLiteral(" | "))));
        }
        const QByteArray rest = half->mid(half->size() / 2);
        if (file.write(rest) != rest.size() || !file.flush()) {
            fault("S15.56", QStringLiteral("the rest of half.wpx could not be written"));
            return;
        }
        file.close();
        const bool listed = readyWith(m, QStringLiteral("Copied,Half,Held"));
        static_cast<void>(history.take());
        expect(listed, QStringLiteral("S15.56: half.wpx was not listed once closed; [%1]").arg(titles(m.wallpapers())));
    }

    // GOAL: S15.56 -- a package moved into the folder whole is listed with no close to wait for.
    // CONTROL: app/library/library_manager.cpp, handle()'s `if ((event.mask & IN_MODIFY) != 0) {` made
    //       `(IN_MODIFY | IN_MOVED_TO)`, so a name moved in is held as being written -> red, "S15.56: a
    //       package moved in was not listed; [Copied,Half,Held]"; restored.
    if (!QFile::rename(staging + QStringLiteral("/moved.wpx"), dir + QStringLiteral("/moved.wpx"))) {
        fault("S15.56", QStringLiteral("moved.wpx could not be moved in"));
        return;
    }
    const bool moved = readyWith(m, QStringLiteral("Copied,Half,Held,Moved"));
    expect(moved, QStringLiteral("S15.56: a package moved in was not listed; [%1]").arg(titles(m.wallpapers())));

    // GOAL: S15.56 -- a hard link to a finished package, made in the folder, is listed with no
    //       close to wait for: a second name for bytes already there never gets one.
    // CONTROL: app/library/library_manager.cpp, handle() made to hold every created name as being
    //       written (`if ((event.mask & IN_CREATE) != 0) { folder.writing.insert(name, holdFromNow());
    //       return false; }` after `folder.writing.remove(name);`) -> red, "S15.56: a hard link put in the
    //       folder was not listed"; restored.
    if (::link(QFile::encodeName(staging + QStringLiteral("/linked.wpx")).constData(), QFile::encodeName(dir + QStringLiteral("/hard.wpx")).constData()) != 0) {
        fault("S15.56", QStringLiteral("the hard link could not be made: ") + QString::fromLocal8Bit(std::strerror(errno)));
        return;
    }
    const bool linked = readyWith(m, QStringLiteral("Copied,Half,Held,Linked,Moved"));
    expect(linked, QStringLiteral("S15.56: a hard link put in the folder was not listed; [%1]").arg(titles(m.wallpapers())));
}

// Holds the global pool's only thread, so a scan started meanwhile waits until release().
class PoolHold {
public:
    PoolHold() : m_pool(*QThreadPool::globalInstance()), m_saved(m_pool.maxThreadCount()) {
        m_pool.setMaxThreadCount(1);
        // Shared, so a blocker outliving this object (release never seen) touches nothing freed.
        m_pool.start([gates = m_gates] {
            gates->started.release();
            static_cast<void>(gates->release.tryAcquire(1, kWaitMs));
        });
        m_ok = m_gates->started.tryAcquire(1, kWaitMs);
    }
    ~PoolHold() {
        release();
        static_cast<void>(m_pool.waitForDone(kWaitMs));
        m_pool.setMaxThreadCount(m_saved);
    }
    PoolHold(const PoolHold&) = delete;
    PoolHold& operator=(const PoolHold&) = delete;

    bool ok() const { return m_ok; }
    void release() { m_gates->release.release(); }

private:
    struct Gates {
        QSemaphore started;
        QSemaphore release;
    };
    QThreadPool& m_pool;
    int m_saved;
    std::shared_ptr<Gates> m_gates = std::make_shared<Gates>();
    bool m_ok = false;
};

// S15.56: a rescan() racing a new file -- its events still unread when rescan() runs, or arriving
// while the scan runs. Each case has a folder of its own holding good.wpx and sentinel.wpx, and
// deletes the sentinel in the same window as its change, so a publish without it proves the
// scan's result was handled (publishedByScan).
std::unique_ptr<LibraryManager> raceManager(const QString& dir) {
    if (!QDir().mkpath(dir) || !writeZip(dir + QStringLiteral("/good.wpx"), planEntries(titled("Good"))).isEmpty() ||
        !writeZip(dir + QStringLiteral("/sentinel.wpx"), planEntries(titled("Sentinel"))).isEmpty()) {
        fault("S15.56", QStringLiteral("the folder %1 could not be built").arg(dir));
        return nullptr;
    }
    auto m = std::make_unique<LibraryManager>(dir, kSettleMs);
    m->rescan();
    if (!readyWith(*m, QStringLiteral("Good,Sentinel"))) {
        expect(false, QStringLiteral("S15.56: %1 never settled with [Good,Sentinel]; [%2]").arg(dir, titles(m->wallpapers())));
        return nullptr;
    }
    return m;
}

// Deletes the sentinel and calls rescan() with the pool held, creates `file` half-written, then
// lets the scan run. Empty when the scan provably read the folder only once the file existed, and
// read it half-written; otherwise why not.
QString heldScanReadsHalf(LibraryManager& m, QFile& file, const QByteArray& bytes) {
    const QString dir = m.directory();
    OpenCounter scans(dir);
    if (!scans.ok()) {
        return QStringLiteral("the open counter was already counting another folder");
    }
    PoolHold hold;
    if (!hold.ok()) {
        return QStringLiteral("the thread pool could not be held");
    }
    if (!QFile::remove(dir + QStringLiteral("/sentinel.wpx"))) {
        return QStringLiteral("sentinel.wpx could not be removed");
    }
    m.rescan();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes.left(bytes.size() / 2)) != bytes.size() / 2 || !file.flush()) {
        return QStringLiteral("%1 could not be written").arg(file.fileName());
    }
    const int beforeRelease = scans.take().folderOpens;
    hold.release();
    const bool done = QThreadPool::globalInstance()->waitForDone(kWaitMs);
    const int afterRelease = scans.take().folderOpens;
    const auto direct = scanLibrary(dir);
    const bool readsBroken = direct && titles(*direct).contains(QFileInfo(file).fileName() + QStringLiteral("(broken)"));
    if (done && beforeRelease == 0 && afterRelease == 1 && readsBroken) {
        return {};
    }
    return QStringLiteral("the scan was not held until %1 existed and then read it half-written: finished %2, folder opens %3 "
                          "before release and %4 after, the half reads broken %5")
        .arg(QFileInfo(file).fileName()).arg(done).arg(beforeRelease).arg(afterRelease).arg(readsBroken);
}

void closedBeforeRescan(const QString& root) {
    const auto whole = packageTitled(root + QStringLiteral("/whole.scratch"), "Whole");
    if (!whole) {
        fault("S15.56", QStringLiteral("whole.wpx could not be built"));
        return;
    }
    const auto m = raceManager(root + QStringLiteral("/race-closed"));
    if (!m) {
        return;
    }
    const QString dir = m->directory();
    History history(*m);

    // GOAL: S15.56 -- a package written and closed just before rescan(), its creation and close
    //       both still unread, is listed by that very scan, not held back for the next one.
    // WHY-THIS-SHAPE: no event loop runs between the close and rescan(), so both events are
    //       provably unread; the result is taken before any timer can run, so a later scan cannot
    //       list it in this one's place. Without rescan()'s own drain, the continuation reads the
    //       creation, cannot tell the file is already finished, and hides it from this scan.
    // CONTROL: app/library/library_manager.cpp, rescan()'s first `readWatchEvents();` (before
    //       `placeWatch();`) deleted -> red, "S15.56: a package closed just before rescan() was
    //       published as [Good]", this check alone. By hand,
    //       test-side: `sendPostedEvents()` removed from publishedByScan() -> NOT VERIFIED, "the
    //       scan's result was not handled"; restored.
    if (!writeBytes(dir + QStringLiteral("/whole.wpx"), *whole) || !QFile::remove(dir + QStringLiteral("/sentinel.wpx"))) {
        fault("S15.56", QStringLiteral("whole.wpx could not be written, or sentinel.wpx removed"));
        return;
    }
    m->rescan();
    const QStringList published = publishedByScan(history);
    if (published.isEmpty()) {
        notVerified("S15.56 (closed just before rescan)", "the scan did not finish, or its result was not handled when the posted events were delivered");
        return;
    }
    expect(published == QStringList{QStringLiteral("Good,Whole")},
           QStringLiteral("S15.56: a package closed just before rescan() was published as [%1]").arg(published.join(QStringLiteral(" | "))));
}

void createdDuringScan(const QString& root) {
    const auto bytes = packageTitled(root + QStringLiteral("/open.scratch"), "Open");
    if (!bytes) {
        fault("S15.56", QStringLiteral("open.wpx could not be built"));
        return;
    }
    const auto m = raceManager(root + QStringLiteral("/race-open"));
    if (!m) {
        return;
    }
    History history(*m);

    // GOAL: S15.56 -- a package created while a scan is held from starting, and still open when
    //       the scan reads it half-written, is not shown by that scan; it is listed once closed.
    // WHY-THIS-SHAPE: the pool's one thread is held, so the scan cannot run until release; the
    //       folder-open count proves it had not read the folder when the file was made (0) and did
    //       read it afterwards (1). The creation is queued after rescan() began, so only the drain
    //       in the scan's continuation can learn of it before the filter.
    // CONTROL: app/library/library_manager.cpp, the `readWatchEvents();` in rescan()'s continuation
    //       deleted -> red, "S15.56: a package created during a held scan and still open was
    //       published as [Good,open.wpx(broken)]"; handle()'s `m_unfinishedDuringScan.insert(name);`
    //       (its IN_MODIFY branch) deleted -> red, the same line; restored.
    QFile file(m->directory() + QStringLiteral("/open.wpx"));
    if (const QString why = heldScanReadsHalf(*m, file, *bytes); !why.isEmpty()) {
        notVerified("S15.56 (created during a scan)", why.toStdString());
        return;
    }
    const QStringList published = publishedByScan(history);
    if (published.isEmpty()) {
        notVerified("S15.56 (created during a scan)", "the scan's result was not handled when the posted events were delivered");
        return;
    }
    expect(published == QStringList{QStringLiteral("Good")},
           QStringLiteral("S15.56: a package created during a held scan and still open was published as [%1]").arg(published.join(QStringLiteral(" | "))));
    const QByteArray rest = bytes->mid(bytes->size() / 2);
    if (file.write(rest) != rest.size() || !file.flush()) {
        fault("S15.56", QStringLiteral("the rest of open.wpx could not be written"));
        return;
    }
    file.close();
    const bool listed = readyWith(*m, QStringLiteral("Good,Open"));
    expect(listed, QStringLiteral("S15.56: open.wpx was not listed once closed; [%1]").arg(titles(m->wallpapers())));
}

void closedDuringScan(const QString& root) {
    const auto bytes = packageTitled(root + QStringLiteral("/quick.scratch"), "Quick");
    if (!bytes) {
        fault("S15.56", QStringLiteral("quick.wpx could not be built"));
        return;
    }
    const auto m = raceManager(root + QStringLiteral("/race-quick"));
    if (!m) {
        return;
    }
    History history(*m);

    // GOAL: S15.56 -- a package created and closed while one scan runs, read half-written by that
    //       scan, is not shown by it; the scan its close schedules lists it.
    // WHY-THIS-SHAPE: held as in createdDuringScan(), so the scan provably reads the half; the file is finished and
    //       closed before the result is handled, so it is unfinished neither when the scan began
    //       nor when its result is filtered -- only a record of "unfinished at any point since the
    //       scan began" hides it. The creation and close are read by the continuation's drain; the
    //       order in which a socket notifier would read them first cannot be forced from a test.
    // CONTROL: app/library/library_manager.cpp, handle()'s `m_unfinishedDuringScan.insert(name);`
    //       (its IN_MODIFY branch) deleted -> red, "S15.56: a package created and closed during a scan
    //       was published as [Good,quick.wpx(broken)]"; the `readWatchEvents();` in rescan()'s
    //       continuation deleted -> red, the same line. By hand, test-side:
    //       heldScanReadsHalf() releasing the hold and draining the pool before creating the file ->
    //       NOT VERIFIED, "folder opens 1 before release and 0 after", here and above; restored.
    QFile file(m->directory() + QStringLiteral("/quick.wpx"));
    if (const QString why = heldScanReadsHalf(*m, file, *bytes); !why.isEmpty()) {
        notVerified("S15.56 (created and closed during a scan)", why.toStdString());
        return;
    }
    const QByteArray rest = bytes->mid(bytes->size() / 2);
    if (file.write(rest) != rest.size() || !file.flush()) {
        fault("S15.56", QStringLiteral("the rest of quick.wpx could not be written"));
        return;
    }
    file.close();
    const QStringList published = publishedByScan(history);
    if (published.isEmpty()) {
        notVerified("S15.56 (created and closed during a scan)", "the scan's result was not handled when the posted events were delivered");
        return;
    }
    expect(published == QStringList{QStringLiteral("Good")},
           QStringLiteral("S15.56: a package created and closed during a scan was published as [%1]").arg(published.join(QStringLiteral(" | "))));
    const bool listed = readyWith(*m, QStringLiteral("Good,Quick"));
    expect(listed, QStringLiteral("S15.56: quick.wpx was not listed by the scan its close scheduled; [%1]").arg(titles(m->wallpapers())));
}

// S15.58: an empty file is never listed.
void emptyFiles(const QString& root) {
    const QString dir = root + QStringLiteral("/downloads");
    const auto downloaded = packageTitled(root + QStringLiteral("/downloaded.scratch"), "Downloaded");
    const auto good = packageTitled(root + QStringLiteral("/good.scratch"), "Good");
    const auto later = packageTitled(root + QStringLiteral("/later.scratch"), "Later");
    if (!downloaded || !good || !later || !QDir().mkpath(dir) || !writeBytes(dir + QStringLiteral("/good.wpx"), *good)) {
        fault("S15.58", QStringLiteral("the folder or its packages could not be built"));
        return;
    }
    OpenCounter scans(dir);
    if (!scans.ok()) {
        harnessFailure("S15.58: the open counter was already counting another folder");
        return;
    }
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.58: the folder never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);

    // GOAL: S15.58 -- a download the way Firefox does it (the final name created empty and closed,
    //       the data written to a .part file, then renamed over the final name) publishes nothing
    //       and reads the folder not once until the rename, then publishes the package once.
    // WHY-THIS-SHAPE: two checks, because the scan's own empty-file rule already keeps an empty
    //       file off the list: only the scan count shows the handler skipped the empty name with no
    //       row rather than scanning to find it empty.
    // CONTROL: app/library/library_manager.cpp, handle()'s final `return !empty || ...;` made
    //       `return true;` -> red, "S15.58: 1 scan(s) ran before the download was renamed into
    //       place"; that and app/library/wpx_package.cpp scanLibrary's empty-file `continue` block
    //       deleted, one patch -> red, "S15.58: the download was published as [Good,x.wpx(broken) |
    //       Downloaded,Good]"; restored.
    // COVERAGE-ASSERT: the counter saw the first scan.
    {
        const int first = scans.take().folderOpens;
        QFile empty(dir + QStringLiteral("/x.wpx"));
        QFile part(dir + QStringLiteral("/x.wpx.part"));
        if (!empty.open(QIODevice::WriteOnly)) {
            fault("S15.58", QStringLiteral("x.wpx could not be created"));
            return;
        }
        empty.close();
        if (!part.open(QIODevice::WriteOnly) || !writeSlowly(part, *downloaded, 5, kWriteGapMs)) {
            fault("S15.58", QStringLiteral("x.wpx.part could not be written"));
            return;
        }
        part.close();
        pump(4 * kSettleMs);
        const int beforeRename = scans.take().folderOpens;
        const QStringList beforeRenamePublished = history.take();
        if (::rename(QFile::encodeName(part.fileName()).constData(), QFile::encodeName(empty.fileName()).constData()) != 0) {
            fault("S15.58", QStringLiteral("x.wpx.part could not be renamed over x.wpx"));
            return;
        }
        const bool listed = readyWith(m, QStringLiteral("Downloaded,Good"));
        pump(4 * kSettleMs);
        const QStringList published = beforeRenamePublished + history.take();
        expect(first >= 1, QStringLiteral("S15.58: the scan counter saw %1 scan(s) of the first read; it is not counting").arg(first));
        expect(beforeRename == 0, QStringLiteral("S15.58: %1 scan(s) ran before the download was renamed into place").arg(beforeRename));
        expect(listed && published == QStringList{QStringLiteral("Downloaded,Good")},
               QStringLiteral("S15.58: the download was published as [%1]").arg(published.join(QStringLiteral(" | "))));
    }

    // GOAL: S15.58 -- a listed package truncated and closed drops out of the list without ever
    //       being shown broken, and refilling it through a second open, with pauses longer than
    //       the quiet period, is held back until that open is closed: two publishes in all.
    // CONTROL: app/library/library_manager.cpp, handle()'s `folder.writing.remove(name);` deleted,
    //       so the close does not release the emptied name -> red, "S15.58: truncating and refilling
    //       good.wpx published []"; handle()'s IN_MODIFY branch made to return true without holding the
    //       name -> red, "... published [Downloaded | Downloaded,good.wpx(broken) | ... | Downloaded,Good]";
    //       restored.
    {
        QFile file(dir + QStringLiteral("/good.wpx"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fault("S15.58", QStringLiteral("good.wpx could not be truncated"));
            return;
        }
        file.close();
        const bool dropped = readyWith(m, QStringLiteral("Downloaded"));
        if (!file.open(QIODevice::WriteOnly) || !writeSlowly(file, *good, 4, kWriteGapMs)) {
            fault("S15.58", QStringLiteral("good.wpx could not be refilled"));
            return;
        }
        file.close();
        const bool back = readyWith(m, QStringLiteral("Downloaded,Good"));
        pump(4 * kSettleMs);
        const QStringList published = history.take();
        expect(dropped && back && published == QStringList{QStringLiteral("Downloaded"), QStringLiteral("Downloaded,Good")},
               QStringLiteral("S15.58: truncating and refilling good.wpx published [%1]").arg(published.join(QStringLiteral(" | "))));
    }

    // GOAL: S15.58 -- a new file created empty and closed, then filled through a second open with
    //       pauses longer than the quiet period, is published once, when that open is closed.
    // CONTROL: app/library/library_manager.cpp, handle()'s IN_MODIFY branch made to return true
    //       without holding the name -> red, "S15.58: an empty file filled later published
    //       [Downloaded,Good,later.wpx(broken) | ... | Downloaded,Good,Later]"; restored.
    {
        QFile file(dir + QStringLiteral("/later.wpx"));
        if (!file.open(QIODevice::WriteOnly)) {
            fault("S15.58", QStringLiteral("later.wpx could not be created"));
            return;
        }
        file.close();
        pump(4 * kSettleMs);
        if (!file.open(QIODevice::WriteOnly) || !writeSlowly(file, *later, 4, kWriteGapMs)) {
            fault("S15.58", QStringLiteral("later.wpx could not be filled"));
            return;
        }
        file.close();
        const bool listed = readyWith(m, QStringLiteral("Downloaded,Good,Later"));
        pump(4 * kSettleMs);
        const QStringList published = history.take();
        expect(listed && published == QStringList{QStringLiteral("Downloaded,Good,Later")},
               QStringLiteral("S15.58: an empty file filled later published [%1]").arg(published.join(QStringLiteral(" | "))));
    }
}

// S15.59: the folder polled instead of watched.
void polling(const QString& root) {
    const QString dir = root + QStringLiteral("/polled");
    const QString away = root + QStringLiteral("/polled-away");
    const auto slow = packageTitled(root + QStringLiteral("/slow.scratch"), "Slow");
    if (!slow || !QDir().mkpath(dir) || !writeZip(dir + QStringLiteral("/good.wpx"), planEntries(titled("Good"))).isEmpty()) {
        fault("S15.59", QStringLiteral("the polled folder could not be built"));
        return;
    }
    OpenCounter opens(dir);
    if (!opens.ok()) {
        harnessFailure("S15.59: the open counter was already counting another folder");
        return;
    }
    LibraryManager m(dir, kSettleMs, kPollMs, LibraryManager::Watching::Poll);

    // GOAL: S15.59 -- a library told to poll does, and reads nothing before its first rescan():
    //       no package is opened and it stays Loading over several poll periods.
    // CONTROL: app/library/library_manager.cpp, the constructor's `if (watching == Watching::Poll) {`
    //       made `if (false) {` -> red, "S15.59: a library told to poll reports polling() 0";
    //       poll()'s `if (m_state == State::Loading && m_scanning == Scanning::No) { return; }` deleted
    //       -> red, "S15.59: before its first rescan() the polled library opened 1 package(s) and is
    //       state 1"; restored.
    pump(5 * kPollMs);
    const OpenCounts beforeRescan = opens.take();
    expect(m.polling(), QStringLiteral("S15.59: a library told to poll reports polling() %1").arg(m.polling()));
    expect(beforeRescan.packageOpens == 0 && m.state() == LibraryManager::State::Loading,
           QStringLiteral("S15.59: before its first rescan() the polled library opened %1 package(s) and is state %2")
               .arg(beforeRescan.packageOpens).arg(int(m.state())));

    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.59: the polled folder never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);

    // GOAL: S15.59 -- an idle polled folder is looked at every period but no package in it is
    //       opened again.
    // WHY-THIS-SHAPE: package opens, not folder opens, are counted: each poll opens the folder
    //       by design, and only a scan opens a package. The window is six polls, waited for, not six
    //       periods of wall-clock time: how many timer ticks fit a fixed time is the scheduler's
    //       business. Polls that stop coming within the bound are a dead poll timer, a failure.
    // CONTROL: app/library/library_manager.cpp, poll()'s `if (quiet && polled.polled != polled.scanned) {`
    //       made `if (quiet) {` -> red, "S15.59: an idle polled folder had 1 package open(s) while the
    //       folder was opened 7 time(s)"; the constructor's `connect(&m_poll, ...)` deleted -> red,
    //       "S15.59: the idle polled folder was polled 0 time(s) in 3000 ms"; restored.
    // COVERAGE-ASSERT: the first scan's package read was counted, so a dead package counter cannot
    //       read as "none opened" here or before the first rescan(); the window held six polls.
    // CONTROL (of the coverage assert): by hand, test-side, open_counter.cpp's notePackage() made
    //       to count nothing -> red, "S15.59: the first scan of the polled folder opened 0 package(s)"; restored.
    const int firstScanOpens = opens.take().packageOpens;
    expect(firstScanOpens >= 1,
           QStringLiteral("S15.59: the first scan of the polled folder opened %1 package(s); the package counter is not counting").arg(firstScanOpens));
    OpenCounts idle;
    const bool polled = waitUntil([&] {
        const OpenCounts seen = opens.take();
        idle.folderOpens += seen.folderOpens;
        idle.packageOpens += seen.packageOpens;
        return idle.folderOpens >= 6;
    }, kWaitMs);
    if (!polled) {
        expect(false, QStringLiteral("S15.59: the idle polled folder was polled %1 time(s) in %2 ms").arg(idle.folderOpens).arg(kWaitMs));
    } else {
        expect(idle.packageOpens == 0, QStringLiteral("S15.59: an idle polled folder had %1 package open(s) while the folder was opened %2 time(s)")
                                           .arg(idle.packageOpens).arg(idle.folderOpens));
    }

    // GOAL: S15.59 -- a package added to a polled folder, and then removed, is noticed without
    //       rescan().
    // CONTROL: app/library/library_manager.cpp, the constructor's `connect(&m_poll, &QTimer::timeout, this, &LibraryManager::poll);`
    //       deleted -> red, "S15.59: a package added to the polled folder was not listed"; restored.
    if (!writeZip(dir + QStringLiteral("/added.wpx"), planEntries(titled("Added"))).isEmpty()) {
        fault("S15.59", QStringLiteral("added.wpx could not be written"));
        return;
    }
    const bool added = readyWith(m, QStringLiteral("Added,Good"));
    expect(added, QStringLiteral("S15.59: a package added to the polled folder was not listed; [%1]").arg(titles(m.wallpapers())));
    if (!QFile::remove(dir + QStringLiteral("/added.wpx"))) {
        fault("S15.59", QStringLiteral("added.wpx could not be removed"));
        return;
    }
    const bool removed = readyWith(m, QStringLiteral("Good"));
    expect(removed, QStringLiteral("S15.59: a package removed from the polled folder is still listed; [%1]").arg(titles(m.wallpapers())));

    // GOAL: S15.59 -- a package's mode changing, with nothing else about it, is noticed by polling:
    //       made unreadable it turns broken, made readable again it is usable.
    // CONTROL: app/library/library_manager.cpp, observe()'s `observed.mode = st.st_mode;` deleted
    //       -> red, "S15.59: good.wpx made unreadable still reads [Good]"; restored.
    if (::geteuid() == 0) {
        notVerified("S15.59 (a mode change)", "running as root, which can read a mode-000 file");
    } else {
        if (!setMode(dir + QStringLiteral("/good.wpx"), 0)) {
            fault("S15.59", QStringLiteral("good.wpx could not be made mode 000"));
            return;
        }
        const bool locked = readyWith(m, QStringLiteral("good.wpx(broken)"));
        const QString lockedList = titles(m.wallpapers());
        if (!setMode(dir + QStringLiteral("/good.wpx"), 0644)) {
            fault("S15.59", QStringLiteral("good.wpx could not be made readable again"));
            return;
        }
        const bool unlocked = readyWith(m, QStringLiteral("Good"));
        expect(locked && unlocked, QStringLiteral("S15.59: good.wpx made unreadable still reads [%1]; made readable again [%2]")
                                       .arg(lockedList, titles(m.wallpapers())));
    }

    // GOAL: S15.59 -- the polled folder renamed away reads as an empty library, and renamed back
    //       is read again.
    // CONTROL: the poll connect deleted, as above -> red, "S15.59: the polled folder renamed away
    //       still reads [Good]"; restored.
    if (!QDir().rename(dir, away)) {
        fault("S15.59", QStringLiteral("the polled folder could not be renamed away"));
        return;
    }
    const bool empty = readyWith(m, QString());
    const QString awayList = titles(m.wallpapers());
    if (!QDir().rename(away, dir)) {
        fault("S15.59", QStringLiteral("the polled folder could not be renamed back"));
        return;
    }
    const bool returned = readyWith(m, QStringLiteral("Good"));
    expect(empty && returned, QStringLiteral("S15.59: the polled folder renamed away still reads [%1]; renamed back [%2]")
                                  .arg(awayList, titles(m.wallpapers())));
    static_cast<void>(history.take());

    // GOAL: S15.59 -- a new package written in pieces less than a poll period apart is published
    //       once, usable, never as a broken half.
    // WHY-THIS-SHAPE: a change is read only once a poll finds nothing further changed; if this
    //       test stalled between two writes for more than half a period, a correct manager may read
    //       it half-written, so the run is abandoned as not verified rather than graded.
    // CONTROL: app/library/library_manager.cpp, poll()'s `const bool quiet = seen == polled.polled;`
    //       made `const bool quiet = true;` -> red, "S15.59: a slow write to the polled folder was
    //       published as [Good,slow.wpx(broken) | Good,Slow]"; restored.
    QFile file(dir + QStringLiteral("/slow.wpx"));
    if (!file.open(QIODevice::WriteOnly)) {
        fault("S15.59", QStringLiteral("slow.wpx could not be created"));
        return;
    }
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    qint64 widestGap = 0;
    const int pieces = 8;
    const qsizetype piece = slow->size() / pieces + 1;
    for (int i = 0; i < pieces; ++i) {
        const QByteArray chunk = slow->mid(i * piece, piece);
        if (file.write(chunk) != chunk.size() || !file.flush()) {
            fault("S15.59", QStringLiteral("a write to slow.wpx failed"));
            return;
        }
        widestGap = std::max(widestGap, clock.elapsed() - last);
        last = clock.elapsed();
        if (i + 1 < pieces) {
            pump(kPollMs / 4);
        }
    }
    file.close();
    const bool listed = readyWith(m, QStringLiteral("Good,Slow"));
    pump(3 * kPollMs);
    const QStringList published = history.take();
    if (widestGap >= kPollMs / 2) {
        notVerified("S15.59 (a slow write)", "this test stalled " + std::to_string(widestGap) + " ms between two writes, over half a poll period");
    } else {
        expect(listed && published == QStringList{QStringLiteral("Good,Slow")},
               QStringLiteral("S15.59: a slow write to the polled folder was published as [%1]").arg(published.join(QStringLiteral(" | "))));
    }
}

// Collects qWarning lines while alive.
QStringList* g_warnings = nullptr;
QtMessageHandler g_previousHandler = nullptr;
void collectWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (g_warnings && type == QtWarningMsg) {
        g_warnings->append(message);
    }
    if (g_previousHandler) {
        g_previousHandler(type, context, message);
    }
}

// S15.59: inotify that cannot start.
void inotifyUnavailable(const QString& root) {
    const QString dir = root + QStringLiteral("/no-inotify");
    if (!QDir().mkpath(dir) || !writeZip(dir + QStringLiteral("/good.wpx"), planEntries(titled("Good"))).isEmpty()) {
        fault("S15.59", QStringLiteral("the folder for the inotify failure could not be built"));
        return;
    }
    // Only this process's descriptors run out: its soft limit is lowered, filled, and restored.
    // The per-user inotify limit, shared with the desktop, is never touched.
    rlimit saved {};
    if (::getrlimit(RLIMIT_NOFILE, &saved) != 0) {
        notVerified("S15.59 (inotify cannot start)", "the descriptor limit could not be read");
        return;
    }
    rlimit lowered = saved;
    lowered.rlim_cur = std::min<rlim_t>(saved.rlim_cur, 512);
    if (::setrlimit(RLIMIT_NOFILE, &lowered) != 0) {
        notVerified("S15.59 (inotify cannot start)", "the descriptor limit could not be lowered");
        return;
    }
    QList<int> filler;
    for (int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC); fd >= 0; fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
        filler.append(fd);
    }
    const int exhausted = errno;
    QStringList warnings;
    g_warnings = &warnings;
    g_previousHandler = qInstallMessageHandler(collectWarnings);
    auto m = std::make_unique<LibraryManager>(dir, kSettleMs, kPollMs);
    qInstallMessageHandler(g_previousHandler);
    g_warnings = nullptr;
    for (const int fd : std::as_const(filler)) {
        ::close(fd);
    }
    if (::setrlimit(RLIMIT_NOFILE, &saved) != 0) {
        harnessFailure("S15.59: the descriptor limit could not be restored");
    }
    if (exhausted != EMFILE || filler.isEmpty()) {
        notVerified("S15.59 (inotify cannot start)", "this process's descriptors could not be used up");
        return;
    }
    // GOAL: S15.59 -- when inotify cannot be started the library says so once and polls: a
    //       package added afterwards is still listed without rescan().
    // WHY-THIS-SHAPE: a real failure, made by using up this process's descriptors while the
    //       manager is built, since Watching::Poll only models it.
    // CONTROL: app/library/library_manager.cpp, the constructor's `if (m_inotify < 0) {` (the one
    //       calling pollInstead("inotify_init1", ...)) made `if (false) {` -> red, "S15.59: with inotify
    //       unavailable the library reports polling() 0 after 0 warning(s)"; restored.
    const qsizetype said = std::ranges::count_if(warnings, [](const QString& w) { return w.contains(QLatin1String("inotify_init1 failed")); });
    expect(m->polling() && said == 1,
           QStringLiteral("S15.59: with inotify unavailable the library reports polling() %1 after %2 warning(s): [%3]")
               .arg(m->polling()).arg(said).arg(warnings.join(QStringLiteral(" | "))));
    m->rescan();
    if (!readyWith(*m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.59: the folder read without inotify never settled; [%1]").arg(titles(m->wallpapers())));
        return;
    }
    if (!writeZip(dir + QStringLiteral("/added.wpx"), planEntries(titled("Added"))).isEmpty()) {
        fault("S15.59", QStringLiteral("added.wpx could not be written"));
        return;
    }
    const bool added = readyWith(*m, QStringLiteral("Added,Good"));
    expect(added, QStringLiteral("S15.59: with inotify unavailable a package added was not listed; [%1]").arg(titles(m->wallpapers())));
}

// S15.52: the list model changes rows one at a time.
void modelRows() {
    // The rows a view holds, rebuilt only from the model's row signals.
    WallpaperListModel model;
    QList<Wallpaper> shadow;
    int inserts = 0, removals = 0, moves = 0, changes = 0, resets = 0;
    const auto rowAt = [&model](int row) { return model.data(model.index(row), Qt::UserRole).value<Wallpaper>(); };
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, [&](const QModelIndex&, int first, int last) {
        for (int row = first; row <= last; ++row) {
            shadow.insert(row, rowAt(row));
        }
        ++inserts;
    });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved, [&](const QModelIndex&, int first, int last) {
        shadow.remove(first, last - first + 1);
        ++removals;
    });
    QObject::connect(&model, &QAbstractItemModel::rowsMoved, [&](const QModelIndex&, int start, int end, const QModelIndex&, int row) {
        const QList<Wallpaper> taken = shadow.mid(start, end - start + 1);
        shadow.remove(start, end - start + 1);
        shadow.insert(row > end ? row - taken.size() : row, taken.size(), Wallpaper{});
        for (qsizetype i = 0; i < taken.size(); ++i) {
            shadow[(row > end ? row - taken.size() : row) + i] = taken[i];
        }
        ++moves;
    });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&](const QModelIndex& from, const QModelIndex& to) {
        for (int row = from.row(); row <= to.row(); ++row) {
            shadow[row] = rowAt(row);
        }
        ++changes;
    });
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&] {
        shadow = model.wallpapers();
        ++resets;
    });
    QStringList testerSaid;
    g_warnings = &testerSaid;
    g_previousHandler = qInstallMessageHandler(collectWarnings);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);

    // GOAL: S15.52 -- WallpaperListModel::assign() turns its rows into any new list by removals,
    //       moves, inserts and per-row changes, never a reset, and a view that follows only those
    //       signals ends holding exactly the new list; Qt's model tester finds nothing wrong.
    // WHY-THIS-SHAPE: 2000 random lists drawn from twelve paths, reordered, with stamps changed
    //       and paths repeated, compared after every assign() with a shadow copy built from the
    //       signals alone -- the tester checks the signals' consistency, not that a view built from
    //       them shows the right rows.
    // CONTROL: app/library/library_manager.cpp, assign()'s `emit dataChanged(index(int(row)), index(int(row)));`
    //       deleted -> red, "S15.52: after N assignment(s) the rows followed from the signals
    //       differ"; assign()'s final `if (m_rows.size() > wallpapers.size()) {` trim made `if (false) {`
    //       -> red, the same line; assign() made a reset -> red, "S15.52: the model was reset 2000
    //       time(s)"; `beginInsertRows({}, int(row), int(row));` announced one row later -> red, "Qt's
    //       model tester said: [Index QModelIndex(-1,-1,...) is not valid (expected valid) | ...]",
    //       so the tester is live; restored.
    // COVERAGE-ASSERT: every kind of row change happened at least once.
    QRandomGenerator random(15052);
    int mismatches = 0;
    int firstMismatch = -1;
    for (int round = 0; round < 2000; ++round) {
        QList<Wallpaper> next;
        const int size = int(random.bounded(9));
        for (int i = 0; i < size; ++i) {
            Wallpaper w;
            w.filePath = QStringLiteral("/lib/p%1.wpx").arg(random.bounded(12));
            w.fileName = w.filePath.mid(5);
            w.stamp.modifiedNs = random.bounded(3);
            next.append(w);
        }
        model.assign(next);
        if (shadow != next || model.wallpapers() != next || model.rowCount() != next.size()) {
            ++mismatches;
            firstMismatch = firstMismatch < 0 ? round : firstMismatch;
        }
    }
    qInstallMessageHandler(g_previousHandler);
    g_warnings = nullptr;
    expect(mismatches == 0, QStringLiteral("S15.52: after %1 assignment(s) the rows followed from the signals differ from the list assigned, the first at round %2")
                                .arg(mismatches).arg(firstMismatch));
    expect(resets == 0 && testerSaid.isEmpty(),
           QStringLiteral("S15.52: the model was reset %1 time(s) and Qt's model tester said: [%2]").arg(resets).arg(testerSaid.mid(0, 3).join(QStringLiteral(" | "))));
    expect(inserts > 0 && removals > 0 && moves > 0 && changes > 0,
           QStringLiteral("S15.52: the random lists made %1 insert(s), %2 removal(s), %3 move(s), %4 change(s); each should be above 0")
               .arg(inserts).arg(removals).arg(moves).arg(changes));
}

// The user's state on a listed package: it is selected and its speed edited to 2.5.
bool selectEdited(LibraryManager& m, const QString& path) {
    return m.select(path) && m.setPropertyValue(QStringLiteral("speed"), 2.5);
}

bool keptEdited(const LibraryManager& m, const QString& path) {
    return m.selectedPath() == path && m.propertyValues().value(QStringLiteral("speed")) == QVariant(2.5);
}

QString selectionOf(const LibraryManager& m) {
    return QStringLiteral("selected \"%1\" with speed %2")
        .arg(QFileInfo(m.selectedPath()).fileName(), m.propertyValues().value(QStringLiteral("speed")).toString());
}

// S15.60: a listed package rewritten in place, directly or through a link's target outside the folder.
void rewrittenInPlace(const QString& root) {
    for (const bool viaLink : {false, true}) {
        const char* goal = viaLink ? "S15.60 (a link's target)" : "S15.60";
        const QString base = root + (viaLink ? QStringLiteral("/rewrite-link") : QStringLiteral("/rewrite"));
        const QString dir = base + QStringLiteral("/lib");
        const QString shown = dir + QStringLiteral("/a.wpx");
        const QString written = viaLink ? base + QStringLiteral("/target.wpx") : shown;
        const auto v2 = packageTitled(base + QStringLiteral("-v2.scratch"), "A v2");
        if (!v2 || !QDir().mkpath(dir) || !writeZip(written, planEntries(titled("A v1"))).isEmpty() ||
            (viaLink && !QFile::link(written, shown)) || !writeZip(dir + QStringLiteral("/b.wpx"), planEntries(titled("B"))).isEmpty() ||
            !writeZip(base + QStringLiteral("/c.wpx"), planEntries(titled("C"))).isEmpty()) {
            fault(goal, QStringLiteral("the folder or its packages could not be built"));
            continue;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        if (!readyWith(m, QStringLiteral("A v1,B")) || !selectEdited(m, shown)) {
            expect(false, QStringLiteral("%1: the folder never settled with [A v1,B], or a.wpx could not be selected and edited; [%2]")
                              .arg(QLatin1String(goal), titles(m.wallpapers())));
            continue;
        }
        History history(m);

        // GOAL: S15.60 -- a listed package rewritten in place (opened with O_TRUNC, written in pieces
        //       further apart than the quiet period) keeps its row, the selection and the edits while
        //       it is written, even when another change makes a scan read the folder meanwhile, and is
        //       published once, as the new version, after the close -- the same through a link whose
        //       target outside the folder is rewritten.
        // WHY-THIS-SHAPE: writes alone schedule no scan, so a check without one mid-write could not
        //       tell a held row from a scan that never ran; c.wpx moved in half-way forces one, and the
        //       half is proven to read broken, so that scan provably saw it. Every publish is recorded.
        // CONTROL: app/library/library_manager.cpp, handle()'s `folder.writing.insert(name, holdFromNow());`
        //       deleted -> red, "S15.60: while a.wpx was rewritten a scan published [B,C,a.wpx(broken)],
        //       selected "" with speed "; holdBack()'s `scan.append(shown);` deleted -> red, "... published
        //       [B,C], selected "" ..."; handleLink()'s `link->writing = holdFromNow();` deleted -> red,
        //       "S15.60 (a link's target): while a.wpx was rewritten a scan published [B,C,a.wpx(broken)]";
        //       restored.
        QFile file(written);
        const qsizetype half = v2->size() / 2;
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || !writeSlowly(file, v2->left(half), 2, kWriteGapMs)) {
            fault(goal, QStringLiteral("the first half of the new a.wpx could not be written"));
            continue;
        }
        const bool halfBroken = !readWallpaper(written).usable();
        if (!QFile::rename(base + QStringLiteral("/c.wpx"), dir + QStringLiteral("/c.wpx"))) {
            fault(goal, QStringLiteral("c.wpx could not be moved in"));
            continue;
        }
        const bool triggered = waitUntil([&] { return titles(m.wallpapers()).split(u',').contains(QStringLiteral("C")); }, kWaitMs);
        const QString during = titles(m.wallpapers());
        const bool keptDuring = keptEdited(m, shown);
        const QString selectionDuring = selectionOf(m);
        if (!writeSlowly(file, v2->mid(half), 3, kWriteGapMs)) {
            fault(goal, QStringLiteral("the rest of the new a.wpx could not be written"));
            continue;
        }
        file.close();
        const bool listed = readyWith(m, QStringLiteral("A v2,B,C"));
        pump(4 * kSettleMs);
        const QStringList published = history.take();
        if (!halfBroken) {
            notVerified(goal, "half of the new a.wpx reads as usable, so a scan showing it could not be seen");
        } else if (!triggered) {
            expect(false, QStringLiteral("%1: c.wpx moved in during the rewrite was never listed; [%2]").arg(QLatin1String(goal), during));
        } else {
            expect(during == u"A v1,B,C" && keptDuring,
                   QStringLiteral("%1: while a.wpx was rewritten a scan published [%2], %3").arg(QLatin1String(goal), during, selectionDuring));
            expect(listed && published == QStringList{QStringLiteral("A v1,B,C"), QStringLiteral("A v2,B,C")} && keptEdited(m, shown),
                   QStringLiteral("%1: the rewrite published [%2] and ended %3")
                       .arg(QLatin1String(goal), published.join(QStringLiteral(" | ")), selectionOf(m)));
        }
    }
}

// S15.60, polled: polling sees no write, so only a package seen changing while a scan ran keeps its row.
void rewrittenWhilePolled(const QString& root) {
    const QString dir = root + QStringLiteral("/rewrite-polled");
    const QString a = dir + QStringLiteral("/a.wpx");
    const auto v2 = packageTitled(root + QStringLiteral("/rewrite-polled.scratch"), "A v2");
    if (!v2 || !QDir().mkpath(dir) || !writeZip(a, planEntries(titled("A v1"))).isEmpty() ||
        !writeZip(dir + QStringLiteral("/sentinel.wpx"), planEntries(titled("Sentinel"))).isEmpty()) {
        fault("S15.60 (polling)", QStringLiteral("the folder or its packages could not be built"));
        return;
    }
    OpenCounter opens(dir);
    if (!opens.ok()) {
        harnessFailure("S15.60 (polling): the open counter was already counting another folder");
        return;
    }
    LibraryManager m(dir, kSettleMs, kPollMs, LibraryManager::Watching::Poll);
    m.rescan();
    if (!readyWith(m, QStringLiteral("A v1,Sentinel")) || !selectEdited(m, a)) {
        expect(false, QStringLiteral("S15.60 (polling): the folder never settled, or a.wpx could not be selected and edited; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);

    // GOAL: S15.60 -- in a polled folder, a listed package rewritten while a scan runs, which that
    //       scan reads half-written, keeps its row, the selection and the edits; the new version is
    //       published by a later scan, with the selection and edits kept.
    // WHY-THIS-SHAPE: the pool's one thread is held so the scan provably reads a.wpx only after its
    //       half is written (a package open counted after release, none before), and the rest is
    //       written before the event loop runs, so the scan's result is handled with the file
    //       changed since the scan began. No event loop runs meanwhile, so no poll interleaves.
    //       sentinel.wpx is removed in the same window: a publish without it proves the result was
    //       handled, since holding a.wpx back leaves the list otherwise unchanged.
    // CONTROL: app/library/library_manager.cpp, rescan()'s continuation block comparing the polled
    //       folder after the scan (`if (const auto* polled = std::get_if<PollWatch>(&m_watch)) {...}`)
    //       deleted -> red, "S15.60 (polling): the scan that read a.wpx half-written published
    //       [a.wpx(broken)], selected "" with speed "; restored.
    QString unproven;
    {
        PoolHold hold;
        QFile file(a);
        const qsizetype half = v2->size() / 2;
        if (!hold.ok()) {
            unproven = QStringLiteral("the thread pool could not be held");
        } else if (!QFile::remove(dir + QStringLiteral("/sentinel.wpx"))) {
            fault("S15.60 (polling)", QStringLiteral("sentinel.wpx could not be removed"));
            return;
        } else {
            m.rescan();
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(v2->left(half)) != half || !file.flush()) {
                fault("S15.60 (polling)", QStringLiteral("the first half of the new a.wpx could not be written"));
                return;
            }
            const bool halfBroken = !readWallpaper(a).usable();
            static_cast<void>(opens.take());
            hold.release();
            const bool done = QThreadPool::globalInstance()->waitForDone(kWaitMs);
            const int read = opens.take().packageOpens;
            const QByteArray rest = v2->mid(half);
            if (file.write(rest) != rest.size() || !file.flush()) {
                fault("S15.60 (polling)", QStringLiteral("the rest of the new a.wpx could not be written"));
                return;
            }
            file.close();
            if (!done || read < 1 || !halfBroken) {
                unproven = QStringLiteral("the scan was not shown to read a.wpx half-written: finished %1, package opens after release %2, "
                                          "the half reads broken %3")
                               .arg(done).arg(read).arg(halfBroken);
            }
        }
    }
    if (!unproven.isEmpty()) {
        notVerified("S15.60 (polling)", unproven.toStdString());
        return;
    }
    QCoreApplication::sendPostedEvents();
    const QStringList published = history.take();
    const QString selection = selectionOf(m);
    if (published.isEmpty()) {
        notVerified("S15.60 (polling)", "the scan's result was not handled when the posted events were delivered");
        return;
    }
    expect(published == QStringList{QStringLiteral("A v1")} && keptEdited(m, a),
           QStringLiteral("S15.60 (polling): the scan that read a.wpx half-written published [%1], %2").arg(published.join(QStringLiteral(" | ")), selection));
    const bool listed = readyWith(m, QStringLiteral("A v2"));
    expect(listed && keptEdited(m, a),
           QStringLiteral("S15.60 (polling): after the rewrite the list is [%1], %2").arg(titles(m.wallpapers()), selectionOf(m)));
}

// S15.61: a package given its name by link() has no write or close under that name.
void linkedIn(const QString& root) {
    const QString dir = root + QStringLiteral("/linked-in");
    const QString staging = root + QStringLiteral("/linked-in-staging");
    const auto tmp = packageTitled(root + QStringLiteral("/tmp.scratch"), "Tmp");
    if (!tmp || !QDir().mkpath(dir) || !QDir().mkpath(staging) || !writeZip(dir + QStringLiteral("/good.wpx"), planEntries(titled("Good"))).isEmpty() ||
        !writeZip(staging + QStringLiteral("/unlinked.wpx"), planEntries(titled("Unlinked"))).isEmpty()) {
        fault("S15.61", QStringLiteral("the folder or its packages could not be built"));
        return;
    }
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.61: the folder never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    const auto listing = [&](const char* title) {
        return waitUntil([&] { return titles(m.wallpapers()).split(u',').contains(QLatin1String(title)); }, kWaitMs);
    };

    // GOAL: S15.61 -- a package written as an unnamed O_TMPFILE and given its name by linkat() is
    //       listed while its descriptor is still open, and one hard-linked in from a staging folder
    //       whose staging name is then unlinked is listed: neither ever reports a write or a close
    //       under its name.
    // WHY-THIS-SHAPE: the O_TMPFILE is checked before it is closed, so the link alone must list it;
    //       the staging name is unlinked before the event loop runs, so the new name has one link
    //       when its creation is read, as a file created by open() would.
    // CONTROL: app/library/library_manager.cpp, handle() given the rule "a name created with one
    //       link is still being written" back (after `folder.writing.remove(name);`, an IN_CREATE whose
    //       lstat shows st_nlink == 1 is held and returns false) -> red, "S15.61: a package linked in
    //       from an O_TMPFILE was not listed" and "S15.61: a package hard-linked in, its staging name
    //       unlinked, was not listed"; restored.
    const int fd = ::open(QFile::encodeName(dir).constData(), O_TMPFILE | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) {
        notVerified("S15.61 (O_TMPFILE)", std::string("this file system refuses O_TMPFILE: ") + std::strerror(errno));
    } else {
        const QByteArray proc = "/proc/self/fd/" + QByteArray::number(fd);
        const bool named = ::write(fd, tmp->constData(), size_t(tmp->size())) == tmp->size() &&
                           ::linkat(AT_FDCWD, proc.constData(), AT_FDCWD, QFile::encodeName(dir + QStringLiteral("/tmp.wpx")).constData(), AT_SYMLINK_FOLLOW) == 0;
        const bool listed = named && listing("Tmp");
        ::close(fd);
        if (!named) {
            fault("S15.61", QStringLiteral("the O_TMPFILE could not be written or linked in: ") + QString::fromLocal8Bit(std::strerror(errno)));
        } else {
            expect(listed, QStringLiteral("S15.61: a package linked in from an O_TMPFILE was not listed; [%1]").arg(titles(m.wallpapers())));
        }
    }
    if (::link(QFile::encodeName(staging + QStringLiteral("/unlinked.wpx")).constData(), QFile::encodeName(dir + QStringLiteral("/unlinked.wpx")).constData()) != 0 ||
        ::unlink(QFile::encodeName(staging + QStringLiteral("/unlinked.wpx")).constData()) != 0) {
        fault("S15.61", QStringLiteral("unlinked.wpx could not be linked in and unlinked: ") + QString::fromLocal8Bit(std::strerror(errno)));
        return;
    }
    const bool listed = listing("Unlinked");
    expect(listed, QStringLiteral("S15.61: a package hard-linked in, its staging name unlinked, was not listed; [%1]").arg(titles(m.wallpapers())));
}

// This process's user namespace, as /proc names it; empty when it cannot be read.
QByteArray userNamespace() {
    char target[64] = {};
    const ssize_t got = ::readlink("/proc/self/ns/user", target, sizeof target - 1);
    return got > 0 ? QByteArray(target, int(got)) : QByteArray();
}

QByteArray hostWatchLimit() {
    QFile file(QStringLiteral("/proc/sys/user/max_inotify_watches"));
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray();
}

// S15.62, the child: run by watchLimit() under `unshare -U -r`, so the watch limit it lowers is
// its own namespace's. Arguments: the limit, a folder to work in, the parent's namespace.
int watchLimitChild(const QStringList& args) {
    wallpaperx::testing::g_testName = "library_changes (watch limit)";
    std::printf("watch-limit child started\n");
    std::fflush(stdout);
    const QString goal = QStringLiteral("S15.62 (a limit of %1 watch(es))").arg(args.value(0));
    const QByteArray own = userNamespace();
    if (args.size() != 3 || own.isEmpty() || own == args[2].toUtf8()) {
        harnessFailure(goal.toStdString() + ": not in a user namespace of its own (" + own.toStdString() + "), so the limit was left alone");
        static_cast<void>(wallpaperx::testing::reportHarnessFaults("in this test file, not in the app"));
        return 1;
    }
    const QByteArray limit = args[0].toUtf8();
    const int limitFd = ::open("/proc/sys/user/max_inotify_watches", O_WRONLY | O_CLOEXEC);
    const bool set = limitFd >= 0 && ::write(limitFd, limit.constData(), size_t(limit.size())) == limit.size();
    if (limitFd >= 0) {
        ::close(limitFd);
    }
    if (!set || hostWatchLimit() != limit) {
        notVerified(goal.toUtf8().constData(), "this namespace's max_inotify_watches could not be set to " + limit.toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const QString dir = args[1] + QStringLiteral("/lib");
    const QString target = args[1] + QStringLiteral("/target.wpx");
    if (!QDir().mkpath(dir) || !writeZip(dir + QStringLiteral("/good.wpx"), planEntries(titled("Good"))).isEmpty() ||
        !writeZip(target, planEntries(titled("Linked"))).isEmpty() || !QFile::link(target, dir + QStringLiteral("/l.wpx"))) {
        fault("S15.62", QStringLiteral("the folder or its packages could not be built"));
        static_cast<void>(wallpaperx::testing::reportHarnessFaults("in this test file's fixtures, not in the app"));
        return 1;
    }
    QStringList warnings;
    g_warnings = &warnings;
    g_previousHandler = qInstallMessageHandler(collectWarnings);
    bool added = false;
    bool relinked = false;
    {
        LibraryManager m(dir, kSettleMs, kPollMs);
        m.rescan();
        if (!readyWith(m, QStringLiteral("Good,Linked"))) {
            expect(false, QStringLiteral("%1: the folder never settled with [Good,Linked]; [%2]").arg(goal, titles(m.wallpapers())));
        } else if (!writeZip(dir + QStringLiteral("/added.wpx"), planEntries(titled("Added"))).isEmpty()) {
            fault("S15.62", QStringLiteral("added.wpx could not be written"));
        } else {
            added = readyWith(m, QStringLiteral("Added,Good,Linked"));
            relinked = added && writeZip(target, planEntries(titled("Linked v2"))).isEmpty() && readyWith(m, QStringLiteral("Added,Good,Linked v2"));
        }
        qInstallMessageHandler(g_previousHandler);
        g_warnings = nullptr;
        const qsizetype said = std::ranges::count_if(
            warnings, [](const QString& w) { return w.contains(QLatin1String("inotify_add_watch failed (No space left on device)")); });
        expect(m.polling() && said == 1 && warnings.size() == 1,
               QStringLiteral("%1: the library reports polling() %2 after %3 warning(s) about it: [%4]")
                   .arg(goal).arg(m.polling()).arg(said).arg(warnings.join(QStringLiteral(" | "))));
        expect(added && relinked, QStringLiteral("%1: a package added was listed %2, and the link's target rewritten was read %3; [%4]")
                                      .arg(goal).arg(added).arg(relinked).arg(titles(m.wallpapers())));
    }
    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file's fixtures, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

// S15.62: the kernel refusing a watch for want of resources.
void watchLimit(const QString& root) {
    // GOAL: S15.62 -- when the kernel refuses a watch for want of resources (the per-user watch
    //       limit), the library logs exactly one warning, polls from then on, and still lists a
    //       package added later and re-reads a link's target rewritten later: with no watch to be
    //       had (limit 0), and with the folder's watch placed but the link's refused (limit 1).
    // WHY-THIS-SHAPE: a real ENOSPC, made by lowering the limit in a user namespace of the child's
    //       own (unshare -U -r), whose max_inotify_watches is that namespace's; the host's value is
    //       read before and after, and the child refuses to write unless its namespace differs from
    //       this process's. Its verdicts are relayed line by line.
    // CONTROL: app/library/library_manager.cpp, missingPath() made `return true;` -> red, "S15.62 (a
    //       limit of 0 watch(es)): the library reports polling() 0 after 0 warning(s)" and the same
    //       for a limit of 1; watchLinks()'s `pollInstead("inotify_add_watch", error);` deleted -> red,
    //       "S15.62 (a limit of 1 watch(es)): the library reports polling() 0 ..." only; restored. By
    //       hand, test-side: the program made "unshare-nonexistent" -> NOT VERIFIED, "unshare could not
    //       be started", and made `false` -> NOT VERIFIED, "could not run this test in a user namespace
    //       of its own"; the child run directly in this namespace -> "FAIL (harness): ... not in a user
    //       namespace of its own ..., so the limit was left alone"; restored.
    const QByteArray hostBefore = hostWatchLimit();
    const QByteArray ours = userNamespace();
    for (const int limit : {0, 1}) {
        const std::string goal = "S15.62 (a limit of " + std::to_string(limit) + " watch(es))";
        QProcess child;
        child.setProgram(QStringLiteral("unshare"));
        child.setArguments({QStringLiteral("-U"), QStringLiteral("-r"), QCoreApplication::applicationFilePath(), QStringLiteral("--watch-limit"),
                            QString::number(limit), root + QStringLiteral("/watch-limit-%1").arg(limit), QString::fromUtf8(ours)});
        child.start();
        if (!child.waitForStarted(10000)) {
            notVerified(goal.c_str(), "unshare could not be started: " + child.errorString().toStdString());
            continue;
        }
        if (!child.waitForFinished(60000)) {
            child.kill();
            static_cast<void>(child.waitForFinished(3000));
            harnessFailure(goal + ": the child did not finish within 60 s");
            continue;
        }
        const QString said = QString::fromLocal8Bit(child.readAllStandardOutput() + child.readAllStandardError());
        const QStringList lines = said.split(u'\n', Qt::SkipEmptyParts);
        if (child.exitStatus() != QProcess::NormalExit || !lines.contains(QStringLiteral("watch-limit child started"))) {
            notVerified(goal.c_str(), "unshare -U -r could not run this test in a user namespace of its own: " + said.trimmed().toStdString());
            continue;
        }
        bool verdict = child.exitCode() == 0;
        for (const QString& line : lines) {
            if (line.startsWith(QLatin1String("FAIL (harness): "))) {
                harnessFailure(line.mid(16).toStdString());
                verdict = true;
            } else if (line.startsWith(QLatin1String("FAIL: "))) {
                expect(false, line.mid(6));
                verdict = true;
            } else if (line.contains(QLatin1String("NOT VERIFIED"))) {
                notVerified(goal.c_str(), "the child said: " + line.toStdString());
                verdict = true;
            }
        }
        if (!verdict) {
            harnessFailure(goal + ": the child exited " + std::to_string(child.exitCode()) + " with no verdict: " + said.toStdString());
        }
    }
    if (hostWatchLimit() != hostBefore) {
        harnessFailure("S15.62: THE HOST'S max_inotify_watches changed from " + hostBefore.toStdString() + " to " + hostWatchLimit().toStdString());
    }
}

// S15.63: a scan is never discarded, so changes arriving while scans run cannot starve the list.
void steadyChanges(const QString& root) {
    constexpr int settle = 20;
    const QString dir = root + QStringLiteral("/steady");
    const QString staging = root + QStringLiteral("/steady-staging");
    const Entries small{{QStringLiteral("wallpaper.json"), titled("Steady")},
                        {QStringLiteral("preview.png"), pngBytes(64, 36)},
                        {QStringLiteral("assets/video.mp4"), QByteArrayLiteral("not really a video")}};
    const auto bytes = zipBytes(root + QStringLiteral("/steady.scratch"), small);
    if (!bytes || !QDir().mkpath(dir) || !QDir().mkpath(staging)) {
        fault("S15.63", QStringLiteral("the folder could not be made"));
        return;
    }
    // A folder big enough that one scan outlasts several quiet periods, doubled until it does.
    int count = 0;
    qint64 scanMs = 0;
    for (int round = 0; round < 5 && scanMs < 6 * settle; ++round) {
        for (const int until = count == 0 ? 400 : 2 * count; count < until; ++count) {
            if (!writeBytes(dir + QStringLiteral("/p%1.wpx").arg(count, 5, 10, QLatin1Char('0')), *bytes)) {
                fault("S15.63", QStringLiteral("package %1 could not be written").arg(count));
                return;
            }
        }
        QElapsedTimer timer;
        timer.start();
        const auto scan = scanLibrary(dir);
        scanMs = timer.elapsed();
        if (!scan || scan->size() != count) {
            fault("S15.63", QStringLiteral("the folder of %1 packages did not scan whole").arg(count));
            return;
        }
    }
    if (scanMs < 6 * settle) {
        notVerified("S15.63", "a scan of " + std::to_string(count) + " packages takes only " + std::to_string(scanMs) +
                                  " ms, too quick for changes to keep arriving while one runs");
        return;
    }
    LibraryManager m(dir, settle);
    m.rescan();
    if (!waitUntil([&] { return m.state() == LibraryManager::State::Ready && m.wallpapers().size() == count; }, 10 * kWaitMs)) {
        expect(false, QStringLiteral("S15.63: the folder of %1 packages never settled; %2 listed").arg(count).arg(m.wallpapers().size()));
        return;
    }
    int published = 0;
    const auto counting = QObject::connect(&m, &LibraryManager::wallpapersChanged, [&] { ++published; });

    // GOAL: S15.63 -- while packages keep arriving, each after a quiet period but before a scan of
    //       the folder could finish, lists are still published: a scan running when a change asks
    //       for another is finished and published, never thrown away for the newer one.
    // WHY-THIS-SHAPE: the gap between arrivals is longer than the quiet period, so each asks for a
    //       scan, and shorter than a scan of this folder (measured, and the folder doubled until one
    //       takes six quiet periods), so every scan has a request arrive while it runs. Arrivals go
    //       on until a list is published or the bound passes, so a slow machine only makes it take
    //       longer; reaching the bound is the failure.
    // CONTROL: app/library/library_manager.cpp, rescan()'s continuation given, first, `if (m_scanning
    //       == Scanning::YesThenAgain) { m_scanning = Scanning::No; rescan(); return; }` -- a scan asked
    //       for again is thrown away and started afresh -> red, "S15.63: no list was published while a
    //       package arrived every N ms for M ms"; restored.
    const qint64 gap = settle + (scanMs - settle) / 3;
    const qint64 bound = std::max<qint64>(40 * scanMs, kWaitMs);
    QElapsedTimer clock;
    clock.start();
    int arrived = 0;
    while (published == 0 && clock.elapsed() < bound) {
        const QString name = QStringLiteral("/n%1.wpx").arg(arrived, 5, 10, QLatin1Char('0'));
        if (!writeBytes(staging + name, *bytes) || !QFile::rename(staging + name, dir + name)) {
            fault("S15.63", QStringLiteral("a package could not be moved in"));
            QObject::disconnect(counting);
            return;
        }
        ++arrived;
        pump(int(gap));
    }
    const qint64 lasted = clock.elapsed();
    QObject::disconnect(counting);
    expect(published > 0, QStringLiteral("S15.63: no list was published while a package arrived every %1 ms for %2 ms (%3 arrivals, one scan "
                                         "of the folder taking %4 ms)")
                              .arg(gap).arg(lasted).arg(arrived).arg(scanMs));
    const bool whole = waitUntil([&] { return m.wallpapers().size() == count + arrived; }, 10 * kWaitMs);
    expect(whole, QStringLiteral("S15.63: after the arrivals stopped the list holds %1 of %2").arg(m.wallpapers().size()).arg(count + arrived));
}

// S15.64 (decision A): an emptied listed package is removed unless a writer holds it.
void emptiedPackages(const QString& root) {
    {
        const QString dir = root + QStringLiteral("/emptied");
        const QString a = dir + QStringLiteral("/a.wpx");
        if (!QDir().mkpath(dir) || !writeZip(a, planEntries(titled("A"))).isEmpty() ||
            !writeZip(dir + QStringLiteral("/b.wpx"), planEntries(titled("B"))).isEmpty()) {
            fault("S15.64", QStringLiteral("the emptied folder could not be built"));
            return;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        if (!readyWith(m, QStringLiteral("A,B")) || !selectEdited(m, a)) {
            expect(false, QStringLiteral("S15.64: the folder never settled, or a.wpx could not be selected and edited; [%1]").arg(titles(m.wallpapers())));
            return;
        }
        History history(m);
        // GOAL: S15.64 -- a listed, selected and edited package opened with O_TRUNC and closed with
        //       nothing written drops out of the list in one publish, taking the selection and the
        //       edits with it.
        // CONTROL: app/library/library_manager.cpp, handle()'s `folder.writing.remove(name);` deleted
        //       -> red, "S15.64: a.wpx emptied and closed published [] and left it selected "a.wpx"
        //       with speed 2.5"; restored.
        QFile file(a);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fault("S15.64", QStringLiteral("a.wpx could not be emptied"));
            return;
        }
        file.close();
        const bool dropped = readyWith(m, QStringLiteral("B"));
        pump(4 * kSettleMs);
        const QStringList published = history.take();
        expect(dropped && published == QStringList{QStringLiteral("B")} && m.selectedPath().isEmpty() && m.propertyValues().isEmpty(),
               QStringLiteral("S15.64: a.wpx emptied and closed published [%1] and left it %2").arg(published.join(QStringLiteral(" | ")), selectionOf(m)));
    }
    {
        const QString base = root + QStringLiteral("/emptied-held");
        const QString dir = base + QStringLiteral("/lib");
        const QString a = dir + QStringLiteral("/a.wpx");
        const auto v2 = packageTitled(base + QStringLiteral("-v2.scratch"), "A v2");
        if (!v2 || !QDir().mkpath(dir) || !writeZip(a, planEntries(titled("A v1"))).isEmpty() ||
            !writeZip(dir + QStringLiteral("/b.wpx"), planEntries(titled("B"))).isEmpty() ||
            !writeZip(base + QStringLiteral("/c.wpx"), planEntries(titled("C"))).isEmpty()) {
            fault("S15.64", QStringLiteral("the held folder could not be built"));
            return;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        if (!readyWith(m, QStringLiteral("A v1,B")) || !selectEdited(m, a)) {
            expect(false, QStringLiteral("S15.64: the held folder never settled, or a.wpx could not be selected and edited; [%1]").arg(titles(m.wallpapers())));
            return;
        }
        History history(m);
        // GOAL: S15.64 -- a listed, selected and edited package opened with O_TRUNC and held open
        //       empty for longer than the quiet period keeps its row, the selection and the edits,
        //       even across a scan another change forces; refilled and closed, it is published once
        //       as the new version, still selected and edited.
        // WHY-THIS-SHAPE: c.wpx moved in while a.wpx is empty forces a scan that finds it empty,
        //       which only the writer's hold can keep from dropping the row.
        // CONTROL: app/library/library_manager.cpp, handle()'s IN_MODIFY branch made to return true
        //       without holding the name -> red, "S15.64: while a.wpx was held open empty a scan
        //       published [B,C], selected "" with speed "; restored.
        QFile file(a);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fault("S15.64", QStringLiteral("a.wpx could not be opened empty"));
            return;
        }
        pump(10 * kSettleMs);
        if (!QFile::rename(base + QStringLiteral("/c.wpx"), dir + QStringLiteral("/c.wpx"))) {
            fault("S15.64", QStringLiteral("c.wpx could not be moved in"));
            return;
        }
        const bool triggered = waitUntil([&] { return titles(m.wallpapers()).split(u',').contains(QStringLiteral("C")); }, kWaitMs);
        const QString during = titles(m.wallpapers());
        const bool keptDuring = keptEdited(m, a);
        const QString selectionDuring = selectionOf(m);
        if (!writeSlowly(file, *v2, 4, kWriteGapMs)) {
            fault("S15.64", QStringLiteral("a.wpx could not be refilled"));
            return;
        }
        file.close();
        const bool listed = readyWith(m, QStringLiteral("A v2,B,C"));
        pump(4 * kSettleMs);
        const QStringList published = history.take();
        if (!triggered) {
            expect(false, QStringLiteral("S15.64: c.wpx moved in while a.wpx was held open empty was never listed; [%1]").arg(during));
        } else {
            expect(during == u"A v1,B,C" && keptDuring,
                   QStringLiteral("S15.64: while a.wpx was held open empty a scan published [%1], %2").arg(during, selectionDuring));
            expect(listed && published == QStringList{QStringLiteral("A v1,B,C"), QStringLiteral("A v2,B,C")} && keptEdited(m, a),
                   QStringLiteral("S15.64: refilling a.wpx published [%1] and ended %2").arg(published.join(QStringLiteral(" | ")), selectionOf(m)));
        }
    }
}

// S15.65 (decision D): a hold with no write for `holdMs` lapses, and the name is read again.
void holdExpiry(const QString& root) {
    constexpr int hold = 300;
    struct Case {
        const char* goal;
        bool viaLink;
        bool toNothing;
        const char* after;
    };
    // GOAL: S15.65 -- a listed, selected and edited package truncated by truncate(2), with no file
    //       open and so no close to come, keeps its row, selection and edits while the hold lasts,
    //       and is read again once the hold passes with no further write: truncated to nothing it
    //       drops out, to half it is listed broken -- the same through a link whose target outside
    //       the folder is truncated. Either way the selection and edits go.
    // WHY-THIS-SHAPE: a short hold (300 ms) is injected through the constructor. The early look is
    //       taken a third of a hold after the truncate and graded only if the clock shows it really
    //       came inside the hold; a stall past it is not verified, since a correct manager may then
    //       have read the file already.
    // CONTROL: app/library/library_manager.cpp, holdFromNow()'s `m_holdExpiry.start(m_hold);` deleted
    //       -> red, all three, "S15.65 (truncated to nothing): after the 300 ms hold the list is [A,B]";
    //       releaseHolds()'s `m_settle.start();` deleted -> red, the same three; releaseHolds()'s
    //       `for (LinkWatch& link : m_linkWatches) {...}` loop deleted -> red, "S15.65 (a link's target):
    //       after the 300 ms hold the list is [A,B]" only; handle()'s IN_MODIFY branch made to return
    //       true without holding -> red, "S15.65 (truncated to nothing): 100 ms after the truncate,
    //       within the 300 ms hold, the list was [B]" and the same for half; restored.
    const Case cases[] = {
        {"S15.65 (truncated to nothing)", false, true, "B"},
        {"S15.65 (truncated to half)", false, false, "B,a.wpx(broken)"},
        {"S15.65 (a link's target)", true, false, "B,a.wpx(broken)"},
    };
    int index = 0;
    for (const Case& c : cases) {
        const QString base = root + QStringLiteral("/expiry-%1").arg(index++);
        const QString dir = base + QStringLiteral("/lib");
        const QString shown = dir + QStringLiteral("/a.wpx");
        const QString written = c.viaLink ? base + QStringLiteral("/target.wpx") : shown;
        if (!QDir().mkpath(dir) || !writeZip(written, planEntries(titled("A"))).isEmpty() || (c.viaLink && !QFile::link(written, shown)) ||
            !writeZip(dir + QStringLiteral("/b.wpx"), planEntries(titled("B"))).isEmpty()) {
            fault(c.goal, QStringLiteral("the folder could not be built"));
            continue;
        }
        LibraryManager m(dir, kSettleMs, kPollMs, LibraryManager::Watching::Inotify, hold);
        m.rescan();
        if (!readyWith(m, QStringLiteral("A,B")) || !selectEdited(m, shown)) {
            expect(false, QStringLiteral("%1: the folder never settled, or a.wpx could not be selected and edited; [%2]")
                              .arg(QLatin1String(c.goal), titles(m.wallpapers())));
            continue;
        }
        const qint64 size = c.toNothing ? 0 : QFileInfo(written).size() / 2;
        QElapsedTimer clock;
        clock.start();
        if (::truncate(QFile::encodeName(written).constData(), size) != 0) {
            fault(c.goal, QStringLiteral("the package could not be truncated"));
            continue;
        }
        pump(hold / 3);
        const qint64 earlyAt = clock.elapsed();
        const QString early = titles(m.wallpapers());
        const bool keptEarly = keptEdited(m, shown);
        const QString selectionEarly = selectionOf(m);
        const bool read = waitUntil([&] { return titles(m.wallpapers()) == QLatin1String(c.after); }, kWaitMs);
        if (earlyAt >= hold) {
            notVerified(c.goal, "this test looked " + std::to_string(earlyAt) + " ms after the truncate, past the hold");
        } else {
            expect(early == u"A,B" && keptEarly, QStringLiteral("%1: %2 ms after the truncate, within the %3 ms hold, the list was [%4], %5")
                                                     .arg(QLatin1String(c.goal)).arg(earlyAt).arg(hold).arg(early, selectionEarly));
        }
        expect(read && m.selectedPath().isEmpty() && m.propertyValues().isEmpty(),
               QStringLiteral("%1: after the %2 ms hold the list is [%3], %4").arg(QLatin1String(c.goal)).arg(hold).arg(titles(m.wallpapers()), selectionOf(m)));
    }
}

// S15.66 (decision D): the hold is from the last write, so a writer that keeps writing is never cut short.
void longCopy(const QString& root) {
    constexpr int hold = 300;
    constexpr int gap = 100;
    constexpr int pieces = 8;
    const QString dir = root + QStringLiteral("/long-copy");
    const auto bytes = packageTitled(root + QStringLiteral("/long.scratch"), "Long");
    if (!bytes || !QDir().mkpath(dir) || !writeZip(dir + QStringLiteral("/good.wpx"), planEntries(titled("Good"))).isEmpty()) {
        fault("S15.66", QStringLiteral("the folder or its package could not be built"));
        return;
    }
    LibraryManager m(dir, kSettleMs, kPollMs, LibraryManager::Watching::Inotify, hold);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.66: the folder never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);
    // GOAL: S15.66 -- a new package copied in pieces a third of a hold apart, the copy lasting more
    //       than two holds, is published once, usable, after the close: each write restarts the
    //       hold, so a writer that keeps writing is never read half-way.
    // WHY-THIS-SHAPE: the hold is injected short (300 ms) so the copy (seven gaps of 100 ms)
    //       outlasts it twice. If this test itself stalled between two writes for two thirds of a
    //       hold, a correct manager may read the half, so the run is not verified rather than graded.
    // CONTROL: app/library/library_manager.cpp, handle()'s `folder.writing.insert(name, holdFromNow());`
    //       made `folder.writing.tryEmplace(name, holdFromNow());`, so only the first write sets the
    //       deadline -> red, "S15.66: a copy writing every 100 ms for 702 ms, with a 300 ms hold,
    //       published [Good,long.wpx(broken) | Good,Long]"; restored.
    QFile file(dir + QStringLiteral("/long.wpx"));
    if (!file.open(QIODevice::WriteOnly)) {
        fault("S15.66", QStringLiteral("long.wpx could not be created"));
        return;
    }
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    qint64 widestGap = 0;
    const qsizetype piece = bytes->size() / pieces + 1;
    for (int i = 0; i < pieces; ++i) {
        const QByteArray chunk = bytes->mid(i * piece, piece);
        if (file.write(chunk) != chunk.size() || !file.flush()) {
            fault("S15.66", QStringLiteral("a write to long.wpx failed"));
            return;
        }
        widestGap = std::max(widestGap, clock.elapsed() - last);
        last = clock.elapsed();
        if (i + 1 < pieces) {
            pump(gap);
        }
    }
    const qint64 lasted = clock.elapsed();
    file.close();
    const bool listed = readyWith(m, QStringLiteral("Good,Long"));
    pump(4 * kSettleMs);
    const QStringList published = history.take();
    if (widestGap >= 2 * hold / 3) {
        notVerified("S15.66", "this test stalled " + std::to_string(widestGap) + " ms between two writes, two thirds of the hold");
    } else {
        expect(listed && published == QStringList{QStringLiteral("Good,Long")},
               QStringLiteral("S15.66: a copy writing every %1 ms for %2 ms, with a %3 ms hold, published [%4]")
                   .arg(gap).arg(lasted).arg(hold).arg(published.join(QStringLiteral(" | "))));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    // Only watchLimit() runs this, under unshare, with the XDG folders it already isolated.
    if (argc > 1 && std::strcmp(argv[1], "--watch-limit") == 0) {
        QCoreApplication app(argc, argv);
        return watchLimitChild(QCoreApplication::arguments().mid(2));
    }
    wallpaperx::testing::g_testName = "library_changes";
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified("S15.52, S15.56, S15.58-S15.66", "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    QCoreApplication app(argc, argv);

    const QString root = scratch.path();
    runPhase("modelRows", modelRows);
    runPhase("finishedFiles", [&] { finishedFiles(root); });
    runPhase("closedBeforeRescan", [&] { closedBeforeRescan(root); });
    runPhase("createdDuringScan", [&] { createdDuringScan(root); });
    runPhase("closedDuringScan", [&] { closedDuringScan(root); });
    runPhase("emptyFiles", [&] { emptyFiles(root); });
    runPhase("polling", [&] { polling(root); });
    runPhase("inotifyUnavailable", [&] { inotifyUnavailable(root); });
    runPhase("rewrittenInPlace", [&] { rewrittenInPlace(root); });
    runPhase("rewrittenWhilePolled", [&] { rewrittenWhilePolled(root); });
    runPhase("linkedIn", [&] { linkedIn(root); });
    runPhase("watchLimit", [&] { watchLimit(root); });
    runPhase("steadyChanges", [&] { steadyChanges(root); });
    runPhase("emptiedPackages", [&] { emptiedPackages(root); });
    runPhase("holdExpiry", [&] { holdExpiry(root); });
    runPhase("longCopy", [&] { longCopy(root); });

    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file's fixtures, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
