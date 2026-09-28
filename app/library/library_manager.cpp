// SPDX-License-Identifier: GPL-2.0-or-later

#include "library_manager.h"

#include <QDir>
#include <QException>
#include <QFuture>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <utility>

#include <dirent.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wallpaperx {

namespace {

constexpr uint32_t kFolderEvents = IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_MODIFY
                                   | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
// IN_ATTRIB: a child made readable is reported to its parent, and a chmod leaves no other trace.
constexpr uint32_t kAncestorEvents = IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_ATTRIB | IN_DELETE_SELF
                                     | IN_MOVE_SELF | IN_ONLYDIR;
constexpr uint32_t kLinkEvents = IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF;

using ScanResult = std::expected<QList<Wallpaper>, QString>;

// The exception being handled, seen through the QUnhandledException QtConcurrent wraps it in.
QString describeCurrentException()
{
    try {
        try {
            throw;
        } catch (const QUnhandledException& wrapped) {
            if (wrapped.exception()) {
                std::rethrow_exception(wrapped.exception());
            }
            throw;
        }
    } catch (const std::exception& e) {
        return QString::fromLocal8Bit(e.what());
    } catch (...) {
    }
    return QStringLiteral("an unknown error");
}

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

} // namespace

void WallpaperListModel::assign(QList<Wallpaper> wallpapers)
{
    // Last first, so the rows still to visit keep their indexes.
    for (qsizetype row = m_rows.size() - 1; row >= 0; --row) {
        if (!std::ranges::contains(wallpapers, m_rows[row].filePath, &Wallpaper::filePath)) {
            beginRemoveRows({}, int(row), int(row));
            m_rows.removeAt(row);
            endRemoveRows();
        }
    }
    // Every row above `row` already matches.
    for (qsizetype row = 0; row < wallpapers.size(); ++row) {
        const Wallpaper& wanted = wallpapers[row];
        const auto found = std::find_if(m_rows.begin() + row, m_rows.end(),
                                        [&wanted](const Wallpaper& w) { return w.filePath == wanted.filePath; });
        if (found == m_rows.end()) {
            beginInsertRows({}, int(row), int(row));
            m_rows.insert(row, wanted);
            endInsertRows();
            continue;
        }
        if (const qsizetype from = found - m_rows.begin(); from != row) {
            beginMoveRows({}, int(from), int(from), {}, int(row));
            m_rows.move(from, row);
            endMoveRows();
        }
        if (m_rows[row] != wanted) {
            m_rows[row] = wanted;
            emit dataChanged(index(int(row)), index(int(row)));
        }
    }
    // Left only by a path listed twice.
    if (m_rows.size() > wallpapers.size()) {
        beginRemoveRows({}, int(wallpapers.size()), int(m_rows.size() - 1));
        m_rows.resize(wallpapers.size());
        endRemoveRows();
    }
}

int WallpaperListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant WallpaperListModel::data(const QModelIndex& index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid) || role != Qt::UserRole) {
        return {};
    }
    return QVariant::fromValue(m_rows[index.row()]);
}

QHash<int, QByteArray> WallpaperListModel::roleNames() const
{
    return {{Qt::UserRole, QByteArrayLiteral("modelData")}};
}

LibraryManager::LibraryManager(QString directory, int settleMs, int pollMs, Watching watching, int holdMs,
                               QObject* parent)
    : QObject(parent)
    , m_directory(QDir::cleanPath(QDir(directory).absolutePath()))
    , m_hold(holdMs)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(settleMs);
    connect(&m_settle, &QTimer::timeout, this, &LibraryManager::rescan);
    m_poll.setInterval(pollMs);
    connect(&m_poll, &QTimer::timeout, this, &LibraryManager::poll);
    m_holdExpiry.setSingleShot(true);
    connect(&m_holdExpiry, &QTimer::timeout, this, &LibraryManager::releaseHolds);

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
    m_inotifyReady = std::make_unique<QSocketNotifier>(m_inotify, QSocketNotifier::Read);
    connect(m_inotifyReady.get(), &QSocketNotifier::activated, this, &LibraryManager::readWatchEvents);
}

void LibraryManager::pollInstead(const char* failed, int error)
{
    qWarning("wallpaperx: %s failed (%s), so the library folder is polled every %d ms", failed, std::strerror(error),
             m_poll.interval());
    m_inotifyReady.reset();
    if (m_inotify >= 0) {
        ::close(m_inotify);
    }
    m_inotify = -1;
    m_linkWatches.clear();
    m_watch = PollWatch{};
    m_poll.start();
}

LibraryManager::~LibraryManager()
{
    // The notifier first: it must not outlive the descriptor it polls.
    m_inotifyReady.reset();
    if (m_inotify >= 0) {
        ::close(m_inotify);
    }
}

Wallpaper LibraryManager::selectedWallpaper() const
{
    const QList<Wallpaper>& wallpapers = m_model.wallpapers();
    const auto it = std::ranges::find(wallpapers, m_selection, &Wallpaper::filePath);
    return (m_selection.isEmpty() || it == wallpapers.end()) ? Wallpaper{} : *it;
}

QVariantMap LibraryManager::propertyValues() const
{
    QVariantMap values;
    for (const WallpaperProperty& property : selectedWallpaper().properties) {
        values.insert(property.key, m_edits.value(property.key, property.defaultValue));
    }
    return values;
}

void LibraryManager::rescan()
{
    // Queued, never restarted: a scan restarted by every change would publish nothing while
    // changes kept coming.
    if (m_scanning != Scanning::No) {
        m_scanning = Scanning::YesThenAgain;
        return;
    }
    if (!polling()) {
        // Queued events first: a write still unread would let the scan show a file being written.
        readWatchEvents();
        placeWatch();
    }
    // Before the listing, so nothing that happens to the folder while it runs goes unreported.
    if (auto* polled = std::get_if<PollWatch>(&m_watch)) {
        polled->scanned = observe();
    }
    m_settle.stop();
    m_unfinishedDuringScan = unfinished();
    m_scanning = Scanning::Yes;
    const QString directory = m_directory;
    QtConcurrent::run([directory] { return scanLibrary(directory); })
        // A throw would otherwise skip the continuation and leave the state as it was, Loading at first.
        .onFailed([directory]() -> ScanResult {
            return std::unexpected(QStringLiteral("Reading %1 failed: %2.").arg(directory, describeCurrentException()));
        })
        .then(this, [this](ScanResult scan) {
            if (scan) {
                // The scan may have read a file whose write is still queued. A file that finished
                // meanwhile is held back too: it has a scan of its own coming.
                readWatchEvents();
                // Polling sees no write: a package that changed while the scan ran is held back
                // until a poll finds it quiet and scans again.
                if (const auto* polled = std::get_if<PollWatch>(&m_watch)) {
                    const Observation now = observe();
                    for (const Observation::Entry& entry : std::as_const(polled->scanned.entries)) {
                        if (!now.entries.contains(entry)) {
                            m_unfinishedDuringScan.insert(entry.name);
                        }
                    }
                    for (const Observation::Entry& entry : now.entries) {
                        if (!polled->scanned.entries.contains(entry)) {
                            m_unfinishedDuringScan.insert(entry.name);
                        }
                    }
                }
                settle(State::Ready, {}, holdBack(std::move(*scan)));
            } else {
                settle(State::Unavailable, std::move(scan.error()), {});
            }
            const bool linksChanged = watchLinks();
            if (std::exchange(m_scanning, Scanning::No) == Scanning::YesThenAgain) {
                rescan();
            } else if (linksChanged) {
                m_settle.start();
            }
        });
}

QList<Wallpaper> LibraryManager::holdBack(QList<Wallpaper> scan) const
{
    const QSet<QByteArray>& held = m_unfinishedDuringScan;
    if (held.isEmpty()) {
        return scan;
    }
    scan.removeIf([&held](const Wallpaper& w) { return held.contains(encodeFileName(w.fileName)); });
    bool kept = false;
    for (const Wallpaper& shown : m_model.wallpapers()) {
        if (held.contains(encodeFileName(shown.fileName))) {
            scan.append(shown);
            kept = true;
        }
    }
    if (kept) {
        std::ranges::sort(scan, listedBefore);
    }
    return scan;
}

void LibraryManager::settle(State state, QString reason, QList<Wallpaper> wallpapers)
{
    const bool stateChanges = m_state != state || m_unavailableReason != reason;
    const bool listChanges = m_model.wallpapers() != wallpapers;
    changeSelection([&] {
        m_state = state;
        m_unavailableReason = std::move(reason);
        // Before the rows change, so no row signal finds a selection the list no longer holds.
        const auto kept = std::ranges::find(wallpapers, m_selection, &Wallpaper::filePath);
        if (kept == wallpapers.end() || !kept->usable()) {
            m_selection.clear();
            m_edits.clear();
        } else {
            QVariantMap edits;
            for (const WallpaperProperty& property : kept->properties) {
                if (const auto value = m_edits.contains(property.key) ? property.accept(m_edits.value(property.key)) : std::nullopt) {
                    edits.insert(property.key, *value);
                }
            }
            m_edits = std::move(edits);
        }
        m_model.assign(std::move(wallpapers));
    });
    // After every field is written, so no handler sees the new state beside the old list.
    if (stateChanges) {
        emit stateChanged();
    }
    if (listChanges) {
        emit wallpapersChanged();
    }
}

void LibraryManager::placeWatch()
{
    QByteArray at = encodeFileName(m_directory);
    QByteArray awaited;
    Watch next;
    for (;;) {
        const int wd = ::inotify_add_watch(m_inotify, at.constData(), awaited.isEmpty() ? kFolderEvents : kAncestorEvents);
        if (wd >= 0) {
            next = awaited.isEmpty() ? Watch(FolderWatch{wd, {}}) : Watch(AncestorWatch{wd, awaited});
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
    if (previous == descriptorOf(next) && m_watch.index() == next.index()) {
        if (auto* ancestor = std::get_if<AncestorWatch>(&m_watch)) {
            ancestor->awaited = std::get<AncestorWatch>(next).awaited;
        }
        return;
    }
    // Adding a path already watched returns its descriptor, so only a different one is removed.
    if (previous >= 0 && previous != descriptorOf(next)) {
        ::inotify_rm_watch(m_inotify, previous);
    }
    m_watch = std::move(next);
}

bool LibraryManager::watchLinks()
{
    QList<LinkWatch> watches;
    bool changed = false;
    for (const Wallpaper& wallpaper : m_model.wallpapers()) {
        const QByteArray path = encodeFileName(wallpaper.filePath);
        struct stat st {};
        if (::lstat(path.constData(), &st) != 0 || !S_ISLNK(st.st_mode)) {
            continue;
        }
        const QByteArray name = encodeFileName(wallpaper.fileName);
        if (m_inotify >= 0) {
            // Links to one file share its watch, so its descriptor comes back for each.
            if (const int wd = ::inotify_add_watch(m_inotify, path.constData(), kLinkEvents); wd >= 0) {
                if (auto same = std::ranges::find(watches, wd, &LinkWatch::wd); same != watches.end()) {
                    same->names.append(name);
                } else {
                    const auto known = std::ranges::find(m_linkWatches, wd, &LinkWatch::wd);
                    watches.append({wd, {name}, known != m_linkWatches.end() ? known->writing : std::nullopt});
                }
            } else if (const int error = errno; !missingPath(error)) {
                pollInstead("inotify_add_watch", error);
                watches.clear();
            }
        }
        // A held-back row shows the target before a write whose close brings a scan of its own.
        if (!m_unfinishedDuringScan.contains(name)) {
            // A row that could not be stat'ed carries an empty stamp.
            changed = changed || stampOf(wallpaper.filePath).value_or(FileStamp{}) != wallpaper.stamp;
        }
    }
    for (const LinkWatch& watch : std::as_const(m_linkWatches)) {
        if (!std::ranges::contains(watches, watch.wd, &LinkWatch::wd)) {
            ::inotify_rm_watch(m_inotify, watch.wd);
        }
    }
    m_linkWatches = std::move(watches);
    return changed;
}

void LibraryManager::readWatchEvents()
{
    if (m_inotify < 0) {
        return;
    }
    alignas(inotify_event) char buffer[16 * 1024];
    bool changed = false;
    for (;;) {
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

bool LibraryManager::handle(const inotify_event& event)
{
    if ((event.mask & IN_Q_OVERFLOW) != 0) {
        // Events were lost, so which entries are still being written is no longer known.
        if (auto* folder = std::get_if<FolderWatch>(&m_watch)) {
            folder->writing.clear();
        }
        for (LinkWatch& link : m_linkWatches) {
            link.writing.reset();
        }
        return true;
    }
    const int watched = descriptorOf(m_watch);
    if (event.wd != watched) {
        return handleLink(event);
    }
    if (event.len == 0) {
        if ((event.mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0) {
            // Its path now names something else or nothing; the next scan places the watch again.
            if ((event.mask & IN_IGNORED) == 0) {
                ::inotify_rm_watch(m_inotify, watched);
            }
            m_watch = std::monostate{};
        }
        return true;
    }
    const QByteArray name(event.name);
    if (const auto* ancestor = std::get_if<AncestorWatch>(&m_watch)) {
        return name == ancestor->awaited;
    }
    auto& folder = std::get<FolderWatch>(m_watch);
    if (!isPackageFileName(name)) {
        return false;
    }
    if ((event.mask & IN_MODIFY) != 0) {
        // Until the close, or the hold lapses, a scan keeps its row as it was, so nothing a scan
        // lists changes yet.
        folder.writing.insert(name, holdFromNow());
        m_unfinishedDuringScan.insert(name);
        return false;
    }
    if ((event.mask & IN_ATTRIB) != 0) {
        return !folder.writing.contains(name);
    }
    folder.writing.remove(name);
    if ((event.mask & (IN_MOVED_FROM | IN_DELETE)) != 0) {
        return true;
    }
    // Created, closed after writing, or moved in: finished, as a file being written reports its
    // writes, so a name given to finished bytes by link() or rename() needs no close. An empty
    // file is never listed, so one without a row changes nothing, unless the running scan read it
    // before it was emptied. Some programs create the final name empty and rename the file over it.
    struct stat st {};
    const QByteArray path = encodeFileName(m_directory) + '/' + name;
    const bool empty = ::lstat(path.constData(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0;
    return !empty || m_scanning != Scanning::No
           || std::ranges::contains(m_model.wallpapers(), decodeFileName(name), &Wallpaper::fileName);
}

QDeadlineTimer LibraryManager::holdFromNow()
{
    if (!m_holdExpiry.isActive()) {
        m_holdExpiry.start(m_hold);
    }
    return QDeadlineTimer(m_hold);
}

void LibraryManager::releaseHolds()
{
    bool released = false;
    std::optional<QDeadlineTimer> next;
    const auto lapsed = [&](QDeadlineTimer until) {
        if (until.hasExpired()) {
            released = true;
            return true;
        }
        next = next ? std::min(*next, until) : until;
        return false;
    };
    if (auto* folder = std::get_if<FolderWatch>(&m_watch)) {
        folder->writing.removeIf([&](const auto& held) { return lapsed(held.value()); });
    }
    for (LinkWatch& link : m_linkWatches) {
        if (link.writing && lapsed(*link.writing)) {
            link.writing.reset();
        }
    }
    if (next) {
        // Rounded up: a timer due before the deadline would find nothing lapsed and come straight back.
        m_holdExpiry.start(std::chrono::ceil<std::chrono::milliseconds>(next->remainingTimeAsDuration()));
    }
    if (released) {
        m_settle.start();
    }
}

bool LibraryManager::handleLink(const inotify_event& event)
{
    const auto link = std::ranges::find(m_linkWatches, event.wd, &LinkWatch::wd);
    if (link == m_linkWatches.end()) {
        return false;
    }
    if ((event.mask & IN_IGNORED) != 0) {
        m_linkWatches.erase(link);
        return false;
    }
    if ((event.mask & IN_MODIFY) != 0) {
        link->writing = holdFromNow();
        for (const QByteArray& name : std::as_const(link->names)) {
            m_unfinishedDuringScan.insert(name);
        }
        return false;
    }
    if ((event.mask & IN_ATTRIB) != 0) {
        return !link->writing;
    }
    // Closed after writing, or no longer the file the links name.
    link->writing.reset();
    return true;
}

QSet<QByteArray> LibraryManager::unfinished() const
{
    QSet<QByteArray> names;
    if (const auto* folder = std::get_if<FolderWatch>(&m_watch)) {
        for (auto it = folder->writing.keyBegin(); it != folder->writing.keyEnd(); ++it) {
            names.insert(*it);
        }
    }
    for (const LinkWatch& link : m_linkWatches) {
        if (link.writing) {
            for (const QByteArray& name : link.names) {
                names.insert(name);
            }
        }
    }
    return names;
}

LibraryManager::Observation LibraryManager::observe() const
{
    Observation seen;
    std::unique_ptr<DIR, int (*)(DIR*)> listing(::opendir(encodeFileName(m_directory).constData()), ::closedir);
    if (!listing) {
        seen.openError = errno;
        return seen;
    }
    // The folder's identity, not its times: those change with every entry, packages or not.
    if (struct stat st {}; ::fstat(::dirfd(listing.get()), &st) == 0) {
        seen.device = st.st_dev;
        seen.inode = st.st_ino;
    }
    while (const dirent* entry = ::readdir(listing.get())) {
        if (!isPackageFileName(entry->d_name)) {
            continue;
        }
        Observation::Entry observed{QByteArray(entry->d_name), {}, 0};
        if (struct stat st {}; ::fstatat(::dirfd(listing.get()), entry->d_name, &st, 0) == 0) {
            observed.stamp = stampFrom(st);
            observed.mode = st.st_mode;
        }
        seen.entries.append(std::move(observed));
    }
    std::ranges::sort(seen.entries, {}, &Observation::Entry::name);
    return seen;
}

void LibraryManager::poll()
{
    // Before the first scan there is nothing to keep current, as with a watch.
    if (m_state == State::Loading && m_scanning == Scanning::No) {
        return;
    }
    auto& polled = std::get<PollWatch>(m_watch);
    Observation seen = observe();
    // A change is read once a poll finds nothing further changed, so a file being written is
    // not read half-way unless its writer pauses for longer than a poll.
    const bool quiet = seen == polled.polled;
    polled.polled = std::move(seen);
    if (quiet && polled.polled != polled.scanned) {
        rescan();
    }
}

template <typename Change>
void LibraryManager::changeSelection(Change change)
{
    const Wallpaper previous = selectedWallpaper();
    const QVariantMap previousValues = propertyValues();
    change();
    if (selectedWallpaper() != previous) {
        emit selectionChanged();
    }
    if (propertyValues() != previousValues) {
        emit propertyValuesChanged();
    }
}

bool LibraryManager::select(const QString& filePath)
{
    const QList<Wallpaper>& wallpapers = m_model.wallpapers();
    const auto it = std::ranges::find(wallpapers, filePath, &Wallpaper::filePath);
    if (it == wallpapers.end() || !it->usable()) {
        return false;
    }
    if (m_selection != filePath) {
        changeSelection([&] {
            m_selection = filePath;
            m_edits.clear();
        });
    }
    return true;
}

void LibraryManager::clearSelection()
{
    changeSelection([&] {
        m_selection.clear();
        m_edits.clear();
    });
}

bool LibraryManager::setPropertyValue(const QString& key, const QVariant& value)
{
    const QList<WallpaperProperty> properties = selectedWallpaper().properties;
    const auto it = std::ranges::find(properties, key, &WallpaperProperty::key);
    if (it == properties.end()) {
        return false;
    }
    const auto accepted = it->accept(value);
    if (!accepted) {
        return false;
    }
    changeSelection([&] { m_edits.insert(key, *accepted); });
    return true;
}

void LibraryManager::resetPropertyValues()
{
    changeSelection([&] { m_edits.clear(); });
}

} // namespace wallpaperx
