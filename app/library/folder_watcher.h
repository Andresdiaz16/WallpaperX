// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "wpx_package.h"

#include <QByteArray>
#include <QDeadlineTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>

#include <chrono>
#include <memory>
#include <variant>

struct inotify_event;

namespace wallpaperx {

// Tells a scanner of one folder when to scan, and which items a scan may have read half-written.
//
// An item is an entry directly in the folder: each *.wpx file (Items::Files), or each folder with
// everything inside it (Items::Folders). Hidden names are neither items nor part of one: editors'
// swap files and the importer's unfinished folders stay out of it.
//
// Watched with inotify. A change schedules a scan once changes have been quiet for `settleMs`. A
// file written to holds its item until it is closed, moved or removed, or `holdMs` pass with no
// write and no process has it open for writing, which is all that ends a truncate(2) with no file
// open; the scan's owner keeps showing a held item as it was. A folder only just watched has
// reported no write made before its watch, so each of its files some process has open for writing
// is held too: a folder copied in is not read before its copy closes every file. When the event
// queue overflows, holds are kept, every watched folder is checked that way again, and a scan
// running then holds back every item. Without inotify, or once a watch cannot be placed for want
// of kernel resources, the folder is polled every `pollMs`, and a scan is due once one poll finds
// nothing further changed. Polling sees no close: an item seen changing while a scan ran, or with
// a file open for writing as it ended, is held.
//
// A *.wpx that is a link is followed, as the importer follows it: the file it names is watched like
// one in the folder, and at every scan its item is held while a process has it open for writing.
// Should that watch not be placed, the folder is still watched, and a write begun after a scan goes
// unseen until the next. A link to a file that does not exist yet brings no scan when it appears.
//
// Whether a file is open for writing is known on local file systems only (ext4, btrfs, XFS, F2FS,
// tmpfs); elsewhere nothing is held for it, and a writer pausing for longer than the quiet period
// or a poll can still be read half-way. A writer that closes a file and opens it again later
// cannot be told from one that has finished.
class FolderWatcher : public QObject {
    Q_OBJECT

public:
    enum class Items { Files, Folders };
    // Poll: never try inotify, as if it could not be started.
    enum class Watching { Inotify, Poll };

    FolderWatcher(QString directory, Items items, int settleMs, int pollMs, Watching watching, int holdMs,
                  QObject* parent = nullptr);
    ~FolderWatcher() override;

    bool polling() const { return std::holds_alternative<PollWatch>(m_watch); }
    // Call as a scan is about to list the folder: nothing that happens from here on goes
    // unreported. Cancels a scan still waiting for quiet.
    void scanStarting();
    // Call once that scan's result is in: every item it may have read half-written, each of which
    // has a scan of its own coming.
    QSet<QByteArray> scanFinished();

signals:
    void scanDue();

private:
    // The folder's own watch, with Items::Folders one per folder inside an item, and with
    // Items::Files one per file an item links to.
    struct TreeWatch {
        int wd = -1;
        // Descriptor -> path relative to the folder, for the folders inside items.
        QHash<int, QByteArray> inside;
        // Descriptor -> the names of the links to that file.
        QHash<int, QList<QByteArray>> targets;
        // Each relative path written to, or found open for writing when its folder was first
        // watched, and not since closed, moved away or removed. Lapses `holdMs` after the last
        // write unless the file is still open for writing then.
        QHash<QByteArray, QDeadlineTimer> writing;
    };
    // While the folder cannot be watched (missing, not a folder, unreadable): the nearest ancestor
    // that can be, waiting for a change to `awaited`, the next name on the way down.
    struct AncestorWatch {
        int wd = -1;
        QByteArray awaited;
    };
    // What one poll sees: how the folder opens, and every entry of every item.
    struct Observation {
        struct Entry {
            // Relative to the folder.
            QByteArray name;
            // Empty when it cannot be stat'ed.
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
    // Instead of any inotify watch, once set for the rest of the watcher's life, which can be
    // while a scan runs.
    struct PollWatch {
        Observation polled;
        // As the latest scan began, or polling did if later; a scan is owed while it differs from
        // `polled`.
        Observation scanned;
        // Files open for writing as the latest scan ended and not since found closed; a scan is
        // owed once one is.
        QSet<QByteArray> open;
    };
    using Watch = std::variant<std::monostate, TreeWatch, AncestorWatch, PollWatch>;

    // Keeps the folder's watch, and what it knows, when it is still on the path it was placed for.
    // Never called while polling.
    void placeWatch();
    // Watches `relative` and every folder inside it, adding each descriptor to `reached`. False
    // once it has switched to polling.
    bool watchTree(TreeWatch& tree, const QByteArray& relative, QSet<int>* reached = nullptr);
    void unwatchTree(TreeWatch& tree, const QByteArray& relative);
    // Every watch but the folder's own; before m_watch stops being that TreeWatch.
    void dropInsideWatches();
    // Holds each item file directly in `relative` that a process has open for writing.
    void holdOpenFiles(TreeWatch& tree, const QByteArray& relative);
    // Watches the file each linked item names, drops the watches no link names any more, and holds
    // each linked item whose file a process has open for writing.
    void watchTargets(TreeWatch& tree);
    // Whether some process, this one included, has the regular file `name` open for writing: the
    // kernel refuses a read lease exactly then. False when that cannot be known, and without
    // opening it when `name` is not a regular file.
    bool openForWriting(int dirFd, const char* name) const;
    void hold(TreeWatch& tree, const QByteArray& path);
    // For good: closing the descriptor drops every watch, and with them what they knew.
    void pollInstead(const char* failed, int error);
    void readWatchEvents();
    // Whether `event` can change what a scan lists.
    bool handle(const inotify_event& event);
    // Whether `name`, directly in the folder, can be an item.
    bool isItemName(const QByteArray& name) const;
    QDeadlineTimer holdFromNow();
    // Drops every hold whose deadline has passed, and schedules a scan if one was.
    void releaseHolds();
    QSet<QByteArray> unfinished() const;
    Observation observe() const;
    void poll();

    QByteArray m_directory;
    Items m_items;
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
    // Nothing is reported before the first scan, which reads everything anyway.
    bool m_scanned = false;
    // Every item unfinished at any point since the latest scan began, with a folder created in it
    // since, or seen changing by a poll during it; every item once events were lost. Its owner
    // never overlaps scans, so one set serves.
    QSet<QByteArray> m_unfinishedDuringScan;
};

} // namespace wallpaperx
