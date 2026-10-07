// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "folder_watcher.h"
#include "wpx_package.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <dirent.h>

#include <atomic>
#include <expected>
#include <memory>
#include <optional>

namespace wallpaperx {

// What became of one .wpx.
struct ImportResult {
    enum class Outcome {
        Imported,
        // The same bytes were imported before, and that folder is still usable.
        AlreadyPresent,
        // The package cannot be imported as it is.
        Refused,
        // This attempt could not finish (no space, no permission, stopped); another may.
        Failed,
    };
    Outcome outcome = Outcome::Failed;
    // Imported and AlreadyPresent: the wallpaper's folder.
    QString folderPath;
    // Refused and Failed: a sentence for the user.
    QString problem;
    // The .wpx as it was opened; nothing when it could not be.
    std::optional<FileStamp> sourceStamp;
    // Imported: the wallpaper's title.
    QString title;
};

// A real wallpaper is a manifest, a preview, one asset and perhaps a few textures; this bounds
// the files a zip bomb can create.
inline constexpr int kMaxPackageEntries = 4096;
// Several minutes of 4K video fit; a zip bomb cannot fill the disk.
inline constexpr qint64 kMaxUnpackedBytes = qint64(4) << 30;
// Folders inside folders. Removing or flushing an unpacked tree holds a descriptor per level, so an
// archive nesting thousands deep would leave a folder that can never be removed.
inline constexpr int kMaxFolderDepth = 32;
// Kept in each folder it creates, so a second import of the same bytes is recognised.
inline constexpr const char* kImportRecordName = ".wallpaperx-source";

// Unpacks the .wpx at `archivePath` into a new folder in `wallpapersDirectory`, never touching
// the .wpx. It is unpacked into a hidden folder there, checked by readWallpaperFolder, then
// renamed into place, so a scan never sees it half-made. The folder is named by
// contentFolderName: the same bytes always get the same name, which is how a re-import is
// recognised. A folder named any other way (a Steam Workshop id) needs only another name at the
// rename. Blocks; for a worker thread. `cancelled` stops it between reads and writes.
ImportResult unpackPackage(const QString& archivePath, const QString& wallpapersDirectory,
                           const std::atomic<bool>& cancelled);
// The same, for the .wpx `name` relative to the open directory `folder` (or AT_FDCWD); `archivePath`
// only names it in messages.
ImportResult unpackPackage(int folder, const QByteArray& name, const QString& archivePath,
                           const QString& wallpapersDirectory, const std::atomic<bool>& cancelled);

// Ten lower-case letters from the archive's SHA-256, a different part of it for each `attempt`
// (0 to 3), taken when an earlier name belongs to other bytes. Letters only, so never a Workshop
// id, which is a number.
QByteArray contentFolderName(const QByteArray& sha256, int attempt);

// Imports .wpx files into the library: those the user names, and each one that finishes arriving
// in the drop folder. A dropped file is left where it is; the drop folder's .auto-imported keeps
// which of its files were dealt with, by any route, so none is imported again at the next start or
// scan. Imports run off the GUI thread, one at a time.
class Importer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application with the folders it imports from and into.")
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // One sentence per import that did not end in the library, and one at each scan while the drop
    // folder's record cannot be read; until dismissed.
    Q_PROPERTY(QStringList problems READ problems NOTIFY problemsChanged)
    // One sentence per wallpaper imported from the drop folder, until dismissed.
    Q_PROPERTY(QStringList notices READ notices NOTIFY noticesChanged)
    Q_PROPERTY(QString dropDirectory READ dropDirectory CONSTANT)

public:
    using Watching = FolderWatcher::Watching;

    Importer(QString dropDirectory, QString wallpapersDirectory, int settleMs = 250, int pollMs = 2000,
             Watching watching = Watching::Inotify, int holdMs = 30000, QObject* parent = nullptr);
    // Stops a running import, which removes what it unpacked.
    ~Importer() override;

    bool busy() const { return m_running.has_value() || !m_queue.isEmpty(); }
    QStringList problems() const { return m_problems; }
    QStringList notices() const { return m_notices; }
    QString dropDirectory() const { return m_dropDirectory; }

    // A file URL, as a file dialog gives it.
    Q_INVOKABLE void importFile(const QUrl& file);
    void importPath(const QString& path);
    Q_INVOKABLE void dismissProblems();
    Q_INVOKABLE void dismissNotices();
    // Reads the drop folder now, importing each file not yet dealt with, a failed one included.
    // The drop folder is neither listed nor watched before the first call.
    Q_INVOKABLE void rescan();

signals:
    void busyChanged();
    void problemsChanged();
    void noticesChanged();
    // An import the user asked for ended with the wallpaper in the library, new or already there.
    void imported(const QString& folderPath);

private:
    // What identifies a dropped file's bytes across restarts. No device: btrfs numbers it anew
    // at each mount.
    struct DroppedKey {
        quint64 inode = 0;
        qint64 size = 0;
        qint64 modifiedNs = 0;
        friend bool operator==(const DroppedKey&, const DroppedKey&) = default;
    };
    // Which folder a drop folder is, across a remount: its filesystem's f_fsid, which ext4 and btrfs
    // take from their UUID, and the folder's inode. Not st_dev, for the reason DroppedKey has
    // none; an f_fsid that is only the device number counts as no identity.
    struct FolderIdentity {
        quint64 fsid = 0;
        quint64 inode = 0;
        friend bool operator==(const FolderIdentity&, const FolderIdentity&) = default;
    };
    // A key the record of `folder` owes. No folder when it could not be told: the drop folder could
    // not be opened, or its filesystem reports no f_fsid (FUSE, some network mounts) or only the
    // device number (XFS, FAT, exFAT, f2fs). Every folder speaks for such a debt, so there a
    // different volume mounted at the path still settles it.
    struct Owed {
        DroppedKey key;
        std::optional<FolderIdentity> folder;
        friend bool operator==(const Owed&, const Owed&) = default;
    };
    struct Job {
        QString path;
        // Where a scan listed it, if one did: that folder, still open, the name in it and the file's
        // key as listed. Its bytes are read and its key recorded through that folder, whatever is at
        // the drop path by then.
        struct Listed {
            std::shared_ptr<DIR> folder;
            QByteArray name;
            DroppedKey key;
        };
        std::optional<Listed> listedIn;
        // Who is told how it ended: a notice for a file only the scan found, imported() for one
        // the user asked for, the scan's own included.
        enum class Asker { Scan, User };
        Asker asker = Asker::User;
    };
    enum class Scanning { No, Yes, YesThenAgain };

    static DroppedKey keyOf(const FileStamp& stamp);
    // `folder`'s identity, or nothing where it cannot be told.
    static std::optional<FolderIdentity> identityOf(int folder);
    // Whether a listing or record of the folder `folder` speaks for `owed`.
    static bool owedTo(const Owed& owed, const std::optional<FolderIdentity>& folder);
    void enqueue(Job job);
    void runNext();
    void finish(const Job& job, const ImportResult& result);
    // Once while it is listed: a failed drop-folder file is retried at every scan.
    void addProblem(const QString& problem);
    void scanDropFolder();
    // The running or queued import of the drop folder's file `key`, whoever asked for it.
    Job* jobFor(const DroppedKey& key);
    // The drop folder, opened so its record is read from and written to the one folder however the
    // path changes meanwhile. Otherwise unread: no sentence when nothing is at the path, the
    // sentence for the user when what is there cannot be opened.
    std::expected<UniqueFd, std::optional<QString>> openDropFolder() const;
    // The files in `folder`, the drop folder as opened, already imported, refused or found in the
    // library, and still there, read afresh at each use: a copy kept between uses would outlive a
    // volume mounted or unmounted at the drop folder, and import its files again. An empty list when
    // the folder has no record yet. Otherwise unread: no sentence when the folder itself is missing,
    // the sentence for the user when the record cannot be read. While it is unread no drop-folder
    // file is imported by a scan and the record is not written, since either could import again a
    // wallpaper the user deleted.
    std::expected<QList<DroppedKey>, std::optional<QString>> loadRecordIn(int folder) const;
    // Writes `record` back into `folder`, the folder it was read from, whose identity the caller read
    // as `identity`, and settles it. Whether it was written; logs a failure.
    bool saveRecord(int folder, const std::optional<FolderIdentity>& identity, const QList<DroppedKey>& record);
    // Drops the debts owed to the folder `identity` that `record`, as read from or written to it, holds.
    void settle(const std::optional<FolderIdentity>& identity, const QList<DroppedKey>& record);

    QString m_dropDirectory;
    QString m_wallpapersDirectory;
    QList<Job> m_queue;
    std::optional<Job> m_running;
    std::shared_ptr<std::atomic<bool>> m_cancelled = std::make_shared<std::atomic<bool>>(false);
    QStringList m_problems;
    QStringList m_notices;
    Scanning m_scanning = Scanning::No;
    // Keys of drop-folder files that finished importing but are not in their folder's record yet,
    // because reading or writing it failed: without them the next scan would import those files
    // again. Added to the record of a listing of their own folder that shows the file, and to the
    // record finish() next writes there; kept until a record of their own folder is read or written
    // holding them, or a listing of their own folder no longer shows the file. Another folder at the
    // path -- a volume mounted or unmounted there -- neither uses nor settles them.
    QList<Owed> m_unrecorded;
    // Keys finish() has recorded or owed since the latest listing began. A listing taken before the
    // file finished may not show it, so these are not pruned as gone, from the record or the debts.
    QList<DroppedKey> m_finishedSinceListing;
    FolderWatcher m_watcher;
};

} // namespace wallpaperx
