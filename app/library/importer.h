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

#include <atomic>
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
    // One sentence per import that did not end in the library, until dismissed.
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
    struct Job {
        QString path;
        // For a file the drop folder's scan found, and the user has not asked for since: it as
        // listed. Empty for one the user asked for, wherever it is.
        std::optional<DroppedKey> dropped;
    };
    enum class Scanning { No, Yes, YesThenAgain };

    static DroppedKey keyOf(const FileStamp& stamp);
    void enqueue(Job job);
    void runNext();
    void finish(const Job& job, const ImportResult& result);
    // Once while it is listed: a failed drop-folder file is retried at every scan.
    void addProblem(const QString& problem);
    void scanDropFolder();
    // The running or queued import of the drop folder's file `key`, whoever asked for it.
    Job* jobFor(const DroppedKey& key);
    void loadRecord();
    void saveRecord() const;

    QString m_dropDirectory;
    QString m_wallpapersDirectory;
    QList<Job> m_queue;
    std::optional<Job> m_running;
    std::shared_ptr<std::atomic<bool>> m_cancelled = std::make_shared<std::atomic<bool>>(false);
    QStringList m_problems;
    QStringList m_notices;
    // The drop folder's files already imported, refused or found in the library, and still there.
    QList<DroppedKey> m_record;
    Scanning m_scanning = Scanning::No;
    FolderWatcher m_watcher;
};

} // namespace wallpaperx
