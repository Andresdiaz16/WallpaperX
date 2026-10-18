// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "folder_watcher.h"
#include "wpx_package.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace wallpaperx {

// The library's wallpapers as rows of one role, "modelData", holding the Wallpaper.
class WallpaperListModel : public QAbstractListModel {
    Q_OBJECT

public:
    using QAbstractListModel::QAbstractListModel;

    const QList<Wallpaper>& wallpapers() const { return m_rows; }
    // Turns the rows into `wallpapers` by removals, moves, inserts and changes matched by filePath,
    // never a reset, so a view keeps its place and the delegates of rows that stayed.
    void assign(QList<Wallpaper> wallpapers);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    QList<Wallpaper> m_rows;
};

// The wallpaper folders in one folder, which of them the user opened, and the property values they
// set.
//
// Scans run off the GUI thread, since every folder's wallpaper.json is read to list it, and one at
// a time; FolderWatcher (Items::Folders) says when. A wallpaper with a file being written is not
// read until it is finished; meanwhile a listed one keeps the row it had, so the selection and its
// values survive the rewrite. Folders present when the library is first read are read at once,
// unless a process has a file in one open for writing.
class LibraryManager : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application with the folder it should read.")
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY stateChanged)
    Q_PROPERTY(QString directory READ directory CONSTANT)
    Q_PROPERTY(QList<wallpaperx::Wallpaper> wallpapers READ wallpapers NOTIFY wallpapersChanged)
    // The same list as a model, for a view that should keep its place when one row changes.
    Q_PROPERTY(QAbstractItemModel* wallpaperModel READ wallpaperModel CONSTANT)
    Q_PROPERTY(QString selectedPath READ selectedPath NOTIFY selectionChanged)
    Q_PROPERTY(wallpaperx::Wallpaper selectedWallpaper READ selectedWallpaper NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap propertyValues READ propertyValues NOTIFY propertyValuesChanged)

public:
    // Loading: the first scan has not finished. Ready: `wallpapers` is the latest scan, empty when
    // the folder does not exist. Unavailable: the folder exists but cannot be listed, `wallpapers`
    // is empty and `unavailableReason` says why.
    enum class State { Loading, Ready, Unavailable };
    Q_ENUM(State)

    using Watching = FolderWatcher::Watching;

    explicit LibraryManager(QString directory, int settleMs = 250, int pollMs = 2000,
                            Watching watching = Watching::Inotify, int holdMs = 30000, QObject* parent = nullptr);

    // Whether the folder is polled: inotify was not asked for, could not be started, or ran out
    // of watches.
    bool polling() const { return m_watcher.polling(); }
    State state() const { return m_state; }
    QString unavailableReason() const { return m_unavailableReason; }
    QString directory() const { return m_directory; }
    QList<Wallpaper> wallpapers() const { return m_model.wallpapers(); }
    QAbstractItemModel* wallpaperModel() { return &m_model; }
    // Empty, or a usable wallpaper in `wallpapers`.
    QString selectedPath() const { return m_selection; }
    Wallpaper selectedWallpaper() const;
    // Every value the selected wallpaper takes by key (Wallpaper::defaultValues), the position keys
    // included: the user's value, or its default. Empty when nothing is selected.
    QVariantMap propertyValues() const;

    // Reads the folder again. The current list stays until the new one replaces it. A scan
    // already running is finished and published first, then one more follows it however many
    // calls came meanwhile.
    Q_INVOKABLE void rescan();
    // False, changing nothing, unless `filePath` is a usable wallpaper. Choosing another
    // wallpaper drops the values set on the previous one.
    Q_INVOKABLE bool select(const QString& filePath);
    Q_INVOKABLE void clearSelection();
    // Selects `filePath` once a scan that began after this call lists it usable, or now if the
    // list already does. A scan that holds the folder back does not count. Given up when the scan
    // that counts does not list it usable, and by select() or clearSelection().
    Q_INVOKABLE void reveal(const QString& filePath);
    // False, changing nothing, unless the selected wallpaper takes `key` and it can hold `value`
    // (Wallpaper::accept).
    Q_INVOKABLE bool setPropertyValue(const QString& key, const QVariant& value);
    // Every value at once, one change: false, changing nothing, if any is refused.
    Q_INVOKABLE bool setPropertyValues(const QVariantMap& values);
    Q_INVOKABLE void resetPropertyValues();

signals:
    void stateChanged();
    void wallpapersChanged();
    void selectionChanged();
    void propertyValuesChanged();

private:
    // YesThenAgain: another scan is owed once this one is published.
    enum class Scanning { No, Yes, YesThenAgain };
    // A reveal() waiting for a scan to begin, then for that scan's result.
    struct Reveal {
        QString filePath;
        bool scanBegun = false;
    };

    // The one writer of state, reason and list. A selection the new list no longer holds as
    // usable is dropped, with its values; an edit the new version cannot hold is dropped for good.
    void settle(State state, QString reason, QList<Wallpaper> wallpapers);
    // `scan` with each folder named in `held` showing the row it had, or none.
    QList<Wallpaper> holdBack(QList<Wallpaper> scan, const QSet<QByteArray>& held) const;
    // Emits whichever of selectionChanged and propertyValuesChanged the change caused.
    template <typename Change>
    void changeSelection(Change change);

    QString m_directory;
    State m_state = State::Loading;
    QString m_unavailableReason;
    WallpaperListModel m_model;
    QString m_selection;
    // Values the user set on the selected wallpaper, each one it accepts: checked when set, and
    // pruned by settle() whenever the wallpaper is read again.
    QVariantMap m_edits;
    std::optional<Reveal> m_reveal;
    Scanning m_scanning = Scanning::No;
    FolderWatcher m_watcher;
};

} // namespace wallpaperx
