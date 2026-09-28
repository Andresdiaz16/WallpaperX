// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "wpx_package.h"

#include <QAbstractListModel>
#include <QDeadlineTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <chrono>
#include <memory>
#include <optional>
#include <variant>

struct inotify_event;

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

// The wallpapers in one folder, which of them the user opened, and the property values they set.
//
// Scans run off the GUI thread, since a package is opened and read to list it, and one at a time.
// The folder is watched with inotify, and a change is read once changes have been quiet for
// `settleMs`. A package being written is not read until it is finished: closed by the program
// writing it, moved in whole, or not written to for `holdMs`, which is all that ends a truncate(2)
// with no file open. Meanwhile a listed one keeps the row it had, so the selection and its values
// survive the rewrite. An empty file is never listed, so a package emptied and not being written
// loses its row. Packages present when the folder is first read are read at once.
//
// Without inotify, or once a watch it needs cannot be placed for want of kernel resources, the
// folder is polled every `pollMs` instead, and a change is read once one poll finds nothing
// further changed. Polling sees no close: only a package seen changing while a scan ran keeps its
// row, and a writer pausing for longer than a poll can still be read half-way.
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

    // Poll: never try inotify, as if it could not be started.
    enum class Watching { Inotify, Poll };

    explicit LibraryManager(QString directory, int settleMs = 250, int pollMs = 2000,
                            Watching watching = Watching::Inotify, int holdMs = 30000, QObject* parent = nullptr);
    ~LibraryManager() override;

    // Whether the folder is polled: inotify was not asked for, could not be started, or ran out
    // of watches.
    bool polling() const { return std::holds_alternative<PollWatch>(m_watch); }
    State state() const { return m_state; }
    QString unavailableReason() const { return m_unavailableReason; }
    QString directory() const { return m_directory; }
    QList<Wallpaper> wallpapers() const { return m_model.wallpapers(); }
    QAbstractItemModel* wallpaperModel() { return &m_model; }
    // Empty, or a usable wallpaper in `wallpapers`.
    QString selectedPath() const { return m_selection; }
    Wallpaper selectedWallpaper() const;
    // Every property of the selected wallpaper by key: the user's value, or its default. Empty
    // when nothing is selected.
    QVariantMap propertyValues() const;

    // Reads the folder again. The current list stays until the new one replaces it. A scan
    // already running is finished and published first, then one more follows it however many
    // calls came meanwhile.
    Q_INVOKABLE void rescan();
    // False, changing nothing, unless `filePath` is a usable wallpaper. Choosing another
    // wallpaper drops the values set on the previous one.
    Q_INVOKABLE bool select(const QString& filePath);
    Q_INVOKABLE void clearSelection();
    // False, changing nothing, unless the selected wallpaper has property `key` and it can hold
    // `value` (WallpaperProperty::accept).
    Q_INVOKABLE bool setPropertyValue(const QString& key, const QVariant& value);
    Q_INVOKABLE void resetPropertyValues();

signals:
    void stateChanged();
    void wallpapersChanged();
    void selectionChanged();
    void propertyValuesChanged();

private:
    // The folder's own watch: it reports every entry's changes, and which entries are being written.
    struct FolderWatch {
        int wd = -1;
        // Each name written to since this watch was placed and not since closed, moved in, moved
        // away or removed, until it is released `holdMs` after its last write.
        QHash<QByteArray, QDeadlineTimer> writing;
    };
    // While the folder cannot be watched (missing, not a folder, unreadable): the nearest ancestor
    // that can be, waiting for a change to `awaited`, the next name on the way down.
    struct AncestorWatch {
        int wd = -1;
        QByteArray awaited;
    };
    // What one poll sees: how the folder opens, and every package entry's file.
    struct Observation {
        struct Entry {
            QByteArray name;
            // Followed through a link; empty when it cannot be stat'ed.
            FileStamp stamp;
            quint32 mode = 0;
            friend bool operator==(const Entry&, const Entry&) = default;
        };
        int openError = 0;
        quint64 device = 0;
        quint64 inode = 0;
        QList<Entry> entries;
        friend bool operator==(const Observation&, const Observation&) = default;
    };
    // Instead of any inotify watch, once set for the rest of the manager's life.
    struct PollWatch {
        Observation polled;
        // As the latest scan began; a scan is owed while it differs from `polled`.
        Observation scanned;
    };
    using Watch = std::variant<std::monostate, FolderWatch, AncestorWatch, PollWatch>;
    // The target of every listed link to one file, which the folder's watch does not see.
    struct LinkWatch {
        int wd = -1;
        QList<QByteArray> names;
        // Set by a write, as FolderWatch::writing is for a name.
        std::optional<QDeadlineTimer> writing;
    };
    // YesThenAgain: another scan is owed once this one is published.
    enum class Scanning { No, Yes, YesThenAgain };

    // The one writer of state, reason and list. A selection the new list no longer holds as
    // usable is dropped, with its values; an edit the new version cannot hold is dropped for good.
    void settle(State state, QString reason, QList<Wallpaper> wallpapers);
    // Keeps the watch, and what it knows, when it is still on the path it was placed for. Never
    // called while polling.
    void placeWatch();
    // Returns whether a link's target changed between being read and being watched.
    bool watchLinks();
    // For good: closing the descriptor drops every watch, and with them what they knew.
    void pollInstead(const char* failed, int error);
    void readWatchEvents();
    // Whether `event` can change what a scan lists.
    bool handle(const inotify_event& event);
    bool handleLink(const inotify_event& event);
    // The deadline for a name or link written to now.
    QDeadlineTimer holdFromNow();
    // Drops every hold whose deadline has passed, and schedules a scan if one was.
    void releaseHolds();
    QSet<QByteArray> unfinished() const;
    // `scan` with each name in m_unfinishedDuringScan showing the row it had, or none.
    QList<Wallpaper> holdBack(QList<Wallpaper> scan) const;
    Observation observe() const;
    void poll();
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
    Scanning m_scanning = Scanning::No;
    QTimer m_settle;
    // Runs only while polling.
    QTimer m_poll;
    std::chrono::milliseconds m_hold;
    // Due no later than the earliest hold's deadline while any is held.
    QTimer m_holdExpiry;
    // -1, with no notifier, exactly when polling.
    int m_inotify = -1;
    std::unique_ptr<QSocketNotifier> m_inotifyReady;
    Watch m_watch;
    // Every name unfinished at any point since the running scan began, or seen changing by a poll
    // during it, which it may have read half-written. Scans never overlap, so one set serves.
    QSet<QByteArray> m_unfinishedDuringScan;
    QList<LinkWatch> m_linkWatches;
};

} // namespace wallpaperx
