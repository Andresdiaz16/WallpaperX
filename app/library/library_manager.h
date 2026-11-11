// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "folder_watcher.h"
#include "wallpaper_engine.h"
#include "wpx_package.h"

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <utility>

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

// The wallpaper folders in one folder and Wallpaper Engine's Workshop items, which of them the user
// opened, and the property values they set.
//
// Scans run off the GUI thread, since every folder's wallpaper.json or project.json is read to list
// it, and one at a time; FolderWatcher says when, one on the library folder (Items::Folders) and one
// on each Workshop item folder (Items::ShallowFolders: an item's own folders are not followed). A
// wallpaper with a file being written is not read until it is finished; meanwhile a listed one keeps
// the row it had, so the selection and its values survive the rewrite. Folders present when the
// library is first read are read at once, unless a process has a file in one open for writing.
//
// The Workshop item folders are those of every library each Steam root's libraryfolders.vdf lists,
// read again by each scan and watched for changes too. A list counts as what it said the last time
// it was read whole: one that cannot be read, or that was being written while the scan read it, is
// not trusted. A scan that read a Workshop folder no watcher was on yet is followed by another.
//
// Each source fails alone: the library folder, a root's list or a Workshop folder that cannot be
// read adds a sentence to `problems` and costs only its own items. A list caught being written is
// not a problem, since a scan reading it whole follows.
class LibraryManager : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application with the folder it should read.")
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY stateChanged)
    Q_PROPERTY(QStringList problems READ problems NOTIFY stateChanged)
    Q_PROPERTY(QString directory READ directory CONSTANT)
    Q_PROPERTY(QList<wallpaperx::Wallpaper> wallpapers READ wallpapers NOTIFY wallpapersChanged)
    // The same list as a model, for a view that should keep its place when one row changes.
    Q_PROPERTY(QAbstractItemModel* wallpaperModel READ wallpaperModel CONSTANT)
    Q_PROPERTY(QString selectedPath READ selectedPath NOTIFY selectionChanged)
    Q_PROPERTY(wallpaperx::Wallpaper selectedWallpaper READ selectedWallpaper NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap propertyValues READ propertyValues NOTIFY propertyValuesChanged)
    Q_PROPERTY(QString wallpaperEngineInstall READ wallpaperEngineInstall NOTIFY wallpaperEngineInstallChanged)

public:
    // Loading: the first scan has not finished. Ready: `wallpapers` and `problems` are the latest
    // scan's. Unavailable: nothing is listed and the latest scan that read could not read the library
    // folder, or no scan has read and one threw; `unavailableReason` is every problem, one per line.
    // Steam and its Workshop failing alone never make the library Unavailable, and neither does a
    // scan that threw once one has read: it changes nothing but adds its sentence to `problems`.
    enum class State { Loading, Ready, Unavailable };
    Q_ENUM(State)

    using Watching = FolderWatcher::Watching;

    // `steamRoots`: the folders Steam may keep its libraryfolders.vdf under (defaultSteamRoots()); none
    // lists no Wallpaper Engine item.
    static constexpr int kSettleMs = 250;
    static constexpr int kPollMs = 2000;
    static constexpr int kHoldMs = 30000;
    explicit LibraryManager(QString directory, int settleMs = kSettleMs, int pollMs = kPollMs,
                            Watching watching = Watching::Inotify, int holdMs = kHoldMs, QStringList steamRoots = {},
                            QObject* parent = nullptr);

    // Whether the folder is polled: inotify was not asked for, could not be started, or ran out
    // of watches.
    bool polling() const { return m_watcher.polling(); }
    State state() const;
    // Empty unless Unavailable.
    QString unavailableReason() const;
    // A sentence for each source the latest scan that read could not read: the library folder, then
    // why the latest scan threw if it did, then the lists in root order, then the Workshop folders.
    QStringList problems() const;
    QString directory() const { return m_directory; }
    QList<Wallpaper> wallpapers() const { return m_model.wallpapers(); }
    QAbstractItemModel* wallpaperModel() { return &m_model; }
    // Empty, or a usable wallpaper in `wallpapers`.
    QString selectedPath() const { return m_selection; }
    Wallpaper selectedWallpaper() const;
    // Every value the selected wallpaper takes by key (Wallpaper::defaultValues), the position keys
    // included: the user's value, or its default. Empty when nothing is selected.
    QVariantMap propertyValues() const;
    // Wallpaper Engine's own folder, whose assets/ its scenes need, as of the latest scan; empty when
    // no Steam library has it.
    QString wallpaperEngineInstall() const { return m_wallpaperEngineInstall; }

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
    // that counts fails or does not list it usable, and by select() or clearSelection().
    Q_INVOKABLE void reveal(const QString& filePath);
    // False, changing nothing, unless the selected wallpaper takes `key` and it can hold `value`
    // (Wallpaper::accept).
    Q_INVOKABLE bool setPropertyValue(const QString& key, const QVariant& value);
    // Every value at once, one change: false, changing nothing, if any is refused.
    Q_INVOKABLE bool setPropertyValues(const QVariantMap& values);
    // Every value back to its default, Paused off included; then propertyValuesReset.
    Q_INVOKABLE void resetPropertyValues();
    // Every value replaced by what `values` holds, those the selected wallpaper does not take left at
    // their default, in one change: the values a screen showing it has, for the controls to show.
    void adoptPropertyValues(const QVariantMap& values);

signals:
    // Also when only `problems` changed.
    void stateChanged();
    void wallpapersChanged();
    void selectionChanged();
    void propertyValuesChanged();
    // Every resetPropertyValues(), after any propertyValuesChanged it caused, and also when every value
    // was at its default already.
    void propertyValuesReset();
    void wallpaperEngineInstallChanged();

private:
    // YesThenAgain: another scan is owed once this one is published.
    enum class Scanning { No, Yes, YesThenAgain };
    // A reveal() waiting for a scan to begin, then for that scan's result.
    struct Reveal {
        QString filePath;
        bool scanBegun = false;
    };

    struct Problems {
        // Why the library folder could not be read, if it could not.
        QString library;
        // The lists', in root order, then the Workshop folders'.
        QStringList steam;
        friend bool operator==(const Problems&, const Problems&) = default;
    };
    // What one scan read; defined beside rescan().
    struct Scan;

    // Off the GUI thread: the library folder, each root's list, and the Workshop folders of every
    // library either `trusted` (each root's list as last read whole) or the new read names.
    static Scan readSources(const QString& directory, const QList<std::pair<QString, QStringList>>& trusted,
                            const QList<Wallpaper>& previous);
    // The scan's result, or why it threw, made the published one.
    void publish(std::expected<Scan, QString> scan);
    // The one writer of problems and list. A selection the new list no longer holds as usable is
    // dropped, with its values; an edit the new version cannot hold is dropped for good.
    void settle(std::optional<Problems> problems, QString scanFailure, QList<Wallpaper> wallpapers);
    // `scan` with each wallpaper whose path is in `held` showing the row it had, or none.
    QList<Wallpaper> holdBack(QList<Wallpaper> scan, const QSet<QString>& held) const;
    // One watcher on each of `itemFolders`, and none on any other.
    void watchItemFolders(const QStringList& itemFolders);
    // Every watcher's scanStarting() and scanFinished(); the second as the paths of wallpapers and
    // of libraryfolders.vdf files.
    void scanStarting();
    QSet<QString> scanFinished();
    // Emits whichever of selectionChanged and propertyValuesChanged the change caused.
    template <typename Change>
    void changeSelection(Change change);

    QString m_directory;
    // The latest scan that read; none until one has.
    std::optional<Problems> m_problems;
    // Why the latest scan published threw; empty when it read.
    QString m_scanFailure;
    WallpaperListModel m_model;
    QString m_selection;
    // Values the user set on the selected wallpaper, each one it accepts: checked when set, and
    // pruned by settle() whenever the wallpaper is read again.
    QVariantMap m_edits;
    std::optional<Reveal> m_reveal;
    Scanning m_scanning = Scanning::No;
    int m_settleMs;
    int m_pollMs;
    Watching m_watching;
    int m_holdMs;
    FolderWatcher m_watcher;
    QStringList m_steamRoots;
    // What each root's libraryfolders.vdf listed the last time it was read whole.
    QHash<QString, QStringList> m_steamLibraries;
    // By root: on its steamapps folder, for its libraryfolders.vdf.
    std::map<QString, std::unique_ptr<FolderWatcher>> m_libraryListWatchers;
    // By folder: the Workshop item folders of the latest scan published.
    std::map<QString, std::unique_ptr<FolderWatcher>> m_itemFolderWatchers;
    QString m_wallpaperEngineInstall;
};

} // namespace wallpaperx
