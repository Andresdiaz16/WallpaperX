// SPDX-License-Identifier: GPL-2.0-or-later

#include "importer.h"

#include "current_exception.h"
#include "unique_fd.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFuture>
#include <QRandomGenerator>
#include <QtConcurrent/QtConcurrentRun>

#include <zip.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace wallpaperx {

namespace {

constexpr QByteArrayView kTemporaryPrefix = ".import-";
constexpr const char* kRecordFileName = ".auto-imported";
// Left free on the disk after an import.
constexpr qint64 kSpareBytes = qint64(256) << 20;
constexpr int kNameAttempts = 4;

using Listing = std::unique_ptr<DIR, int (*)(DIR*)>;

QString systemError(int error)
{
    return qt_error_string(error);
}

ImportResult refused(QString why)
{
    return {ImportResult::Outcome::Refused, {}, std::move(why), {}};
}

ImportResult failed(QString why)
{
    return {ImportResult::Outcome::Failed, {}, std::move(why), {}};
}

bool writeAll(int fd, const char* data, qint64 size)
{
    while (size > 0) {
        const ssize_t done = ::write(fd, data, size_t(size));
        if (done < 0 && errno == EINTR) {
            continue;
        }
        if (done <= 0) {
            // A write of nothing sets no errno, and callers report it.
            if (done == 0) {
                errno = EIO;
            }
            return false;
        }
        data += done;
        size -= done;
    }
    return true;
}

QList<QByteArray> entryNames(int dirFd)
{
    QList<QByteArray> names;
    const int own = ::fcntl(dirFd, F_DUPFD_CLOEXEC, 0);
    const Listing listing(own >= 0 ? ::fdopendir(own) : nullptr, ::closedir);
    if (!listing) {
        if (own >= 0) {
            ::close(own);
        }
        return names;
    }
    while (const dirent* entry = ::readdir(listing.get())) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
            names.append(QByteArray(entry->d_name));
        }
    }
    return names;
}

// Removes `name` in `parentFd` with everything inside it, following no link. False, with errno
// set, when it is still there.
bool removeTree(int parentFd, const QByteArray& name)
{
    if (const UniqueFd dir(::openat(parentFd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)); dir) {
        for (const QByteArray& inside : entryNames(dir.get())) {
            if (::unlinkat(dir.get(), inside.constData(), 0) != 0 && errno == EISDIR) {
                removeTree(dir.get(), inside);
            }
        }
    }
    return ::unlinkat(parentFd, name.constData(), AT_REMOVEDIR) == 0;
}

void removeImport(int parentFd, const QByteArray& name)
{
    if (!removeTree(parentFd, name)) {
        const int error = errno;
        qWarning("wallpaperx: the unfinished import %s cannot be removed (%s)", name.constData(), std::strerror(error));
    }
}

// Folders an import left behind when the app stopped mid-way. A running import holds a lock on its own.
void removeStaleImports(int dirFd)
{
    for (const QByteArray& name : entryNames(dirFd)) {
        if (!name.startsWith(kTemporaryPrefix)) {
            continue;
        }
        const UniqueFd stale(::openat(dirFd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (stale && ::flock(stale.get(), LOCK_EX | LOCK_NB) == 0) {
            removeImport(dirFd, name);
        }
    }
}

// Removed with everything in it unless kept.
struct TemporaryFolder {
    int parentFd = -1;
    QByteArray name;
    UniqueFd fd;

    TemporaryFolder() = default;
    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;
    ~TemporaryFolder()
    {
        if (!name.isEmpty()) {
            removeImport(parentFd, name);
        }
    }
};

// Every missing folder on the way to `path`, like mkdir -p. 0, or the errno.
int makePath(const QByteArray& path)
{
    for (qsizetype slash = path.indexOf('/', 1);; slash = path.indexOf('/', slash + 1)) {
        const QByteArray at = slash < 0 ? path : path.left(slash);
        if (::mkdir(at.constData(), 0755) != 0 && errno != EEXIST) {
            return errno;
        }
        if (slash < 0) {
            return 0;
        }
    }
}

std::optional<QByteArray> sha256Of(int fd, const std::atomic<bool>& cancelled, int& error)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    for (off_t offset = 0;;) {
        if (cancelled) {
            error = ECANCELED;
            return std::nullopt;
        }
        const ssize_t got = ::pread(fd, buffer.data(), size_t(buffer.size()), offset);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            error = errno;
            return std::nullopt;
        }
        if (got == 0) {
            return hash.result();
        }
        hash.addData(QByteArrayView(buffer.constData(), got));
        offset += got;
    }
}

struct PlannedEntry {
    zip_uint64_t index = 0;
    QString name;
    bool folder = false;
    qint64 size = 0;
};

struct Plan {
    QList<PlannedEntry> entries;
    qint64 total = 0;
};

// Every entry, checked before anything is written: where it lands, what it is, how big.
std::expected<Plan, QString> planEntries(zip_t* zip)
{
    const zip_int64_t count = zip_get_num_entries(zip, 0);
    if (count > kMaxPackageEntries) {
        return std::unexpected(QStringLiteral("It holds %1 files; a wallpaper may hold at most %2.")
                                   .arg(count)
                                   .arg(kMaxPackageEntries));
    }
    Plan plan;
    for (zip_uint64_t index = 0; index < zip_uint64_t(std::max<zip_int64_t>(count, 0)); ++index) {
        zip_stat_t st;
        zip_stat_init(&st);
        if (zip_stat_index(zip, index, 0, &st) != 0 || (st.valid & ZIP_STAT_NAME) == 0 || (st.valid & ZIP_STAT_SIZE) == 0) {
            return std::unexpected(QStringLiteral("Entry %1 of the archive cannot be read.").arg(index));
        }
        PlannedEntry entry{index, QString::fromUtf8(st.name), false, 0};
        if (entry.name.endsWith(u'/')) {
            entry.folder = true;
            entry.name.chop(1);
        }
        if (!isPlainRelativePath(entry.name)) {
            return std::unexpected(QStringLiteral("It holds \"%1\", which would land outside its folder.").arg(entry.name));
        }
        if (entry.name == QLatin1StringView(kImportRecordName)) {
            return std::unexpected(QStringLiteral("It holds %1, a name WallpaperX keeps for itself.").arg(entry.name));
        }
        zip_uint8_t system = 0;
        zip_uint32_t attributes = 0;
        if (zip_file_get_external_attributes(zip, index, 0, &system, &attributes) == 0 && system == ZIP_OPSYS_UNIX) {
            const mode_t type = (attributes >> 16) & S_IFMT;
            if (type != 0 && type != S_IFREG && type != S_IFDIR) {
                return std::unexpected(QStringLiteral("%1 is a link or a special file, which a wallpaper may not hold.")
                                           .arg(entry.name));
            }
            entry.folder = entry.folder || type == S_IFDIR;
        }
        if (entry.name.count(u'/') + (entry.folder ? 1 : 0) > kMaxFolderDepth) {
            return std::unexpected(QStringLiteral("It nests folders more than %1 deep.").arg(kMaxFolderDepth));
        }
        if (!entry.folder) {
            if (st.size > zip_uint64_t(kMaxUnpackedBytes - plan.total)) {
                return std::unexpected(QStringLiteral("It unpacks to more than %1 GiB.").arg(kMaxUnpackedBytes >> 30));
            }
            entry.size = qint64(st.size);
            plan.total += entry.size;
        }
        plan.entries.append(std::move(entry));
    }
    return plan;
}

// The folder `parts` below `root`, each missing part made. The errno on failure.
std::expected<UniqueFd, int> openFolder(int root, const QStringList& parts)
{
    UniqueFd at(::fcntl(root, F_DUPFD_CLOEXEC, 0));
    if (!at) {
        return std::unexpected(errno);
    }
    for (const QString& part : parts) {
        const QByteArray name = part.toUtf8();
        if (::mkdirat(at.get(), name.constData(), 0755) != 0 && errno != EEXIST) {
            return std::unexpected(errno);
        }
        UniqueFd next(::openat(at.get(), name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next) {
            return std::unexpected(errno);
        }
        at = std::move(next);
    }
    return at;
}

// Unpacks every planned entry below `root`. Nothing on success.
std::optional<ImportResult> extract(zip_t* zip, const Plan& plan, int root, const std::atomic<bool>& cancelled)
{
    const auto folderProblem = [](const QString& name, int error) {
        return error == ENOTDIR || error == ELOOP || error == EEXIST
                   ? refused(QStringLiteral("It holds %1 as both a file and a folder.").arg(name))
                   : failed(QStringLiteral("%1 cannot be created: %2.").arg(name, systemError(error)));
    };
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    for (const PlannedEntry& entry : plan.entries) {
        QStringList parts = entry.name.split(u'/');
        const QString leaf = entry.folder ? QString() : parts.takeLast();
        auto parent = openFolder(root, parts);
        if (!parent) {
            return folderProblem(entry.name, parent.error());
        }
        if (entry.folder) {
            continue;
        }
        const UniqueFd out(::openat(parent->get(), leaf.toUtf8().constData(),
                                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
        if (!out) {
            if (errno != EEXIST) {
                return failed(QStringLiteral("%1 cannot be created: %2.").arg(entry.name, systemError(errno)));
            }
            struct stat there {};
            const bool folder = ::fstatat(parent->get(), leaf.toUtf8().constData(), &there, AT_SYMLINK_NOFOLLOW) == 0
                                && S_ISDIR(there.st_mode);
            return folder ? folderProblem(entry.name, ENOTDIR) : refused(QStringLiteral("It holds %1 twice.").arg(entry.name));
        }
        const std::unique_ptr<zip_file_t, int (*)(zip_file_t*)> in(zip_fopen_index(zip, entry.index, 0), zip_fclose);
        if (!in) {
            return refused(QStringLiteral("%1 cannot be unpacked: %2.").arg(entry.name, QString::fromUtf8(zip_strerror(zip))));
        }
        qint64 written = 0;
        for (;;) {
            if (cancelled) {
                return failed(QStringLiteral("The import was stopped."));
            }
            const zip_int64_t got = zip_fread(in.get(), buffer.data(), zip_uint64_t(buffer.size()));
            if (got < 0) {
                return refused(QStringLiteral("%1 is damaged in the archive: %2.")
                                   .arg(entry.name, QString::fromUtf8(zip_file_strerror(in.get()))));
            }
            if (got == 0) {
                break;
            }
            // The archive's own size record is what the limits were checked against.
            written += got;
            if (written > entry.size) {
                return refused(QStringLiteral("%1 is larger than the archive says.").arg(entry.name));
            }
            if (!writeAll(out.get(), buffer.constData(), got)) {
                return failed(QStringLiteral("%1 cannot be written: %2.").arg(entry.name, systemError(errno)));
            }
        }
        if (written != entry.size) {
            return refused(QStringLiteral("%1 is damaged in the archive.").arg(entry.name));
        }
        if (::fsync(out.get()) != 0) {
            return failed(QStringLiteral("%1 cannot be written: %2.").arg(entry.name, systemError(errno)));
        }
    }
    return std::nullopt;
}

// Flushes every folder below `root` and `root` itself, so a folder renamed into place after a
// crash holds what it names.
void syncFolders(int root)
{
    for (const QByteArray& name : entryNames(root)) {
        if (const UniqueFd inside(::openat(root, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)); inside) {
            syncFolders(inside.get());
        }
    }
    ::fsync(root);
}

enum class Occupant { Free, Same, Other };

Occupant occupantOf(const QString& folder, const QByteArray& record)
{
    struct stat st {};
    if (::lstat(encodeFileName(folder).constData(), &st) != 0) {
        return errno == ENOENT ? Occupant::Free : Occupant::Other;
    }
    if (!S_ISDIR(st.st_mode)) {
        return Occupant::Other;
    }
    const auto kept = readWallpaperEntry(folder, QString::fromLatin1(kImportRecordName), 1024);
    // A folder the user has since broken is not what they asked to import.
    return kept && *kept == record && readWallpaperFolder(folder).usable() ? Occupant::Same : Occupant::Other;
}

struct Dropped {
    QByteArray name;
    QString path;
    FileStamp stamp;
    // Why it cannot be imported as it stands, such as a dangling link; empty when it can be.
    QString problem;
};
struct DropListing {
    // The folder listed, open, so its record is read from the same folder however the path changes.
    std::shared_ptr<DIR> folder;
    QList<Dropped> files;
};
// Unread: no sentence when the folder is missing, the reason otherwise. A missing folder is never an
// empty listing: its record, read a moment later, may be back and would be pruned of every file.
using DropScan = std::expected<DropListing, std::optional<QString>>;

// Whether opening the folder `directory` failed with `error` because nothing is at the path. ENOTDIR
// is both a missing folder (a parent is a file) and something at the path that is not a folder,
// which the user has to hear about.
bool nothingAt(const QString& directory, int error)
{
    struct stat st {};
    return error == ENOENT || (error == ENOTDIR && ::stat(encodeFileName(directory).constData(), &st) != 0);
}

// Each *.wpx in the drop folder, followed through links. An empty one is left out: it is still being
// made.
DropScan listDropFolder(const QString& directory)
{
    Listing listing(::opendir(encodeFileName(directory).constData()), ::closedir);
    if (!listing) {
        const int error = errno;
        if (nothingAt(directory, error)) {
            return std::unexpected(std::nullopt);
        }
        return std::unexpected(QStringLiteral("%1 cannot be read: %2.").arg(directory, systemError(error)));
    }
    QList<Dropped> dropped;
    while (const dirent* entry = ::readdir(listing.get())) {
        if (!isPackageFileName(entry->d_name)) {
            continue;
        }
        const QByteArray name(entry->d_name);
        const QString path = directory + u'/' + decodeFileName(name);
        struct stat st {};
        if (::fstatat(::dirfd(listing.get()), entry->d_name, &st, 0) != 0) {
            const int error = errno;
            // Gone since it was listed.
            if (struct stat link {}; ::fstatat(::dirfd(listing.get()), entry->d_name, &link, AT_SYMLINK_NOFOLLOW) != 0) {
                continue;
            }
            dropped.append({name, path, {},
                            error == ENOENT ? QStringLiteral("It is a link to a file that does not exist.")
                                            : QStringLiteral("It cannot be read: %1.").arg(systemError(error))});
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            dropped.append({name, path, {}, QStringLiteral("It is not a regular file.")});
            continue;
        }
        if (st.st_size == 0) {
            continue;
        }
        dropped.append({name, path, stampFrom(st), {}});
    }
    return DropListing{std::shared_ptr<DIR>(listing.release(), ::closedir), std::move(dropped)};
}

// Whether `path` names a package directly in `directory`, however either is spelled.
bool isInFolder(const QString& path, const QString& directory)
{
    const QFileInfo file(path);
    struct stat parent {};
    struct stat folder {};
    return isPackageFileName(encodeFileName(file.fileName()))
           && ::stat(encodeFileName(file.absolutePath()).constData(), &parent) == 0
           && ::stat(encodeFileName(directory).constData(), &folder) == 0 && parent.st_dev == folder.st_dev
           && parent.st_ino == folder.st_ino;
}

QString recordUnreadable(const QString& directory, const QString& why)
{
    return QStringLiteral("%1 cannot be read (%2), so files in the drop folder are not imported until it can.")
        .arg(directory + u'/' + QLatin1StringView(kRecordFileName), why);
}

QString notImported(const QString& path, const QString& why)
{
    return QStringLiteral("%1 could not be imported. %2").arg(QFileInfo(path).fileName(), why);
}

// Replaces `name` in the open directory `folder` with `bytes`, whole or not at all. 0, or the errno.
// The temporary file is named by hand, since mkostemp has no form relative to a directory.
int writeFileAtomically(int folder, const char* name, const QByteArray& bytes)
{
    static constexpr char kLetters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    QByteArray temporary;
    UniqueFd fd;
    for (int attempt = 0; attempt < 100 && !fd; ++attempt) {
        temporary = QByteArray(name) + '.';
        for (int i = 0; i < 6; ++i) {
            temporary += kLetters[QRandomGenerator::system()->bounded(int(sizeof kLetters - 1))];
        }
        fd.reset(::openat(folder, temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (!fd && errno != EEXIST) {
            return errno;
        }
    }
    if (!fd) {
        return EEXIST;
    }
    if (!writeAll(fd.get(), bytes.constData(), bytes.size()) || ::fsync(fd.get()) != 0
        || ::renameat(folder, temporary.constData(), folder, name) != 0) {
        const int error = errno;
        ::unlinkat(folder, temporary.constData(), 0);
        return error;
    }
    return 0;
}

} // namespace

QByteArray contentFolderName(const QByteArray& sha256, int attempt)
{
    quint64 bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits = (bits << 8) | quint8(sha256.at(attempt * 8 + i));
    }
    QByteArray name(10, Qt::Uninitialized);
    for (char& letter : name) {
        letter = char('a' + bits % 26);
        bits /= 26;
    }
    return name;
}

ImportResult unpackPackage(const QString& archivePath, const QString& wallpapersDirectory,
                           const std::atomic<bool>& cancelled)
{
    return unpackPackage(AT_FDCWD, encodeFileName(archivePath), archivePath, wallpapersDirectory, cancelled);
}

ImportResult unpackPackage(int folder, const QByteArray& name, const QString& archivePath,
                           const QString& wallpapersDirectory, const std::atomic<bool>& cancelled)
{
    const auto opened = openRegularFile(folder, name.constData(), O_RDONLY);
    if (!opened) {
        return opened.error() == kNotRegularFile
                   ? refused(QStringLiteral("It is not a regular file."))
                   : failed(QStringLiteral("It cannot be opened: %1.").arg(systemError(opened.error())));
    }
    const UniqueFd& source = *opened;
    struct stat sourceStat {};
    if (::fstat(source.get(), &sourceStat) != 0) {
        return failed(QStringLiteral("It cannot be opened: %1.").arg(systemError(errno)));
    }
    const FileStamp sourceStamp = stampFrom(sourceStat);
    const auto withStamp = [&sourceStamp](ImportResult result) {
        result.sourceStamp = sourceStamp;
        return result;
    };

    int error = 0;
    const auto hash = sha256Of(source.get(), cancelled, error);
    if (!hash) {
        return withStamp(failed(error == ECANCELED ? QStringLiteral("The import was stopped.")
                                                   : QStringLiteral("It cannot be read: %1.").arg(systemError(error))));
    }
    const QByteArray record = "sha256:" + hash->toHex() + '\n';

    // The same rules the library applies, before anything is written.
    if (const Wallpaper checked = readWallpaper(source.get(), archivePath); !checked.usable()) {
        return withStamp(refused(checked.problem));
    }

    const QString directory = QDir::cleanPath(QDir(wallpapersDirectory).absolutePath());
    const QByteArray directoryBytes = encodeFileName(directory);
    if (const int made = makePath(directoryBytes); made != 0) {
        return withStamp(failed(QStringLiteral("%1 cannot be created: %2.").arg(directory, systemError(made))));
    }
    const UniqueFd dirFd(::open(directoryBytes.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dirFd) {
        return withStamp(failed(QStringLiteral("%1 cannot be opened: %2.").arg(directory, systemError(errno))));
    }
    removeStaleImports(dirFd.get());

    const auto folderPath = [&directory](const QByteArray& name) { return directory + u'/' + QString::fromLatin1(name); };
    int attempt = 0;
    for (; attempt < kNameAttempts; ++attempt) {
        const Occupant occupant = occupantOf(folderPath(contentFolderName(*hash, attempt)), record);
        if (occupant == Occupant::Same) {
            return withStamp({ImportResult::Outcome::AlreadyPresent, folderPath(contentFolderName(*hash, attempt)), {}, {}});
        }
        if (occupant == Occupant::Free) {
            break;
        }
    }
    if (attempt == kNameAttempts) {
        return withStamp(failed(QStringLiteral("Every name for it in %1 is taken by another folder.").arg(directory)));
    }

    const int archiveFd = ::fcntl(source.get(), F_DUPFD_CLOEXEC, 0);
    if (archiveFd < 0) {
        return withStamp(failed(QStringLiteral("It cannot be opened: %1.").arg(systemError(errno))));
    }
    int zipError = 0;
    const std::unique_ptr<zip_t, void (*)(zip_t*)> zip(zip_fdopen(archiveFd, 0, &zipError), zip_discard);
    if (!zip) {
        ::close(archiveFd);
        return withStamp(refused(QStringLiteral("It cannot be read as a ZIP archive.")));
    }
    const auto plan = planEntries(zip.get());
    if (!plan) {
        return withStamp(refused(plan.error()));
    }
    if (struct statvfs space {}; ::fstatvfs(dirFd.get(), &space) == 0
                                 && qint64(space.f_bavail) * qint64(space.f_frsize) < plan->total + kSpareBytes) {
        return withStamp(failed(QStringLiteral("There is not enough free space in %1: it needs %2 MiB.")
                                    .arg(directory)
                                    .arg((plan->total >> 20) + 1)));
    }

    TemporaryFolder temporary;
    temporary.parentFd = dirFd.get();
    QByteArray pattern = directoryBytes + '/' + kTemporaryPrefix.toByteArray() + "XXXXXX";
    if (::mkdtemp(pattern.data()) == nullptr) {
        return withStamp(failed(QStringLiteral("A folder cannot be made in %1: %2.").arg(directory, systemError(errno))));
    }
    temporary.name = pattern.mid(directoryBytes.size() + 1);
    temporary.fd.reset(::openat(dirFd.get(), temporary.name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!temporary.fd || ::flock(temporary.fd.get(), LOCK_EX) != 0) {
        return withStamp(failed(QStringLiteral("A folder cannot be made in %1: %2.").arg(directory, systemError(errno))));
    }

    if (auto problem = extract(zip.get(), *plan, temporary.fd.get(), cancelled)) {
        return withStamp(std::move(*problem));
    }
    const UniqueFd recordFd(::openat(temporary.fd.get(), kImportRecordName, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
    if (!recordFd || !writeAll(recordFd.get(), record.constData(), record.size()) || ::fsync(recordFd.get()) != 0) {
        return withStamp(failed(QStringLiteral("%1 cannot be written: %2.").arg(QLatin1StringView(kImportRecordName), systemError(errno))));
    }
    syncFolders(temporary.fd.get());

    // What the library will read, read the way it will.
    const Wallpaper unpacked = readWallpaperFolder(folderPath(temporary.name));
    if (!unpacked.usable()) {
        return withStamp(refused(unpacked.problem));
    }

    for (; attempt < kNameAttempts; ++attempt) {
        const QByteArray name = contentFolderName(*hash, attempt);
        int moved = ::renameat2(dirFd.get(), temporary.name.constData(), dirFd.get(), name.constData(), RENAME_NOREPLACE);
        // A file system without RENAME_NOREPLACE, or a sandbox refusing renameat2 (ENOSYS, EPERM): a
        // plain rename still refuses a folder with anything in it.
        if (moved != 0 && (errno == EINVAL || errno == ENOSYS || errno == EPERM)) {
            moved = ::renameat(dirFd.get(), temporary.name.constData(), dirFd.get(), name.constData());
        }
        if (moved == 0) {
            temporary.name.clear();
            ::fsync(dirFd.get());
            return withStamp({ImportResult::Outcome::Imported, folderPath(name), {}, {}, unpacked.title});
        }
        if (errno != EEXIST && errno != ENOTEMPTY) {
            return withStamp(failed(QStringLiteral("It cannot be moved into %1: %2.").arg(directory, systemError(errno))));
        }
        // Taken since it was looked at: the same import finishing elsewhere, or another folder.
        if (occupantOf(folderPath(name), record) == Occupant::Same) {
            return withStamp({ImportResult::Outcome::AlreadyPresent, folderPath(name), {}, {}});
        }
    }
    return withStamp(failed(QStringLiteral("Every name for it in %1 is taken by another folder.").arg(directory)));
}

Importer::Importer(QString dropDirectory, QString wallpapersDirectory, int settleMs, int pollMs, Watching watching,
                   int holdMs, QObject* parent)
    : QObject(parent)
    , m_dropDirectory(QDir::cleanPath(QDir(dropDirectory).absolutePath()))
    , m_wallpapersDirectory(QDir::cleanPath(QDir(wallpapersDirectory).absolutePath()))
    , m_watcher(m_dropDirectory, FolderWatcher::Items::Files, settleMs, pollMs, watching, holdMs)
{
    connect(&m_watcher, &FolderWatcher::scanDue, this, &Importer::scanDropFolder);
}

Importer::~Importer()
{
    m_cancelled->store(true);
}

void Importer::importFile(const QUrl& file)
{
    const auto path = localPathOfFileUrl(file);
    if (!path) {
        addProblem(QStringLiteral("%1 is not a file on this computer, so it cannot be imported.").arg(file.toDisplayString()));
        return;
    }
    importPath(decodeFileName(*path));
}

void Importer::importPath(const QString& path)
{
    // The drop folder's scan may have queued this file already; it becomes the user's import.
    if (struct stat st {}; isInFolder(path, m_dropDirectory) && ::stat(encodeFileName(path).constData(), &st) == 0) {
        if (Job* queued = jobFor(keyOf(stampFrom(st)))) {
            queued->asker = Job::Asker::User;
            return;
        }
    }
    enqueue({path, std::nullopt, Job::Asker::User});
}

void Importer::addProblem(const QString& problem)
{
    if (!m_problems.contains(problem)) {
        m_problems.append(problem);
        emit problemsChanged();
    }
}

void Importer::dismissProblems()
{
    if (!m_problems.isEmpty()) {
        m_problems.clear();
        emit problemsChanged();
    }
}

void Importer::dismissNotices()
{
    if (!m_notices.isEmpty()) {
        m_notices.clear();
        emit noticesChanged();
    }
}

void Importer::rescan()
{
    scanDropFolder();
}

Importer::DroppedKey Importer::keyOf(const FileStamp& stamp)
{
    return {stamp.inode, stamp.size, stamp.modifiedNs};
}

std::optional<Importer::FolderIdentity> Importer::identityOf(int folder)
{
    struct statfs fs {};
    struct stat st {};
    if (::fstatfs(folder, &fs) != 0 || ::fstat(folder, &st) != 0) {
        return std::nullopt;
    }
    quint64 fsid = 0;
    static_assert(sizeof(fs.f_fsid) == sizeof(fsid));
    std::memcpy(&fsid, &fs.f_fsid, sizeof(fsid));
    // Zero is a filesystem that sets none, which would make every such folder the same one. XFS, FAT,
    // exFAT and f2fs put the device number in the first word, encoded as the kernel's huge_encode_dev,
    // and a remount can change that.
    const unsigned devMajor = major(st.st_dev);
    const unsigned devMinor = minor(st.st_dev);
    const quint32 device = (devMinor & 0xffu) | (devMajor << 8) | ((devMinor & ~0xffu) << 12);
    if (fsid == 0 || (quint32(fs.f_fsid.__val[1]) == 0 && quint32(fs.f_fsid.__val[0]) == device)) {
        return std::nullopt;
    }
    return FolderIdentity{fsid, st.st_ino};
}

bool Importer::owedTo(const Owed& owed, const std::optional<FolderIdentity>& folder)
{
    return !owed.folder || owed.folder == folder;
}

void Importer::enqueue(Job job)
{
    const bool wasBusy = busy();
    m_queue.append(std::move(job));
    runNext();
    if (busy() != wasBusy) {
        emit busyChanged();
    }
}

void Importer::runNext()
{
    if (m_running || m_queue.isEmpty()) {
        return;
    }
    m_running = m_queue.takeFirst();
    QtConcurrent::run([path = m_running->path, listed = m_running->listedIn, directory = m_wallpapersDirectory,
                       cancelled = m_cancelled] {
        return listed ? unpackPackage(::dirfd(listed->folder.get()), listed->name, path, directory, *cancelled)
                      : unpackPackage(path, directory, *cancelled);
    })
        .onFailed([] { return failed(QStringLiteral("Importing failed: %1.").arg(describeCurrentException())); })
        .then(this, [this](const ImportResult& result) {
            const bool wasBusy = busy();
            const Job job = *std::exchange(m_running, std::nullopt);
            finish(job, result);
            runNext();
            if (busy() != wasBusy) {
                emit busyChanged();
            }
        });
}

void Importer::finish(const Job& job, const ImportResult& result)
{
    const bool scanOnly = job.asker == Job::Asker::Scan;
    switch (result.outcome) {
    case ImportResult::Outcome::Imported:
        if (scanOnly) {
            m_notices.append(QStringLiteral("Imported %1.").arg(result.title));
            emit noticesChanged();
        } else {
            emit imported(result.folderPath);
        }
        break;
    case ImportResult::Outcome::AlreadyPresent:
        if (!scanOnly) {
            emit imported(result.folderPath);
        }
        break;
    case ImportResult::Outcome::Refused:
    case ImportResult::Outcome::Failed:
        addProblem(notImported(job.path, result.problem));
        break;
    }
    // A failure may pass (space, permissions), so that file is tried again at the next scan; and a
    // name that was never opened is judged again then from what is there.
    if (result.outcome == ImportResult::Outcome::Failed || !result.sourceStamp) {
        return;
    }
    // The file as opened, which may be newer than as listed; a newer version is new bytes.
    const DroppedKey key = keyOf(*result.sourceStamp);
    const auto addOnce = [&key](QList<DroppedKey>& keys) {
        if (!keys.contains(key)) {
            keys.append(key);
        }
    };
    const auto owe = [this, &key, &addOnce](const std::optional<FolderIdentity>& folder) {
        if (const Owed owed{key, folder}; !m_unrecorded.contains(owed)) {
            m_unrecorded.append(owed);
        }
        addOnce(m_finishedSinceListing);
    };
    // The folder the scan listed it in, whatever is at the path now; for a file only the user named,
    // the drop folder if it is in it.
    UniqueFd opened;
    int folder = -1;
    if (job.listedIn) {
        folder = ::dirfd(job.listedIn->folder.get());
    } else if (isInFolder(job.path, m_dropDirectory)) {
        auto dropFolder = openDropFolder();
        if (!dropFolder) {
            owe(std::nullopt);
            if (dropFolder.error()) {
                addProblem(*dropFolder.error());
            }
            return;
        }
        opened = std::move(*dropFolder);
        folder = opened.get();
    } else {
        return;
    }
    const std::optional<FolderIdentity> identity = identityOf(folder);
    auto record = loadRecordIn(folder);
    if (!record) {
        owe(identity);
        if (record.error()) {
            addProblem(*record.error());
        }
        return;
    }
    settle(identity, *record);
    // This folder's earlier debts go in with it, so a write that works settles them too.
    bool grew = false;
    const auto addToRecord = [&record, &grew](const DroppedKey& owedKey) {
        if (!record->contains(owedKey)) {
            record->append(owedKey);
            grew = true;
        }
    };
    addToRecord(key);
    for (const Owed& owed : std::as_const(m_unrecorded)) {
        if (owedTo(owed, identity)) {
            addToRecord(owed.key);
        }
    }
    if (grew && !saveRecord(folder, identity, *record)) {
        owe(identity);
        return;
    }
    addOnce(m_finishedSinceListing);
}

Importer::Job* Importer::jobFor(const DroppedKey& key)
{
    const auto holds = [this, &key](const Job& job) {
        if (job.listedIn) {
            return job.listedIn->key == key;
        }
        struct stat st {};
        return isInFolder(job.path, m_dropDirectory) && ::stat(encodeFileName(job.path).constData(), &st) == 0
               && keyOf(stampFrom(st)) == key;
    };
    if (m_running && holds(*m_running)) {
        return &*m_running;
    }
    const auto queued = std::ranges::find_if(m_queue, holds);
    return queued == m_queue.end() ? nullptr : &*queued;
}

void Importer::scanDropFolder()
{
    if (m_scanning != Scanning::No) {
        m_scanning = Scanning::YesThenAgain;
        return;
    }
    m_watcher.scanStarting();
    m_scanning = Scanning::Yes;
    m_finishedSinceListing.clear();
    const QString directory = m_dropDirectory;
    QtConcurrent::run([directory] { return listDropFolder(directory); })
        .onFailed([directory]() -> DropScan {
            return std::unexpected(QStringLiteral("Reading %1 failed: %2.").arg(directory, describeCurrentException()));
        })
        .then(this, [this](DropScan listing) {
            const QSet<QByteArray> held = m_watcher.scanFinished();
            if (listing) {
                const int folder = ::dirfd(listing->folder.get());
                const std::optional<FolderIdentity> identity = identityOf(folder);
                auto record = loadRecordIn(folder);
                if (record) {
                    settle(identity, *record);
                } else if (record.error()) {
                    addProblem(*record.error());
                }
                bool absorbed = false;
                QList<DroppedKey> present;
                for (const Dropped& file : std::as_const(listing->files)) {
                    // Not recorded, so a repaired link is imported.
                    if (!file.problem.isEmpty()) {
                        addProblem(notImported(file.path, file.problem));
                        continue;
                    }
                    const DroppedKey key = keyOf(file.stamp);
                    present.append(key);
                    if (!record || record->contains(key)) {
                        continue;
                    }
                    // Only this folder's debts: one to another folder is settled by that folder alone. A
                    // remount that changes f_fsid therefore imports the file once more.
                    if (std::ranges::any_of(m_unrecorded, [&key, &identity](const Owed& owed) {
                            return owedTo(owed, identity) && owed.key == key;
                        })) {
                        record->append(key);
                        absorbed = true;
                    } else if (!held.contains(file.name) && jobFor(key) == nullptr) {
                        enqueue({file.path, Job::Listed{listing->folder, file.name, key}, Job::Asker::Scan});
                    }
                }
                // Forgets files no longer there, so one dropped in again is imported again -- from this
                // folder's debts too, read or not, since a file that is gone needs no protection.
                const auto gone = [this, &present](const DroppedKey& key) {
                    return !present.contains(key) && !m_finishedSinceListing.contains(key);
                };
                if (record && (record->removeIf(gone) > 0 || absorbed)) {
                    saveRecord(folder, identity, *record);
                }
                m_unrecorded.removeIf(
                    [&gone, &identity](const Owed& owed) { return owedTo(owed, identity) && gone(owed.key); });
            } else if (listing.error()) {
                qWarning("wallpaperx: %s", qUtf8Printable(*listing.error()));
            }
            if (std::exchange(m_scanning, Scanning::No) == Scanning::YesThenAgain) {
                scanDropFolder();
            }
        });
}

std::expected<UniqueFd, std::optional<QString>> Importer::openDropFolder() const
{
    // The record is looked for in the folder this opened, so a missing folder is never read as a
    // folder with no record.
    UniqueFd folder(::open(encodeFileName(m_dropDirectory).constData(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!folder) {
        const int error = errno;
        if (nothingAt(m_dropDirectory, error)) {
            return std::unexpected(std::nullopt);
        }
        return std::unexpected(
            QStringLiteral("The drop folder %1 cannot be opened (%2), so the files imported from it are recorded there "
                           "once it can.")
                .arg(m_dropDirectory, systemError(error)));
    }
    return folder;
}

std::expected<QList<Importer::DroppedKey>, std::optional<QString>> Importer::loadRecordIn(int folder) const
{
    const auto unreadable = [this](const QString& why) {
        return std::expected<QList<DroppedKey>, std::optional<QString>>(std::unexpect,
                                                                        recordUnreadable(m_dropDirectory, why));
    };
    const auto fd = openRegularFile(folder, kRecordFileName, O_RDONLY);
    if (!fd) {
        if (fd.error() == ENOENT) {
            return QList<DroppedKey>{};
        }
        return unreadable(fd.error() == kNotRegularFile ? QStringLiteral("not a regular file") : systemError(fd.error()));
    }
    // A record over the cap was not written by this code, so nothing in it is trusted.
    const auto bytes = readToEnd(fd->get(), 1 << 20);
    if (!bytes) {
        return unreadable(bytes.error() == EFBIG ? QStringLiteral("larger than 1 MiB") : systemError(bytes.error()));
    }
    QList<DroppedKey> record;
    for (const QByteArray& line : bytes->split('\n')) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.size() != 3) {
            continue;
        }
        bool ok[3] = {};
        const DroppedKey key{fields[0].toULongLong(&ok[0]), fields[1].toLongLong(&ok[1]), fields[2].toLongLong(&ok[2])};
        if (ok[0] && ok[1] && ok[2] && !record.contains(key)) {
            record.append(key);
        }
    }
    return record;
}

bool Importer::saveRecord(int folder, const std::optional<FolderIdentity>& identity, const QList<DroppedKey>& record)
{
    QByteArray bytes;
    for (const DroppedKey& key : record) {
        bytes += QByteArray::number(key.inode) + ' ' + QByteArray::number(key.size) + ' '
                 + QByteArray::number(key.modifiedNs) + '\n';
    }
    if (const int error = writeFileAtomically(folder, kRecordFileName, bytes); error != 0) {
        const QByteArray path = encodeFileName(m_dropDirectory) + '/' + kRecordFileName;
        qWarning("wallpaperx: %s cannot be written (%s), so files in %s may be imported again",
                 path.constData(), std::strerror(error), encodeFileName(m_dropDirectory).constData());
        return false;
    }
    settle(identity, record);
    return true;
}

void Importer::settle(const std::optional<FolderIdentity>& identity, const QList<DroppedKey>& record)
{
    m_unrecorded.removeIf(
        [&record, &identity](const Owed& owed) { return owedTo(owed, identity) && record.contains(owed.key); });
}

} // namespace wallpaperx
