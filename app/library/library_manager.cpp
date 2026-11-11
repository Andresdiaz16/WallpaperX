// SPDX-License-Identifier: GPL-2.0-or-later

#include "library_manager.h"

#include "current_exception.h"

#include <QDir>
#include <QFileInfo>
#include <QFuture>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace wallpaperx {

struct LibraryManager::Scan {
    // Sorted.
    QList<Wallpaper> wallpapers;
    // Why the library folder could not be read, if it could not.
    QString libraryProblem;
    struct ListRead {
        QString root;
        std::expected<QStringList, QString> libraries;
    };
    // In root order.
    QList<ListRead> lists;
    QList<WorkshopScan::Unread> unreadItemFolders;
    // Of the libraries read, those Wallpaper Engine itself is installed in.
    QSet<QString> installs;
};

namespace {

// The folder `path` is directly in.
QString folderOf(const QString& path)
{
    return path.left(path.lastIndexOf(u'/'));
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
                               QStringList steamRoots, QObject* parent)
    : QObject(parent)
    , m_directory(QDir::cleanPath(QDir(directory).absolutePath()))
    , m_settleMs(settleMs)
    , m_pollMs(pollMs)
    , m_watching(watching)
    , m_holdMs(holdMs)
    , m_watcher(m_directory, FolderWatcher::Items::Folders, settleMs, pollMs, watching, holdMs)
    , m_steamRoots(std::move(steamRoots))
{
    connect(&m_watcher, &FolderWatcher::scanDue, this, &LibraryManager::rescan);
    // In the form rows' paths take, which publish() compares with.
    for (QString& root : m_steamRoots) {
        root = QDir::cleanPath(QDir(root).absolutePath());
    }
    m_steamRoots.removeDuplicates();
    for (const QString& root : std::as_const(m_steamRoots)) {
        auto watcher = std::make_unique<FolderWatcher>(steamAppsFolder(root), isLibraryListName, settleMs, pollMs,
                                                       watching, holdMs);
        connect(watcher.get(), &FolderWatcher::scanDue, this, &LibraryManager::rescan);
        m_libraryListWatchers.emplace(root, std::move(watcher));
    }
}

LibraryManager::State LibraryManager::state() const
{
    if (!m_problems) {
        // Every scan so far threw, so nothing says the library folder can be read.
        return m_scanFailure.isEmpty() ? State::Loading : State::Unavailable;
    }
    return m_problems->library.isEmpty() || !m_model.wallpapers().isEmpty() ? State::Ready : State::Unavailable;
}

QString LibraryManager::unavailableReason() const
{
    return state() == State::Unavailable ? problems().join(u'\n') : QString();
}

QStringList LibraryManager::problems() const
{
    QStringList all;
    if (m_problems && !m_problems->library.isEmpty()) {
        all.append(m_problems->library);
    }
    if (!m_scanFailure.isEmpty()) {
        all.append(m_scanFailure);
    }
    if (m_problems) {
        all.append(m_problems->steam);
    }
    return all;
}

void LibraryManager::watchItemFolders(const QStringList& itemFolders)
{
    std::erase_if(m_itemFolderWatchers, [this, &itemFolders](auto& watched) {
        if (itemFolders.contains(watched.first)) {
            return false;
        }
        // Later, not now: this can run inside that watcher's own scanDue.
        watched.second->disconnect(this);
        watched.second.release()->deleteLater();
        return true;
    });
    for (const QString& folder : itemFolders) {
        if (m_itemFolderWatchers.contains(folder)) {
            continue;
        }
        auto watcher = std::make_unique<FolderWatcher>(folder, FolderWatcher::Items::ShallowFolders, m_settleMs, m_pollMs,
                                                       m_watching, m_holdMs);
        connect(watcher.get(), &FolderWatcher::scanDue, this, &LibraryManager::rescan);
        m_itemFolderWatchers.emplace(folder, std::move(watcher));
    }
}

void LibraryManager::scanStarting()
{
    m_watcher.scanStarting();
    for (const auto& [root, watcher] : m_libraryListWatchers) {
        watcher->scanStarting();
    }
    for (const auto& [folder, watcher] : m_itemFolderWatchers) {
        watcher->scanStarting();
    }
}

QSet<QString> LibraryManager::scanFinished()
{
    QSet<QString> held;
    const auto add = [&held](FolderWatcher& watcher, const QString& folder) {
        const QDir dir(folder);
        for (const QByteArray& name : watcher.scanFinished()) {
            held.insert(dir.absoluteFilePath(decodeFileName(name)));
        }
    };
    add(m_watcher, m_directory);
    for (const auto& [folder, watcher] : m_itemFolderWatchers) {
        add(*watcher, folder);
    }
    // A list caught being written has a scan of its own coming, which reads it whole.
    for (const auto& [root, watcher] : m_libraryListWatchers) {
        if (!watcher->scanFinished().isEmpty()) {
            held.insert(libraryListOf(root));
        }
    }
    return held;
}

Wallpaper LibraryManager::selectedWallpaper() const
{
    const QList<Wallpaper>& wallpapers = m_model.wallpapers();
    const auto it = std::ranges::find(wallpapers, m_selection, &Wallpaper::filePath);
    return (m_selection.isEmpty() || it == wallpapers.end()) ? Wallpaper{} : *it;
}

QVariantMap LibraryManager::propertyValues() const
{
    QVariantMap values = selectedWallpaper().defaultValues();
    for (auto it = m_edits.cbegin(); it != m_edits.cend(); ++it) {
        values.insert(it.key(), it.value());
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
    // Before anything is read, so nothing that changes from here on goes unreported.
    scanStarting();
    m_scanning = Scanning::Yes;
    if (m_reveal) {
        m_reveal->scanBegun = true;
    }
    QList<std::pair<QString, QStringList>> trusted;
    for (const QString& root : std::as_const(m_steamRoots)) {
        trusted.append({root, m_steamLibraries.value(root, {root})});
    }
    // A copy: the rows can change on this thread while the scan runs.
    QtConcurrent::run([directory = m_directory, trusted = std::move(trusted), previous = m_model.wallpapers()] {
        return std::expected<Scan, QString>(readSources(directory, trusted, previous));
    })
        // A throw would otherwise skip the continuation and leave the state as it was, Loading at first.
        .onFailed([]() -> std::expected<Scan, QString> {
            return std::unexpected(QStringLiteral("Listing wallpapers failed: %1.").arg(describeCurrentException()));
        })
        .then(this, [this](std::expected<Scan, QString> scan) { publish(std::move(scan)); });
}

LibraryManager::Scan LibraryManager::readSources(const QString& directory,
                                                 const QList<std::pair<QString, QStringList>>& trusted,
                                                 const QList<Wallpaper>& previous)
{
    Scan scan;
    std::expected<QList<Wallpaper>, QString> library;
    try {
        library = scanLibrary(directory, previous);
    } catch (...) {
        library = std::unexpected(readingFailed(directory));
    }
    if (library) {
        scan.wallpapers = std::move(*library);
    } else {
        scan.libraryProblem = std::move(library.error());
    }
    // Both, as only the GUI thread knows whether the list was being written while it was read.
    QStringList libraries;
    for (const auto& [root, last] : trusted) {
        std::expected<QStringList, QString> read;
        try {
            read = steamLibrariesOf(root);
        } catch (...) {
            read = std::unexpected(readingFailed(libraryListOf(root)));
        }
        libraries.append(last);
        if (read) {
            libraries.append(*read);
        }
        scan.lists.append({root, std::move(read)});
    }
    libraries.removeDuplicates();
    QStringList itemFolders;
    for (const QString& folder : std::as_const(libraries)) {
        itemFolders.append(workshopItemsOf(folder));
        if (QFileInfo(wallpaperEngineInstallOf(folder)).isDir()) {
            scan.installs.insert(folder);
        }
    }
    WorkshopScan items = scanWorkshop(itemFolders, previous);
    scan.wallpapers.append(std::move(items.items));
    scan.unreadItemFolders = std::move(items.unread);
    std::ranges::sort(scan.wallpapers, listedBefore);
    return scan;
}

void LibraryManager::publish(std::expected<Scan, QString> scan)
{
    QSet<QString> held = scanFinished();
    if (!scan) {
        // Nothing read can be trusted: the rows, the lists, the watchers and what the latest scan that
        // read found stay as they were.
        settle(m_problems, std::move(scan.error()), m_model.wallpapers());
    } else {
        Problems problems{std::move(scan->libraryProblem), {}};
        QStringList libraries;
        for (auto& [root, read] : scan->lists) {
            // Being written while it was read: what it said then is not trusted, and a scan follows.
            if (!held.contains(libraryListOf(root))) {
                if (read) {
                    m_steamLibraries.insert(root, std::move(*read));
                } else {
                    problems.steam.append(QStringLiteral("Some Wallpaper Engine items may be missing. %1").arg(read.error()));
                }
            }
            libraries.append(m_steamLibraries.value(root, {root}));
        }
        libraries.removeDuplicates();
        QStringList itemFolders;
        QString install;
        for (const QString& folder : std::as_const(libraries)) {
            itemFolders.append(workshopItemsOf(folder));
            if (install.isEmpty() && scan->installs.contains(folder)) {
                install = wallpaperEngineInstallOf(folder);
            }
        }
        // Read only for a list this scan did not trust.
        scan->wallpapers.removeIf([&itemFolders](const Wallpaper& w) {
            return w.fromWallpaperEngine() && !itemFolders.contains(folderOf(w.filePath));
        });
        // A row is held back only in a folder this scan listed: one that could not be read loses its rows.
        QSet<QString> listed;
        if (problems.library.isEmpty()) {
            listed.insert(m_directory);
        }
        for (const QString& folder : std::as_const(itemFolders)) {
            const auto unread = std::ranges::find(scan->unreadItemFolders, folder, &WorkshopScan::Unread::folder);
            if (unread == scan->unreadItemFolders.end()) {
                listed.insert(folder);
            } else {
                problems.steam.append(unread->problem);
            }
        }
        held.removeIf([&listed](const QString& path) { return !listed.contains(folderOf(path)); });
        // A scan holding the folder back has not answered; the scan owed after it will.
        if (m_reveal && held.contains(m_reveal->filePath)) {
            m_reveal->scanBegun = false;
        }
        // Read with no watcher on it yet, so an item there may have been read half-written.
        const bool unwatched = std::ranges::any_of(itemFolders, [this](const QString& folder) {
            return !m_itemFolderWatchers.contains(folder);
        });
        settle(std::move(problems), {}, holdBack(std::move(scan->wallpapers), held));
        watchItemFolders(itemFolders);
        if (unwatched) {
            m_scanning = Scanning::YesThenAgain;
        }
        if (m_wallpaperEngineInstall != install) {
            m_wallpaperEngineInstall = install;
            emit wallpaperEngineInstallChanged();
        }
    }
    if (std::exchange(m_scanning, Scanning::No) == Scanning::YesThenAgain) {
        rescan();
    }
}

QList<Wallpaper> LibraryManager::holdBack(QList<Wallpaper> scan, const QSet<QString>& held) const
{
    if (held.isEmpty()) {
        return scan;
    }
    scan.removeIf([&held](const Wallpaper& w) { return held.contains(w.filePath); });
    bool kept = false;
    for (const Wallpaper& shown : m_model.wallpapers()) {
        if (held.contains(shown.filePath)) {
            scan.append(shown);
            kept = true;
        }
    }
    if (kept) {
        std::ranges::sort(scan, listedBefore);
    }
    return scan;
}

void LibraryManager::settle(std::optional<Problems> problems, QString scanFailure, QList<Wallpaper> wallpapers)
{
    const State stateBefore = state();
    const bool problemsChange = m_problems != problems || m_scanFailure != scanFailure;
    const bool listChanges = m_model.wallpapers() != wallpapers;
    changeSelection([&] {
        m_problems = std::move(problems);
        m_scanFailure = std::move(scanFailure);
        // Before the rows change, so no row signal finds a selection the list no longer holds.
        const auto kept = std::ranges::find(wallpapers, m_selection, &Wallpaper::filePath);
        if (kept == wallpapers.end() || !kept->usable()) {
            m_selection.clear();
            m_edits.clear();
        } else {
            QVariantMap edits;
            for (auto it = m_edits.cbegin(); it != m_edits.cend(); ++it) {
                if (const auto value = kept->accept(it.key(), it.value())) {
                    edits.insert(it.key(), *value);
                }
            }
            m_edits = std::move(edits);
        }
        if (m_reveal && m_reveal->scanBegun) {
            const auto shown = std::ranges::find(wallpapers, m_reveal->filePath, &Wallpaper::filePath);
            if (shown != wallpapers.end() && shown->usable() && m_selection != shown->filePath) {
                m_selection = shown->filePath;
                m_edits.clear();
            }
            m_reveal.reset();
        }
        m_model.assign(std::move(wallpapers));
    });
    // After every field is written, so no handler sees the new state beside the old list. The
    // reason follows from the problems and the state.
    if (problemsChange || state() != stateBefore) {
        emit stateChanged();
    }
    if (listChanges) {
        emit wallpapersChanged();
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
    m_reveal.reset();
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
    m_reveal.reset();
    changeSelection([&] {
        m_selection.clear();
        m_edits.clear();
    });
}

void LibraryManager::reveal(const QString& filePath)
{
    if (select(filePath)) {
        return;
    }
    m_reveal = Reveal{filePath};
    rescan();
}

bool LibraryManager::setPropertyValue(const QString& key, const QVariant& value)
{
    return setPropertyValues({{key, value}});
}

bool LibraryManager::setPropertyValues(const QVariantMap& values)
{
    const Wallpaper selected = selectedWallpaper();
    QVariantMap accepted;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const auto value = selected.accept(it.key(), it.value());
        if (!value) {
            return false;
        }
        accepted.insert(it.key(), *value);
    }
    changeSelection([&] { m_edits.insert(accepted); });
    return true;
}

void LibraryManager::resetPropertyValues()
{
    changeSelection([&] { m_edits.clear(); });
    emit propertyValuesReset();
}

void LibraryManager::adoptPropertyValues(const QVariantMap& values)
{
    const Wallpaper selected = selectedWallpaper();
    QVariantMap accepted;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (const auto value = selected.accept(it.key(), it.value())) {
            accepted.insert(it.key(), *value);
        }
    }
    changeSelection([&] { m_edits = std::move(accepted); });
}

} // namespace wallpaperx
