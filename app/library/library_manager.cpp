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

namespace {

using ScanResult = std::expected<QList<Wallpaper>, QString>;

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
    , m_watcher(m_directory, FolderWatcher::Items::Folders, settleMs, pollMs, watching, holdMs)
{
    connect(&m_watcher, &FolderWatcher::scanDue, this, &LibraryManager::rescan);
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
    m_watcher.scanStarting();
    m_scanning = Scanning::Yes;
    if (m_reveal) {
        m_reveal->scanBegun = true;
    }
    const QString directory = m_directory;
    QtConcurrent::run([directory] { return scanLibrary(directory); })
        // A throw would otherwise skip the continuation and leave the state as it was, Loading at first.
        .onFailed([directory]() -> ScanResult {
            return std::unexpected(QStringLiteral("Reading %1 failed: %2.").arg(directory, describeCurrentException()));
        })
        .then(this, [this](ScanResult scan) {
            if (scan) {
                const QSet<QByteArray> held = m_watcher.scanFinished();
                // A scan holding the folder back has not answered; the scan owed after it will.
                if (m_reveal && held.contains(encodeFileName(QFileInfo(m_reveal->filePath).fileName()))) {
                    m_reveal->scanBegun = false;
                }
                settle(State::Ready, {}, holdBack(std::move(*scan), held));
            } else {
                settle(State::Unavailable, std::move(scan.error()), {});
            }
            if (std::exchange(m_scanning, Scanning::No) == Scanning::YesThenAgain) {
                rescan();
            }
        });
}

QList<Wallpaper> LibraryManager::holdBack(QList<Wallpaper> scan, const QSet<QByteArray>& held) const
{
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
    // After every field is written, so no handler sees the new state beside the old list.
    if (stateChanges) {
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
