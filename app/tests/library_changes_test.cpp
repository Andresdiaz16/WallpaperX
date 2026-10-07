// SPDX-License-Identifier: GPL-2.0-or-later

// app/library/library_manager.cpp and app/library/folder_watcher.cpp following a library of
// wallpaper folders as it changes: a folder copied in is read only once no file in it is open for
// writing, a wallpaper being rewritten keeps its row, holds lapse (and do not while a file is
// provably still open), the library is polled when inotify is not used or runs out of watches,
// scans are never thrown away, file systems where a lease says nothing behave as before, and the
// list model changes row by row, and an overflowing event queue or a switch to polling mid-scan
// loses nothing. Goals S15.52, S15.56, S15.59-S15.66, S15.70-S15.74, S15.86-S15.88 in GOALS.md.
//
// Scans are counted inside this process by open_counter.cpp. A copier is this process writing
// through one descriptor it keeps open across its pauses; a read lease is refused to a file open
// for writing by any process, this one included, so the watcher cannot tell it from another's.

#include "library_manager.h"
#include "open_counter.h"
#include "openat2_shim.h"
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
#include <QThreadPool>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/mount.h>
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

QString titles(const QList<Wallpaper>& list) {
    QStringList out;
    for (const Wallpaper& w : list) {
        out.append(w.usable() ? w.title : w.fileName + QStringLiteral("(broken)"));
    }
    return out.join(u',');
}

// PLAN's files with this title and an asset of `assetBytes` bytes.
Entries wallpaperTitled(const char* title, qsizetype assetBytes = 18) {
    return {{QStringLiteral("wallpaper.json"), titled(title)},
            {QStringLiteral("preview.png"), pngBytes(64, 36)},
            {QStringLiteral("assets/video.mp4"), QByteArray(assetBytes, 'v')}};
}

bool readyWith(LibraryManager& m, const QString& list) {
    return waitUntil([&] { return m.state() == LibraryManager::State::Ready && titles(m.wallpapers()) == list; }, kWaitMs);
}

const Wallpaper* rowTitled(const QList<Wallpaper>& list, const QString& title) {
    const auto it = std::ranges::find(list, title, &Wallpaper::title);
    return it == list.end() ? nullptr : &*it;
}

// Every list the manager published, in order.
class History {
public:
    explicit History(LibraryManager& m)
        : m_connection(QObject::connect(&m, &LibraryManager::wallpapersChanged, [this, &m] { m_lists.append(m.wallpapers()); })) {}
    ~History() { QObject::disconnect(m_connection); }
    History(const History&) = delete;
    History& operator=(const History&) = delete;

    QList<QList<Wallpaper>> takeLists() { return std::exchange(m_lists, {}); }
    QStringList take() {
        QStringList out;
        for (const QList<Wallpaper>& list : takeLists()) {
            out.append(titles(list));
        }
        return out;
    }

private:
    QList<QList<Wallpaper>> m_lists;
    QMetaObject::Connection m_connection;
};

// Each publish as its titles, a usable row titled `title` also showing its asset's size.
QString withAssetSizes(const QList<QList<Wallpaper>>& lists, const QString& title) {
    QStringList out;
    for (const QList<Wallpaper>& list : lists) {
        QStringList rows;
        for (const Wallpaper& w : list) {
            rows.append(!w.usable() ? w.fileName + QStringLiteral("(broken)")
                                    : w.title == title ? QStringLiteral("%1@%2").arg(w.title).arg(w.assetStamp.size) : w.title);
        }
        out.append(rows.join(u','));
    }
    return out.join(QStringLiteral(" | "));
}

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

// A copy of a wallpaper folder in progress, the way cp -r makes one: the folder, its small files
// written and closed, then the asset through one descriptor held open until finish(). Nothing
// runs the event loop between the folder's creation and the asset's first bytes, so the watcher
// cannot have seen that first write: it stands for a copy that began before the watch.
class Copy {
public:
    Copy(const QString& folder, const Entries& entries, qsizetype firstBytes) : m_asset(folder + QStringLiteral("/assets/video.mp4")) {
        Entries small = entries;
        m_bytes = small.takeLast().second;
        m_ok = writeFolder(folder, small).isEmpty() && QDir().mkpath(folder + QStringLiteral("/assets")) &&
               m_asset.open(QIODevice::WriteOnly) && write(m_bytes.left(firstBytes));
        m_written = firstBytes;
    }
    bool ok() const { return m_ok; }
    // The next `count` bytes.
    bool write(qsizetype count) {
        const bool done = write(m_bytes.mid(m_written, count));
        m_written += count;
        return done;
    }
    bool rest() { return write(m_bytes.size() - m_written); }
    void finish() { m_asset.close(); }
    qsizetype size() const { return m_bytes.size(); }

private:
    bool write(const QByteArray& chunk) { return m_asset.write(chunk) == chunk.size() && m_asset.flush(); }

    QFile m_asset;
    QByteArray m_bytes;
    qsizetype m_written = 0;
    bool m_ok = false;
};

// S15.56: a folder copied in is read only once no file in it is open for writing.
void copiedFolders(const QString& root) {
    const QString dir = root + QStringLiteral("/copied");
    const QString staging = root + QStringLiteral("/copied-staging");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty()) {
        fault("S15.56", QStringLiteral("the library could not be built"));
        return;
    }
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.56: the library never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);

    // GOAL: S15.56 -- a folder copied in by hand, whose copy began before the watcher could place
    //       a watch on it and whose asset is then written in pieces with pauses longer than the
    //       quiet period, is published once, after the asset is closed, with the asset at its full
    //       size; never with a partial asset.
    // WHY-THIS-SHAPE: the folder, its small files and the asset's first bytes are written with no
    //       event loop running (Copy), which stands for a GUI thread busy as the copy starts: the
    //       watch lands after that first write, so no event can say the asset is open, and only
    //       asking the kernel (a refused read lease) can. Each publish is recorded with the asset's
    //       size, since a partial asset reads as usable.
    // CONTROL: app/library/folder_watcher.cpp, watchTree()'s `holdOpenFiles(tree, at);` deleted ->
    //       red, "S15.56: a folder copied in with pauses was published as [Copied@1000,Good |
    //       Copied@5000,Good]"; restored.
    {
        Copy copy(dir + QStringLiteral("/copied"), wallpaperTitled("Copied", 5000), 1000);
        if (!copy.ok()) {
            fault("S15.56", QStringLiteral("the copy could not be started"));
            return;
        }
        bool written = true;
        for (int i = 0; i < 4 && written; ++i) {
            pump(4 * kWriteGapMs);
            written = copy.write(1000);
        }
        copy.finish();
        const bool listed = readyWith(m, QStringLiteral("Copied,Good"));
        pump(4 * kSettleMs);
        const QList<QList<Wallpaper>> published = history.takeLists();
        if (!written) {
            fault("S15.56", QStringLiteral("the copy could not be written"));
        } else {
            expect(listed && withAssetSizes(published, QStringLiteral("Copied")) == u"Copied@5000,Good",
                   QStringLiteral("S15.56: a folder copied in with pauses was published as [%1]").arg(withAssetSizes(published, QStringLiteral("Copied"))));
        }
    }

    // GOAL: S15.56 -- a folder whose asset is written whole but still held open is not listed; it
    //       is listed as soon as its writer closes it.
    // CONTROL: app/library/folder_watcher.cpp, watchTree()'s `holdOpenFiles(tree, at);` deleted ->
    //       red, "S15.56: a folder whose asset was still open was listed: [Copied,Good,Held]";
    //       IN_CLOSE_WRITE deleted from kInsideEvents -> red, "S15.56: a folder whose asset was held
    //       open was not listed when it closed"; restored.
    {
        Copy copy(dir + QStringLiteral("/held"), wallpaperTitled("Held"), 18);
        if (!copy.ok()) {
            fault("S15.56", QStringLiteral("held could not be written"));
            return;
        }
        pump(10 * kSettleMs);
        const QString whileOpen = titles(m.wallpapers());
        copy.finish();
        const bool listed = readyWith(m, QStringLiteral("Copied,Good,Held"));
        static_cast<void>(history.take());
        expect(whileOpen == u"Copied,Good", QStringLiteral("S15.56: a folder whose asset was still open was listed: [%1]").arg(whileOpen));
        expect(listed, QStringLiteral("S15.56: a folder whose asset was held open was not listed when it closed; [%1]").arg(titles(m.wallpapers())));
    }

    // GOAL: S15.56 -- a folder moved into the library whole, as an import renames one in, is
    //       listed with no close to wait for.
    // CONTROL: app/library/folder_watcher.cpp, handle()'s `if ((event.mask & IN_MODIFY) != 0) {`
    //       made `(IN_MODIFY | IN_MOVED_TO)`, so a folder moved in is held as being written -> red,
    //       "S15.56: a folder moved in was not listed; [Copied,Good,Held]"; restored.
    if (const QString why = moveInFolder(staging, dir, QStringLiteral("moved"), wallpaperTitled("Moved")); !why.isEmpty()) {
        fault("S15.56", why);
        return;
    }
    const bool moved = readyWith(m, QStringLiteral("Copied,Good,Held,Moved"));
    expect(moved, QStringLiteral("S15.56: a folder moved in was not listed; [%1]").arg(titles(m.wallpapers())));
}

// S15.70: a library that starts while a copy into it is under way.
void startedMidCopy(const QString& root) {
    const QString dir = root + QStringLiteral("/midcopy");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty()) {
        fault("S15.70", QStringLiteral("the library could not be built"));
        return;
    }
    Copy copy(dir + QStringLiteral("/copying"), wallpaperTitled("Copying", 4000), 2000);
    if (!copy.ok()) {
        fault("S15.70", QStringLiteral("the copy could not be started"));
        return;
    }
    // GOAL: S15.70 -- a library first read while a folder in it has a file open for writing (the
    //       app started mid-copy) does not list that folder until the file is closed, and then
    //       lists it with the file whole.
    // WHY-THIS-SHAPE: the first read has no earlier watch at all, so only the check made as each
    //       folder is first watched can see the open file. The copy pauses for longer than the
    //       quiet period with a scan forced meanwhile, so a missed hold is published.
    // CONTROL: app/library/folder_watcher.cpp, watchTree()'s `const bool fresh = !tree.inside.contains(wd);`
    //       made `const bool fresh = false;` -> red, "S15.70: a library started mid-copy published
    //       [Copying@2000,Good | Copying@4000,Good]"; restored.
    LibraryManager m(dir, kSettleMs);
    History history(m);
    m.rescan();
    const bool first = waitUntil([&] { return m.state() == LibraryManager::State::Ready; }, kWaitMs);
    pump(4 * kSettleMs);
    m.rescan();
    pump(4 * kSettleMs);
    const bool written = copy.rest();
    copy.finish();
    const bool listed = readyWith(m, QStringLiteral("Copying,Good"));
    pump(4 * kSettleMs);
    const QList<QList<Wallpaper>> published = history.takeLists();
    if (!written) {
        fault("S15.70", QStringLiteral("the rest of the copy could not be written"));
        return;
    }
    expect(first && listed && withAssetSizes(published, QStringLiteral("Copying")) == u"Good | Copying@4000,Good",
           QStringLiteral("S15.70: a library started mid-copy published [%1]").arg(withAssetSizes(published, QStringLiteral("Copying"))));
}

// S15.71: a hold renewed while its file is provably open, and lapsing when nothing has it open.
void pausedPastTheHold(const QString& root) {
    constexpr int hold = 300;
    const QString dir = root + QStringLiteral("/pause");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty()) {
        fault("S15.71", QStringLiteral("the library could not be built"));
        return;
    }
    LibraryManager m(dir, kSettleMs, kPollMs, LibraryManager::Watching::Inotify, hold);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.71: the library never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);
    // GOAL: S15.71 -- a copier that pauses for longer than the hold, with its file still open, is
    //       not read: the folder is published once, after the close, with the asset whole.
    // WHY-THIS-SHAPE: the hold is injected short (300 ms) and each pause is two and a half holds,
    //       so the hold -- from the first watch's check, then from each write -- provably lapses by
    //       the clock during every pause, and only the check that the file is still open renews it.
    // CONTROL: app/library/folder_watcher.cpp, releaseHolds()'s `if (!openForWriting(AT_FDCWD, ...)) {`
    //       made `if (true) {` -> red, "S15.71: a copy pausing 750 ms with a 300 ms hold published
    //       [Good,Paused@1000 | Good,Paused@2000 | Good,Paused@3000]", the only line red; restored.
    QElapsedTimer clock;
    clock.start();
    Copy copy(dir + QStringLiteral("/paused"), wallpaperTitled("Paused", 3000), 1000);
    if (!copy.ok()) {
        fault("S15.71", QStringLiteral("the copy could not be started"));
        return;
    }
    bool written = true;
    for (int i = 0; i < 2 && written; ++i) {
        pump(5 * hold / 2);
        written = copy.write(1000);
    }
    copy.finish();
    const bool listed = readyWith(m, QStringLiteral("Good,Paused"));
    pump(4 * kSettleMs);
    const QList<QList<Wallpaper>> published = history.takeLists();
    if (!written) {
        fault("S15.71", QStringLiteral("the copy could not be written"));
    } else {
        expect(listed && withAssetSizes(published, QStringLiteral("Paused")) == u"Good,Paused@3000",
               QStringLiteral("S15.71: a copy pausing %1 ms with a %2 ms hold published [%3]")
                   .arg(5 * hold / 2).arg(hold).arg(withAssetSizes(published, QStringLiteral("Paused"))));
    }
}

// S15.56: rescan() racing a new folder -- its events still unread when rescan() runs, or arriving
// while the scan runs. Each case has a library of its own holding good and sentinel, and deletes
// the sentinel in the same window as its change, so a publish without it proves the scan's result
// was handled (publishedByScan).
std::unique_ptr<LibraryManager> raceManager(const QString& dir) {
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty() ||
        !writeFolder(dir + QStringLiteral("/sentinel"), wallpaperTitled("Sentinel")).isEmpty()) {
        fault("S15.56", QStringLiteral("the library %1 could not be built").arg(dir));
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

void closedBeforeRescan(const QString& root) {
    const auto m = raceManager(root + QStringLiteral("/race-closed"));
    if (!m) {
        return;
    }
    const QString dir = m->directory();
    History history(*m);

    // GOAL: S15.56 -- a folder copied in whole and closed just before rescan(), its events still
    //       unread, is listed by that very scan, not held back for the next one.
    // WHY-THIS-SHAPE: no event loop runs between the copy and rescan(), so its events are provably
    //       unread when the scan starts; the result is taken before any timer can run, so a later
    //       scan cannot list it in this one's place. A folder created during a scan is held back
    //       from it, so what keeps this one listed is that its creation is read before the scan
    //       and forgotten when the scan starts afresh.
    // CONTROL: app/library/folder_watcher.cpp, scanStarting()'s `readWatchEvents();` deleted, so the
    //       creation is first read in scanFinished()'s drain -> red, "S15.56: a folder closed just
    //       before rescan() was published as [Good]", the only line red; restored.
    if (!writeFolder(dir + QStringLiteral("/whole"), wallpaperTitled("Whole")).isEmpty() ||
        !QDir(dir + QStringLiteral("/sentinel")).removeRecursively()) {
        fault("S15.56", QStringLiteral("whole could not be written, or sentinel removed"));
        return;
    }
    m->rescan();
    const QStringList published = publishedByScan(history);
    if (published.isEmpty()) {
        notVerified("S15.56 (closed just before rescan)", "the scan did not finish, or its result was not handled when the posted events were delivered");
        return;
    }
    expect(published == QStringList{QStringLiteral("Good,Whole")},
           QStringLiteral("S15.56: a folder closed just before rescan() was published as [%1]").arg(published.join(QStringLiteral(" | "))));
}

// Deletes the sentinel and calls rescan() with the pool held, starts `copy`, then lets the scan
// run. Empty when the scan provably read the library only once the copy had begun; otherwise why not.
QString heldScanReadsCopy(LibraryManager& m, const std::function<bool()>& startCopy) {
    const QString dir = m.directory();
    OpenCounter scans(dir);
    if (!scans.ok()) {
        return QStringLiteral("the open counter was already counting another folder");
    }
    PoolHold hold(kWaitMs);
    if (!hold.ok()) {
        return QStringLiteral("the thread pool could not be held");
    }
    if (!QDir(dir + QStringLiteral("/sentinel")).removeRecursively()) {
        return QStringLiteral("sentinel could not be removed");
    }
    m.rescan();
    if (!startCopy()) {
        return QStringLiteral("the copy could not be started");
    }
    const int beforeRelease = scans.take().scans;
    hold.release();
    const bool done = QThreadPool::globalInstance()->waitForDone(kWaitMs);
    const int afterRelease = scans.take().scans;
    if (done && beforeRelease == 0 && afterRelease == 1) {
        return {};
    }
    return QStringLiteral("the scan was not held until the copy began: finished %1, scans %2 before release and %3 after")
        .arg(done).arg(beforeRelease).arg(afterRelease);
}

void createdDuringScan(const QString& root) {
    const auto m = raceManager(root + QStringLiteral("/race-open"));
    if (!m) {
        return;
    }
    History history(*m);

    // GOAL: S15.56 -- a folder copied in while a scan is held from starting, its asset still open
    //       when the scan reads it, is not shown by that scan; it is listed once closed.
    // WHY-THIS-SHAPE: the pool's one thread is held, so the scan cannot run until release; the
    //       scan count proves it had not read the library when the copy began (0) and did read it
    //       afterwards (1). The folder's creation is queued after rescan() began, so only the drain
    //       in the scan's continuation can learn of it before the result is filtered. Two things
    //       then hold it back, each enough alone: its creation during the scan, and the writer
    //       found when that drain watches the folder -- so breaking either one leaves this green.
    // CONTROL: app/library/folder_watcher.cpp, scanFinished()'s first `readWatchEvents();` deleted
    //       -> red, "S15.56: a folder copied in during a held scan, its asset still open, was
    //       published as [Good,Open]". handle()'s `m_unfinishedDuringScan.insert(itemOf(path));`
    //       under IN_CREATE deleted together with watchTree()'s `holdOpenFiles(tree, at);`, one
    //       patch -> red, the same line; the same insert deleted together with handle()'s
    //       `watchTree(*tree, path);` (in the `else` after `unwatchTree(*tree, path);`) -> red, the
    //       same line. The insert, holdOpenFiles and that watchTree call each deleted alone left this
    //       line green; restored.
    std::optional<Copy> copy;
    if (const QString why = heldScanReadsCopy(*m, [&] {
            copy.emplace(m->directory() + QStringLiteral("/open"), wallpaperTitled("Open", 2000), 1000);
            return copy->ok();
        });
        !why.isEmpty()) {
        notVerified("S15.56 (created during a scan)", why.toStdString());
        return;
    }
    const QStringList published = publishedByScan(history);
    if (published.isEmpty()) {
        notVerified("S15.56 (created during a scan)", "the scan's result was not handled when the posted events were delivered");
        return;
    }
    expect(published == QStringList{QStringLiteral("Good")},
           QStringLiteral("S15.56: a folder copied in during a held scan, its asset still open, was published as [%1]").arg(published.join(QStringLiteral(" | "))));
    const bool written = copy->rest();
    copy->finish();
    const bool listed = readyWith(*m, QStringLiteral("Good,Open"));
    expect(written && listed, QStringLiteral("S15.56: open was not listed once closed; [%1]").arg(titles(m->wallpapers())));
}

void closedDuringScan(const QString& root) {
    const auto m = raceManager(root + QStringLiteral("/race-quick"));
    if (!m) {
        return;
    }
    History history(*m);

    // GOAL: S15.56 -- a folder copied in and closed while one scan runs, its asset read half-way
    //       by that scan, is not shown by it; the scan the copy's events schedule lists it whole.
    // WHY-THIS-SHAPE: held as in createdDuringScan(), so the scan provably reads the half; the copy
    //       is finished and closed before the result is handled, so the file is open neither when
    //       the scan began nor when its result is filtered -- only a record of "unfinished at any
    //       point since the scan began" could hide it. Each publish is recorded with the asset's
    //       size, since a half-read asset reads as usable.
    // CONTROL: app/library/folder_watcher.cpp, handle()'s `m_unfinishedDuringScan.insert(itemOf(path));`
    //       under `if ((event.mask & IN_CREATE) != 0) {` deleted -> red, "S15.56: a folder copied in
    //       and closed during a scan was published as [Good,Quick@1000]", with S15.88 (mid-scan), whose
    //       folder is also created during a scan, the only other line red; restored.
    std::optional<Copy> copy;
    if (const QString why = heldScanReadsCopy(*m, [&] {
            copy.emplace(m->directory() + QStringLiteral("/quick"), wallpaperTitled("Quick", 2000), 1000);
            return copy->ok();
        });
        !why.isEmpty()) {
        notVerified("S15.56 (created and closed during a scan)", why.toStdString());
        return;
    }
    const bool written = copy->rest();
    copy->finish();
    if (!written) {
        fault("S15.56", QStringLiteral("the rest of quick could not be written"));
        return;
    }
    const QList<QList<Wallpaper>> handled = [&] {
        static_cast<void>(QThreadPool::globalInstance()->waitForDone(kWaitMs));
        QCoreApplication::sendPostedEvents();
        return history.takeLists();
    }();
    if (handled.isEmpty()) {
        notVerified("S15.56 (created and closed during a scan)", "the scan's result was not handled when the posted events were delivered");
        return;
    }
    expect(withAssetSizes(handled, QStringLiteral("Quick")) == u"Good",
           QStringLiteral("S15.56: a folder copied in and closed during a scan was published as [%1]").arg(withAssetSizes(handled, QStringLiteral("Quick"))));
    const bool listed = waitUntil([&] { const Wallpaper* q = rowTitled(m->wallpapers(), QStringLiteral("Quick")); return q && q->assetStamp.size == 2000; }, kWaitMs);
    expect(listed, QStringLiteral("S15.56: quick was not listed whole by the scan its copy scheduled; [%1]").arg(titles(m->wallpapers())));
}

// S15.59: the library polled instead of watched.
void polling(const QString& root) {
    const QString dir = root + QStringLiteral("/polled");
    const QString away = root + QStringLiteral("/polled-away");
    const QString staging = root + QStringLiteral("/polled-staging");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty()) {
        fault("S15.59", QStringLiteral("the polled library could not be built"));
        return;
    }
    OpenCounter opens(dir);
    FileOpens files(dir);
    if (!opens.ok() || !files.ok()) {
        harnessFailure("S15.59: an open counter was already counting another folder, or the folder could not be resolved");
        return;
    }
    LibraryManager m(dir, kSettleMs, kPollMs, LibraryManager::Watching::Poll);

    // GOAL: S15.59 -- a library told to poll does, and reads nothing before its first rescan():
    //       no scan runs, no wallpaper file is opened, and it stays Loading over several poll periods.
    // CONTROL: app/library/folder_watcher.cpp, the constructor's `if (watching == Watching::Poll) {`
    //       made `if (false) {` -> red, "S15.59: a library told to poll reports polling() 0";
    //       poll()'s `if (!m_scanned) { return; }` deleted -> red, "S15.59: before its first
    //       rescan() the polled library ran 1 scan(s), opened 6 wallpaper file(s) and is state 1"; restored.
    pump(5 * kPollMs);
    const OpenCounts beforeRescan = opens.take();
    const int openedBeforeRescan = files.take();
    expect(m.polling(), QStringLiteral("S15.59: a library told to poll reports polling() %1").arg(m.polling()));
    expect(beforeRescan.scans == 0 && openedBeforeRescan == 0 && m.state() == LibraryManager::State::Loading,
           QStringLiteral("S15.59: before its first rescan() the polled library ran %1 scan(s), opened %2 wallpaper file(s) and is state %3")
               .arg(beforeRescan.scans).arg(openedBeforeRescan).arg(int(m.state())));

    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.59: the polled library never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);

    // GOAL: S15.59 -- an idle polled library is looked at every period but neither scanned again
    //       nor read: no wallpaper's files are opened, on any thread, by any open call.
    // WHY-THIS-SHAPE: scans (the scan thread opening the library) are counted apart from polls
    //       (the main thread opening it), and every file below the library opened through open,
    //       openat or openat2, O_PATH included (FileOpens, openat2_shim.cpp), whatever thread asks.
    //       Counting openat2 alone would miss the watcher's own lease probe (openForWriting, an
    //       openat) and the reader's fallback when openat2 is refused. The window is six polls,
    //       waited for, not six periods of wall-clock time: how many timer ticks fit a fixed time
    //       is the scheduler's business. Polls that stop coming within the bound are a dead poll
    //       timer, a failure.
    // CONTROL: app/library/folder_watcher.cpp, poll()'s `if (closed || (quiet && polled.polled != polled.scanned)) {`
    //       made `if (closed || quiet) {` -> red, "S15.59: an idle polled library was scanned 1
    //       time(s) and opened 6 wallpaper file(s) while it was polled 6 time(s)"; the constructor's
    //       `connect(&m_poll, ...)` deleted -> red, "S15.59: the idle polled library was polled 0
    //       time(s) in 3000 ms"; poll() given `for (const auto& e : seen.entries) { readWallpaperFolder(...); }`
    //       after its observe() -> red, "... scanned 0 time(s) and opened 90 wallpaper file(s) ...";
    //       poll() given a lease probe of every entry, `for (const auto& e : polled.polled.entries) { openForWriting(AT_FDCWD, ...); }`
    //       -> red, "... scanned 0 time(s) and opened 18 wallpaper file(s) ..." (an openat, which
    //       counting openat2 alone never saw); restored.
    // COVERAGE-ASSERT: the first scan was counted, and its file opens, so neither a dead scan counter
    //       nor a dead file counter can read as "none" here or before the first rescan(); the window
    //       held six polls. Test-side control: FileOpens aimed at another folder -> red, "the first
    //       scan of the polled library was counted 1 time(s), opening 0 file(s)"; restored.
    const int firstScans = opens.take().scans;
    const int firstOpened = files.take();
    expect(firstScans >= 1 && firstOpened >= 3,
           QStringLiteral("S15.59: the first scan of the polled library was counted %1 time(s), opening %2 file(s); a counter is not counting")
               .arg(firstScans).arg(firstOpened));
    OpenCounts idle;
    const bool polled = waitUntil([&] {
        const OpenCounts seen = opens.take();
        idle.scans += seen.scans;
        idle.watcherOpens += seen.watcherOpens;
        return idle.watcherOpens >= 6;
    }, kWaitMs);
    const int idleOpened = files.take();
    if (!polled) {
        expect(false, QStringLiteral("S15.59: the idle polled library was polled %1 time(s) in %2 ms").arg(idle.watcherOpens).arg(kWaitMs));
    } else {
        expect(idle.scans == 0 && idleOpened == 0, QStringLiteral("S15.59: an idle polled library was scanned %1 time(s) and opened %2 wallpaper file(s) while it was polled %3 time(s)")
                                                       .arg(idle.scans).arg(idleOpened).arg(idle.watcherOpens));
    }

    // GOAL: S15.59 -- a wallpaper folder added to a polled library, and then removed, is noticed
    //       without rescan().
    // CONTROL: app/library/folder_watcher.cpp, the constructor's `connect(&m_poll, &QTimer::timeout, this, &FolderWatcher::poll);`
    //       deleted -> red, "S15.59: a wallpaper added to the polled library was not listed"; restored.
    if (!moveInFolder(staging, dir, QStringLiteral("added"), wallpaperTitled("Added")).isEmpty()) {
        fault("S15.59", QStringLiteral("added could not be moved in"));
        return;
    }
    const bool added = readyWith(m, QStringLiteral("Added,Good"));
    expect(added, QStringLiteral("S15.59: a wallpaper added to the polled library was not listed; [%1]").arg(titles(m.wallpapers())));
    if (!QDir(dir + QStringLiteral("/added")).removeRecursively()) {
        fault("S15.59", QStringLiteral("added could not be removed"));
        return;
    }
    const bool removed = readyWith(m, QStringLiteral("Good"));
    expect(removed, QStringLiteral("S15.59: a wallpaper removed from the polled library is still listed; [%1]").arg(titles(m.wallpapers())));

    // GOAL: S15.59 -- a wallpaper.json's mode changing, with nothing else about it, is noticed by
    //       polling: made unreadable the wallpaper turns broken, made readable again it is usable.
    // CONTROL: app/library/folder_watcher.cpp, observe()'s `observed.mode = st.st_mode;` deleted
    //       -> red, "S15.59: good's wallpaper.json made unreadable still reads [Good]"; restored.
    const QString manifest = dir + QStringLiteral("/good/wallpaper.json");
    if (::geteuid() == 0) {
        notVerified("S15.59 (a mode change)", "running as root, which can read a mode-000 file");
    } else {
        if (!setMode(manifest, 0)) {
            fault("S15.59", QStringLiteral("good's wallpaper.json could not be made mode 000"));
            return;
        }
        const bool locked = readyWith(m, QStringLiteral("good(broken)"));
        const QString lockedList = titles(m.wallpapers());
        if (!setMode(manifest, 0644)) {
            fault("S15.59", QStringLiteral("good's wallpaper.json could not be made readable again"));
            return;
        }
        const bool unlocked = readyWith(m, QStringLiteral("Good"));
        expect(locked && unlocked, QStringLiteral("S15.59: good's wallpaper.json made unreadable still reads [%1]; made readable again [%2]")
                                       .arg(lockedList, titles(m.wallpapers())));
    }

    // GOAL: S15.59 -- the polled library folder renamed away reads as an empty library, and renamed
    //       back is read again.
    // CONTROL: the poll connect deleted, as above -> red, "S15.59: the polled library renamed away
    //       still reads [Good]"; restored.
    if (!QDir().rename(dir, away)) {
        fault("S15.59", QStringLiteral("the polled library could not be renamed away"));
        return;
    }
    const bool empty = readyWith(m, QString());
    const QString awayList = titles(m.wallpapers());
    if (!QDir().rename(away, dir)) {
        fault("S15.59", QStringLiteral("the polled library could not be renamed back"));
        return;
    }
    const bool returned = readyWith(m, QStringLiteral("Good"));
    expect(empty && returned, QStringLiteral("S15.59: the polled library renamed away still reads [%1]; renamed back [%2]")
                                  .arg(awayList, titles(m.wallpapers())));
    static_cast<void>(history.take());

    // GOAL: S15.59 -- a folder copied in whose asset is written in pieces less than a poll period
    //       apart is published once, with the asset whole.
    // WHY-THIS-SHAPE: a change is read only once a poll finds nothing further changed, and a scan
    //       that finds the asset open holds the folder: two defences, so only breaking both is red.
    //       If this test stalled between two writes for more than half a period, a correct manager
    //       may scan mid-write, so the run is abandoned as not verified rather than graded.
    // CONTROL: app/library/folder_watcher.cpp, poll()'s `const bool quiet = seen == polled.polled;`
    //       made `const bool quiet = true;` together with scanFinished()'s `if (S_ISREG(entry.mode) && openForWriting(...))`
    //       made `if (false)`, one patch -> red, "S15.59: a slow copy into the polled library was
    //       published as [Good,Slow@4000 | Good,Slow@8000]"; either alone left it green; restored.
    Copy copy(dir + QStringLiteral("/slow"), wallpaperTitled("Slow", 8000), 1000);
    if (!copy.ok()) {
        fault("S15.59", QStringLiteral("slow could not be started"));
        return;
    }
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    qint64 widestGap = 0;
    for (int i = 1; i < 8; ++i) {
        pump(kPollMs / 4);
        if (!copy.write(1000)) {
            fault("S15.59", QStringLiteral("a write to slow failed"));
            return;
        }
        widestGap = std::max(widestGap, clock.elapsed() - last);
        last = clock.elapsed();
    }
    copy.finish();
    const bool listed = readyWith(m, QStringLiteral("Good,Slow"));
    pump(3 * kPollMs);
    const QList<QList<Wallpaper>> published = history.takeLists();
    if (widestGap >= kPollMs / 2) {
        notVerified("S15.59 (a slow copy)", "this test stalled " + std::to_string(widestGap) + " ms between two writes, over half a poll period");
    } else {
        expect(listed && withAssetSizes(published, QStringLiteral("Slow")) == u"Good,Slow@8000",
               QStringLiteral("S15.59: a slow copy into the polled library was published as [%1]").arg(withAssetSizes(published, QStringLiteral("Slow"))));
    }
}

// S15.72: polling cannot see a close, so a scan checks for writers and a poll checks the ones it found.
void pollingOpenFiles(const QString& root) {
    const QString dir = root + QStringLiteral("/polled-open");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty()) {
        fault("S15.72", QStringLiteral("the polled library could not be built"));
        return;
    }
    LibraryManager m(dir, kSettleMs, kPollMs, LibraryManager::Watching::Poll);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.72: the polled library never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);
    OpenCounter opens(dir);
    // GOAL: S15.72 -- in a polled library, a folder copied in whose writer pauses for longer than
    //       a poll, its asset still open, is not listed while it is open, even by the scans its
    //       pauses let run; and once the writer closes it -- with no byte written after the last
    //       scan, so nothing a poll compares has changed -- a scan follows and lists it whole.
    // WHY-THIS-SHAPE: each pause is four polls, so a quiet poll provably asks for a scan while the
    //       file is open (counted); the last pause comes after the final byte, so only the poll
    //       that finds the file closed can owe the scan that lists it. The wait for that listing is
    //       bounded and its end is the failure.
    // CONTROL: app/library/folder_watcher.cpp, scanFinished()'s `if (S_ISREG(entry.mode) && openForWriting(...)) {`
    //       made `if (false) {` -> red, "S15.72: while its asset was open the polled library published
    //       [Good,Paused]"; poll()'s `closed ||` deleted -> red, "S15.72: after its asset was closed
    //       the polled library still lists [Good]"; restored.
    // COVERAGE-ASSERT: scans ran during the pauses, so the "not listed" half was tested by them.
    Copy copy(dir + QStringLiteral("/paused"), wallpaperTitled("Paused", 2000), 1000);
    if (!copy.ok()) {
        fault("S15.72", QStringLiteral("the copy could not be started"));
        return;
    }
    static_cast<void>(opens.take());
    pump(4 * kPollMs);
    const bool written = copy.rest();
    pump(4 * kPollMs);
    const int scansWhileOpen = opens.take().scans;
    const QString whileOpen = titles(m.wallpapers());
    copy.finish();
    const bool listed = waitUntil([&] { const Wallpaper* p = rowTitled(m.wallpapers(), QStringLiteral("Paused")); return p && p->assetStamp.size == 2000; }, kWaitMs);
    if (!written || !opens.ok()) {
        fault("S15.72", QStringLiteral("the copy could not be written, or the counter was busy"));
        return;
    }
    expect(scansWhileOpen >= 1, QStringLiteral("S15.72: no scan ran while the copy paused (%1); the pauses tested nothing").arg(scansWhileOpen));
    expect(whileOpen == u"Good", QStringLiteral("S15.72: while its asset was open the polled library published [%1]").arg(whileOpen));
    expect(listed, QStringLiteral("S15.72: after its asset was closed the polled library still lists [%1]").arg(titles(m.wallpapers())));
}

// S15.59: inotify that cannot start.
void inotifyUnavailable(const QString& root) {
    const QString dir = root + QStringLiteral("/no-inotify");
    const QString staging = root + QStringLiteral("/no-inotify-staging");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty()) {
        fault("S15.59", QStringLiteral("the library for the inotify failure could not be built"));
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
    std::unique_ptr<LibraryManager> m;
    {
        const WarningLog log;
        m = std::make_unique<LibraryManager>(dir, kSettleMs, kPollMs);
        warnings = log.lines();
    }
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
    //       wallpaper added afterwards is still listed without rescan().
    // WHY-THIS-SHAPE: a real failure, made by using up this process's descriptors while the
    //       manager is built, since Watching::Poll only models it.
    // CONTROL: app/library/folder_watcher.cpp, the constructor's `if (m_inotify < 0) {` (the one
    //       calling pollInstead("inotify_init1", ...)) made `if (false) {` -> red, "S15.59: with inotify
    //       unavailable the library reports polling() 0 after 0 warning(s)"; restored.
    const qsizetype said = std::ranges::count_if(warnings, [](const QString& w) { return w.contains(QLatin1String("inotify_init1 failed")); });
    expect(m->polling() && said == 1,
           QStringLiteral("S15.59: with inotify unavailable the library reports polling() %1 after %2 warning(s): [%3]")
               .arg(m->polling()).arg(said).arg(warnings.join(QStringLiteral(" | "))));
    m->rescan();
    if (!readyWith(*m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.59: the library read without inotify never settled; [%1]").arg(titles(m->wallpapers())));
        return;
    }
    if (!moveInFolder(staging, dir, QStringLiteral("added"), wallpaperTitled("Added")).isEmpty()) {
        fault("S15.59", QStringLiteral("added could not be moved in"));
        return;
    }
    const bool added = readyWith(*m, QStringLiteral("Added,Good"));
    expect(added, QStringLiteral("S15.59: with inotify unavailable a wallpaper added was not listed; [%1]").arg(titles(m->wallpapers())));
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
    std::optional<WarningLog> log(std::in_place);
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
    const QStringList testerSaid = log->lines();
    log.reset();
    expect(mismatches == 0, QStringLiteral("S15.52: after %1 assignment(s) the rows followed from the signals differ from the list assigned, the first at round %2")
                                .arg(mismatches).arg(firstMismatch));
    expect(resets == 0 && testerSaid.isEmpty(),
           QStringLiteral("S15.52: the model was reset %1 time(s) and Qt's model tester said: [%2]").arg(resets).arg(testerSaid.mid(0, 3).join(QStringLiteral(" | "))));
    expect(inserts > 0 && removals > 0 && moves > 0 && changes > 0,
           QStringLiteral("S15.52: the random lists made %1 insert(s), %2 removal(s), %3 move(s), %4 change(s); each should be above 0")
               .arg(inserts).arg(removals).arg(moves).arg(changes));
}

// The user's state on a listed wallpaper: it is selected and its speed edited to 1.8, which is not
// its default and within the range every manifest here declares.
bool selectEdited(LibraryManager& m, const QString& path) {
    return m.select(path) && m.setPropertyValue(QStringLiteral("speed"), 1.8);
}

bool keptEdited(const LibraryManager& m, const QString& path) {
    return m.selectedPath() == path && m.propertyValues().value(QStringLiteral("speed")) == QVariant(1.8);
}

QString selectionOf(const LibraryManager& m) {
    return QStringLiteral("selected \"%1\" with speed %2")
        .arg(QFileInfo(m.selectedPath()).fileName(), m.propertyValues().value(QStringLiteral("speed")).toString());
}

// S15.60: a listed wallpaper's own file rewritten in place, its wallpaper.json or its asset.
void rewrittenInPlace(const QString& root) {
    for (const bool asset : {false, true}) {
        const char* goal = asset ? "S15.60 (the asset)" : "S15.60";
        const QString base = root + (asset ? QStringLiteral("/rewrite-asset") : QStringLiteral("/rewrite"));
        const QString dir = base + QStringLiteral("/lib");
        const QString shown = dir + QStringLiteral("/a");
        const QString written = shown + (asset ? QStringLiteral("/assets/video.mp4") : QStringLiteral("/wallpaper.json"));
        const QByteArray v2 = asset ? QByteArray(3000, 'w') : titled("A v2");
        if (!writeFolder(shown, wallpaperTitled("A v1")).isEmpty() || !writeFolder(dir + QStringLiteral("/b"), wallpaperTitled("B")).isEmpty() ||
            !writeFolder(base + QStringLiteral("/c"), wallpaperTitled("C")).isEmpty()) {
            fault(goal, QStringLiteral("the library could not be built"));
            continue;
        }
        LibraryManager m(dir, kSettleMs);
        m.rescan();
        if (!readyWith(m, QStringLiteral("A v1,B")) || !selectEdited(m, shown)) {
            expect(false, QStringLiteral("%1: the library never settled with [A v1,B], or a could not be selected and edited; [%2]")
                              .arg(QLatin1String(goal), titles(m.wallpapers())));
            continue;
        }
        const qint64 oldAsset = m.wallpapers()[0].assetStamp.size;
        History history(m);

        // GOAL: S15.60 -- a listed, selected and edited wallpaper whose wallpaper.json, or whose
        //       asset, is rewritten in place (opened with O_TRUNC, written in pieces further apart
        //       than the quiet period) keeps its row, the selection and the edits while it is
        //       written, even when another change makes a scan read the library meanwhile, and is
        //       published once, as the new version, after the close.
        // WHY-THIS-SHAPE: writes alone schedule no scan, so a check without one mid-write could not
        //       tell a held row from a scan that never ran; c moved in half-way forces one. For
        //       wallpaper.json the half is proven to read broken, so that scan provably saw it; for
        //       the asset the kept row is recognised by its old asset size. Every publish is recorded.
        // CONTROL: app/library/folder_watcher.cpp, handle()'s `hold(*tree, path);` (its IN_MODIFY
        //       branch) deleted -> red, "S15.60: while a was rewritten a scan published
        //       [B,C,a(broken)], selected "" with speed " and "S15.60 (the asset): ... [A v1@1500,B,C]";
        //       app/library/library_manager.cpp holdBack()'s `scan.append(shown);` deleted -> red,
        //       "... published [B,C], selected "" ..."; restored.
        QFile file(written);
        const qsizetype half = v2.size() / 2;
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || !writeSlowly(file, v2.left(half), 2, kWriteGapMs)) {
            fault(goal, QStringLiteral("the first half of the new file could not be written"));
            continue;
        }
        const bool halfBroken = asset || !readWallpaperFolder(shown).usable();
        if (!QDir().rename(base + QStringLiteral("/c"), dir + QStringLiteral("/c"))) {
            fault(goal, QStringLiteral("c could not be moved in"));
            continue;
        }
        const bool triggered = waitUntil([&] { return rowTitled(m.wallpapers(), QStringLiteral("C")) != nullptr; }, kWaitMs);
        const QList<Wallpaper> duringList = m.wallpapers();
        const bool keptDuring = keptEdited(m, shown);
        const QString selectionDuring = selectionOf(m);
        if (!writeSlowly(file, v2.mid(half), 3, kWriteGapMs)) {
            fault(goal, QStringLiteral("the rest of the new file could not be written"));
            continue;
        }
        file.close();
        const QString after = asset ? QStringLiteral("A v1,B,C") : QStringLiteral("A v2,B,C");
        const bool listed = waitUntil([&] {
            return titles(m.wallpapers()) == after && (!asset || m.wallpapers()[0].assetStamp.size == v2.size());
        }, kWaitMs);
        pump(4 * kSettleMs);
        const QList<QList<Wallpaper>> published = history.takeLists();
        const QString during = withAssetSizes({duringList}, QStringLiteral("A v1"));
        const QString wantDuring = QStringLiteral("A v1@%1,B,C").arg(oldAsset);
        if (!halfBroken) {
            notVerified(goal, "half of the new wallpaper.json reads as usable, so a scan showing it could not be seen");
        } else if (!triggered) {
            expect(false, QStringLiteral("%1: c moved in during the rewrite was never listed; [%2]").arg(QLatin1String(goal), during));
        } else {
            expect(during == wantDuring && keptDuring,
                   QStringLiteral("%1: while a was rewritten a scan published [%2], %3").arg(QLatin1String(goal), during, selectionDuring));
            const QString wantAll = asset ? QStringLiteral("A v1@%1,B,C | A v1@%2,B,C").arg(oldAsset).arg(v2.size())
                                          : QStringLiteral("A v1,B,C | A v2,B,C");
            const QString got = asset ? withAssetSizes(published, QStringLiteral("A v1")) : withAssetSizes(published, QString());
            expect(listed && got == wantAll && keptEdited(m, shown),
                   QStringLiteral("%1: the rewrite published [%2] and ended %3").arg(QLatin1String(goal), got, selectionOf(m)));
        }
    }
}

// S15.60, polled: polling sees no write, so only a wallpaper seen changing while a scan ran keeps its row.
void rewrittenWhilePolled(const QString& root) {
    const QString dir = root + QStringLiteral("/rewrite-polled");
    const QString a = dir + QStringLiteral("/a");
    const QString aManifest = a + QStringLiteral("/wallpaper.json");
    const QByteArray v2 = titled("A v2");
    if (!writeFolder(a, wallpaperTitled("A v1")).isEmpty() || !writeFolder(dir + QStringLiteral("/sentinel"), wallpaperTitled("Sentinel")).isEmpty()) {
        fault("S15.60 (polling)", QStringLiteral("the library could not be built"));
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
        expect(false, QStringLiteral("S15.60 (polling): the library never settled, or a could not be selected and edited; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);

    // GOAL: S15.60 -- in a polled library, a listed wallpaper whose wallpaper.json is rewritten
    //       while a scan runs, which that scan reads half-written, keeps its row, the selection and
    //       the edits; the new version is published by a later scan, with the selection and edits kept.
    // WHY-THIS-SHAPE: the pool's one thread is held so the scan provably reads a only after its
    //       half is written (a scan counted after release, none before), and the rest is written
    //       and closed before the event loop runs, so the scan's result is handled with the file
    //       changed since the scan began, and closed, so no writer check can be what keeps the row.
    //       No event loop runs meanwhile, so no poll interleaves. sentinel is removed in the same
    //       window: a publish without it proves the result was handled.
    // CONTROL: app/library/folder_watcher.cpp, scanFinished()'s `if (auto* polled = std::get_if<PollWatch>(&m_watch)) {...}`
    //       block deleted -> red, "S15.60 (polling): the scan that read a half-written published
    //       [a(broken)], selected "" with speed "; restored.
    QString unproven;
    {
        PoolHold hold(kWaitMs);
        QFile file(aManifest);
        const qsizetype half = v2.size() / 2;
        if (!hold.ok()) {
            fault("S15.60 (polling)", QStringLiteral("the pool's thread could not be held"));
            return;
        }
        if (!QDir(dir + QStringLiteral("/sentinel")).removeRecursively()) {
            fault("S15.60 (polling)", QStringLiteral("sentinel could not be removed"));
            return;
        } else {
            m.rescan();
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(v2.left(half)) != half || !file.flush()) {
                fault("S15.60 (polling)", QStringLiteral("the first half of the new wallpaper.json could not be written"));
                return;
            }
            const bool halfBroken = !readWallpaperFolder(a).usable();
            static_cast<void>(opens.take());
            hold.release();
            const bool done = QThreadPool::globalInstance()->waitForDone(kWaitMs);
            const int read = opens.take().scans;
            const QByteArray rest = v2.mid(half);
            if (file.write(rest) != rest.size() || !file.flush()) {
                fault("S15.60 (polling)", QStringLiteral("the rest of the new wallpaper.json could not be written"));
                return;
            }
            file.close();
            if (!done || read < 1 || !halfBroken) {
                unproven = QStringLiteral("the scan was not shown to read a half-written: finished %1, scans after release %2, the half reads broken %3")
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
           QStringLiteral("S15.60 (polling): the scan that read a half-written published [%1], %2").arg(published.join(QStringLiteral(" | ")), selection));
    const bool listed = readyWith(m, QStringLiteral("A v2"));
    expect(listed && keptEdited(m, a),
           QStringLiteral("S15.60 (polling): after the rewrite the list is [%1], %2").arg(titles(m.wallpapers()), selectionOf(m)));
}

// S15.73: a folder made inside a listed wallpaper later is watched too.
void subfolderLater(const QString& root) {
    const QString dir = root + QStringLiteral("/subfolder");
    const QString staging = root + QStringLiteral("/subfolder-staging");
    OrderedJson json = planManifest();
    json["title"] = "Media";
    json["file"] = "media/video.mp4";
    const Entries entries{{QStringLiteral("wallpaper.json"), jsonBytes(json)}, {QStringLiteral("preview.png"), pngBytes(64, 36)}};
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty() ||
        !writeFolder(staging + QStringLiteral("/c"), wallpaperTitled("C")).isEmpty()) {
        fault("S15.73", QStringLiteral("the library could not be built"));
        return;
    }
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good")) || !moveInFolder(staging, dir, QStringLiteral("media"), entries).isEmpty() ||
        !readyWith(m, QStringLiteral("Good,media(broken)"))) {
        expect(false, QStringLiteral("S15.73: the library never listed media, still without its asset, broken; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    if (!QDir().mkpath(dir + QStringLiteral("/media/media"))) {
        fault("S15.73", QStringLiteral("the media folder could not be made"));
        return;
    }
    pump(6 * kSettleMs);
    History history(m);
    // GOAL: S15.73 -- a folder made inside a listed wallpaper after it was first watched is
    //       watched too: its asset written there in pieces, with a scan forced mid-write, is not
    //       read until it is closed, and is then published once, whole.
    // WHY-THIS-SHAPE: the folder is made and the event loop run before the asset is opened, so no
    //       check made as the folder is first watched can find a writer; only a watch on the new
    //       folder reports the writes. It gets one from two places -- as its creation is read, and
    //       at the walk before every scan -- so removing the first alone leaves this green.
    // CONTROL: app/library/folder_watcher.cpp, handle()'s `watchTree(*tree, path);` (in the `else`
    //       after `unwatchTree(*tree, path);`) deleted together with placeWatch()'s
    //       `if (!watchTree(*tree, {}, &reached)) {` walk made `if (false) {`, one patch -> red,
    //       "S15.73: an asset written in a folder made later was published as [C,Good,Media@1000]";
    //       the handle() half alone left it green; restored.
    QFile asset(dir + QStringLiteral("/media/media/video.mp4"));
    const QByteArray bytes(3000, 'm');
    if (!asset.open(QIODevice::WriteOnly) || !writeSlowly(asset, bytes.left(1000), 1, kWriteGapMs)) {
        fault("S15.73", QStringLiteral("the asset could not be started"));
        return;
    }
    pump(3 * kWriteGapMs);
    if (!QDir().rename(staging + QStringLiteral("/c"), dir + QStringLiteral("/c"))) {
        fault("S15.73", QStringLiteral("c could not be moved in"));
        return;
    }
    const bool triggered = waitUntil([&] { return rowTitled(m.wallpapers(), QStringLiteral("C")) != nullptr; }, kWaitMs);
    if (!writeSlowly(asset, bytes.mid(1000), 2, kWriteGapMs)) {
        fault("S15.73", QStringLiteral("the rest of the asset could not be written"));
        return;
    }
    asset.close();
    const bool listed = waitUntil([&] { const Wallpaper* w = rowTitled(m.wallpapers(), QStringLiteral("Media")); return w && w->assetStamp.size == 3000; }, kWaitMs);
    pump(4 * kSettleMs);
    const QString published = withAssetSizes(history.takeLists(), QStringLiteral("Media"));
    expect(triggered && listed && published == u"C,Good,media(broken) | C,Good,Media@3000",
           QStringLiteral("S15.73: an asset written in a folder made later was published as [%1] (c listed %2)").arg(published).arg(triggered));
}

// S15.86: a folder made while the library is watched is watched from the moment its creation is read.
void filledAfterCreation(const QString& root) {
    const QString dir = root + QStringLiteral("/filled");
    const QString folder = dir + QStringLiteral("/new");
    // Long against the gap between pieces, so only a stall of half of it can let a correct watcher
    // scan mid-fill; that stall abandons the phase below.
    constexpr int settle = 8 * kSettleMs;
    constexpr int gap = kSettleMs;
    constexpr int pieces = 16;
    constexpr qsizetype piece = 100;
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty()) {
        fault("S15.86", QStringLiteral("the library could not be built"));
        return;
    }
    LibraryManager m(dir, settle);
    History history(m);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.86: the library never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    pump(settle + kSettleMs);
    static_cast<void>(history.take());

    // GOAL: S15.86 -- a folder made while no scan runs, then filled file by file, each closed, each
    //       change sooner after the last than the quiet period, is scanned once, after the last, and
    //       published whole: never part-way, as it would be if changes inside it went unseen until
    //       a scan placed its watch.
    // WHY-THIS-SHAPE: "write and close a file in the new folder, then see it noticed promptly"
    //       stays green without the watch: the scan the creation itself schedules places the watch
    //       in its walk, so a change after that scan is seen. Only the window before that scan is
    //       lost, and it shows only as a scan mid-fill. Each piece is appended through its own open
    //       and close, so no open file is left for the walk's writer check to find.
    // CONTROL: app/library/folder_watcher.cpp, handle()'s `watchTree(*tree, path);` (in the `else`
    //       after `unwatchTree(*tree, path);`) deleted -> red, "S15.86: a folder filled piece by
    //       piece after it was made was published as [Good,New@800 | Good,New@1600]", the only line
    //       red (the first size is wherever the fill was when the quiet period ran out); restored.
    Entries entries = wallpaperTitled("New", piece);
    const QByteArray first = entries.takeLast().second;
    QFile asset(folder + QStringLiteral("/assets/video.mp4"));
    if (!writeFolder(folder, entries).isEmpty() || !QDir().mkpath(folder + QStringLiteral("/assets")) ||
        !asset.open(QIODevice::WriteOnly) || asset.write(first) != piece) {
        fault("S15.86", QStringLiteral("new could not be started"));
        return;
    }
    asset.close();
    QElapsedTimer clock;
    clock.start();
    qint64 longest = 0;
    bool written = true;
    for (int i = 1; i < pieces && written; ++i) {
        const qint64 before = clock.elapsed();
        pump(gap);
        written = asset.open(QIODevice::WriteOnly | QIODevice::Append) && asset.write(QByteArray(piece, 'v')) == piece;
        asset.close();
        longest = std::max(longest, clock.elapsed() - before);
    }
    if (!written) {
        fault("S15.86", QStringLiteral("the rest of new could not be written"));
        return;
    }
    if (longest >= settle / 2) {
        notVerified("S15.86", "a piece came " + std::to_string(longest) + " ms after the one before, so a scan mid-fill would be legitimate");
        return;
    }
    const qsizetype whole = pieces * piece;
    const bool listed = waitUntil([&] { const Wallpaper* w = rowTitled(m.wallpapers(), QStringLiteral("New")); return w && w->assetStamp.size == whole; }, settle + kWaitMs);
    pump(2 * settle);
    const QString published = withAssetSizes(history.takeLists(), QStringLiteral("New"));
    expect(listed && published == QStringLiteral("Good,New@%1").arg(whole),
           QStringLiteral("S15.86: a folder filled piece by piece after it was made was published as [%1]").arg(published));
}

// S15.61: an asset given its name by link() has no write or close under that name.
void linkedIn(const QString& root) {
    const QString dir = root + QStringLiteral("/linked-in");
    const QString staging = root + QStringLiteral("/linked-in-staging");
    Entries noAsset = wallpaperTitled("Linked");
    noAsset.removeLast();
    if (!writeFolder(dir + QStringLiteral("/linked"), noAsset).isEmpty() || !QDir().mkpath(dir + QStringLiteral("/linked/assets")) ||
        !writeFolder(staging, {{QStringLiteral("video.mp4"), QByteArray(500, 'v')}}).isEmpty()) {
        fault("S15.61", QStringLiteral("the library could not be built"));
        return;
    }
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!readyWith(m, QStringLiteral("linked(broken)"))) {
        expect(false, QStringLiteral("S15.61: the library never listed linked broken; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    // GOAL: S15.61 -- an asset hard-linked into a listed wallpaper's folder from a staging folder,
    //       its staging name then unlinked, makes the wallpaper usable with no write or close under
    //       the new name.
    // WHY-THIS-SHAPE: the staging name is unlinked before the event loop runs, so the new name has
    //       one link when its creation is read, as a file created by open() would.
    // CONTROL: app/library/folder_watcher.cpp, handle() given the rule "a created name is still
    //       being written" (`if ((event.mask & IN_CREATE) != 0 && (event.mask & IN_ISDIR) == 0) {
    //       hold(*tree, path); return false; }` before the IN_MODIFY branch) -> red, "S15.61: an asset
    //       hard-linked in, its staging name unlinked, left the wallpaper [linked(broken)]"; restored.
    if (::link(QFile::encodeName(staging + QStringLiteral("/video.mp4")).constData(),
               QFile::encodeName(dir + QStringLiteral("/linked/assets/video.mp4")).constData()) != 0 ||
        ::unlink(QFile::encodeName(staging + QStringLiteral("/video.mp4")).constData()) != 0) {
        fault("S15.61", QStringLiteral("the asset could not be linked in and unlinked: ") + QString::fromLocal8Bit(std::strerror(errno)));
        return;
    }
    const bool listed = readyWith(m, QStringLiteral("Linked"));
    expect(listed, QStringLiteral("S15.61: an asset hard-linked in, its staging name unlinked, left the wallpaper [%1]").arg(titles(m.wallpapers())));
}

QByteArray hostWatchLimit() {
    QFile file(QStringLiteral("/proc/sys/user/max_inotify_watches"));
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray();
}

// Sets this user namespace's watch limit; whether it now reads back as `limit`.
bool setOwnWatchLimit(int limit) {
    const QByteArray text = QByteArray::number(limit);
    const int fd = ::open("/proc/sys/user/max_inotify_watches", O_WRONLY | O_CLOEXEC);
    const bool written = fd >= 0 && ::write(fd, text.constData(), size_t(text.size())) == text.size();
    if (fd >= 0) {
        ::close(fd);
    }
    return written && hostWatchLimit() == text;
}

// S15.62, the child: run by watchLimit() under `unshare -U -r`, so the watch limit it lowers is
// its own namespace's. Arguments: the limit, a folder to work in, the parent's namespace.
int watchLimitChild(const QStringList& args) {
    wallpaperx::testing::g_testName = "library_changes (watch limit)";
    const QString goal = QStringLiteral("S15.62 (a limit of %1 watch(es))").arg(args.value(0));
    if (args.size() != 3) {
        harnessFailure("S15.62: the child was given " + std::to_string(args.size()) + " argument(s), not 3");
        return verdict();
    }
    if (const auto refused = refuseUnlessOwnNamespace(goal, args[2])) {
        return *refused;
    }
    if (!setOwnWatchLimit(args[0].toInt())) {
        notVerified(goal.toUtf8().constData(), "this namespace's max_inotify_watches could not be set to " + args[0].toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const QString dir = args[1] + QStringLiteral("/lib");
    const QString staging = args[1] + QStringLiteral("/staging");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty() ||
        !writeFolder(dir + QStringLiteral("/other"), wallpaperTitled("Other")).isEmpty()) {
        fault("S15.62", QStringLiteral("the library could not be built"));
        return verdict();
    }
    std::optional<WarningLog> log(std::in_place);
    childReady();
    bool added = false;
    bool rewritten = false;
    {
        LibraryManager m(dir, kSettleMs, kPollMs);
        m.rescan();
        if (!readyWith(m, QStringLiteral("Good,Other"))) {
            expect(false, QStringLiteral("%1: the library never settled with [Good,Other]; [%2]").arg(goal, titles(m.wallpapers())));
        } else if (!moveInFolder(staging, dir, QStringLiteral("added"), wallpaperTitled("Added")).isEmpty()) {
            fault("S15.62", QStringLiteral("added could not be moved in"));
        } else {
            added = readyWith(m, QStringLiteral("Added,Good,Other"));
            rewritten = added && writeBytes(dir + QStringLiteral("/other/wallpaper.json"), titled("Other v2")) &&
                        readyWith(m, QStringLiteral("Added,Good,Other v2"));
        }
        const QStringList warnings = log->lines();
        log.reset();
        const qsizetype said = std::ranges::count_if(
            warnings, [](const QString& w) { return w.contains(QLatin1String("inotify_add_watch failed (No space left on device)")); });
        expect(m.polling() && said == 1 && warnings.size() == 1,
               QStringLiteral("%1: the library reports polling() %2 after %3 warning(s) about it: [%4]")
                   .arg(goal).arg(m.polling()).arg(said).arg(warnings.join(QStringLiteral(" | "))));
        expect(added && rewritten, QStringLiteral("%1: a wallpaper added was listed %2, and a wallpaper.json rewritten inside a folder was read %3; [%4]")
                                       .arg(goal).arg(added).arg(rewritten).arg(titles(m.wallpapers())));
    }
    return verdict();
}

// S15.74, the child: run by untrustedFileSystem() under `unshare -U -r -m`, so the ramfs it mounts
// is in a mount namespace of its own and vanishes with it. Arguments: a folder to mount on, the
// parent's namespace.
int untrustedChild(const QStringList& args) {
    wallpaperx::testing::g_testName = "library_changes (untrusted file system)";
    const QString goal = QStringLiteral("S15.74");
    if (args.size() != 2) {
        harnessFailure("S15.74: the child was given " + std::to_string(args.size()) + " argument(s), not 2");
        return verdict();
    }
    if (const auto refused = refuseUnlessOwnNamespace(goal, args[1])) {
        return *refused;
    }
    const QString mountPoint = args[0];
    if (!QDir().mkpath(mountPoint) || ::mount("none", QFile::encodeName(mountPoint).constData(), "ramfs", 0, nullptr) != 0) {
        notVerified("S15.74", std::string("a ramfs could not be mounted in this namespace: ") + std::strerror(errno));
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    constexpr int hold = 300;
    const QString dir = mountPoint + QStringLiteral("/lib");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty()) {
        fault("S15.74", QStringLiteral("the library could not be built on the ramfs"));
        return verdict();
    }
    // A read lease on a file nobody has open, asked directly: ramfs refuses it (EINVAL), so a
    // lease says nothing about writers here, as on a network file system.
    const bool leaseRefused = [&] {
        const int fd = ::open(QFile::encodeName(dir + QStringLiteral("/good/wallpaper.json")).constData(), O_RDONLY | O_CLOEXEC);
        const bool refused = fd >= 0 && ::fcntl(fd, F_SETLEASE, F_RDLCK) != 0;
        if (fd >= 0) {
            ::close(fd);
        }
        return refused;
    }();
    if (!leaseRefused) {
        notVerified("S15.74", "this ramfs grants read leases, so it does not stand for a file system where a lease says nothing");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    // GOAL: S15.74 -- on a file system outside the ones where a refused read lease means a writer,
    //       no wallpaper is held for a lease the file system refuses: the wallpapers present at the
    //       first read are listed, and a folder whose asset is written and then left open with no
    //       further write is re-read, whole, once the hold passes -- not hidden, or kept at its old
    //       row, for as long as the file stays open.
    // WHY-THIS-SHAPE: run on a ramfs mounted in a namespace of the child's own, where a lease is
    //       refused for every file (checked above), as a network file system refuses one it holds
    //       no delegation for. The second write is seen by the watch, so the row is held by the
    //       ordinary 300 ms hold with the file open throughout; on a file system in the list the
    //       same row would stay at its first size until the close. A network file system's refusal
    //       is EAGAIN, which cannot be staged here, so the control makes every refusal read as a
    //       writer, as one would there.
    // CONTROL: app/library/folder_watcher.cpp, leasesShowWriters()'s `default: return false;` made
    //       `default: return true;` together with openForWriting()'s `return errno == EAGAIN;` made
    //       `return true;`, one patch -> red, "S15.74: the ramfs library never settled with
    //       [Good]", and the same for S15.87 (holds kept), also on a ramfs; S15.74's line re-run via
    //       control_batch.sh 2026-10-05; restored.
    childReady();
    LibraryManager m(dir, kSettleMs, kPollMs, LibraryManager::Watching::Inotify, hold);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.74: the ramfs library never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return verdict();
    }
    Copy copy(dir + QStringLiteral("/open"), wallpaperTitled("Open", 2000), 1000);
    if (!copy.ok()) {
        fault("S15.74", QStringLiteral("the copy could not be started on the ramfs"));
        return verdict();
    }
    const bool first = waitUntil([&] { const Wallpaper* w = rowTitled(m.wallpapers(), QStringLiteral("Open")); return w && w->assetStamp.size == 1000; }, kWaitMs);
    const bool written = copy.write(1000);
    const bool reread = waitUntil([&] { const Wallpaper* w = rowTitled(m.wallpapers(), QStringLiteral("Open")); return w && w->assetStamp.size == 2000; }, 8 * hold);
    const QString listed = titles(m.wallpapers());
    copy.finish();
    if (!written) {
        fault("S15.74", QStringLiteral("the asset could not be written on the ramfs"));
        return verdict();
    }
    expect(first && reread, QStringLiteral("S15.74: a folder whose asset stayed open on a ramfs was listed at its first size %1 and re-read whole %2 "
                                           "within %3 ms, the file still open; [%4]")
                                .arg(first).arg(reread).arg(8 * hold).arg(listed));
    return verdict();
}

// The kernel's bound on one inotify queue; 0 when it cannot be read.
int queuedEventsLimit() {
    QFile file(QStringLiteral("/proc/sys/fs/inotify/max_queued_events"));
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toInt() : 0;
}

// An inotify queue of this test's own on `dir`, the same size as the watcher's and fed the same
// events, so its overflow shows the watcher's queue overflowed too.
class OverflowWitness {
public:
    explicit OverflowWitness(const QString& dir) : m_fd(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
        m_ok = m_fd >= 0 && ::inotify_add_watch(m_fd, QFile::encodeName(dir).constData(), IN_CREATE | IN_CLOSE_WRITE) >= 0;
    }
    ~OverflowWitness() {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
    }
    OverflowWitness(const OverflowWitness&) = delete;
    OverflowWitness& operator=(const OverflowWitness&) = delete;

    // Whether the queue overflowed; reads it empty.
    bool overflowed() {
        bool seen = false;
        alignas(inotify_event) char buffer[16 * 1024];
        for (ssize_t got; m_ok && (got = ::read(m_fd, buffer, sizeof buffer)) > 0;) {
            for (const char* p = buffer; p < buffer + got; p += sizeof(inotify_event) + reinterpret_cast<const inotify_event*>(p)->len) {
                seen = seen || (reinterpret_cast<const inotify_event*>(p)->mask & IN_Q_OVERFLOW) != 0;
            }
        }
        return seen;
    }

private:
    int m_fd;
    bool m_ok = false;
};

// Makes and closes enough hidden files in `dir` to overflow an inotify queue -- two events each --
// with no event loop running, so the watcher reads none of them meanwhile. Empty on success.
QString flood(const QString& dir) {
    const int limit = queuedEventsLimit();
    if (limit <= 0 || limit > 200000) {
        return QStringLiteral("max_queued_events is %1, too many files to make").arg(limit);
    }
    const QByteArray base = QFile::encodeName(dir) + "/.flood-";
    for (int i = 0; i < limit / 2 + 1000; ++i) {
        const int fd = ::open((base + QByteArray::number(i)).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            return QStringLiteral("flood file %1 could not be made: %2").arg(i).arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        ::close(fd);
    }
    return {};
}

// Waits for a scan counted by `scans` to have run, then for its result to be handled.
bool scannedAndSettled(OpenCounter& scans) {
    int seen = 0;
    const bool ran = waitUntil([&] { seen += scans.take().scans; return seen >= 1; }, kWaitMs);
    const bool idle = ran && QThreadPool::globalInstance()->waitForDone(kWaitMs);
    pump(4 * kSettleMs);
    return idle;
}

// S15.87, after the flood, with the first half of slow's wallpaper.json written through `file`:
// the scan the overflow brings must keep slow's row, selection and edits, and the rest written and
// closed must publish the new version once.
void gradeHeldThroughOverflow(LibraryManager& m, History& history, OpenCounter& scans, QFile& file, const QByteArray& v2,
                              const QString& slow, const QString& label) {
    static_cast<void>(scans.take());
    const bool scanned = scannedAndSettled(scans);
    const QString during = titles(m.wallpapers());
    const bool keptDuring = keptEdited(m, slow);
    const QString selectionDuring = selectionOf(m);
    const bool written = file.write(v2.mid(v2.size() / 2)) == v2.size() - v2.size() / 2 && file.flush();
    file.close();
    const bool listed = readyWith(m, QStringLiteral("Good,Slow v2"));
    const QStringList published = history.take();
    if (!scanned || !written) {
        expect(false, QStringLiteral("%1: no scan followed the overflow (%2), or the rest could not be written (%3)").arg(label).arg(scanned).arg(written));
        return;
    }
    expect(during == u"Good,Slow" && keptDuring, QStringLiteral("%1: after the overflow a scan published [%2], %3").arg(label, during, selectionDuring));
    expect(listed && published == QStringList{QStringLiteral("Good,Slow v2")} && keptEdited(m, slow),
           QStringLiteral("%1: the rewrite published [%2] and ended %3").arg(label, published.join(u" | "), selectionOf(m)));
}

// S15.87, the child: run by overflowKeptHolds() under `unshare -U -r -m`, on a ramfs, where no
// read lease says a file is open for writing, so a hold that survives an overflow can only be one
// kept from before it. Arguments: a folder to mount on, the parent's namespace.
int overflowKeptChild(const QStringList& args) {
    wallpaperx::testing::g_testName = "library_changes (overflow on a ramfs)";
    const QString goal = QStringLiteral("S15.87 (holds kept)");
    if (args.size() != 2) {
        harnessFailure("S15.87: the child was given " + std::to_string(args.size()) + " argument(s), not 2");
        return verdict();
    }
    if (const auto refused = refuseUnlessOwnNamespace(goal, args[1])) {
        return *refused;
    }
    if (!QDir().mkpath(args[0]) || ::mount("none", QFile::encodeName(args[0]).constData(), "ramfs", 0, nullptr) != 0) {
        notVerified("S15.87 (holds kept)", std::string("a ramfs could not be mounted in this namespace: ") + std::strerror(errno));
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const QString dir = args[0] + QStringLiteral("/lib");
    const QString slow = dir + QStringLiteral("/slow");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty() || !writeFolder(slow, wallpaperTitled("Slow")).isEmpty()) {
        fault("S15.87", QStringLiteral("the library could not be built on the ramfs"));
        return verdict();
    }
    childReady();
    OpenCounter scans(dir);
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!scans.ok() || !readyWith(m, QStringLiteral("Good,Slow")) || !selectEdited(m, slow)) {
        expect(false, QStringLiteral("S15.87 (holds kept): the ramfs library never settled with [Good,Slow], or slow could not be selected and edited; [%1]").arg(titles(m.wallpapers())));
        return verdict();
    }
    History history(m);
    // GOAL: S15.87 -- a hold on a file being written survives an inotify queue overflow: a
    //       selected, edited wallpaper whose wallpaper.json is being rewritten in place, its writes
    //       seen before the overflow, is not read by the scan the overflow brings; it keeps its row,
    //       selection and edits, and is published once, as the new version, after the close.
    // WHY-THIS-SHAPE: on a ramfs no read lease is granted, so the check made again for writers
    //       after an overflow finds none: only a hold kept across the overflow can hold the file.
    //       On ext4 or tmpfs that check would hold it anyway, so there a kept hold cannot be told
    //       from a found one. The overflow is made by hidden files alone, with no event loop
    //       running, and shown by a queue of this test's own.
    // CONTROL: app/library/folder_watcher.cpp, handle()'s overflow branch given
    //       `if (auto* t = std::get_if<TreeWatch>(&m_watch)) { t->writing.clear(); }` first -> red,
    //       "S15.87 (holds kept): after the overflow a scan published [Good,slow(broken)], selected
    //       "" ..."; restored.
    QFile file(slow + QStringLiteral("/wallpaper.json"));
    const QByteArray v2 = titled("Slow v2");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(v2.left(v2.size() / 2)) != v2.size() / 2 || !file.flush()) {
        fault("S15.87", QStringLiteral("slow's wallpaper.json could not be half rewritten"));
        return verdict();
    }
    // The writes are read, so the hold is placed, before the flood.
    pump(4 * kSettleMs);
    OverflowWitness witness(dir);
    if (const QString why = flood(dir); !why.isEmpty()) {
        notVerified("S15.87 (holds kept)", why.toStdString());
        return verdict();
    }
    if (!witness.overflowed() || readWallpaperFolder(slow).usable()) {
        notVerified("S15.87 (holds kept)", "the flood did not overflow an inotify queue, or half of slow's wallpaper.json reads as usable");
        return verdict();
    }
    gradeHeldThroughOverflow(m, history, scans, file, v2, slow, goal);
    return verdict();
}

// S15.87: the kept holds, on a ramfs of the child's own.
void overflowKeptHolds(const QString& root) {
    inNamespace("S15.87 (holds kept)", {QStringLiteral("-U"), QStringLiteral("-r"), QStringLiteral("-m")}, QStringLiteral("--overflow-kept"),
                {root + QStringLiteral("/overflow-ramfs")});
}

// S15.87: an overflow on a file system where a lease shows writers.
void overflowReprobe(const QString& root) {
    const QString dir = root + QStringLiteral("/overflow-reprobe");
    const QString slow = dir + QStringLiteral("/slow");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty() || !writeFolder(slow, wallpaperTitled("Slow")).isEmpty()) {
        fault("S15.87", QStringLiteral("the library could not be built"));
        return;
    }
    OpenCounter scans(dir);
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!scans.ok() || !readyWith(m, QStringLiteral("Good,Slow")) || !selectEdited(m, slow)) {
        expect(false, QStringLiteral("S15.87 (writers found again): the library never settled with [Good,Slow], or slow could not be selected and edited; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);
    // GOAL: S15.87 -- after an inotify queue overflow, a file some process has open for writing
    //       whose every event was lost is held: a selected, edited wallpaper whose wallpaper.json
    //       is rewritten after the queue filled keeps its row, selection and edits through the
    //       scan the overflow brings, and is published once, as the new version, after the close.
    // WHY-THIS-SHAPE: the rewrite starts only once the queue has overflowed, so no event reports it;
    //       only asking the kernel which files are open for writing, after the overflow, can. A
    //       lease showing a writer is checked for first, else the case is not verified.
    // CONTROL: app/library/folder_watcher.cpp, the overflow branch's
    //       `if (auto* tree = std::get_if<TreeWatch>(&m_watch)) { holdOpenFiles(*tree, {}); for (...) { holdOpenFiles(*tree, inside); } }`
    //       deleted -> red, "S15.87 (writers found again): after the overflow a scan published
    //       [Good,slow(broken)], selected "" ..."; restored.
    OverflowWitness witness(dir);
    if (const QString why = flood(dir); !why.isEmpty()) {
        notVerified("S15.87 (writers found again)", why.toStdString());
        return;
    }
    QFile file(slow + QStringLiteral("/wallpaper.json"));
    const QByteArray v2 = titled("Slow v2");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(v2.left(v2.size() / 2)) != v2.size() / 2 || !file.flush()) {
        fault("S15.87", QStringLiteral("slow's wallpaper.json could not be half rewritten"));
        return;
    }
    const int probe = ::open(QFile::encodeName(slow + QStringLiteral("/wallpaper.json")).constData(), O_RDONLY | O_CLOEXEC);
    const bool leaseShowsWriter = probe >= 0 && ::fcntl(probe, F_SETLEASE, F_RDLCK) != 0 && errno == EAGAIN;
    if (probe >= 0) {
        ::close(probe);
    }
    if (!witness.overflowed() || !leaseShowsWriter || readWallpaperFolder(slow).usable()) {
        notVerified("S15.87 (writers found again)", "the flood did not overflow an inotify queue, a lease here does not show a writer, or half of slow's wallpaper.json reads as usable");
        return;
    }
    gradeHeldThroughOverflow(m, history, scans, file, v2, slow, QStringLiteral("S15.87 (writers found again)"));
}

// S15.87: an overflow read while a scan runs.
void overflowDuringScan(const QString& root) {
    const QString dir = root + QStringLiteral("/overflow-scan");
    const QString a = dir + QStringLiteral("/a");
    if (!writeFolder(a, wallpaperTitled("A v1")).isEmpty() || !writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty() ||
        !writeFolder(dir + QStringLiteral("/sentinel"), wallpaperTitled("Sentinel")).isEmpty()) {
        fault("S15.87", QStringLiteral("the library could not be built"));
        return;
    }
    OpenCounter scans(dir);
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!scans.ok() || !readyWith(m, QStringLiteral("A v1,Good,Sentinel")) || !selectEdited(m, a)) {
        expect(false, QStringLiteral("S15.87 (during a scan): the library never settled with [A v1,Good,Sentinel], or a could not be selected and edited; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);
    // GOAL: S15.87 -- a scan running when the inotify queue overflows shows no item as it read
    //       it: a wallpaper whose wallpaper.json the scan read half-rewritten, the rewrite's events
    //       lost and the file closed before the result is handled, keeps its row, selection and
    //       edits; the scan the overflow brings publishes the new version.
    // WHY-THIS-SHAPE: the pool's one thread is held so the scan provably reads the half after the
    //       queue overflowed (counted: none before the release, one after); the rest is written
    //       and closed before the result is handled, so no writer is left for any check to find,
    //       and only "every item unfinished once events were lost" can hide the half. sentinel is
    //       removed in the same window, so a publish without it proves the result was handled.
    // CONTROL: app/library/folder_watcher.cpp, the overflow branch's listing loop
    //       `if (const Listing listing(::opendir(m_directory.constData()), ::closedir); listing) {...}`
    //       deleted -> red, "S15.87 (during a scan): the scan that read a half-written published
    //       [Good,a(broken)], selected "" ..."; restored.
    OverflowWitness witness(dir);
    static_cast<void>(scans.take());
    QString unproven;
    {
        PoolHold hold(kWaitMs);
        QFile file(a + QStringLiteral("/wallpaper.json"));
        const QByteArray v2 = titled("A v2");
        if (!hold.ok()) {
            fault("S15.87", QStringLiteral("the pool's thread could not be held"));
            return;
        }
        {
            m.rescan();
            if (const QString why = flood(dir); !why.isEmpty()) {
                notVerified("S15.87 (during a scan)", why.toStdString());
                return;
            }
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(v2.left(v2.size() / 2)) != v2.size() / 2 || !file.flush() ||
                !QDir(dir + QStringLiteral("/sentinel")).removeRecursively()) {
                fault("S15.87", QStringLiteral("a's wallpaper.json could not be half rewritten, or sentinel removed"));
                return;
            }
            const bool halfBroken = !readWallpaperFolder(a).usable();
            const bool overflowed = witness.overflowed();
            const int before = scans.take().scans;
            hold.release();
            const bool done = QThreadPool::globalInstance()->waitForDone(kWaitMs);
            const int read = scans.take().scans;
            if (file.write(v2.mid(v2.size() / 2)) != v2.size() - v2.size() / 2 || !file.flush()) {
                fault("S15.87", QStringLiteral("the rest of a's wallpaper.json could not be written"));
                return;
            }
            file.close();
            if (!done || before != 0 || read != 1 || !halfBroken || !overflowed) {
                unproven = QStringLiteral("the scan was not shown to read a half-written after an overflow: finished %1, scans %2 before release and %3 after, "
                                          "the half reads broken %4, overflowed %5")
                               .arg(done).arg(before).arg(read).arg(halfBroken).arg(overflowed);
            }
        }
    }
    if (!unproven.isEmpty()) {
        notVerified("S15.87 (during a scan)", unproven.toStdString());
        return;
    }
    QCoreApplication::sendPostedEvents();
    const QStringList published = history.take();
    const QString selection = selectionOf(m);
    const bool kept = keptEdited(m, a);
    if (published.isEmpty()) {
        notVerified("S15.87 (during a scan)", "the scan's result was not handled when the posted events were delivered");
        return;
    }
    const bool listed = readyWith(m, QStringLiteral("A v2,Good"));
    expect(published == QStringList{QStringLiteral("A v1,Good")} && kept,
           QStringLiteral("S15.87 (during a scan): the scan that read a half-written published [%1], %2").arg(published.join(u" | "), selection));
    expect(listed && keptEdited(m, a), QStringLiteral("S15.87 (during a scan): after the overflow the list is [%1], %2").arg(titles(m.wallpapers()), selectionOf(m)));
}

// S15.88, the child: run by switchToPolling() under `unshare -U -r`, with this namespace's watch
// limit set to exactly the watches the library needs. Arguments: which case, a folder to work in,
// the parent's namespace.
int switchChild(const QStringList& args) {
    wallpaperx::testing::g_testName = "library_changes (switch to polling)";
    if (args.size() != 3) {
        harnessFailure("S15.88: the child was given " + std::to_string(args.size()) + " argument(s), not 3");
        return verdict();
    }
    const bool midScan = args[0] == u"mid-scan";
    const QString goal = midScan ? QStringLiteral("S15.88 (mid-scan)") : QStringLiteral("S15.88 (in the drain)");
    if (const auto refused = refuseUnlessOwnNamespace(goal, args[2])) {
        return *refused;
    }
    // The library folder and one per folder inside each wallpaper: lib, calm, calm/assets, slow, slow/assets.
    constexpr int kWatchesNeeded = 5;
    if (!setOwnWatchLimit(kWatchesNeeded)) {
        notVerified(goal.toUtf8().constData(), "this namespace's max_inotify_watches could not be set");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const QString dir = args[1] + QStringLiteral("/lib");
    if (!writeFolder(dir + QStringLiteral("/calm"), wallpaperTitled("Calm")).isEmpty() || !writeFolder(dir + QStringLiteral("/slow"), wallpaperTitled("Slow")).isEmpty()) {
        fault("S15.88", QStringLiteral("the library could not be built"));
        return verdict();
    }
    std::optional<WarningLog> log(std::in_place);
    childReady();
    LibraryManager m(dir, kSettleMs, kPollMs);
    if (midScan) {
        // GOAL: S15.88 -- a watch refused for want of kernel resources while the library's first
        //       scan runs (a folder made during it) switches to polling without holding back what
        //       that scan read: it publishes every wallpaper.
        // WHY-THIS-SHAPE: the pool's one thread is held, so the folder is provably made after the
        //       scan placed its five watches and before the result is handled; the result is taken
        //       with the pool drained and posted events delivered, and polling is shown to begin
        //       there, not before. A hold that fails, or a folder not made, is this file's fault and
        //       is reported as one; only "polling began elsewhere" is not verified (test-side
        //       control: PoolHold(0) -> "FAIL (harness): S15.88: the pool's thread could not be held").
        // CONTROL: app/library/folder_watcher.cpp, pollInstead()'s `m_watch = PollWatch{seen, seen, {}};`
        //       made `m_watch = PollWatch{};` -> red, "S15.88 (mid-scan): the first scan, the switch
        //       to polling inside it, published [] (state 1)", the only line red; handle()'s
        //       `m_unfinishedDuringScan.insert(itemOf(path));` under IN_CREATE deleted -> red, "...
        //       published [Calm,Slow,new(broken)]", the folder made mid-scan shown; restored.
        bool pollingBefore = true;
        {
            PoolHold hold(kWaitMs);
            if (!hold.ok()) {
                fault("S15.88", QStringLiteral("the pool's thread could not be held"));
                return verdict();
            }
            m.rescan();
            pollingBefore = m.polling();
            if (!QDir().mkpath(dir + QStringLiteral("/new"))) {
                fault("S15.88", QStringLiteral("new could not be made"));
                return verdict();
            }
        }
        QCoreApplication::sendPostedEvents();
        const QString published = titles(m.wallpapers());
        const int state = int(m.state());
        if (pollingBefore || !m.polling()) {
            notVerified(goal.toUtf8().constData(), "the switch to polling did not happen inside the first scan's result");
            return verdict();
        }
        expect(m.state() == LibraryManager::State::Ready && published == u"Calm,Slow",
               QStringLiteral("%1: the first scan, the switch to polling inside it, published [%2] (state %3)").arg(goal, published).arg(state));
    } else {
        m.rescan();
        if (!readyWith(m, QStringLiteral("Calm,Slow")) || m.polling()) {
            expect(false, QStringLiteral("%1: the library never settled watched with [Calm,Slow]; [%2]").arg(goal, titles(m.wallpapers())));
            return verdict();
        }
        // GOAL: S15.88 -- a switch to polling made by the drain at the start of a scan logs one
        //       warning, with the kernel's reason, and the library goes on: the new folder is listed.
        // WHY-THIS-SHAPE: the folder is made and rescan() called with no event loop between, so
        //       its creation is first read by scanStarting()'s own drain, which is where the switch
        //       happens; a second warning would come from placing a watch on the descriptor the
        //       switch has just closed.
        // CONTROL: app/library/folder_watcher.cpp, scanStarting()'s `if (!polling()) {` before
        //       `placeWatch();` made `if (true) {` -> red, "S15.88 (in the drain): the switch logged
        //       [... No space left on device ... | ... Bad file descriptor ...]" (both S15.62 lines too,
        //       each switching the same way at a later scan); restored.
        if (!QDir().mkpath(dir + QStringLiteral("/new"))) {
            fault("S15.88", QStringLiteral("new could not be made"));
            return verdict();
        }
        static_cast<void>(log->lines());
        log.emplace();
        m.rescan();
        const QStringList said = log->containing(u"failed (");
        const bool listed = readyWith(m, QStringLiteral("Calm,Slow,new(broken)"));
        expect(m.polling() && said.size() == 1 && said.constFirst().contains(QLatin1String("inotify_add_watch failed (No space left on device)")) && listed,
               QStringLiteral("%1: the switch logged [%2], polling() %3, and the list is [%4]").arg(goal, said.join(u" | ")).arg(m.polling()).arg(titles(m.wallpapers())));
    }
    return verdict();
}

// S15.88: the switch to polling, mid-scan and in the drain, each in a user namespace of its own.
void switchToPolling(const QString& root) {
    const QByteArray hostBefore = hostWatchLimit();
    for (const char* which : {"mid-scan", "in-drain"}) {
        inNamespace(std::string("S15.88 (") + which + ")", {QStringLiteral("-U"), QStringLiteral("-r")}, QStringLiteral("--switch"),
                    {QLatin1String(which), root + QStringLiteral("/switch-") + QLatin1String(which)});
    }
    if (hostWatchLimit() != hostBefore) {
        harnessFailure("S15.88: THE HOST'S max_inotify_watches changed from " + hostBefore.toStdString() + " to " + hostWatchLimit().toStdString());
    }
}

// S15.62: the kernel refusing a watch for want of resources.
void watchLimit(const QString& root) {
    // GOAL: S15.62 -- when the kernel refuses a watch for want of resources (the per-user watch
    //       limit), the library logs exactly one warning, polls from then on, and still lists a
    //       wallpaper added later and re-reads a wallpaper.json rewritten later: with no watch to be
    //       had (limit 0), and with the library's watch placed but a wallpaper folder's refused
    //       (limit 1).
    // WHY-THIS-SHAPE: a real ENOSPC, made by lowering the limit in a user namespace of the child's
    //       own (unshare -U -r), whose max_inotify_watches is that namespace's; the host's value is
    //       read before and after, and the child refuses to write unless its namespace differs from
    //       this process's. Its verdicts are relayed line by line.
    // CONTROL: app/library/folder_watcher.cpp, missingPath() made `return true;` -> red, "S15.62 (a
    //       limit of 0 watch(es)): the library reports polling() 0 after 0 warning(s)" and the same
    //       for a limit of 1; watchTree()'s `pollInstead("inotify_add_watch", error);` deleted -> red,
    //       "S15.62 (a limit of 1 watch(es)): the library reports polling() 0 ..." and not for a limit
    //       of 0 (each also turns S15.88 (in the drain) red, which switches the same way);
    //       scanStarting()'s `if (!polling()) {` made `if (true) {` -> red, both limits, "... after 1
    //       warning(s) about it: [... No space left on device ... | ... Bad file descriptor ...]"; restored.
    const QByteArray hostBefore = hostWatchLimit();
    for (const int limit : {0, 1}) {
        inNamespace("S15.62 (a limit of " + std::to_string(limit) + " watch(es))", {QStringLiteral("-U"), QStringLiteral("-r")},
                    QStringLiteral("--watch-limit"), {QString::number(limit), root + QStringLiteral("/watch-limit-%1").arg(limit)});
    }
    if (hostWatchLimit() != hostBefore) {
        harnessFailure("S15.62: THE HOST'S max_inotify_watches changed from " + hostBefore.toStdString() + " to " + hostWatchLimit().toStdString());
    }
}

// S15.74: a file system where a lease says nothing about writers.
void untrustedFileSystem(const QString& root) {
    inNamespace("S15.74", {QStringLiteral("-U"), QStringLiteral("-r"), QStringLiteral("-m")}, QStringLiteral("--untrusted-fs"),
                {root + QStringLiteral("/ramfs")});
}

// S15.63: a scan is never discarded, so changes arriving while scans run cannot starve the list.
void steadyChanges(const QString& root) {
    constexpr int settle = 20;
    const QString dir = root + QStringLiteral("/steady");
    const QString staging = root + QStringLiteral("/steady-staging");
    // Flat, so each folder costs the watcher one watch.
    const Entries small{{QStringLiteral("wallpaper.json"), [] {
                             OrderedJson json = planManifest();
                             json["title"] = "Steady";
                             json["file"] = "video.mp4";
                             return jsonBytes(json);
                         }()},
                        {QStringLiteral("preview.png"), pngBytes(16, 9)},
                        {QStringLiteral("video.mp4"), QByteArrayLiteral("not really a video")}};
    if (!QDir().mkpath(dir) || !QDir().mkpath(staging)) {
        fault("S15.63", QStringLiteral("the library could not be made"));
        return;
    }
    // A library big enough that one scan outlasts several quiet periods, doubled until it does.
    int count = 0;
    qint64 scanMs = 0;
    for (int round = 0; round < 5 && scanMs < 6 * settle; ++round) {
        for (const int until = count == 0 ? 400 : 2 * count; count < until; ++count) {
            if (!writeFolder(dir + QStringLiteral("/p%1").arg(count, 5, 10, QLatin1Char('0')), small).isEmpty()) {
                fault("S15.63", QStringLiteral("wallpaper %1 could not be written").arg(count));
                return;
            }
        }
        QElapsedTimer timer;
        timer.start();
        const auto scan = scanLibrary(dir);
        scanMs = timer.elapsed();
        if (!scan || scan->size() != count) {
            fault("S15.63", QStringLiteral("the library of %1 wallpapers did not scan whole").arg(count));
            return;
        }
    }
    if (scanMs < 6 * settle) {
        notVerified("S15.63", "a scan of " + std::to_string(count) + " wallpapers takes only " + std::to_string(scanMs) +
                                  " ms, too quick for changes to keep arriving while one runs");
        return;
    }
    LibraryManager m(dir, settle);
    m.rescan();
    if (!waitUntil([&] { return m.state() == LibraryManager::State::Ready && m.wallpapers().size() == count; }, 10 * kWaitMs)) {
        expect(false, QStringLiteral("S15.63: the library of %1 wallpapers never settled; %2 listed").arg(count).arg(m.wallpapers().size()));
        return;
    }
    int published = 0;
    const auto counting = QObject::connect(&m, &LibraryManager::wallpapersChanged, [&] { ++published; });

    // GOAL: S15.63 -- while wallpapers keep arriving, each after a quiet period but before a scan
    //       of the library could finish, lists are still published: a scan running when a change
    //       asks for another is finished and published, never thrown away for the newer one.
    // WHY-THIS-SHAPE: the gap between arrivals is longer than the quiet period, so each asks for a
    //       scan, and shorter than a scan of this library (measured, and the library doubled until
    //       one takes six quiet periods), so every scan has a request arrive while it runs. Arrivals
    //       go on until a list is published or the bound passes, so a slow machine only makes it
    //       take longer; reaching the bound is the failure.
    // CONTROL: app/library/library_manager.cpp, rescan()'s continuation given, first, `if (m_scanning
    //       == Scanning::YesThenAgain) { m_scanning = Scanning::No; rescan(); return; }` -- a scan asked
    //       for again is thrown away and started afresh -> red, "S15.63: no list was published while a
    //       wallpaper arrived every N ms for M ms"; restored.
    const qint64 gap = settle + (scanMs - settle) / 3;
    const qint64 bound = std::max<qint64>(40 * scanMs, kWaitMs);
    QElapsedTimer clock;
    clock.start();
    int arrived = 0;
    while (published == 0 && clock.elapsed() < bound) {
        const QString name = QStringLiteral("n%1").arg(arrived, 5, 10, QLatin1Char('0'));
        if (!moveInFolder(staging, dir, name, small).isEmpty()) {
            fault("S15.63", QStringLiteral("a wallpaper could not be moved in"));
            QObject::disconnect(counting);
            return;
        }
        ++arrived;
        pump(int(gap));
    }
    const qint64 lasted = clock.elapsed();
    QObject::disconnect(counting);
    expect(published > 0, QStringLiteral("S15.63: no list was published while a wallpaper arrived every %1 ms for %2 ms (%3 arrivals, one scan "
                                         "of the library taking %4 ms)")
                              .arg(gap).arg(lasted).arg(arrived).arg(scanMs));
    const bool whole = waitUntil([&] { return m.wallpapers().size() == count + arrived; }, 10 * kWaitMs);
    expect(whole, QStringLiteral("S15.63: after the arrivals stopped the list holds %1 of %2").arg(m.wallpapers().size()).arg(count + arrived));
}

// S15.64 (decision A): an emptied wallpaper.json a writer holds keeps its row.
void emptiedManifest(const QString& root) {
    const QString base = root + QStringLiteral("/emptied-held");
    const QString dir = base + QStringLiteral("/lib");
    const QString a = dir + QStringLiteral("/a");
    if (!writeFolder(a, wallpaperTitled("A v1")).isEmpty() || !writeFolder(dir + QStringLiteral("/b"), wallpaperTitled("B")).isEmpty() ||
        !writeFolder(base + QStringLiteral("/c"), wallpaperTitled("C")).isEmpty()) {
        fault("S15.64", QStringLiteral("the library could not be built"));
        return;
    }
    LibraryManager m(dir, kSettleMs);
    m.rescan();
    if (!readyWith(m, QStringLiteral("A v1,B")) || !selectEdited(m, a)) {
        expect(false, QStringLiteral("S15.64: the library never settled, or a could not be selected and edited; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);
    // GOAL: S15.64 -- a listed, selected and edited wallpaper whose wallpaper.json is opened with
    //       O_TRUNC and held open empty for longer than the quiet period keeps its row, the
    //       selection and the edits, even across a scan another change forces; refilled and
    //       closed, it is published once as the new version, still selected and edited.
    // WHY-THIS-SHAPE: c moved in while wallpaper.json is empty forces a scan that finds it empty,
    //       which only the writer's hold can keep from listing the wallpaper broken.
    // CONTROL: app/library/folder_watcher.cpp, handle()'s `hold(*tree, path);` (its IN_MODIFY
    //       branch) deleted -> red, "S15.64: while a's wallpaper.json was held open empty a scan
    //       published [B,C,a(broken)], selected "" with speed "; restored.
    QFile file(a + QStringLiteral("/wallpaper.json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fault("S15.64", QStringLiteral("a's wallpaper.json could not be opened empty"));
        return;
    }
    pump(10 * kSettleMs);
    if (!QDir().rename(base + QStringLiteral("/c"), dir + QStringLiteral("/c"))) {
        fault("S15.64", QStringLiteral("c could not be moved in"));
        return;
    }
    const bool triggered = waitUntil([&] { return rowTitled(m.wallpapers(), QStringLiteral("C")) != nullptr; }, kWaitMs);
    const QString during = titles(m.wallpapers());
    const bool keptDuring = keptEdited(m, a);
    const QString selectionDuring = selectionOf(m);
    if (!writeSlowly(file, titled("A v2"), 4, kWriteGapMs)) {
        fault("S15.64", QStringLiteral("a's wallpaper.json could not be refilled"));
        return;
    }
    file.close();
    const bool listed = readyWith(m, QStringLiteral("A v2,B,C"));
    pump(4 * kSettleMs);
    const QStringList published = history.take();
    if (!triggered) {
        expect(false, QStringLiteral("S15.64: c moved in while a's wallpaper.json was held open empty was never listed; [%1]").arg(during));
    } else {
        expect(during == u"A v1,B,C" && keptDuring,
               QStringLiteral("S15.64: while a's wallpaper.json was held open empty a scan published [%1], %2").arg(during, selectionDuring));
        expect(listed && published == QStringList{QStringLiteral("A v1,B,C"), QStringLiteral("A v2,B,C")} && keptEdited(m, a),
               QStringLiteral("S15.64: refilling a's wallpaper.json published [%1] and ended %2").arg(published.join(QStringLiteral(" | ")), selectionOf(m)));
    }
}

// S15.65 (decision D): a hold with no write for `holdMs`, and no file open, lapses.
void holdExpiry(const QString& root) {
    constexpr int hold = 300;
    struct Case {
        const char* goal;
        bool toNothing;
    };
    // GOAL: S15.65 -- a listed, selected and edited wallpaper whose wallpaper.json is cut short by
    //       truncate(2), with no file open and so no close to come, keeps its row, selection and
    //       edits while the hold lasts, and is read again once the hold passes with no further
    //       write: truncated to nothing or to half, it is listed broken, and the selection and
    //       edits go.
    // WHY-THIS-SHAPE: a short hold (300 ms) is injected through the constructor. The early look is
    //       taken a third of a hold after the truncate and graded only if the clock shows it really
    //       came inside the hold; a stall past it is not verified, since a correct manager may then
    //       have read the file already. Nothing has the file open, so the check for a writer at the
    //       lapse must find none.
    // CONTROL: app/library/folder_watcher.cpp, holdFromNow()'s `m_holdExpiry.start(m_hold);` deleted
    //       -> red, both, "S15.65 (truncated to nothing): after the 300 ms hold the list is [A,B]";
    //       releaseHolds()'s `m_settle.start();` deleted -> red, the same two; handle()'s IN_MODIFY
    //       branch made to return true without holding -> red, "S15.65 (truncated to nothing): 100
    //       ms after the truncate, within the 300 ms hold, the list was [B,a(broken)]" and the same
    //       for half; restored.
    const Case cases[] = {
        {"S15.65 (truncated to nothing)", true},
        {"S15.65 (truncated to half)", false},
    };
    int index = 0;
    for (const Case& c : cases) {
        const QString dir = root + QStringLiteral("/expiry-%1").arg(index++);
        const QString shown = dir + QStringLiteral("/a");
        const QString manifest = shown + QStringLiteral("/wallpaper.json");
        if (!writeFolder(shown, wallpaperTitled("A")).isEmpty() || !writeFolder(dir + QStringLiteral("/b"), wallpaperTitled("B")).isEmpty()) {
            fault(c.goal, QStringLiteral("the library could not be built"));
            continue;
        }
        LibraryManager m(dir, kSettleMs, kPollMs, LibraryManager::Watching::Inotify, hold);
        m.rescan();
        if (!readyWith(m, QStringLiteral("A,B")) || !selectEdited(m, shown)) {
            expect(false, QStringLiteral("%1: the library never settled, or a could not be selected and edited; [%2]")
                              .arg(QLatin1String(c.goal), titles(m.wallpapers())));
            continue;
        }
        const qint64 size = c.toNothing ? 0 : QFileInfo(manifest).size() / 2;
        QElapsedTimer clock;
        clock.start();
        if (::truncate(QFile::encodeName(manifest).constData(), size) != 0) {
            fault(c.goal, QStringLiteral("wallpaper.json could not be truncated"));
            continue;
        }
        pump(hold / 3);
        const qint64 earlyAt = clock.elapsed();
        const QString early = titles(m.wallpapers());
        const bool keptEarly = keptEdited(m, shown);
        const QString selectionEarly = selectionOf(m);
        const bool read = waitUntil([&] { return titles(m.wallpapers()) == u"B,a(broken)"; }, kWaitMs);
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

// S15.66 (decision D): a writer that keeps writing is never cut short.
void longCopy(const QString& root) {
    constexpr int hold = 300;
    constexpr int gap = 100;
    constexpr int pieces = 8;
    const QString dir = root + QStringLiteral("/long-copy");
    if (!writeFolder(dir + QStringLiteral("/good"), wallpaperTitled("Good")).isEmpty()) {
        fault("S15.66", QStringLiteral("the library could not be built"));
        return;
    }
    LibraryManager m(dir, kSettleMs, kPollMs, LibraryManager::Watching::Inotify, hold);
    m.rescan();
    if (!readyWith(m, QStringLiteral("Good"))) {
        expect(false, QStringLiteral("S15.66: the library never settled with [Good]; [%1]").arg(titles(m.wallpapers())));
        return;
    }
    History history(m);
    // GOAL: S15.66 -- a folder copied in whose asset is written in pieces a third of a hold apart,
    //       the copy lasting more than two holds, is published once, whole, after the close: a
    //       writer that keeps writing is never read half-way.
    // WHY-THIS-SHAPE: the hold is injected short (300 ms) so the copy (seven gaps of 100 ms)
    //       outlasts it twice. Two things keep it held -- each write restarts the hold, and a hold
    //       that lapses on a file still open is renewed -- so only breaking both is red. If this
    //       test itself stalled between two writes for two thirds of a hold, a correct manager may
    //       read the half, so the run is not verified rather than graded.
    // CONTROL: app/library/folder_watcher.cpp, hold()'s `tree.writing.insert(path, holdFromNow());`
    //       made `tree.writing.tryEmplace(path, holdFromNow());`, so only the first write sets the
    //       deadline, together with releaseHolds()'s `if (!openForWriting(AT_FDCWD, ...)) {` made
    //       `if (true) {`, one patch -> red, "S15.66: a copy writing every 100 ms for 701 ms, with a
    //       300 ms hold, published [Good,Long@4000 | Good,Long@8000]" (and S15.71, which the second
    //       half turns red alone); the first half alone left everything green; restored.
    Copy copy(dir + QStringLiteral("/long"), wallpaperTitled("Long", 8000), 1000);
    if (!copy.ok()) {
        fault("S15.66", QStringLiteral("the copy could not be started"));
        return;
    }
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    qint64 widestGap = 0;
    for (int i = 1; i < pieces; ++i) {
        pump(gap);
        if (!copy.write(1000)) {
            fault("S15.66", QStringLiteral("a write to long failed"));
            return;
        }
        widestGap = std::max(widestGap, clock.elapsed() - last);
        last = clock.elapsed();
    }
    const qint64 lasted = clock.elapsed();
    copy.finish();
    const bool listed = readyWith(m, QStringLiteral("Good,Long"));
    pump(4 * kSettleMs);
    const QString published = withAssetSizes(history.takeLists(), QStringLiteral("Long"));
    if (widestGap >= 2 * hold / 3) {
        notVerified("S15.66", "this test stalled " + std::to_string(widestGap) + " ms between two writes, two thirds of the hold");
    } else {
        expect(listed && published == u"Good,Long@8000",
               QStringLiteral("S15.66: a copy writing every %1 ms for %2 ms, with a %3 ms hold, published [%4]").arg(gap).arg(lasted).arg(hold).arg(published));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    // Only the namespace phases (inNamespace) run these, under unshare, with the XDG folders
    // they already isolated.
    if (argc > 1 && std::strcmp(argv[1], "--watch-limit") == 0) {
        QCoreApplication app(argc, argv);
        return watchLimitChild(QCoreApplication::arguments().mid(2));
    }
    if (argc > 1 && std::strcmp(argv[1], "--untrusted-fs") == 0) {
        QCoreApplication app(argc, argv);
        return untrustedChild(QCoreApplication::arguments().mid(2));
    }
    if (argc > 1 && std::strcmp(argv[1], "--overflow-kept") == 0) {
        QCoreApplication app(argc, argv);
        return overflowKeptChild(QCoreApplication::arguments().mid(2));
    }
    if (argc > 1 && std::strcmp(argv[1], "--switch") == 0) {
        QCoreApplication app(argc, argv);
        return switchChild(QCoreApplication::arguments().mid(2));
    }
    wallpaperx::testing::g_testName = "library_changes";
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified("S15.52, S15.56, S15.59-S15.66, S15.70-S15.74, S15.86-S15.88", "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    QCoreApplication app(argc, argv);

    const QString root = scratch.path();
    runPhase("modelRows", modelRows);
    runPhase("copiedFolders", [&] { copiedFolders(root); });
    runPhase("startedMidCopy", [&] { startedMidCopy(root); });
    runPhase("pausedPastTheHold", [&] { pausedPastTheHold(root); });
    runPhase("closedBeforeRescan", [&] { closedBeforeRescan(root); });
    runPhase("createdDuringScan", [&] { createdDuringScan(root); });
    runPhase("closedDuringScan", [&] { closedDuringScan(root); });
    runPhase("polling", [&] { polling(root); });
    runPhase("pollingOpenFiles", [&] { pollingOpenFiles(root); });
    runPhase("inotifyUnavailable", [&] { inotifyUnavailable(root); });
    runPhase("rewrittenInPlace", [&] { rewrittenInPlace(root); });
    runPhase("rewrittenWhilePolled", [&] { rewrittenWhilePolled(root); });
    runPhase("subfolderLater", [&] { subfolderLater(root); });
    runPhase("filledAfterCreation", [&] { filledAfterCreation(root); });
    runPhase("linkedIn", [&] { linkedIn(root); });
    runPhase("watchLimit", [&] { watchLimit(root); });
    runPhase("untrustedFileSystem", [&] { untrustedFileSystem(root); });
    runPhase("overflowKeptHolds", [&] { overflowKeptHolds(root); });
    runPhase("overflowReprobe", [&] { overflowReprobe(root); });
    runPhase("overflowDuringScan", [&] { overflowDuringScan(root); });
    runPhase("switchToPolling", [&] { switchToPolling(root); });
    runPhase("steadyChanges", [&] { steadyChanges(root); });
    runPhase("emptiedManifest", [&] { emptiedManifest(root); });
    runPhase("holdExpiry", [&] { holdExpiry(root); });
    runPhase("longCopy", [&] { longCopy(root); });
    return verdict();
}
