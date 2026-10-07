// SPDX-License-Identifier: GPL-2.0-or-later

#include "folder_watcher.h"

#include "unique_fd.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <optional>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace wallpaperx {

namespace {

// handle() takes any event on these masks as a change. Never add IN_OPEN, IN_ACCESS or
// IN_CLOSE_NOWRITE: listing a folder raises them, and handle() lists folders, so it would loop
// forever on the GUI thread.
constexpr uint32_t kFolderEvents = IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_MODIFY
                                   | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
// The folder above reports these folders' removal and moves, so they need no self events.
constexpr uint32_t kInsideEvents = IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_MODIFY
                                   | IN_ATTRIB | IN_ONLYDIR | IN_DONT_FOLLOW;
// IN_ATTRIB: a child made readable is reported to its parent, and a chmod leaves no other trace.
constexpr uint32_t kAncestorEvents = IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_ATTRIB | IN_DELETE_SELF
                                     | IN_MOVE_SELF | IN_ONLYDIR;
// IN_MASK_ADD: should the target have become the folder itself since it was stat'ed, the folder's
// watch keeps its events.
constexpr uint32_t kTargetEvents = IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_MASK_ADD;

using Listing = std::unique_ptr<DIR, int (*)(DIR*)>;

// Whether a watch failed for the path itself, so the nearest ancestor that can be watched waits
// for it; anything else is the kernel refusing another watch.
bool missingPath(int error)
{
    return error == ENOENT || error == ENOTDIR || error == EACCES || error == ELOOP || error == ENAMETOOLONG;
}

// -1 for no watch.
template <typename Watch>
int descriptorOf(const Watch& watch)
{
    return std::visit([](const auto& w) {
        if constexpr (requires { w.wd; }) {
            return w.wd;
        } else {
            return -1;
        }
    }, watch);
}

QByteArray itemOf(const QByteArray& path)
{
    const qsizetype slash = path.indexOf('/');
    return slash < 0 ? path : path.left(slash);
}

bool isAtOrUnder(const QByteArray& path, const QByteArray& top)
{
    return path.startsWith(top) && (path.size() == top.size() || path.at(top.size()) == '/');
}

bool isDirectory(int dirFd, const dirent& entry)
{
    if (entry.d_type != DT_UNKNOWN) {
        return entry.d_type == DT_DIR;
    }
    struct stat st {};
    return ::fstatat(dirFd, entry.d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
}

// Where a refused read lease means a writer. Network file systems refuse one with EAGAIN when they
// hold no delegation or oplock, which would read as a writer that never goes away.
bool leasesShowWriters(const struct statfs& fs)
{
    switch (fs.f_type) {
    case EXT4_SUPER_MAGIC:
    case BTRFS_SUPER_MAGIC:
    case XFS_SUPER_MAGIC:
    case F2FS_SUPER_MAGIC:
    case TMPFS_MAGIC:
        return true;
    default:
        return false;
    }
}

} // namespace

bool FolderWatcher::openForWriting(int dirFd, const char* name) const
{
    // A package is followed through a link, as the importer reads it; a link in a wallpaper folder
    // is its own entry.
    const auto fd = openRegularFile(dirFd, name, O_RDONLY | (m_items == Items::Files ? 0 : O_NOFOLLOW));
    struct statfs fs {};
    if (!fd || ::fstatfs(fd->get(), &fs) != 0 || !leasesShowWriters(fs)) {
        return false;
    }
    // A writer opening the file while the lease is held breaks it, which signals this process:
    // SIGURG, ignored by default, instead of SIGIO, which would end it.
    if (::fcntl(fd->get(), F_SETSIG, SIGURG) != 0) {
        return false;
    }
    if (::fcntl(fd->get(), F_SETLEASE, F_RDLCK) == 0) {
        ::fcntl(fd->get(), F_SETLEASE, F_UNLCK);
        return false;
    }
    return errno == EAGAIN;
}

FolderWatcher::FolderWatcher(QString directory, Items items, int settleMs, int pollMs, Watching watching, int holdMs,
                             QObject* parent)
    : QObject(parent)
    , m_directory(encodeFileName(directory))
    , m_items(items)
    , m_hold(holdMs)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(settleMs);
    connect(&m_settle, &QTimer::timeout, this, &FolderWatcher::scanDue);
    m_poll.setInterval(pollMs);
    connect(&m_poll, &QTimer::timeout, this, &FolderWatcher::poll);
    m_holdExpiry.setSingleShot(true);
    connect(&m_holdExpiry, &QTimer::timeout, this, &FolderWatcher::releaseHolds);

    if (watching == Watching::Poll) {
        m_watch = PollWatch{};
        m_poll.start();
        return;
    }
    m_inotify = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_inotify < 0) {
        pollInstead("inotify_init1", errno);
        return;
    }
    m_inotifyReady = std::make_unique<QSocketNotifier>(m_inotify, QSocketNotifier::Read, this);
    connect(m_inotifyReady.get(), &QSocketNotifier::activated, this, &FolderWatcher::readWatchEvents);
}

FolderWatcher::~FolderWatcher()
{
    // The notifier first: it must not outlive the descriptor it polls.
    m_inotifyReady.reset();
    if (m_inotify >= 0) {
        ::close(m_inotify);
    }
}

void FolderWatcher::pollInstead(const char* failed, int error)
{
    qWarning("wallpaperx: %s failed (%s), so %s is polled every %d ms", failed, std::strerror(error),
             m_directory.constData(), m_poll.interval());
    // Later, not now: this can run inside the notifier's own signal.
    if (m_inotifyReady) {
        m_inotifyReady->setEnabled(false);
        m_inotifyReady.release()->deleteLater();
    }
    if (m_inotify >= 0) {
        ::close(m_inotify);
    }
    m_inotify = -1;
    // This can happen mid-scan, and scanFinished() compares against `scanned`.
    const Observation seen = observe();
    m_watch = PollWatch{seen, seen, {}};
    m_poll.start();
}

bool FolderWatcher::isItemName(const QByteArray& name) const
{
    return m_items == Items::Files ? isPackageFileName(name) : !name.startsWith('.');
}

void FolderWatcher::scanStarting()
{
    // Queued events first: a write still unread would let the scan show a file being written. The
    // drain can switch to polling, so polling() is asked after it.
    readWatchEvents();
    if (!polling()) {
        placeWatch();
    }
    // Before the listing, so nothing that happens to the folder while it runs goes unreported.
    if (auto* polled = std::get_if<PollWatch>(&m_watch)) {
        polled->scanned = observe();
    }
    // After the drain, so a change it found does not bring a scan after the one about to run.
    m_settle.stop();
    m_unfinishedDuringScan = unfinished();
    m_scanned = true;
}

QSet<QByteArray> FolderWatcher::scanFinished()
{
    // The scan may have read a file whose write is still queued. A file that finished meanwhile
    // is held back too: it has a scan of its own coming.
    readWatchEvents();
    // Polling sees no write: an item that changed while the scan ran is held back until a poll
    // finds it quiet and a scan is due again.
    if (auto* polled = std::get_if<PollWatch>(&m_watch)) {
        const Observation now = observe();
        for (const Observation::Entry& entry : std::as_const(polled->scanned.entries)) {
            if (!now.entries.contains(entry)) {
                m_unfinishedDuringScan.insert(itemOf(entry.name));
            }
        }
        polled->open.clear();
        for (const Observation::Entry& entry : now.entries) {
            if (!polled->scanned.entries.contains(entry)) {
                m_unfinishedDuringScan.insert(itemOf(entry.name));
            }
            // A writer that has paused for longer than a poll.
            if (S_ISREG(entry.mode) && openForWriting(AT_FDCWD, (m_directory + '/' + entry.name).constData())) {
                polled->open.insert(entry.name);
                m_unfinishedDuringScan.insert(itemOf(entry.name));
            }
        }
    }
    return m_unfinishedDuringScan;
}

void FolderWatcher::placeWatch()
{
    QByteArray at = m_directory;
    QByteArray awaited;
    Watch next;
    for (;;) {
        const int wd = ::inotify_add_watch(m_inotify, at.constData(), awaited.isEmpty() ? kFolderEvents : kAncestorEvents);
        if (wd >= 0) {
            next = awaited.isEmpty() ? Watch(TreeWatch{wd, {}, {}, {}}) : Watch(AncestorWatch{wd, awaited});
            break;
        }
        if (const int error = errno; !missingPath(error)) {
            pollInstead("inotify_add_watch", error);
            return;
        }
        if (at == "/") {
            break;
        }
        const qsizetype slash = at.lastIndexOf('/');
        awaited = at.mid(slash + 1);
        at = slash > 0 ? at.left(slash) : QByteArrayLiteral("/");
    }

    const int previous = descriptorOf(m_watch);
    bool placed = false;
    if (previous == descriptorOf(next) && m_watch.index() == next.index()) {
        if (auto* ancestor = std::get_if<AncestorWatch>(&m_watch)) {
            ancestor->awaited = std::get<AncestorWatch>(next).awaited;
        }
    } else {
        placed = true;
        dropInsideWatches();
        // Adding a path already watched returns its descriptor, so only a different one is removed.
        if (previous >= 0 && previous != descriptorOf(next)) {
            ::inotify_rm_watch(m_inotify, previous);
        }
        m_watch = std::move(next);
    }

    // Walked again from disk each scan: a folder whose events were lost is still found.
    auto* tree = std::get_if<TreeWatch>(&m_watch);
    if (tree == nullptr) {
        return;
    }
    if (m_items == Items::Files) {
        if (placed) {
            holdOpenFiles(*tree, {});
        }
        watchTargets(*tree);
        return;
    }
    QSet<int> reached;
    if (!watchTree(*tree, {}, &reached)) {
        return;
    }
    tree->inside.removeIf([&](const auto& watched) {
        if (reached.contains(watched.key())) {
            return false;
        }
        ::inotify_rm_watch(m_inotify, watched.key());
        return true;
    });
}

bool FolderWatcher::watchTree(TreeWatch& tree, const QByteArray& relative, QSet<int>* reached)
{
    QList<QByteArray> pending{relative};
    while (!pending.isEmpty()) {
        const QByteArray at = pending.takeLast();
        const QByteArray path = at.isEmpty() ? m_directory : m_directory + '/' + at;
        if (!at.isEmpty()) {
            const int wd = ::inotify_add_watch(m_inotify, path.constData(), kInsideEvents);
            if (wd < 0) {
                // Gone, or not a folder after all: the folder above reports what became of it.
                if (const int error = errno; !missingPath(error)) {
                    pollInstead("inotify_add_watch", error);
                    return false;
                }
                continue;
            }
            // Adding a watch already there returns its descriptor.
            const bool fresh = !tree.inside.contains(wd);
            tree.inside.insert(wd, at);
            if (reached != nullptr) {
                reached->insert(wd);
            }
            if (fresh) {
                holdOpenFiles(tree, at);
            }
        }
        // Listed after the watch is placed, so a folder made meanwhile is listed or reported.
        const Listing listing(::opendir(path.constData()), ::closedir);
        if (!listing) {
            continue;
        }
        while (const dirent* entry = ::readdir(listing.get())) {
            const QByteArray name(entry->d_name);
            if (name.startsWith('.') || !isDirectory(::dirfd(listing.get()), *entry)) {
                continue;
            }
            pending.append(at.isEmpty() ? name : at + '/' + name);
        }
    }
    return true;
}

void FolderWatcher::holdOpenFiles(TreeWatch& tree, const QByteArray& relative)
{
    const QByteArray path = relative.isEmpty() ? m_directory : m_directory + '/' + relative;
    const Listing listing(::opendir(path.constData()), ::closedir);
    if (!listing) {
        return;
    }
    while (const dirent* entry = ::readdir(listing.get())) {
        const QByteArray name(entry->d_name);
        if (name.startsWith('.') || (relative.isEmpty() && !isItemName(name))) {
            continue;
        }
        if (openForWriting(::dirfd(listing.get()), entry->d_name)) {
            hold(tree, relative.isEmpty() ? name : relative + '/' + name);
        }
    }
}

void FolderWatcher::watchTargets(TreeWatch& tree)
{
    QHash<int, QList<QByteArray>> targets;
    if (const Listing listing(::opendir(m_directory.constData()), ::closedir); listing) {
        const int dirFd = ::dirfd(listing.get());
        while (const dirent* entry = ::readdir(listing.get())) {
            const QByteArray name(entry->d_name);
            struct stat st {};
            if (name.startsWith('.') || !isItemName(name) || (entry->d_type != DT_LNK && entry->d_type != DT_UNKNOWN)
                || ::fstatat(dirFd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISLNK(st.st_mode)
                || ::fstatat(dirFd, entry->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            // Links to one file share its watch. One that cannot be placed, even for want of kernel
            // resources, does not switch the folder to polling: the probe below still holds it.
            const int wd = ::inotify_add_watch(m_inotify, (m_directory + '/' + name).constData(), kTargetEvents);
            if (wd >= 0 && wd != tree.wd) {
                targets[wd].append(name);
            }
            // Every scan, not only when first watched: a write begun before the watch, or with no
            // watch at all, is seen only this way.
            if (openForWriting(dirFd, entry->d_name)) {
                hold(tree, name);
            }
        }
    }
    for (auto it = tree.targets.keyBegin(); it != tree.targets.keyEnd(); ++it) {
        if (!targets.contains(*it)) {
            ::inotify_rm_watch(m_inotify, *it);
        }
    }
    tree.targets = std::move(targets);
}

void FolderWatcher::hold(TreeWatch& tree, const QByteArray& path)
{
    tree.writing.insert(path, holdFromNow());
    m_unfinishedDuringScan.insert(itemOf(path));
}

void FolderWatcher::dropInsideWatches()
{
    if (const auto* tree = std::get_if<TreeWatch>(&m_watch)) {
        for (auto it = tree->inside.keyBegin(); it != tree->inside.keyEnd(); ++it) {
            ::inotify_rm_watch(m_inotify, *it);
        }
        for (auto it = tree->targets.keyBegin(); it != tree->targets.keyEnd(); ++it) {
            ::inotify_rm_watch(m_inotify, *it);
        }
    }
}

void FolderWatcher::unwatchTree(TreeWatch& tree, const QByteArray& relative)
{
    tree.inside.removeIf([&](const auto& watched) {
        if (!isAtOrUnder(watched.value(), relative)) {
            return false;
        }
        ::inotify_rm_watch(m_inotify, watched.key());
        return true;
    });
}

void FolderWatcher::readWatchEvents()
{
    alignas(inotify_event) char buffer[16 * 1024];
    bool changed = false;
    // m_inotify is -1 once handle() has switched to polling.
    while (m_inotify >= 0) {
        const ssize_t got = ::read(m_inotify, buffer, sizeof buffer);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        for (const char* p = buffer; p < buffer + got;) {
            const auto* event = reinterpret_cast<const inotify_event*>(p);
            changed = handle(*event) || changed;
            p += sizeof(inotify_event) + event->len;
        }
    }
    // Restarted by every change, so a burst of them costs one scan after the last.
    if (changed) {
        m_settle.start();
    }
}

bool FolderWatcher::handle(const inotify_event& event)
{
    if ((event.mask & IN_Q_OVERFLOW) != 0) {
        // Lost events may have been writes, closes or creations. A hold still ends by its close or
        // lapse, a file still open for writing is held, and a scan running now counts every item
        // unfinished; the scan this schedules lists them.
        if (auto* tree = std::get_if<TreeWatch>(&m_watch)) {
            holdOpenFiles(*tree, {});
            for (const QByteArray& inside : std::as_const(tree->inside)) {
                holdOpenFiles(*tree, inside);
            }
        }
        if (const Listing listing(::opendir(m_directory.constData()), ::closedir); listing) {
            while (const dirent* entry = ::readdir(listing.get())) {
                if (const QByteArray name(entry->d_name); isItemName(name)) {
                    m_unfinishedDuringScan.insert(name);
                }
            }
        }
        return true;
    }
    const auto dropWatch = [this](int wd, uint32_t mask) {
        // Its path now names something else or nothing; the next scan places the watch again.
        if ((mask & IN_IGNORED) == 0) {
            ::inotify_rm_watch(m_inotify, wd);
        }
        dropInsideWatches();
        m_watch = std::monostate{};
    };
    constexpr uint32_t kGone = IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT;

    if (const auto* ancestor = std::get_if<AncestorWatch>(&m_watch)) {
        if (event.wd != ancestor->wd) {
            return false;
        }
        if (event.len == 0) {
            if ((event.mask & kGone) != 0) {
                dropWatch(event.wd, event.mask);
            }
            return true;
        }
        return QByteArray(event.name) == ancestor->awaited;
    }
    auto* tree = std::get_if<TreeWatch>(&m_watch);
    if (tree == nullptr) {
        return false;
    }
    QByteArray dir;
    if (event.wd == tree->wd) {
        if (event.len == 0) {
            if ((event.mask & kGone) != 0) {
                dropWatch(event.wd, event.mask);
            }
            return true;
        }
    } else if (const auto target = tree->targets.constFind(event.wd); target != tree->targets.cend()) {
        const QList<QByteArray> names = *target;
        if ((event.mask & IN_MODIFY) != 0) {
            for (const QByteArray& name : names) {
                hold(*tree, name);
            }
            return false;
        }
        if ((event.mask & IN_ATTRIB) != 0) {
            return std::ranges::any_of(names, [tree](const QByteArray& name) { return !tree->writing.contains(name); });
        }
        // Closed after writing, or no longer the file the links name.
        if ((event.mask & IN_IGNORED) != 0) {
            tree->targets.remove(event.wd);
        }
        for (const QByteArray& name : names) {
            tree->writing.remove(name);
        }
        return true;
    } else if (const auto inside = tree->inside.constFind(event.wd); inside != tree->inside.cend()) {
        if ((event.mask & IN_IGNORED) != 0) {
            tree->inside.erase(inside);
            return false;
        }
        if (event.len == 0) {
            return false;
        }
        dir = *inside;
    } else {
        return false;
    }

    const QByteArray name(event.name);
    if (name.startsWith('.') || (dir.isEmpty() && !isItemName(name))) {
        return false;
    }
    const QByteArray path = dir.isEmpty() ? name : dir + '/' + name;
    if ((event.mask & IN_MODIFY) != 0) {
        // Until the close, or the hold lapses, a scan keeps the item as it was, so nothing a scan
        // lists changes yet.
        hold(*tree, path);
        return false;
    }
    if ((event.mask & IN_ATTRIB) != 0) {
        return !tree->writing.contains(path);
    }
    // Created, closed after writing, moved in: finished, as a file being written reports its
    // writes, so a name given to finished bytes by link() or rename() needs no close. Moved away
    // or removed: whatever was being written under it is no longer there.
    tree->writing.removeIf([&path](const auto& held) { return isAtOrUnder(held.key(), path); });
    if (m_items == Items::Folders && (event.mask & IN_ISDIR) != 0) {
        // Its watch comes only now, so writes into it may have gone unseen while a scan read it.
        // One renamed in, as an import is, was finished before it had this name.
        if ((event.mask & IN_CREATE) != 0) {
            m_unfinishedDuringScan.insert(itemOf(path));
        }
        if ((event.mask & (IN_MOVED_FROM | IN_DELETE)) != 0) {
            unwatchTree(*tree, path);
        } else {
            // Can switch to polling, which ends `tree`.
            watchTree(*tree, path);
        }
    }
    return true;
}

QDeadlineTimer FolderWatcher::holdFromNow()
{
    if (!m_holdExpiry.isActive()) {
        m_holdExpiry.start(m_hold);
    }
    return QDeadlineTimer(m_hold);
}

void FolderWatcher::releaseHolds()
{
    auto* tree = std::get_if<TreeWatch>(&m_watch);
    if (tree == nullptr) {
        return;
    }
    bool released = false;
    std::optional<QDeadlineTimer> next;
    for (auto held = tree->writing.begin(); held != tree->writing.end();) {
        if (held.value().hasExpired()) {
            // The lapse is for a write no close will end, as truncate(2) makes; a writer still
            // holding the file open has one coming.
            if (!openForWriting(AT_FDCWD, (m_directory + '/' + held.key()).constData())) {
                held = tree->writing.erase(held);
                released = true;
                continue;
            }
            held.value() = QDeadlineTimer(m_hold);
        }
        next = next ? std::min(*next, held.value()) : held.value();
        ++held;
    }
    if (next) {
        // Rounded up: a timer due before the deadline would find nothing lapsed and come straight back.
        m_holdExpiry.start(std::chrono::ceil<std::chrono::milliseconds>(next->remainingTimeAsDuration()));
    }
    if (released) {
        m_settle.start();
    }
}

QSet<QByteArray> FolderWatcher::unfinished() const
{
    QSet<QByteArray> items;
    if (const auto* tree = std::get_if<TreeWatch>(&m_watch)) {
        for (auto it = tree->writing.keyBegin(); it != tree->writing.keyEnd(); ++it) {
            items.insert(itemOf(*it));
        }
    }
    return items;
}

FolderWatcher::Observation FolderWatcher::observe() const
{
    Observation seen;
    const Listing root(::opendir(m_directory.constData()), ::closedir);
    if (!root) {
        seen.openError = errno;
        return seen;
    }
    // The folder's identity, not its times: those change with every entry, items or not.
    if (struct stat st {}; ::fstat(::dirfd(root.get()), &st) == 0) {
        seen.device = st.st_dev;
        seen.inode = st.st_ino;
    }
    const auto list = [&](DIR* listing, const QByteArray& relative, const auto& self) -> void {
        while (const dirent* entry = ::readdir(listing)) {
            const QByteArray name(entry->d_name);
            if (name.startsWith('.') || (relative.isEmpty() && !isItemName(name))) {
                continue;
            }
            Observation::Entry observed{relative.isEmpty() ? name : relative + '/' + name, {}, 0};
            // A package is followed through a link; a link in a wallpaper folder is its own entry.
            const int follow = m_items == Items::Files ? 0 : AT_SYMLINK_NOFOLLOW;
            struct stat st {};
            const bool known = ::fstatat(::dirfd(listing), entry->d_name, &st, follow) == 0;
            if (known) {
                observed.mode = st.st_mode;
                // A folder's times change with its hidden entries too, which are no part of an item.
                observed.stamp = S_ISDIR(st.st_mode) ? FileStamp{quint64(st.st_dev), quint64(st.st_ino), 0, 0} : stampFrom(st);
            }
            const QByteArray path = observed.name;
            seen.entries.append(std::move(observed));
            if (m_items == Items::Folders && known && S_ISDIR(st.st_mode)) {
                const int fd = ::openat(::dirfd(listing), entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (fd >= 0) {
                    const Listing inner(::fdopendir(fd), ::closedir);
                    if (inner) {
                        self(inner.get(), path, self);
                    } else {
                        ::close(fd);
                    }
                }
            }
        }
    };
    list(root.get(), {}, list);
    std::ranges::sort(seen.entries, {}, &Observation::Entry::name);
    return seen;
}

void FolderWatcher::poll()
{
    // Before the first scan there is nothing to keep current, as with a watch.
    if (!m_scanned) {
        return;
    }
    auto& polled = std::get<PollWatch>(m_watch);
    Observation seen = observe();
    // A change is scanned once a poll finds nothing further changed. A writer pausing for longer
    // than a poll is caught by the scan, which finds its file open.
    const bool quiet = seen == polled.polled;
    polled.polled = std::move(seen);
    // A close changes nothing a poll compares, so the scan it is owed comes from here.
    const bool closed = polled.open.removeIf([this](const QByteArray& path) {
        return !openForWriting(AT_FDCWD, (m_directory + '/' + path).constData());
    }) > 0;
    if (closed || (quiet && polled.polled != polled.scanned)) {
        emit scanDue();
    }
}

} // namespace wallpaperx
