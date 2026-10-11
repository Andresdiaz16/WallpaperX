// SPDX-License-Identifier: GPL-2.0-or-later

// app/library/importer.cpp: unpacking a .wpx into a wallpaper folder (atomic, refusing what could
// reach outside it, bounded, named from its bytes), and the Importer that imports what the user
// names and what finishes arriving in the drop folder, once, links in it included. No bus. Goals
// S15.58, S15.75-S15.81, S15.89-S15.93 in GOALS.md.

#include "importer.h"
#include "open_counter.h"
#include "wpx_fixtures.h"
#include "wpx_package.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThreadPool>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::check;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;

namespace {

// Armed by the collision check: the next renameat2 finds its target name just taken by an empty
// folder, as when another process makes one between the importer's check and its rename.
std::atomic<bool> g_takeNameFirst{false};
// While not 0, renameat2 fails with this errno without reaching the kernel: EINVAL as a file
// system without RENAME_NOREPLACE answers, ENOSYS or EPERM as a sandbox does.
std::atomic<int> g_renameat2Refused{0};
// Unfinished import folders made so far.
std::atomic<int> g_unfinishedMade{0};
// Opens of the one path armed here, by any thread: how many times an import read that package. A
// lookup of its name relative to its folder counts too when it is made with O_PATH off the thread
// that armed the count: an import of a file the scan found, read through the listed folder, as
// against the drop-folder watch's check for a writer, which runs on the main thread.
std::mutex g_countedLock;
QByteArray g_countedPath;
QByteArray g_countedFolder;  // the canonical path of the folder holding it
QByteArray g_countedName;
pthread_t g_countingThread{};
std::atomic<bool> g_counting{false};
std::atomic<int> g_countedOpens{0};

// What `dirFd` names, as /proc/self/fd says: its canonical path.
QByteArray folderOf(int dirFd) {
    char link[64];
    std::snprintf(link, sizeof link, "/proc/self/fd/%d", dirFd);
    char target[4096];
    const ssize_t n = dirFd >= 0 ? ::readlink(link, target, sizeof target) : -1;
    return n > 0 ? QByteArray(target, n) : QByteArray();
}

void noteOpen(const char* path) {
    if (path != nullptr && g_counting.load()) {
        const std::scoped_lock lock(g_countedLock);
        if (g_countedPath == path) {
            ++g_countedOpens;
        }
    }
}

void noteOpenAt(int dirFd, const char* path, int flags) {
    if (path == nullptr || path[0] == '/' || (flags & O_PATH) == 0 || !g_counting.load()) {
        return;
    }
    const std::scoped_lock lock(g_countedLock);
    if (g_countedName == path && !pthread_equal(pthread_self(), g_countingThread) && folderOf(dirFd) == g_countedFolder) {
        ++g_countedOpens;
    }
}

// Called before every openat() the binary makes, from whichever thread; null for nothing. For staging
// what happens to a folder just before a file in it is looked up.
std::atomic<void (*)(int dirFd, const char* path, int flags)> g_beforeOpenat{nullptr};

int modeOf(int flags, va_list args) {
    return ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) ? va_arg(args, int) : 0;
}

// While set, inotify_add_watch of exactly this path fails with ENOSPC without reaching the kernel,
// as when the per-user watch limit is reached; counted in g_watchesRefused.
std::mutex g_refusedWatchLock;
QByteArray g_refusedWatch;
std::atomic<bool> g_refusingWatch{false};
std::atomic<int> g_watchesRefused{0};
std::atomic<int (*)(int, const char*, uint32_t)> g_realAddWatch{nullptr};

} // namespace

extern "C" int inotify_add_watch(int fd, const char* path, uint32_t mask) noexcept {
    if (g_refusingWatch.load() && path != nullptr) {
        const std::scoped_lock lock(g_refusedWatchLock);
        if (g_refusedWatch == path) {
            ++g_watchesRefused;
            errno = ENOSPC;
            return -1;
        }
    }
    auto real = g_realAddWatch.load();
    if (real == nullptr) {
        real = reinterpret_cast<int (*)(int, const char*, uint32_t)>(::dlsym(RTLD_NEXT, "inotify_add_watch"));
        g_realAddWatch.store(real);
    }
    return real(fd, path, mask);
}

namespace {

// Refuses the watch of `path` while alive. One at a time.
class RefuseWatchOf {
public:
    explicit RefuseWatchOf(const QString& path) {
        const std::scoped_lock lock(g_refusedWatchLock);
        g_refusedWatch = QFile::encodeName(path);
        g_watchesRefused = 0;
        g_refusingWatch = true;
    }
    ~RefuseWatchOf() { g_refusingWatch = false; }
    RefuseWatchOf(const RefuseWatchOf&) = delete;
    RefuseWatchOf& operator=(const RefuseWatchOf&) = delete;
    int refused() const { return g_watchesRefused.load(); }
};

} // namespace

extern "C" int renameat2(int oldDirFd, const char* oldPath, int newDirFd, const char* newPath, unsigned int flags) noexcept {
    static const auto real = reinterpret_cast<int (*)(int, const char*, int, const char*, unsigned int)>(::dlsym(RTLD_NEXT, "renameat2"));
    if (const int refused = g_renameat2Refused.load(); refused != 0) {
        errno = refused;
        return -1;
    }
    if (g_takeNameFirst.exchange(false)) {
        ::mkdirat(newDirFd, newPath, 0755);
    }
    return real(oldDirFd, oldPath, newDirFd, newPath, flags);
}

namespace {

// Blocks the calling thread while `hold` is set, with `held` raised meanwhile; at most 20 s, so a test
// that never releases it fails on its own bounds rather than hanging the run.
void waitWhileHeld(const std::atomic<bool>& hold, std::atomic<bool>& held) {
    held = true;
    for (int waited = 0; hold.load() && waited < 20000; waited += 5) {
        ::usleep(5000);
    }
    held = false;
}

// While set, an import blocks in its mkdtemp -- its source open, nothing recorded yet -- until cleared.
std::atomic<bool> g_holdImports{false};
std::atomic<bool> g_importHeld{false};

// While not empty, open() of exactly this path with O_PATH fails with EACCES without reaching the kernel:
// a drop folder that cannot be opened.
std::mutex g_refusedOpenLock;
QByteArray g_refusedOpen;  // guarded by g_refusedOpenLock
std::atomic<bool> g_refusingOpen{false};

bool openRefused(const char* path, int flags) {
    if (!g_refusingOpen.load() || path == nullptr || (flags & O_PATH) == 0) {
        return false;
    }
    const std::scoped_lock lock(g_refusedOpenLock);
    return g_refusedOpen == path;
}

} // namespace

extern "C" char* mkdtemp(char* pattern) noexcept {
    static const auto real = reinterpret_cast<char* (*)(char*)>(::dlsym(RTLD_NEXT, "mkdtemp"));
    if (std::strstr(pattern, "/.import-") != nullptr) {
        ++g_unfinishedMade;
        if (g_holdImports.load()) {
            waitWhileHeld(g_holdImports, g_importHeld);
        }
    }
    return real(pattern);
}

extern "C" int open(const char* path, int flags, ...) {
    static const auto real = reinterpret_cast<int (*)(const char*, int, ...)>(::dlsym(RTLD_NEXT, "open"));
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    noteOpen(path);
    if (openRefused(path, flags)) {
        errno = EACCES;
        return -1;
    }
    return real(path, flags, mode);
}

extern "C" int open64(const char* path, int flags, ...) {
    static const auto real = reinterpret_cast<int (*)(const char*, int, ...)>(::dlsym(RTLD_NEXT, "open64"));
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    noteOpen(path);
    if (openRefused(path, flags)) {
        errno = EACCES;
        return -1;
    }
    return real(path, flags, mode);
}

namespace {

// While not Off, fstatfs of a folder at or under g_fakeFsidUnder (its canonical path) reports a filesystem
// ID built from that folder's own st_dev, which is the kernel's huge_encode_dev of it: Device as vfat
// measurably reports it (and XFS, exFAT, f2fs), the other two as near misses that are real IDs. Counted in
// g_fsidsFaked.
enum class FakeFsid { Off, Device, DeviceSecondWordSet, NextDevice };
std::atomic<FakeFsid> g_fakeFsid{FakeFsid::Off};
std::mutex g_fakeFsidLock;
QByteArray g_fakeFsidUnder;  // guarded by g_fakeFsidLock
std::atomic<int> g_fsidsFaked{0};

int fakingFsid(int fd, struct statfs* buf, int result) {
    const FakeFsid mode = g_fakeFsid.load();
    if (result != 0 || mode == FakeFsid::Off) {
        return result;
    }
    const QByteArray path = folderOf(fd);
    {
        const std::scoped_lock lock(g_fakeFsidLock);
        if (g_fakeFsidUnder.isEmpty() || (path != g_fakeFsidUnder && !path.startsWith(g_fakeFsidUnder + '/'))) {
            return result;
        }
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        return result;
    }
    const quint32 device = quint32(st.st_dev);
    buf->f_fsid.__val[0] = int(mode == FakeFsid::NextDevice ? device + 1 : device);
    buf->f_fsid.__val[1] = mode == FakeFsid::DeviceSecondWordSet ? 1 : 0;
    ++g_fsidsFaked;
    return result;
}

} // namespace

// Both symbols by name, whichever one <sys/statfs.h> redirects fstatfs to; the layouts are one on 64-bit.
static_assert(sizeof(struct statfs) == sizeof(struct statfs64));
extern "C" int wpxFstatfs(int fd, struct statfs* buf) noexcept __asm__("fstatfs");
extern "C" int wpxFstatfs64(int fd, struct statfs* buf) noexcept __asm__("fstatfs64");

extern "C" int wpxFstatfs(int fd, struct statfs* buf) noexcept {
    static const auto real = reinterpret_cast<int (*)(int, struct statfs*)>(::dlsym(RTLD_NEXT, "fstatfs"));
    return fakingFsid(fd, buf, real(fd, buf));
}

extern "C" int wpxFstatfs64(int fd, struct statfs* buf) noexcept {
    static const auto real = reinterpret_cast<int (*)(int, struct statfs*)>(::dlsym(RTLD_NEXT, "fstatfs64"));
    return fakingFsid(fd, buf, real(fd, buf));
}

namespace {

// While set, a worker's listing of the folder at exactly `g_heldListingPath` blocks once it has read
// the folder to its end -- the listing taken, its continuation not yet run -- until cleared. The DIR
// is recognised from its opendir, through open_counter's hook.
std::mutex g_listingLock;
QByteArray g_heldListingPath;  // guarded by g_listingLock
DIR* g_heldListingDir = nullptr;  // guarded by g_listingLock
pthread_t g_listingMain{};
std::atomic<bool> g_holdListing{false};
std::atomic<bool> g_listingHeld{false};

void noteHeldListing(const char* path, DIR* result) {
    if (result == nullptr || path == nullptr || !g_holdListing.load() || pthread_equal(pthread_self(), g_listingMain)) {
        return;
    }
    const std::scoped_lock lock(g_listingLock);
    if (g_heldListingPath == path) {
        g_heldListingDir = result;
    }
}

void holdAtListingEnd(DIR* dir) {
    if (!g_holdListing.load()) {
        return;
    }
    {
        const std::scoped_lock lock(g_listingLock);
        if (dir != g_heldListingDir) {
            return;
        }
        g_heldListingDir = nullptr;
    }
    waitWhileHeld(g_holdListing, g_listingHeld);
}

} // namespace

extern "C" struct dirent* readdir(DIR* dir) {
    static const auto real = reinterpret_cast<struct dirent* (*)(DIR*)>(::dlsym(RTLD_NEXT, "readdir"));
    struct dirent* entry = real(dir);
    if (entry == nullptr) {
        const int saved = errno;
        holdAtListingEnd(dir);
        errno = saved;
    }
    return entry;
}

extern "C" struct dirent64* readdir64(DIR* dir) {
    static const auto real = reinterpret_cast<struct dirent64* (*)(DIR*)>(::dlsym(RTLD_NEXT, "readdir64"));
    struct dirent64* entry = real(dir);
    if (entry == nullptr) {
        const int saved = errno;
        holdAtListingEnd(dir);
        errno = saved;
    }
    return entry;
}

namespace {

// Armed by the swap checks (S15.340): the first O_PATH lookup of exactly `g_swapName`, as the code
// under test spells it, is followed -- before that lookup returns -- by `g_swapFrom` renamed over
// `g_swapTo`: a name re-pointed between a type check and an open. Renamed with renameat, which
// nothing here interposes.
std::mutex g_swapLock;
QByteArray g_swapName;
QByteArray g_swapFrom;
QByteArray g_swapTo;
std::atomic<bool> g_swapArmed{false};
std::atomic<int> g_swapsDone{0};

void swapAfterLookup(const char* path, int flags) {
    if (!g_swapArmed.load() || path == nullptr || (flags & O_PATH) == 0) {
        return;
    }
    const std::scoped_lock lock(g_swapLock);
    if (g_swapArmed.load() && g_swapName == path) {
        g_swapArmed = false;
        if (::renameat(AT_FDCWD, g_swapFrom.constData(), AT_FDCWD, g_swapTo.constData()) == 0) {
            ++g_swapsDone;
        }
    }
}

int openatCounted(const char* symbol, int dirFd, const char* path, int flags, int mode) {
    using Real = int (*)(int, const char*, int, ...);
    static const auto realOpenat = reinterpret_cast<Real>(::dlsym(RTLD_NEXT, "openat"));
    static const auto realOpenat64 = reinterpret_cast<Real>(::dlsym(RTLD_NEXT, "openat64"));
    noteOpen(path);
    noteOpenAt(dirFd, path, flags);
    if (const auto before = g_beforeOpenat.load()) {
        before(dirFd, path, flags);
    }
    const int fd = (std::strcmp(symbol, "openat") == 0 ? realOpenat : realOpenat64)(dirFd, path, flags, mode);
    const int saved = errno;
    swapAfterLookup(path, flags);
    errno = saved;
    return fd;
}

// While set, renameat() onto exactly this file -- a folder's descriptor and a name in it, which is how
// the importer writes its record -- fails with ENOSPC without reaching the kernel, after the temporary
// file beside it was written: a record write failing at its last step.
std::mutex g_refusedRenameLock;
QByteArray g_refusedRenameFolder;  // canonical
QByteArray g_refusedRenameName;
std::atomic<bool> g_refusingRename{false};
std::atomic<int> g_renamesToRefuse{-1};  // how many more to refuse; every one while below 0
std::atomic<int> g_renamesRefused{0};

bool renameRefused(int newDirFd, const char* newPath) {
    if (!g_refusingRename.load() || newPath == nullptr) {
        return false;
    }
    const std::scoped_lock lock(g_refusedRenameLock);
    if (g_refusedRenameName != newPath || g_renamesToRefuse.load() == 0 || folderOf(newDirFd) != g_refusedRenameFolder) {
        return false;
    }
    if (g_renamesToRefuse.load() > 0) {
        --g_renamesToRefuse;
    }
    ++g_renamesRefused;
    return true;
}

} // namespace

// Archives are looked up with openat(O_PATH) and read through /proc/self/fd, so an import of a
// path is counted here as well as in open().
extern "C" int openat(int dirFd, const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    return openatCounted("openat", dirFd, path, flags, mode);
}

extern "C" int openat64(int dirFd, const char* path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    return openatCounted("openat64", dirFd, path, flags, mode);
}

namespace {
// Armed by S15.342: the next opendir() of exactly this path that finds nothing renames `g_returnFrom`
// to it before returning -- a drop folder that comes back while the scan that found it missing runs.
std::mutex g_returnLock;
QByteArray g_returnTo;
QByteArray g_returnFrom;
std::atomic<bool> g_returnArmed{false};
std::atomic<int> g_returnsDone{0};
pthread_t g_returnMain{};  // the thread that armed it, whose opendirs are the watcher's, never the scan's

void returnAfterMissedListing(const char* path, DIR* result) {
    if (result != nullptr || !g_returnArmed.load() || path == nullptr || pthread_equal(pthread_self(), g_returnMain)) {
        return;
    }
    const std::scoped_lock lock(g_returnLock);
    if (g_returnArmed.load() && g_returnTo == path) {
        g_returnArmed = false;
        if (::renameat(AT_FDCWD, g_returnFrom.constData(), AT_FDCWD, g_returnTo.constData()) == 0) {
            ++g_returnsDone;
        }
    }
}
} // namespace

extern "C" int renameat(int oldDirFd, const char* oldPath, int newDirFd, const char* newPath) noexcept {
    static const auto real = reinterpret_cast<int (*)(int, const char*, int, const char*)>(::dlsym(RTLD_NEXT, "renameat"));
    if (renameRefused(newDirFd, newPath)) {
        errno = ENOSPC;
        return -1;
    }
    return real(oldDirFd, oldPath, newDirFd, newPath);
}

namespace {

// Refuses renames onto `path` -- the first `count`, or every one when it is negative -- while
// alive. One at a time.
class RefuseRenameTo {
public:
    explicit RefuseRenameTo(const QString& path, int count = -1) {
        const std::scoped_lock lock(g_refusedRenameLock);
        const QFileInfo file(path);
        g_refusedRenameFolder = QFile::encodeName(file.absoluteDir().canonicalPath());
        g_refusedRenameName = QFile::encodeName(file.fileName());
        g_renamesToRefuse = count;
        g_renamesRefused = 0;
        g_refusingRename = true;
    }
    ~RefuseRenameTo() { g_refusingRename = false; }
    RefuseRenameTo(const RefuseRenameTo&) = delete;
    RefuseRenameTo& operator=(const RefuseRenameTo&) = delete;
    int refused() const { return g_renamesRefused.load(); }
};

} // namespace

namespace {

// Counts the opens of `path` while alive: by its full path, and by its name relative to its folder
// off this thread. One at a time.
class OpensOf {
public:
    explicit OpensOf(const QString& path) {
        const std::scoped_lock lock(g_countedLock);
        const QFileInfo file(path);
        g_countedPath = QFile::encodeName(path);
        g_countedFolder = QFile::encodeName(file.absoluteDir().canonicalPath());
        g_countedName = QFile::encodeName(file.fileName());
        g_countingThread = pthread_self();
        g_countedOpens = 0;
        g_counting = true;
    }
    ~OpensOf() { g_counting = false; }
    OpensOf(const OpensOf&) = delete;
    OpensOf& operator=(const OpensOf&) = delete;
    int count() const { return g_countedOpens.load(); }
};

} // namespace

namespace {

constexpr int kSettleMs = 30;
constexpr int kWriteGapMs = 60;
// Every wait below ends as soon as its condition holds; this is only the failure bound.
constexpr int kWaitMs = 5000;

const std::atomic<bool> kNotCancelled{false};

QString outcomeName(ImportResult::Outcome outcome) {
    switch (outcome) {
    case ImportResult::Outcome::Imported:
        return QStringLiteral("Imported");
    case ImportResult::Outcome::AlreadyPresent:
        return QStringLiteral("AlreadyPresent");
    case ImportResult::Outcome::Refused:
        return QStringLiteral("Refused");
    case ImportResult::Outcome::Failed:
        return QStringLiteral("Failed");
    }
    return {};
}

QString describe(const ImportResult& r) {
    return QStringLiteral("%1 %2 \"%3\"").arg(outcomeName(r.outcome), QFileInfo(r.folderPath).fileName(), r.problem);
}

// Every name in `dir`, hidden ones included, sorted.
QStringList entriesOf(const QString& dir) {
    QStringList names = QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    names.sort();
    return names;
}

// Deletes every wallpaper, as the user may: an import of the same file is then a real one, not "already
// present".
[[nodiscard]] bool deleteWallpapers(const QString& wallpapers) {
    bool all = true;
    for (const QString& entry : entriesOf(wallpapers)) {
        all = QDir(wallpapers + QLatin1Char('/') + entry).removeRecursively() && all;
    }
    return all;
}

QStringList unfinishedIn(const QString& dir) {
    return entriesOf(dir).filter(QRegularExpression(QStringLiteral("^\\.import-")));
}

// The ten-letter folder name the design gives the bytes with this SHA-256 on `attempt`: 8 bytes of
// the hash, big-endian, written in base 26 lowest digit first. Written out here, not taken from
// contentFolderName, so a change to how names are made -- which would stop every re-import from
// finding the folder an earlier version made -- is caught.
QString expectedName(const QByteArray& sha256, int attempt) {
    quint64 bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits = (bits << 8) | quint8(sha256.at(attempt * 8 + i));
    }
    QString name;
    for (int i = 0; i < 10; ++i) {
        name.append(QChar(char16_t('a' + bits % 26)));
        bits /= 26;
    }
    return name;
}

QByteArray sha256Of(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

// Rewrites the file at `path` with every `from` replaced by `to` (the same length). False when
// nothing was replaced.
[[nodiscard]] bool patchBytes(const QString& path, const QByteArray& from, const QByteArray& to) {
    auto bytes = readBytes(path);
    if (!bytes || from.size() != to.size() || !bytes->contains(from)) {
        return false;
    }
    bytes->replace(from, to);
    return writeBytes(path, *bytes);
}

// Sets the uncompressed size the central directory records for entry `name` to `size`.
[[nodiscard]] bool declareSize(const QString& path, const QByteArray& name, quint32 size) {
    auto bytes = readBytes(path);
    if (!bytes) {
        return false;
    }
    for (qsizetype at = bytes->indexOf("PK\x01\x02"); at >= 0; at = bytes->indexOf("PK\x01\x02", at + 4)) {
        const auto u16 = [&](qsizetype o) { return quint16(quint8(bytes->at(at + o)) | quint8(bytes->at(at + o + 1)) << 8); };
        if (bytes->mid(at + 46, u16(28)) == name) {
            for (int i = 0; i < 4; ++i) {
                (*bytes)[at + 24 + i] = char((size >> (8 * i)) & 0xff);
            }
            return writeBytes(path, *bytes);
        }
    }
    return false;
}

struct SourceState {
    QByteArray bytes;
    qint64 modifiedNs = 0;
    friend bool operator==(const SourceState&, const SourceState&) = default;
};

std::optional<SourceState> sourceState(const QString& path) {
    const auto bytes = readBytes(path);
    struct stat st {};
    if (!bytes || ::stat(QFile::encodeName(path).constData(), &st) != 0) {
        return std::nullopt;
    }
    return SourceState{*bytes, qint64(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec};
}

// S15.75
void imports(const QString& root) {
    const QString dir = root + QStringLiteral("/imports");
    const QString wallpapers = dir + QStringLiteral("/wallpapers");
    const QString ocean = dir + QStringLiteral("/ocean.wpx");
    const QString copy = dir + QStringLiteral("/copy of ocean.wpx");
    if (!QDir().mkpath(dir) || !writeZip(ocean, planEntries(QByteArray(kPlanManifest))).isEmpty() || !QFile::copy(ocean, copy)) {
        fault("S15.75", QStringLiteral("ocean.wpx could not be built"));
        return;
    }
    const auto before = sourceState(ocean);
    const QByteArray hash = sha256Of(before ? before->bytes : QByteArray());
    const QString name = expectedName(hash, 0);
    const QString folder = wallpapers + QLatin1Char('/') + name;

    // GOAL: S15.75 -- importing a .wpx makes one folder in wallpapers/ named by ten lower-case
    //       letters derived from the archive's SHA-256, holding exactly the archive's files and a
    //       hidden record of that hash, readable as a usable wallpaper; nothing else is left in
    //       wallpapers/, and the .wpx itself is only read.
    // WHY-THIS-SHAPE: the expected name is computed here from the hash by the design's own rule,
    //       not by calling contentFolderName, so a changed rule -- which would stop a later import
    //       of the same bytes from finding this folder -- is caught; the source's bytes and time
    //       are compared before and after.
    // CONTROL: app/library/importer.cpp, contentFolderName's `letter = char('a' + bits % 26);` made
    //       `char('b' + bits % 26)` -> red, "S15.75: ocean.wpx imported as Imported <other name> ...";
    //       the record's `hash->toHex()` made `hash->toBase64()` -> red, "... its record reads
    //       "sha256:<base64>""; restored.
    const ImportResult first = unpackPackage(ocean, wallpapers, kNotCancelled);
    const auto record = readBytes(folder + QLatin1Char('/') + QLatin1String(kImportRecordName));
    const QStringList inside = entriesOf(folder);
    const Wallpaper read = readWallpaperFolder(folder);
    const auto asset = readBytes(folder + QStringLiteral("/assets/video.mp4"));
    expect(first.outcome == ImportResult::Outcome::Imported && first.folderPath == folder && QRegularExpression(QStringLiteral("^[a-z]{10}$")).match(name).hasMatch() &&
               record == "sha256:" + hash.toHex() + '\n' &&
               inside == QStringList{QStringLiteral(".wallpaperx-source"), QStringLiteral("assets"), QStringLiteral("preview.png"), QStringLiteral("wallpaper.json")} &&
               read.usable() && read.title == u"Ocean Waves" && first.title == u"Ocean Waves" && asset == QByteArrayLiteral("not really a video"),
           QStringLiteral("S15.75: ocean.wpx imported as %1 (wanted %2), holding [%3], its record reads \"%4\", read as usable=%5 \"%6\"")
               .arg(describe(first), name, inside.join(u", "), QString::fromLatin1(record.value_or(QByteArray()).trimmed())).arg(read.usable()).arg(read.problem));
    expect(entriesOf(wallpapers) == QStringList{name} && sourceState(ocean) == before,
           QStringLiteral("S15.75: after the import wallpapers/ holds [%1] and the source is %2").arg(entriesOf(wallpapers).join(u", "),
                                                                                                       sourceState(ocean) == before ? QStringLiteral("unchanged") : QStringLiteral("changed")));

    // GOAL: S15.75 -- the same bytes imported again, by the same name or another, are recognised:
    //       AlreadyPresent, the same folder, nothing made or changed in wallpapers/ -- even when the
    //       user has since edited that folder, which comes back as edited.
    // CONTROL: app/library/importer.cpp, occupantOf()'s `? Occupant::Same : Occupant::Other` made to
    //       answer Other always -> red, "S15.75: ocean.wpx imported again gave Imported <attempt-1
    //       name> ..."; restored.
    const ImportResult again = unpackPackage(ocean, wallpapers, kNotCancelled);
    const ImportResult copied = unpackPackage(copy, wallpapers, kNotCancelled);
    OrderedJson edited = planManifest();
    edited["title"] = "Edited by the user";
    const bool editMade = writeBytes(folder + QStringLiteral("/wallpaper.json"), jsonBytes(edited));
    const ImportResult afterEdit = unpackPackage(ocean, wallpapers, kNotCancelled);
    expect(again.outcome == ImportResult::Outcome::AlreadyPresent && again.folderPath == folder &&
               copied.outcome == ImportResult::Outcome::AlreadyPresent && copied.folderPath == folder && editMade &&
               afterEdit.outcome == ImportResult::Outcome::AlreadyPresent && afterEdit.folderPath == folder &&
               readWallpaperFolder(folder).title == u"Edited by the user" && entriesOf(wallpapers) == QStringList{name},
           QStringLiteral("S15.75: ocean.wpx imported again gave %1, a copy of it %2, after the user's edit %3; wallpapers/ holds [%4]")
               .arg(describe(again), describe(copied), describe(afterEdit), entriesOf(wallpapers).join(u", ")));

    // GOAL: S15.75 -- a folder under the name that no longer reads as a usable wallpaper, or that
    //       holds other bytes, is not taken for this import: the import goes to the next name the
    //       hash gives, and the other folder is left as it was.
    // CONTROL: app/library/importer.cpp, occupantOf()'s `&& readWallpaperFolder(folder).usable()`
    //       deleted -> red, "S15.75: with its folder broken by the user ocean.wpx imported as
    //       AlreadyPresent <name> ..."; restored.
    const bool broke = writeBytes(folder + QStringLiteral("/wallpaper.json"), "the user broke this");
    const ImportResult afterBreak = unpackPackage(ocean, wallpapers, kNotCancelled);
    const QString second = expectedName(hash, 1);
    expect(broke && afterBreak.outcome == ImportResult::Outcome::Imported && afterBreak.folderPath == wallpapers + QLatin1Char('/') + second &&
               readBytes(folder + QStringLiteral("/wallpaper.json")) == QByteArrayLiteral("the user broke this"),
           QStringLiteral("S15.75: with its folder broken by the user ocean.wpx imported as %1 (wanted Imported %2)").arg(describe(afterBreak), second));

    // GOAL: S15.75 -- a name taken between the importer's look and its rename (another process
    //       making the folder) is never replaced: the rename refuses and the import moves to the
    //       next name.
    // WHY-THIS-SHAPE: the race is made, not waited for: this binary's renameat2 makes an empty
    //       folder at the target just before the real call. An empty folder is the case a plain
    //       rename(2) would silently replace.
    // CONTROL: app/library/importer.cpp, `RENAME_NOREPLACE` made `0` -> red, "S15.75: with its name
    //       taken just before the rename, a new package imported as Imported <attempt-0 name> ...";
    //       restored.
    const QString raced = dir + QStringLiteral("/raced.wpx");
    if (!writeZip(raced, planEntries(titled("Raced"))).isEmpty()) {
        fault("S15.75", QStringLiteral("raced.wpx could not be built"));
        return;
    }
    const QByteArray racedHash = sha256Of(readBytes(raced).value_or(QByteArray()));
    g_takeNameFirst = true;
    // Disarmed on every way out, a throw included, so no later rename finds a name taken.
    const auto disarmRace = qScopeGuard([] { g_takeNameFirst = false; });
    const ImportResult racedResult = unpackPackage(raced, wallpapers, kNotCancelled);
    const bool armedUsed = !g_takeNameFirst.exchange(false);
    const QString racedFirst = wallpapers + QLatin1Char('/') + expectedName(racedHash, 0);
    expect(armedUsed && racedResult.outcome == ImportResult::Outcome::Imported &&
               racedResult.folderPath == wallpapers + QLatin1Char('/') + expectedName(racedHash, 1) && entriesOf(racedFirst).isEmpty() &&
               QFileInfo(racedFirst).isDir(),
           QStringLiteral("S15.75: with its name taken just before the rename, a new package imported as %1 (wanted Imported %2; the race was made %3)")
               .arg(describe(racedResult), expectedName(racedHash, 1)).arg(armedUsed));
    expect(unfinishedIn(wallpapers).isEmpty(), QStringLiteral("S15.75: wallpapers/ was left holding [%1]").arg(unfinishedIn(wallpapers).join(u", ")));
}

// S15.76
void refusals(const QString& root) {
    const QString dir = root + QStringLiteral("/refuse");
    const QString wallpapers = dir + QStringLiteral("/wallpapers");
    const QByteArray plan(kPlanManifest);
    const QString absoluteTarget = root + QStringLiteral("/absolute-evil.txt");
    const auto at = [&dir](const char* name) { return dir + QLatin1Char('/') + QLatin1String(name); };
    const auto plain = [](zip_t*, zip_uint64_t, const QString&) { return true; };
    Entries many = planEntries(plan);
    for (int i = 0; many.size() < 4097; ++i) {
        many.append({QStringLiteral("extra/%1").arg(i), QByteArrayLiteral("x")});
    }
    Entries atLimit = many.mid(0, 4096);
    Entries big = planEntries(plan);
    big.append({QStringLiteral("big1"), QByteArrayLiteral("one")});
    big.append({QStringLiteral("big2"), QByteArrayLiteral("two")});
    Entries damaged = planEntries(plan);
    damaged.append({QStringLiteral("assets/extra.bin"), QByteArray(65536, 'Q')});

    QString why;
    const auto build = [&why](const QString& w) {
        if (why.isEmpty() && !w.isEmpty()) {
            why = w;
        }
    };
    build(QDir().mkpath(wallpapers) ? QString() : QStringLiteral("wallpapers/ could not be made"));
    build(writeZip(at("dotdot.wpx"), planEntries(plan) + Entries{{QStringLiteral("../evil.txt"), "evil"}}));
    build(writeZip(at("absolute.wpx"), planEntries(plan) + Entries{{absoluteTarget, "evil"}}));
    build(writeZipWith(at("symlink.wpx"), planEntries(plan) + Entries{{QStringLiteral("assets/link"), "/etc/passwd"}},
                       [](zip_t* zip, zip_uint64_t index, const QString& name) {
                           return name != u"assets/link" || zip_file_set_external_attributes(zip, index, 0, ZIP_OPSYS_UNIX, zip_uint32_t(0120777) << 16) == 0;
                       }));
    build(writeZip(at("many.wpx"), many));
    build(writeZip(at("atlimit.wpx"), atLimit));
    build(writeZip(at("reserved.wpx"), planEntries(plan) + Entries{{QLatin1String(kImportRecordName), "sha256:0\n"}}));
    build(writeZip(at("clash.wpx"), planEntries(plan) + Entries{{QStringLiteral("assets"), "a file"}}));
    build(writeZip(at("duplicate.wpx"), planEntries(plan) + Entries{{QStringLiteral("dupA.txt"), "one"}, {QStringLiteral("dupB.txt"), "two"}}));
    build(patchBytes(at("duplicate.wpx"), "dupB.txt", "dupA.txt") ? QString() : QStringLiteral("duplicate.wpx could not be patched"));
    build(writeBytes(at("notzip.wpx"), "hello") ? QString() : QStringLiteral("notzip.wpx could not be written"));
    build(writeZip(at("badmanifest.wpx"), planEntries(QByteArray(R"({"schema_version": "2"})"))));
    build(writeZip(at("bigdeclared.wpx"), big));
    build(declareSize(at("bigdeclared.wpx"), "big1", 0x80000001u) && declareSize(at("bigdeclared.wpx"), "big2", 0x80000001u)
              ? QString() : QStringLiteral("bigdeclared.wpx's sizes could not be patched"));
    build(writeZipWith(at("damaged.wpx"), damaged, [](zip_t* zip, zip_uint64_t index, const QString& name) {
        return name != u"assets/extra.bin" || zip_set_file_compression(zip, index, ZIP_CM_STORE, 0) == 0;
    }));
    build(patchBytes(at("damaged.wpx"), QByteArray(16, 'Q'), QByteArray("QQQQQQQQRQQQQQQQ")) ? QString() : QStringLiteral("damaged.wpx could not be damaged"));
    OrderedJson loud = planManifest();
    loud["properties"]["volume"] = OrderedJson::parse(R"({"type": "float", "default": 50, "min": 0, "max": 100})");
    build(writeZip(at("loudvolume.wpx"), planEntriesWith(jsonBytes(loud), soundVideo())));
    if (!why.isEmpty()) {
        fault("S15.76", why);
        return;
    }
    struct Case {
        const char* file;
        QString reason;
    };
    const Case cases[] = {
        {"dotdot.wpx", QStringLiteral("It holds \"../evil.txt\", which would land outside its folder.")},
        {"absolute.wpx", QStringLiteral("It holds \"%1\", which would land outside its folder.").arg(absoluteTarget)},
        {"symlink.wpx", QStringLiteral("assets/link is a link or a special file, which a wallpaper may not hold.")},
        {"many.wpx", QStringLiteral("It holds 4097 files; a wallpaper may hold at most 4096.")},
        {"reserved.wpx", QStringLiteral("It holds .wallpaperx-source, a name WallpaperX keeps for itself.")},
        {"clash.wpx", QStringLiteral("It holds assets as both a file and a folder.")},
        {"duplicate.wpx", QStringLiteral("It holds dupA.txt twice.")},
        {"notzip.wpx", QStringLiteral("It is not a ZIP archive, so not a WallpaperX package.")},
        {"badmanifest.wpx", QStringLiteral("wallpaper.json is for schema version \"2\"")},
        {"bigdeclared.wpx", QStringLiteral("It unpacks to more than 4 GiB.")},
        {"damaged.wpx", QStringLiteral("assets/extra.bin is damaged in the archive")},
        // GOAL: S15.488's archive half -- a .wpx whose video has sound and whose wallpaper.json declares a
        //       volume of 0 to 100 is refused at import with the volume's sentence: an archive is not probed,
        //       so the refusal comes from reading the unpacked folder.
        // CONTROL: via control_batch.sh, --phase '^refusals$', 2026-10-11, settleSoundProperties' `return
        //       problem;` made a no-op (app/library/wpx_package.cpp) -> "FAIL: S15.76: loudvolume.wpx gave
        //       Imported ...; wanted Refused "Property "volume" ... allows 0 to 100 ..."". Reverted.
        {"loudvolume.wpx",
         QStringLiteral("Property \"volume\" in wallpaper.json allows 0 to 100, and WallpaperX plays a video's volume from 0 to 1.")},
    };
    // GOAL: S15.76 -- a package that could put a file outside its folder, holds a link or special
    //       file, has more than 4096 files, declares more than 4 GiB unpacked, uses the name kept
    //       for the import record, holds a name as both a file and a folder or twice, is damaged,
    //       or is not a usable package, is Refused with its own reason, and leaves nothing behind:
    //       no folder or unfinished folder in wallpapers/, no file where an entry pointed, and the
    //       .wpx as it was.
    // WHY-THIS-SHAPE: Refused is asserted, not merely "not imported": a refusal is recorded so the
    //       drop folder never retries it, a failure is retried, so the two must not be confused.
    //       The 4 GiB case declares its sizes in the central directory (the data is tiny), which is
    //       what the limit is checked against; the damaged case stores an entry uncompressed and
    //       changes one byte, so only its checksum can tell. Three cases (clash, duplicate, damaged)
    //       are refused mid-unpack, after the unfinished folder exists, so they grade its removal.
    // CONTROL: app/library/importer.cpp, one per run, each red on its own row and restored:
    //       planEntries' `if (!isPlainRelativePath(entry.name)) {` made `if (false) {` -> "dotdot.wpx
    //       gave Imported" and "absolute.wpx gave Failed"; the `type != 0 && type != S_IFREG && type
    //       != S_IFDIR` test made `false` -> "symlink.wpx gave Imported"; `if (count > kMaxPackageEntries) {`
    //       made `if (false) {` -> "many.wpx gave Imported"; the record-name test made `false` ->
    //       "reserved.wpx gave Failed"; folderProblem's `error == ENOTDIR || error == ELOOP ||
    //       error == EEXIST` made `false` -> "clash.wpx gave Failed"; `refused(QStringLiteral("It holds
    //       %1 twice.")` made `failed(...)` -> "duplicate.wpx gave Failed"; the total-size test made
    //       `false` -> "bigdeclared.wpx gave Refused ... is damaged in the archive."; extract()'s
    //       `if (got < 0) {` branch made to `break` -> "damaged.wpx gave Imported"; ~TemporaryFolder's
    //       body emptied -> "clash.wpx ... left [.import-...]".
    // COVERAGE-ASSERT: clash, duplicate and damaged are counted being refused after their
    //       unfinished folder was made, so its removal is graded, not merely never needed.
    static_assert(std::size(cases) == 12, "a refusal row was added or dropped");
    QStringList afterUnfinished;
    for (const Case& c : cases) {
        const auto before = sourceState(at(c.file));
        const int made = g_unfinishedMade.load();
        const ImportResult r = unpackPackage(at(c.file), wallpapers, kNotCancelled);
        if (g_unfinishedMade.load() > made) {
            afterUnfinished.append(QLatin1String(c.file));
        }
        const QStringList left = entriesOf(wallpapers);
        expect(r.outcome == ImportResult::Outcome::Refused && r.problem.startsWith(c.reason) && r.folderPath.isEmpty() && left.isEmpty() &&
                   sourceState(at(c.file)) == before,
               QStringLiteral("S15.76: %1 gave %2, left [%3] in wallpapers/, the source %4; wanted Refused \"%5\"")
                   .arg(QLatin1String(c.file), describe(r), left.join(u", "),
                        sourceState(at(c.file)) == before ? QStringLiteral("unchanged") : QStringLiteral("changed"), c.reason));
        QDir(wallpapers).removeRecursively();
        QDir().mkpath(wallpapers);
    }
    // CONTROL: app/library/importer.cpp, the free-space test's `< plan->total + kSpareBytes` made
    //       `< qint64(1) << 62`, so every package stops before its folder is made -> red, "S15.76:
    //       the refusals made after an unfinished folder existed were []"; restored.
    expect(afterUnfinished.contains(QStringLiteral("clash.wpx")) && afterUnfinished.contains(QStringLiteral("duplicate.wpx")) &&
               afterUnfinished.contains(QStringLiteral("damaged.wpx")),
           QStringLiteral("S15.76: the refusals made after an unfinished folder existed were [%1], not clash, duplicate and damaged; "
                          "the removal of an unfinished folder is graded by those alone")
               .arg(afterUnfinished.join(u", ")));
    expect(!QFileInfo::exists(dir + QStringLiteral("/evil.txt")) && !QFileInfo::exists(absoluteTarget) && !QFileInfo::exists(root + QStringLiteral("/evil.txt")),
           QStringLiteral("S15.76: an entry pointing outside its folder was written: %1 %2").arg(dir + QStringLiteral("/evil.txt"), absoluteTarget));

    // GOAL: S15.76 -- exactly 4096 files is within the limit.
    // CONTROL: app/library/importer.cpp, `if (count > kMaxPackageEntries) {` made `>=` -> red,
    //       "S15.76: a package of exactly 4096 files gave Refused ..."; restored.
    const ImportResult limit = unpackPackage(at("atlimit.wpx"), wallpapers, kNotCancelled);
    expect(limit.outcome == ImportResult::Outcome::Imported, QStringLiteral("S15.76: a package of exactly 4096 files gave %1").arg(describe(limit)));
}

// S15.76: unfinished folders left by an import that never ended.
void staleImports(const QString& root) {
    const QString dir = root + QStringLiteral("/stale");
    const QString wallpapers = dir + QStringLiteral("/wallpapers");
    const QString package = dir + QStringLiteral("/ocean.wpx");
    if (!writeFolder(wallpapers + QStringLiteral("/.import-stale1"), {{QStringLiteral("half.bin"), "left behind"}}).isEmpty() ||
        !writeFolder(wallpapers + QStringLiteral("/.import-held1"), {{QStringLiteral("half.bin"), "still being made"}}).isEmpty() ||
        !writeZip(package, planEntries(QByteArray(kPlanManifest))).isEmpty()) {
        fault("S15.76", QStringLiteral("the stale folders could not be built"));
        return;
    }
    const int held = ::open(QFile::encodeName(wallpapers + QStringLiteral("/.import-held1")).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (held < 0 || ::flock(held, LOCK_EX | LOCK_NB) != 0) {
        fault("S15.76", QStringLiteral("the held folder could not be locked"));
        if (held >= 0) {
            ::close(held);
        }
        return;
    }
    // GOAL: S15.76 -- an unfinished folder a crashed import left behind is removed by the next
    //       import, while one another import is still making (it holds the folder's lock) is left
    //       alone.
    // CONTROL: app/library/importer.cpp, removeStaleImports()'s `&& ::flock(stale.get(), LOCK_EX | LOCK_NB) == 0`
    //       deleted -> red, "S15.76: after an import wallpapers/ holds [<name>]" (the held one
    //       gone too); unpackPackage's `removeStaleImports(dirFd.get());` deleted -> red, "... holds
    //       [.import-held1, .import-stale1, <name>]"; restored.
    const ImportResult r = unpackPackage(package, wallpapers, kNotCancelled);
    ::close(held);
    const QStringList left = entriesOf(wallpapers);
    expect(r.outcome == ImportResult::Outcome::Imported && left == QStringList{QStringLiteral(".import-held1"), QFileInfo(r.folderPath).fileName()},
           QStringLiteral("S15.76: after an import (%1) wallpapers/ holds [%2]").arg(describe(r), left.join(u", ")));
}

// S15.76: an import stopped half-way.
void cancelledImport(const QString& root) {
    const QString dir = root + QStringLiteral("/cancel");
    const QString wallpapers = dir + QStringLiteral("/wallpapers");
    const QString package = dir + QStringLiteral("/big.wpx");
    Entries entries = planEntries(QByteArray(kPlanManifest));
    entries.last().second = QByteArray(128 << 20, '\0');
    if (!QDir().mkpath(wallpapers) || !writeZip(package, entries).isEmpty()) {
        fault("S15.76", QStringLiteral("big.wpx could not be built"));
        return;
    }
    // GOAL: S15.76 -- an import stopped while it unpacks ends Failed, "The import was stopped.",
    //       and leaves nothing in wallpapers/.
    // WHY-THIS-SHAPE: the stop is given only once the unfinished folder exists (watched for from
    //       another thread), so it lands after the hash, which checks the same flag. "The folder
    //       was seen" does not prove the stop was early enough: the folder lives until the rename,
    //       after the last read. So after the stop the asset is measured through the folder as it
    //       was found; below 127 of its 128 MiB (reads are at most 1 MiB), at least one more read
    //       must pass the flag. A stop not shown early is not verified, never graded.
    // CONTROL: app/library/importer.cpp, extract()'s `if (cancelled) { return failed(QStringLiteral("The import was stopped.")); }`
    //       deleted -> red, "S15.76: an import stopped while unpacking (the asset at N bytes) gave
    //       Imported ..."; restored.
    std::atomic<bool> cancelled{false};
    std::atomic<bool> seen{false};
    std::atomic<qint64> assetAtStop{-1};
    std::jthread stopper([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            const QStringList unfinished = unfinishedIn(wallpapers);
            if (unfinished.isEmpty()) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            seen = true;
            const int folder = ::open(QFile::encodeName(wallpapers + QLatin1Char('/') + unfinished.constFirst()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            cancelled = true;
            // Through the folder's own descriptor, so a rename since cannot make a whole asset
            // look missing; a folder already gone proves nothing.
            if (folder >= 0) {
                struct stat st {};
                assetAtStop = ::fstatat(folder, "assets/video.mp4", &st, 0) == 0 ? qint64(st.st_size) : errno == ENOENT ? 0 : -1;
                ::close(folder);
            }
            return;
        }
    });
    const ImportResult r = unpackPackage(package, wallpapers, cancelled);
    stopper.request_stop();
    stopper.join();
    const qint64 at = assetAtStop.load();
    const bool early = at >= 0 && at < (qint64(127) << 20);
    const bool stoppedClean = r.outcome == ImportResult::Outcome::Failed && r.problem == u"The import was stopped." && entriesOf(wallpapers).isEmpty();
    if (!early && (stoppedClean || r.outcome == ImportResult::Outcome::Imported)) {
        notVerified("S15.76 (an import stopped)", seen ? "the stop was not shown to come before the asset's last read (asset at " + std::to_string(at) + " bytes)"
                                                       : std::string("the import finished before its unfinished folder was seen"));
        return;
    }
    expect(early && stoppedClean,
           QStringLiteral("S15.76: an import stopped while unpacking (the asset at %1 bytes) gave %2 and left [%3]").arg(at).arg(describe(r), entriesOf(wallpapers).join(u", ")));
}

// S15.76, the child: run by freeSpace() under `unshare -U -r -m`, so the small tmpfs it mounts is
// in a mount namespace of its own. Arguments: a folder to mount on, the parent's user namespace.
int smallDiskChild(const QStringList& args) {
    wallpaperx::testing::g_testName = "importer (free space)";
    if (args.size() != 2) {
        harnessFailure("S15.76 (free space): the child was given " + std::to_string(args.size()) + " argument(s), not 2");
        return verdict();
    }
    if (const auto refused = refuseUnlessOwnNamespace(QStringLiteral("S15.76 (free space)"), args[1])) {
        return *refused;
    }
    const QString mountPoint = args[0];
    if (!QDir().mkpath(mountPoint) || ::mount("none", QFile::encodeName(mountPoint).constData(), "tmpfs", 0, "size=32m") != 0) {
        notVerified("S15.76 (free space)", std::string("a small tmpfs could not be mounted in this namespace: ") + std::strerror(errno));
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    const QString wallpapers = mountPoint + QStringLiteral("/wallpapers");
    const QString package = mountPoint + QStringLiteral("/ocean.wpx");
    if (!QDir().mkpath(wallpapers) || !writeZip(package, planEntries(QByteArray(kPlanManifest))).isEmpty()) {
        fault("S15.76", QStringLiteral("the package could not be built on the small tmpfs"));
        return verdict();
    }
    childReady();
    // GOAL: S15.76 -- an import that would leave less than its spare room free on the disk ends
    //       Failed (another attempt may pass once room is made), saying how much it needs, and
    //       leaves nothing in wallpapers/.
    // WHY-THIS-SHAPE: a 32 MiB tmpfs, mounted in a mount namespace of the child's own, cannot hold
    //       the 256 MiB the import keeps free, whatever the package's size.
    // CONTROL: app/library/importer.cpp, the free-space test's `< plan->total + kSpareBytes` made
    //       `< 0` -> red, "S15.76 (free space): on a 32 MiB disk the import gave Imported ..."; restored.
    const ImportResult r = unpackPackage(package, wallpapers, kNotCancelled);
    expect(r.outcome == ImportResult::Outcome::Failed && r.problem == QStringLiteral("There is not enough free space in %1: it needs 1 MiB.").arg(wallpapers) &&
               entriesOf(wallpapers).isEmpty(),
           QStringLiteral("S15.76 (free space): on a 32 MiB disk the import gave %1 and left [%2]").arg(describe(r), entriesOf(wallpapers).join(u", ")));
    return verdict();
}

void freeSpace(const QString& root) {
    inNamespace("S15.76 (free space)", {QStringLiteral("-U"), QStringLiteral("-r"), QStringLiteral("-m")}, QStringLiteral("--small-disk"),
                {root + QStringLiteral("/small-disk")});
}

// The drop folder's record, as the lines it holds.
QStringList recordOf(const QString& drop) {
    const auto bytes = readBytes(drop + QStringLiteral("/.auto-imported"));
    QStringList lines = bytes ? QString::fromLatin1(*bytes).split(u'\n', Qt::SkipEmptyParts) : QStringList();
    lines.sort();
    return lines;
}

// A file's line in the record: inode, size, modification time in ns.
QString keyOf(const QString& path) {
    struct stat st {};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0) {
        return QStringLiteral("(no such file)");
    }
    return QStringLiteral("%1 %2 %3").arg(quint64(st.st_ino)).arg(qint64(st.st_size)).arg(qint64(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec);
}

// A package put in `drop` whole, by rename, as a download or a file manager's move finishes.
[[nodiscard]] bool dropIn(const QString& drop, const QString& name, const QByteArray& bytes) {
    const QString staged = drop + QStringLiteral("/.staging-") + name;
    return writeBytes(staged, bytes) && ::rename(QFile::encodeName(staged).constData(), QFile::encodeName(drop + QLatin1Char('/') + name).constData()) == 0;
}

// One run of the app's importer: a fresh Importer is what a restart makes.
class Session {
public:
    Session(const QString& drop, const QString& wallpapers, int holdMs = 30000, Importer::Watching watching = Importer::Watching::Inotify,
            int pollMs = 2000)
        : m_importer(drop, wallpapers, kSettleMs, pollMs, watching, holdMs) {
        QObject::connect(&m_importer, &Importer::busyChanged, [this] { m_runs += m_importer.busy() ? 1 : 0; });
        QObject::connect(&m_importer, &Importer::imported, [this](const QString& folder) { m_imported.append(folder); });
        QObject::connect(&m_importer, &Importer::noticesChanged, [this] { ++m_noticeSignals; });
    }
    Importer& importer() { return m_importer; }
    // Import runs begun since the last call: the importer going busy.
    int takeRuns() { return std::exchange(m_runs, 0); }
    QStringList takeImported() { return std::exchange(m_imported, {}); }
    int takeNoticeSignals() { return std::exchange(m_noticeSignals, 0); }
    // Waits until no listing or import runs and none is queued, across a few quiet periods.
    bool quiesce() {
        for (int i = 0; i < 3; ++i) {
            if (!waitUntil([&] { return QThreadPool::globalInstance()->activeThreadCount() == 0 && !m_importer.busy(); }, kWaitMs)) {
                return false;
            }
            pump(4 * kSettleMs);
        }
        return QThreadPool::globalInstance()->activeThreadCount() == 0 && !m_importer.busy();
    }

private:
    Importer m_importer;
    int m_runs = 0;
    int m_noticeSignals = 0;
    QStringList m_imported;
};

// S15.77, S15.78
void autoImport(const QString& root) {
    const QString drop = root + QStringLiteral("/auto");
    const QString wallpapers = drop + QStringLiteral("/wallpapers");
    const auto present = zipBytes(root + QStringLiteral("/present.scratch"), planEntries(titled("Present")));
    const auto second = zipBytes(root + QStringLiteral("/second.scratch"), planEntries(titled("Second")));
    if (!present || !second || !QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/present.wpx"), *present)) {
        fault("S15.77", QStringLiteral("the drop folder could not be built"));
        return;
    }
    const QString presentFolder = wallpapers + QLatin1Char('/') + expectedName(sha256Of(*present), 0);
    const QString presentKey = keyOf(drop + QStringLiteral("/present.wpx"));

    // GOAL: S15.77, S15.78 -- a .wpx in the drop folder when the importer starts is imported once,
    //       recorded by (inode, size, time), and announced by exactly one notice, "Imported
    //       <title>."; one dropped in while it runs likewise; Dismiss empties the notices.
    // CONTROL: app/library/importer.cpp, Importer::finish's Imported case `if (scanOnly) {` made
    //       `if (!scanOnly) {` -> red, "S15.78: the first session ran 1 import(s), noticed [] ..."
    //       and "a .wpx dropped in while the importer ran gave the notices []"; dismissNotices()'s
    //       `m_notices.clear();` deleted -> red, "S15.78: after Dismiss the notices are [Imported
    //       Present. | Imported Second.]".
    {
        Session s(drop, wallpapers);
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        const QStringList notices = s.importer().notices();
        expect(quiet && runs == 1 && notices == QStringList{QStringLiteral("Imported Present.")} && s.importer().problems().isEmpty() &&
                   s.takeImported().isEmpty() && QFileInfo(presentFolder).isDir() && recordOf(drop) == QStringList{presentKey},
               QStringLiteral("S15.78: the first session ran %1 import(s), noticed [%2], reported [%3], made %4 and recorded [%5]")
                   .arg(runs).arg(notices.join(QStringLiteral(" | ")), s.importer().problems().join(QStringLiteral(" | ")),
                        QFileInfo(presentFolder).isDir() ? presentFolder : QStringLiteral("nothing"), recordOf(drop).join(QStringLiteral(" | "))));
        if (!dropIn(drop, QStringLiteral("second.wpx"), *second)) {
            fault("S15.77", QStringLiteral("second.wpx could not be dropped in"));
            return;
        }
        const bool imported = waitUntil([&] { return s.importer().notices().size() == 2; }, kWaitMs) && s.quiesce();
        const QStringList both = s.importer().notices();
        expect(imported && both == QStringList{QStringLiteral("Imported Present."), QStringLiteral("Imported Second.")},
               QStringLiteral("S15.78: a .wpx dropped in while the importer ran gave the notices [%1]").arg(both.join(QStringLiteral(" | "))));
        s.takeNoticeSignals();
        s.importer().dismissNotices();
        expect(s.importer().notices().isEmpty() && s.takeNoticeSignals() == 1,
               QStringLiteral("S15.78: after Dismiss the notices are [%1]").arg(s.importer().notices().join(QStringLiteral(" | "))));
    }

    // GOAL: S15.77 -- after a restart nothing already dealt with is imported again: no import runs
    //       and no notice appears. A broken .wpx dropped in is reported once, recorded, and not
    //       reported again after the next restart.
    // WHY-THIS-SHAPE: each restart is a fresh Importer over the same folders, and import runs are
    //       counted by the importer going busy, which every run does, AlreadyPresent ones included.
    // CONTROL: app/library/importer.cpp, `|| record->contains(key)` deleted from the skip test in
    //       Importer::scanDropFolder -> red, "S15.77: after a restart 1 import(s) ran, noticing []" (both files in one busy
    //       stretch); loadRecordIn() made to return an empty list once the record is open -> the same
    //       line.
    const QString brokenLine = QStringLiteral("broken.wpx could not be imported. It is not a ZIP archive, so not a WallpaperX package.");
    {
        Session s(drop, wallpapers);
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(quiet && runs == 0 && s.importer().notices().isEmpty() && s.importer().problems().isEmpty(),
               QStringLiteral("S15.77: after a restart %1 import(s) ran, noticing [%2]").arg(runs).arg(s.importer().notices().join(QStringLiteral(" | "))));
        if (!dropIn(drop, QStringLiteral("broken.wpx"), "not a zip")) {
            fault("S15.77", QStringLiteral("broken.wpx could not be dropped in"));
            return;
        }
        const bool reported = waitUntil([&] { return !s.importer().problems().isEmpty(); }, kWaitMs) && s.quiesce();
        expect(reported && s.importer().problems() == QStringList{brokenLine} && recordOf(drop).contains(keyOf(drop + QStringLiteral("/broken.wpx"))),
               QStringLiteral("S15.77: a broken .wpx dropped in was reported as [%1] and recorded %2")
                   .arg(s.importer().problems().join(QStringLiteral(" | "))).arg(recordOf(drop).contains(keyOf(drop + QStringLiteral("/broken.wpx")))));
    }
    {
        Session s(drop, wallpapers);
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(quiet && runs == 0 && s.importer().problems().isEmpty(),
               QStringLiteral("S15.77: after another restart %1 import(s) ran and [%2] was reported").arg(runs).arg(s.importer().problems().join(QStringLiteral(" | "))));
    }

    // GOAL: S15.77 -- a wallpaper the user deleted stays deleted across a restart while its .wpx
    //       is still in the drop folder; an explicit import of that .wpx brings it back.
    // CONTROL: the two record breaks above -> red, "S15.77: the deleted wallpaper came back after a
    //       restart: 1".
    if (!QDir(presentFolder).removeRecursively()) {
        fault("S15.77", QStringLiteral("the imported folder could not be deleted"));
        return;
    }
    {
        Session s(drop, wallpapers);
        s.importer().rescan();
        const bool quiet = s.quiesce();
        expect(quiet && !QFileInfo::exists(presentFolder) && s.takeRuns() == 0,
               QStringLiteral("S15.77: the deleted wallpaper came back after a restart: %1").arg(QFileInfo::exists(presentFolder)));
        s.importer().importPath(drop + QStringLiteral("/present.wpx"));
        const bool back = s.quiesce();
        expect(back && s.takeImported() == QStringList{presentFolder} && QFileInfo(presentFolder).isDir() && s.importer().notices().isEmpty(),
               QStringLiteral("S15.77: an explicit import of the deleted wallpaper's .wpx left it %1, noticing [%2]")
                   .arg(QFileInfo(presentFolder).isDir() ? QStringLiteral("back") : QStringLiteral("gone"), s.importer().notices().join(QStringLiteral(" | "))));

        // GOAL: S15.77 -- the record keeps only files still in the drop folder: one removed is
        //       forgotten, and put back (a new file) it is imported again, AlreadyPresent here, with
        //       no notice.
        // CONTROL: app/library/importer.cpp, Importer::scanDropFolder's `record->removeIf(gone) > 0`
        //       made `false` -> red, "S15.77: with present.wpx removed the record still holds its line:
        //       [...]".
        if (!QFile::remove(drop + QStringLiteral("/present.wpx"))) {
            fault("S15.77", QStringLiteral("present.wpx could not be removed"));
            return;
        }
        const bool pruned = waitUntil([&] { return !recordOf(drop).contains(presentKey); }, kWaitMs) && s.quiesce();
        expect(pruned, QStringLiteral("S15.77: with present.wpx removed the record still holds its line: [%1]").arg(recordOf(drop).join(QStringLiteral(" | "))));
        s.takeRuns();
        if (!dropIn(drop, QStringLiteral("present.wpx"), *present)) {
            fault("S15.77", QStringLiteral("present.wpx could not be put back"));
            return;
        }
        const bool again = waitUntil([&] { return recordOf(drop).contains(keyOf(drop + QStringLiteral("/present.wpx"))); }, kWaitMs) && s.quiesce();
        expect(again && s.takeRuns() == 1 && s.importer().notices().isEmpty(),
               QStringLiteral("S15.77: present.wpx put back was imported %1 with notices [%2]").arg(again).arg(s.importer().notices().join(QStringLiteral(" | "))));
    }
}

// S15.79, S15.80
void failuresAndMarking(const QString& root) {
    if (::geteuid() == 0) {
        notVerified("S15.79, S15.80", "running as root, which can write into a read-only folder");
        return;
    }
    const auto package = zipBytes(root + QStringLiteral("/retry.scratch"), planEntries(titled("Retried")));
    if (!package) {
        fault("S15.80", QStringLiteral("the package could not be built"));
        return;
    }
    // GOAL: S15.80 -- a dropped .wpx whose import failed for a passing reason (wallpapers/ not
    //       writable) is reported, not recorded, and is imported by the next Rescan once the
    //       reason is gone.
    // CONTROL: app/library/importer.cpp, finish()'s `result.outcome == ImportResult::Outcome::Failed ||`
    //       deleted, so a failure is recorded -> red, "S15.80: after wallpapers/ was made writable
    //       and Rescan pressed, [] was noticed". (Importer::rescan()'s `scanDropFolder();`
    //       deleted also turns this line red, but it now stops the first listing too, so it shows
    //       only that rescan() lists at all.)
    {
        const QString drop = root + QStringLiteral("/retry");
        const QString wallpapers = drop + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(wallpapers) || !setMode(wallpapers, 0555)) {
            fault("S15.80", QStringLiteral("the read-only wallpapers/ could not be made"));
            return;
        }
        Session s(drop, wallpapers);
        s.importer().rescan();
        const bool started = s.quiesce() && dropIn(drop, QStringLiteral("x.wpx"), *package);
        const bool failed = started && waitUntil([&] { return !s.importer().problems().isEmpty(); }, kWaitMs) && s.quiesce();
        // GOAL: S15.80 -- a failure retried at every scan is listed once, not once per retry.
        // CONTROL: app/library/importer.cpp, addProblem()'s `if (!m_problems.contains(problem)) {`
        //       made `if (true) {` -> red, "S15.80: after three more scans the failure is listed 4
        //       time(s)"; restored.
        bool retriedQuietly = failed;
        s.takeRuns();
        for (int scan = 0; scan < 3 && retriedQuietly; ++scan) {
            s.importer().rescan();
            retriedQuietly = s.quiesce();
        }
        const int retries = s.takeRuns();
        expect(retriedQuietly && retries == 3 && s.importer().problems().size() == 1,
               QStringLiteral("S15.80: after three more scans (%1 retries) the failure is listed %2 time(s)").arg(retries).arg(s.importer().problems().size()));
        const QStringList problems = s.importer().problems();
        const QStringList recorded = recordOf(drop);
        if (!setMode(wallpapers, 0755)) {
            fault("S15.80", QStringLiteral("wallpapers/ could not be made writable again"));
            return;
        }
        expect(failed && problems.size() == 1 && problems[0].startsWith(QStringLiteral("x.wpx could not be imported. A folder cannot be made in")) && recorded.isEmpty(),
               QStringLiteral("S15.80: an import into a read-only wallpapers/ reported [%1] and recorded [%2]").arg(problems.join(QStringLiteral(" | ")), recorded.join(QStringLiteral(" | "))));
        s.importer().dismissProblems();
        s.importer().rescan();
        const bool retried = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        expect(retried && s.importer().notices() == QStringList{QStringLiteral("Imported Retried.")},
               QStringLiteral("S15.80: after wallpapers/ was made writable and Rescan pressed, [%1] was noticed").arg(s.importer().notices().join(QStringLiteral(" | "))));
    }

    // GOAL: S15.79 -- an explicit import of a .wpx that sits directly in the drop folder, however
    //       its path is spelled, is recorded like an automatic one: the user then deleting the
    //       wallpaper and restarting imports nothing. An explicit import of a file elsewhere
    //       records nothing.
    // WHY-THIS-SHAPE: the automatic import fails first (wallpapers/ read-only), so only the
    //       explicit one can have recorded the file; its path goes through "../" so the drop folder
    //       is recognised by identity, not by text.
    // CONTROL: app/library/importer.cpp, Importer::finish's `else if (isInFolder(job.path,
    //       m_dropDirectory))` made `else if (false)` -> red, "S15.79: after the explicit import the
    //       record holds []", "after the user deleted it and restarted, 1 import(s) ran and the folder is
    //       back" and the outside-import line ("recorded []"); isInFolder()'s body made `return true;` ->
    //       red, "S15.79: an explicit import from outside the drop folder recorded [...]".
    {
        const QString drop = root + QStringLiteral("/mark");
        const QString wallpapers = drop + QStringLiteral("/wallpapers");
        const QString outside = root + QStringLiteral("/elsewhere/y.wpx");
        const auto other = zipBytes(root + QStringLiteral("/other.scratch"), planEntries(titled("Elsewhere")));
        if (!other || !QDir().mkpath(wallpapers) || !setMode(wallpapers, 0555) || !QDir().mkpath(root + QStringLiteral("/elsewhere")) ||
            !writeBytes(outside, *other)) {
            fault("S15.79", QStringLiteral("the drop folder could not be built"));
            return;
        }
        const QString folder = wallpapers + QLatin1Char('/') + expectedName(sha256Of(*package), 0);
        {
            Session s(drop, wallpapers);
            s.importer().rescan();
            const bool started = s.quiesce() && dropIn(drop, QStringLiteral("x.wpx"), *package);
            const bool failed = started && waitUntil([&] { return !s.importer().problems().isEmpty(); }, kWaitMs) && s.quiesce();
            if (!setMode(wallpapers, 0755) || !failed || !recordOf(drop).isEmpty()) {
                fault("S15.79", QStringLiteral("the automatic import did not fail unrecorded first (failed %1, record [%2])")
                                    .arg(failed).arg(recordOf(drop).join(QStringLiteral(" | "))));
                return;
            }
            s.importer().importPath(drop + QStringLiteral("/../mark/x.wpx"));
            const bool imported = s.quiesce();
            expect(imported && s.takeImported() == QStringList{folder} && recordOf(drop) == QStringList{keyOf(drop + QStringLiteral("/x.wpx"))},
                   QStringLiteral("S15.79: after the explicit import the record holds [%1]").arg(recordOf(drop).join(QStringLiteral(" | "))));
            s.importer().importPath(outside);
            const bool outsideDone = s.quiesce();
            expect(outsideDone && recordOf(drop) == QStringList{keyOf(drop + QStringLiteral("/x.wpx"))},
                   QStringLiteral("S15.79: an explicit import from outside the drop folder recorded [%1]").arg(recordOf(drop).join(QStringLiteral(" | "))));
        }
        if (!QDir(folder).removeRecursively()) {
            fault("S15.79", QStringLiteral("the imported folder could not be deleted"));
            return;
        }
        Session s(drop, wallpapers);
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(quiet && runs == 0 && !QFileInfo::exists(folder),
               QStringLiteral("S15.79: after the user deleted it and restarted, %1 import(s) ran and the folder is %2").arg(runs)
                   .arg(QFileInfo::exists(folder) ? QStringLiteral("back") : QStringLiteral("gone")));
    }
}

// S15.58, S15.81: files arriving in the drop folder the ways they do.
void arrivals(const QString& root) {
    const auto downloaded = zipBytes(root + QStringLiteral("/downloaded.scratch"), planEntries(titled("Downloaded")));
    const auto copied = zipBytes(root + QStringLiteral("/copied.scratch"), planEntries(titled("Copied")));
    const auto early = zipBytes(root + QStringLiteral("/early.scratch"), planEntries(titled("Early")));
    if (!downloaded || !copied || !early) {
        fault("S15.58", QStringLiteral("the packages could not be built"));
        return;
    }
    const QString drop = root + QStringLiteral("/arrivals");
    const QString wallpapers = drop + QStringLiteral("/wallpapers");
    if (!QDir().mkpath(drop)) {
        fault("S15.58", QStringLiteral("the drop folder could not be made"));
        return;
    }
    Session s(drop, wallpapers);
    s.importer().rescan();
    if (!s.quiesce()) {
        expect(false, QStringLiteral("S15.58: the empty drop folder never went quiet"));
        return;
    }

    // GOAL: S15.58 -- a download into the drop folder the way Firefox makes one (the final name
    //       created empty and closed, the data written to a .part file, renamed over it) is
    //       imported once, after the rename, with nothing reported and nothing run before it.
    // CONTROL: app/library/importer.cpp, listDropFolder()'s `if (st.st_size == 0) {` made
    //       `if (false) {` -> red, "S15.58: before the rename 1 import(s) ran, reporting [x.wpx
    //       could not be imported. It is not a ZIP archive ...]"; restored.
    {
        QFile empty(drop + QStringLiteral("/x.wpx"));
        QFile part(drop + QStringLiteral("/x.wpx.part"));
        if (!empty.open(QIODevice::WriteOnly)) {
            fault("S15.58", QStringLiteral("x.wpx could not be created"));
            return;
        }
        empty.close();
        const qsizetype piece = downloaded->size() / 4 + 1;
        bool written = part.open(QIODevice::WriteOnly);
        for (int i = 0; i < 4 && written; ++i) {
            written = part.write(downloaded->mid(i * piece, piece)) >= 0 && part.flush();
            pump(kWriteGapMs);
        }
        part.close();
        pump(4 * kSettleMs);
        const int beforeRename = s.takeRuns();
        const QStringList reportedBefore = s.importer().problems();
        if (!written || ::rename(QFile::encodeName(part.fileName()).constData(), QFile::encodeName(empty.fileName()).constData()) != 0) {
            fault("S15.58", QStringLiteral("the download could not be written or renamed"));
            return;
        }
        const bool imported = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        expect(beforeRename == 0 && reportedBefore.isEmpty(), QStringLiteral("S15.58: before the rename %1 import(s) ran, reporting [%2]")
                                                                  .arg(beforeRename).arg(reportedBefore.join(QStringLiteral(" | "))));
        expect(imported && s.takeRuns() == 1 && s.importer().notices() == QStringList{QStringLiteral("Imported Downloaded.")} && s.importer().problems().isEmpty(),
               QStringLiteral("S15.58: after the rename the download gave notices [%1] and problems [%2]")
                   .arg(s.importer().notices().join(QStringLiteral(" | ")), s.importer().problems().join(QStringLiteral(" | "))));
        s.importer().dismissNotices();
    }

    // GOAL: S15.81 -- a .wpx copied into the drop folder in pieces, with pauses longer than the
    //       quiet period, is imported once, after it is closed, with nothing reported.
    // CONTROL: app/library/folder_watcher.cpp, handle()'s `hold(*tree, path);` (its IN_MODIFY
    //       branch) deleted -> red, "S15.81: a .wpx copied in with pauses gave notices [Imported
    //       Copied.] and problems [c.wpx could not be imported. It cannot be read as a ZIP archive
    //       ...]" (a half read mid-copy, then the whole after the close); restored.
    {
        QFile file(drop + QStringLiteral("/c.wpx"));
        bool written = file.open(QIODevice::WriteOnly);
        const qsizetype piece = copied->size() / 5 + 1;
        for (int i = 0; i < 5 && written; ++i) {
            written = file.write(copied->mid(i * piece, piece)) >= 0 && file.flush();
            pump(3 * kWriteGapMs);
        }
        file.close();
        const bool imported = written && waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        expect(imported && s.importer().notices() == QStringList{QStringLiteral("Imported Copied.")} && s.importer().problems().isEmpty(),
               QStringLiteral("S15.81: a .wpx copied in with pauses gave notices [%1] and problems [%2]")
                   .arg(s.importer().notices().join(QStringLiteral(" | ")), s.importer().problems().join(QStringLiteral(" | "))));
    }

    // GOAL: S15.81 -- a .wpx still open for writing when the importer starts (the app launched
    //       mid-download) is not imported while it stays open, even across a Rescan; once closed it
    //       is imported once, with nothing reported.
    // WHY-THIS-SHAPE: the importer is built after the file was opened and half-written, so no
    //       event can tell it the file is being written; only the check made when the drop folder
    //       is first watched can.
    // CONTROL: app/library/folder_watcher.cpp, placeWatch()'s `if (placed) { holdOpenFiles(*tree, {}); }`
    //       deleted (watchTargets() after it checks links only) -> red, "S15.81: while early.wpx was
    //       open the importer ran 1 import(s) and reported [early.wpx could not be imported. ...]";
    //       restored.
    {
        const QString drop2 = root + QStringLiteral("/early");
        QFile file(drop2 + QStringLiteral("/early.wpx"));
        const qsizetype half = early->size() / 2;
        if (!QDir().mkpath(drop2) || !file.open(QIODevice::WriteOnly) || file.write(early->left(half)) != half || !file.flush()) {
            fault("S15.81", QStringLiteral("early.wpx could not be started"));
            return;
        }
        Session e(drop2, drop2 + QStringLiteral("/wallpapers"));
        e.importer().rescan();
        const bool quiet = e.quiesce();
        e.importer().rescan();
        const bool quietAgain = e.quiesce();
        const int runsWhileOpen = e.takeRuns();
        const QStringList reported = e.importer().problems();
        const QByteArray rest = early->mid(half);
        const bool written = file.write(rest) == rest.size() && file.flush();
        file.close();
        const bool imported = written && waitUntil([&] { return !e.importer().notices().isEmpty(); }, kWaitMs) && e.quiesce();
        expect(quiet && quietAgain && runsWhileOpen == 0 && reported.isEmpty(),
               QStringLiteral("S15.81: while early.wpx was open the importer ran %1 import(s) and reported [%2]").arg(runsWhileOpen).arg(reported.join(QStringLiteral(" | "))));
        expect(imported && e.takeRuns() == 1 && e.importer().notices() == QStringList{QStringLiteral("Imported Early.")},
               QStringLiteral("S15.81: once closed early.wpx gave notices [%1]").arg(e.importer().notices().join(QStringLiteral(" | "))));
    }
}

// S15.81: the drop folder's listings never overlap.
void queuedListings(const QString& root) {
    const QString drop = root + QStringLiteral("/queued");
    if (!QDir().mkpath(drop)) {
        fault("S15.81", QStringLiteral("the drop folder could not be made"));
        return;
    }
    OpenCounter listings(drop);
    if (!listings.ok()) {
        harnessFailure("S15.81: the open counter was already counting another folder");
        return;
    }
    Importer importer(drop, drop + QStringLiteral("/wallpapers"), 600000);
    // GOAL: S15.81 -- a drop-folder scan asked for while one runs, any number of times, is run
    //       once after it: two listings in all.
    // WHY-THIS-SHAPE: the pool's one thread is held (PoolHold, its blocker seen to start), so the
    //       first listing is queued and three more requests provably arrive while it is outstanding;
    //       listings are counted by the drop folder being opened off the main thread, and none may
    //       have run before the release.
    // CONTROL: app/library/importer.cpp, scanDropFolder()'s `if (m_scanning != Scanning::No) {...}`
    //       block deleted -> red, "S15.81: 4 listing(s) of the drop folder ran, not 2"; restored.
    int beforeRelease = -1;
    {
        PoolHold hold(kWaitMs);
        if (!hold.ok()) {
            fault("S15.81", QStringLiteral("the pool's thread could not be held"));
            return;
        }
        for (int i = 0; i < 4; ++i) {
            importer.rescan();
        }
        beforeRelease = listings.take().scans;
    }
    // The second listing starts from the first one's continuation, so it is waited for by count;
    // then the pool is let go idle twice around a pause, so any further listing is counted too.
    int ran = 0;
    const bool second = waitUntil([&] { ran += listings.take().scans; return ran >= 2; }, kWaitMs);
    const auto idle = [] { return waitUntil([] { return QThreadPool::globalInstance()->activeThreadCount() == 0; }, kWaitMs); };
    bool settled = idle();
    pump(100);
    settled = idle() && settled;
    ran += listings.take().scans;
    expect(second && settled && beforeRelease == 0 && ran == 2,
           QStringLiteral("S15.81: %1 listing(s) of the drop folder ran, not 2 (%2 before the pool was released; settled %3)").arg(ran).arg(beforeRelease).arg(settled));
}

// S15.76: folders nested too deep to remove.
void nesting(const QString& root) {
    const QString dir = root + QStringLiteral("/nesting");
    const QString wallpapers = dir + QStringLiteral("/wallpapers");
    Entries at32 = planEntries(QByteArray(kPlanManifest));
    at32.append({QStringLiteral("d/").repeated(32) + QStringLiteral("leaf.txt"), "32 folders down"});
    Entries at33 = planEntries(titled("Deeper"));
    at33.append({QStringLiteral("d/").repeated(33) + QStringLiteral("leaf.txt"), "33 folders down"});
    if (!QDir().mkpath(wallpapers) || !writeZip(dir + QStringLiteral("/at32.wpx"), at32).isEmpty() || !writeZip(dir + QStringLiteral("/at33.wpx"), at33).isEmpty()) {
        fault("S15.76", QStringLiteral("the nested packages could not be built"));
        return;
    }
    // GOAL: S15.76 -- a package nesting a file 32 folders deep is imported with that file in place;
    //       one nesting 33 is Refused, "It nests folders more than 32 deep.", before anything is
    //       written: no unfinished folder is ever made, and wallpapers/ holds only the first import.
    // WHY-THIS-SHAPE: the unfinished folders made are counted as mkdtemp is called, so "refused
    //       before anything is written" is graded, not only "nothing left afterwards".
    // CONTROL: app/library/importer.cpp, planEntries' `if (entry.name.count(u'/') + (entry.folder ? 1 : 0) > kMaxFolderDepth) {`
    //       made `if (false) {` -> red, "S15.76: a package nesting 33 folders gave Imported ..."; restored.
    const ImportResult deep32 = unpackPackage(dir + QStringLiteral("/at32.wpx"), wallpapers, kNotCancelled);
    const bool leafThere = readBytes(deep32.folderPath + QLatin1Char('/') + QStringLiteral("d/").repeated(32) + QStringLiteral("leaf.txt")) == QByteArrayLiteral("32 folders down");
    expect(deep32.outcome == ImportResult::Outcome::Imported && leafThere,
           QStringLiteral("S15.76: a package nesting 32 folders gave %1, its deepest file %2").arg(describe(deep32), leafThere ? QStringLiteral("in place") : QStringLiteral("missing")));
    const int made = g_unfinishedMade.load();
    const ImportResult deep33 = unpackPackage(dir + QStringLiteral("/at33.wpx"), wallpapers, kNotCancelled);
    const int madeFor33 = g_unfinishedMade.load() - made;
    expect(deep33.outcome == ImportResult::Outcome::Refused && deep33.problem == u"It nests folders more than 32 deep." && madeFor33 == 0 &&
               entriesOf(wallpapers) == QStringList{QFileInfo(deep32.folderPath).fileName()},
           QStringLiteral("S15.76: a package nesting 33 folders gave %1, making %2 unfinished folder(s) and leaving [%3]")
               .arg(describe(deep33)).arg(madeFor33).arg(entriesOf(wallpapers).join(u", ")));
}

// S15.76: an unfinished folder that cannot be removed.
void stuckImport(const QString& root) {
    if (::geteuid() == 0) {
        notVerified("S15.76 (an unfinished folder that cannot be removed)", "running as root, which can remove a file from a read-only folder");
        return;
    }
    const QString dir = root + QStringLiteral("/stuck");
    const QString wallpapers = dir + QStringLiteral("/wallpapers");
    const QString locked = wallpapers + QStringLiteral("/.import-stuck1/locked");
    if (!writeFolder(locked, {{QStringLiteral("half.bin"), "left behind"}}).isEmpty() || !setMode(locked, 0555) ||
        !writeZip(dir + QStringLiteral("/ocean.wpx"), planEntries(QByteArray(kPlanManifest))).isEmpty()) {
        fault("S15.76", QStringLiteral("the unfinished folder that cannot be removed could not be built"));
        static_cast<void>(setMode(locked, 0755));
        return;
    }
    // GOAL: S15.76 -- an unfinished folder a crashed import left that cannot be removed is reported
    //       once, by name and reason, and the import still goes ahead.
    // CONTROL: app/library/importer.cpp, removeImport()'s `if (!removeTree(parentFd, name)) {` made
    //       `if (!removeTree(parentFd, name) && false) {` -> red, "S15.76: an unfinished folder that
    //       cannot be removed gave the warnings []", the only line red; restored.
    QStringList said;
    ImportResult r;
    {
        const WarningLog log;
        r = unpackPackage(dir + QStringLiteral("/ocean.wpx"), wallpapers, kNotCancelled);
        said = log.containing(u"unfinished import");
    }
    if (!setMode(locked, 0755)) {
        fault("S15.76", QStringLiteral("the locked folder could not be made writable again"));
    }
    expect(r.outcome == ImportResult::Outcome::Imported &&
               said == QStringList{QStringLiteral("wallpaperx: the unfinished import .import-stuck1 cannot be removed (Directory not empty)")},
           QStringLiteral("S15.76: an unfinished folder that cannot be removed gave the warnings [%1], the import %2").arg(said.join(u" | "), describe(r)));
}

// S15.76: a source that is not a readable regular file.
void unreadableSources(const QString& root) {
    const QString dir = root + QStringLiteral("/unreadable");
    const QString wallpapers = dir + QStringLiteral("/wallpapers");
    const QString fifo = dir + QStringLiteral("/fifo.wpx");
    if (!QDir().mkpath(wallpapers) || ::mkfifo(QFile::encodeName(fifo).constData(), 0644) != 0) {
        fault("S15.76", QStringLiteral("the FIFO could not be made"));
        return;
    }
    // GOAL: S15.76 -- a FIFO put in a .wpx's place is Refused, "It is not a regular file.", without
    //       blocking the importing thread on the open.
    // WHY-THIS-SHAPE: run on its own thread with a deadline (runBounded), so a hang is reported as
    //       a failure, never as a stuck ctest entry and never as a pass.
    // CONTROL: app/library/importer.cpp, unpackPackage's openRegularFile() replaced by a plain
    //       `::openat(folder, name, O_RDONLY | O_CLOEXEC)` with the type check after it, re-run via
    //       control_batch.sh 2026-10-06 -> red, "S15.76: unpackPackage on a FIFO blocked for 3000 ms";
    //       restored.
    const auto read = runBounded<ImportResult>([&] { return unpackPackage(fifo, wallpapers, kNotCancelled); }, fifo,
                                               QStringLiteral("S15.76: unpackPackage on a FIFO"), 3000);
    if (read) {
        expect(read->outcome == ImportResult::Outcome::Refused && read->problem == u"It is not a regular file." && entriesOf(wallpapers).isEmpty(),
               QStringLiteral("S15.76: a FIFO gave %1").arg(describe(*read)));
    }

    // GOAL: S15.76 -- a .wpx that cannot be opened is Failed (another attempt may pass) with the
    //       kernel's reason.
    // CONTROL: app/library/importer.cpp, the source open's `failed(QStringLiteral("It cannot be opened: %1.").arg(systemError(errno)))`
    //       made `failed(QStringLiteral("It cannot be opened."))` -> red, "S15.76: a mode-000 .wpx gave
    //       Failed  "It cannot be opened.""; restored.
    if (::geteuid() == 0) {
        notVerified("S15.76 (an unreadable .wpx)", "running as root, which can read a mode-000 file");
        return;
    }
    const QString locked = dir + QStringLiteral("/locked.wpx");
    if (!writeZip(locked, planEntries(QByteArray(kPlanManifest))).isEmpty() || !setMode(locked, 0)) {
        fault("S15.76", QStringLiteral("locked.wpx could not be made mode 000"));
        return;
    }
    const ImportResult r = unpackPackage(locked, wallpapers, kNotCancelled);
    if (!setMode(locked, 0644)) {
        fault("S15.76", QStringLiteral("locked.wpx could not be made readable again"));
    }
    expect(r.outcome == ImportResult::Outcome::Failed && r.problem == u"It cannot be opened: Permission denied." && entriesOf(wallpapers).isEmpty(),
           QStringLiteral("S15.76: a mode-000 .wpx gave %1").arg(describe(r)));
}

// S15.75: the rename into place where renameat2 cannot keep a name from being replaced.
void renameFallback(const QString& root) {
    const QString dir = root + QStringLiteral("/rename");
    const QString wallpapers = dir + QStringLiteral("/wallpapers");
    // GOAL: S15.75 -- when renameat2 is refused -- EINVAL (a file system without
    //       RENAME_NOREPLACE), ENOSYS or EPERM (a sandbox filtering it) -- the import still lands
    //       under its name, with nothing unfinished left behind.
    // WHY-THIS-SHAPE: this binary's renameat2 answers the errno without reaching the kernel, so each
    //       refusal is real to the importer on any file system.
    // CONTROL: app/library/importer.cpp, `(errno == EINVAL || errno == ENOSYS || errno == EPERM)`
    //       made `(errno == EINVAL || errno == ENOSYS)` -> red, "S15.75: with renameat2 refused
    //       (EPERM) the import gave Failed  "It cannot be moved into ... Operation not permitted.""
    //       only; `errno == EINVAL || ` dropped -> red, the same for EINVAL only; restored.
    for (const auto& [error, name] : {std::pair{EINVAL, "EINVAL"}, std::pair{ENOSYS, "ENOSYS"}, std::pair{EPERM, "EPERM"}}) {
        const QString package = dir + QStringLiteral("/%1.wpx").arg(QLatin1String(name));
        if (!QDir().mkpath(wallpapers) || !writeZip(package, planEntries(titled(name))).isEmpty()) {
            fault("S15.75", QStringLiteral("%1.wpx could not be built").arg(QLatin1String(name)));
            return;
        }
        const QString wanted = wallpapers + QLatin1Char('/') + expectedName(sha256Of(readBytes(package).value_or(QByteArray())), 0);
        g_renameat2Refused = error;
        ImportResult r;
        {
            // Off on every way out, a throw included, so no later phase renames through the fallback.
            const auto disarm = qScopeGuard([] { g_renameat2Refused = 0; });
            r = unpackPackage(package, wallpapers, kNotCancelled);
        }
        expect(r.outcome == ImportResult::Outcome::Imported && r.folderPath == wanted && readWallpaperFolder(wanted).title == QLatin1String(name) &&
                   unfinishedIn(wallpapers).isEmpty(),
               QStringLiteral("S15.75: with renameat2 refused (%1) the import gave %2, leaving [%3]")
                   .arg(QLatin1String(name), describe(r), unfinishedIn(wallpapers).join(u", ")));
    }
}

// S15.77: a drop-folder file the user also asks for, in either order.
void askedWhileDropped(const QString& root) {
    const auto package = zipBytes(root + QStringLiteral("/asked.scratch"), planEntries(titled("Asked")));
    if (!package) {
        fault("S15.77", QStringLiteral("the package could not be built"));
        return;
    }
    // GOAL: S15.77 -- a file in the drop folder that the user also imports by hand is imported once,
    //       whichever comes first -- the hand import, then a drop-folder scan finding it still
    //       queued; or the scan queuing it, then the hand import -- and it is the user's import:
    //       imported() names the folder, no "Imported ..." notice, and the file is recorded.
    // WHY-THIS-SHAPE: the pool's one thread is held so each order is made, not waited for: in the
    //       first, the listing is queued ahead of the hand import and its result handled while that
    //       import is still running; in the second, the listing's import is queued behind a
    //       second hold when the hand import is asked for. Imports are counted as lookups of the
    //       .wpx, since both would end in the same folder: a hand import looks it up by its full path,
    //       a scan's import by its name in the listed folder with O_PATH, off the main thread. The
    //       drop-folder watch also looks files up by name in the folder, but on the main thread, so it
    //       is not counted.
    // CONTROL: app/library/importer.cpp, Importer::jobFor's branch for a job no scan listed
    //       `return isInFolder(job.path, m_dropDirectory) && ::stat(...) == 0 && keyOf(stampFrom(st)) == key;`
    //       made `return false;` -> "FAIL: S15.77: asked by hand, then found by the scan: 2 import(s) of
    //       the file, ..."; importPath()'s take-over block deleted -> "FAIL: S15.77: found by the scan, then
    //       asked by hand: 2 import(s) of the file, notices [Imported Asked.], ...".
    for (const bool handFirst : {true, false}) {
        const QString drop = root + (handFirst ? QStringLiteral("/asked-hand-first") : QStringLiteral("/asked-scan-first"));
        const QString wallpapers = drop + QStringLiteral("/wallpapers");
        const QString file = drop + QStringLiteral("/good.wpx");
        const QString order = handFirst ? QStringLiteral("asked by hand, then found by the scan") : QStringLiteral("found by the scan, then asked by hand");
        if (!QDir().mkpath(drop) || !writeBytes(file, *package)) {
            fault("S15.77", QStringLiteral("the drop folder could not be built"));
            return;
        }
        const QString folder = wallpapers + QLatin1Char('/') + expectedName(sha256Of(*package), 0);
        Session s(drop, wallpapers);
        const OpensOf opens(file);
        bool held = true;
        {
            PoolHold hold(kWaitMs);
            held = hold.ok();
            s.importer().rescan();
            if (handFirst) {
                s.importer().importPath(file);
            }
        }
        if (!handFirst && held) {
            PoolHold hold(kWaitMs);
            held = hold.ok();
            // The listing's result, which queues its import behind this hold.
            QCoreApplication::sendPostedEvents();
            s.importer().importPath(file);
        }
        if (!held) {
            fault("S15.77", QStringLiteral("the pool's thread could not be held"));
            return;
        }
        const bool quiet = s.quiesce();
        expect(quiet && opens.count() == 1 && s.takeImported() == QStringList{folder} && s.importer().notices().isEmpty() &&
                   recordOf(drop) == QStringList{keyOf(file)},
               QStringLiteral("S15.77: %1: %2 import(s) of the file, notices [%3], the record [%4]")
                   .arg(order).arg(opens.count()).arg(s.importer().notices().join(u" | "), recordOf(drop).join(u" | ")));
    }
}

// S15.77: drop-folder entries that cannot be imported as they stand.
void unusableDrops(const QString& root) {
    const QString drop = root + QStringLiteral("/unusable");
    const QString wallpapers = drop + QStringLiteral("/wallpapers");
    const QString target = root + QStringLiteral("/unusable-target.bin");
    const auto package = zipBytes(root + QStringLiteral("/repaired.scratch"), planEntries(titled("Repaired")));
    if (!package || !QDir().mkpath(drop + QStringLiteral("/folder.wpx")) ||
        ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(drop + QStringLiteral("/dangle.wpx")).constData()) != 0) {
        fault("S15.77", QStringLiteral("the drop folder's unusable entries could not be made"));
        return;
    }
    const QString dangling = QStringLiteral("dangle.wpx could not be imported. It is a link to a file that does not exist.");
    const QString notFile = QStringLiteral("folder.wpx could not be imported. It is not a regular file.");
    // GOAL: S15.77 -- a *.wpx link in the drop folder whose target does not exist, and a folder
    //       named *.wpx, are each reported once, however many scans run, and never recorded: the
    //       link, once its target exists, is imported.
    // CONTROL: app/library/importer.cpp, listDropFolder()'s `if (struct stat link {}; ::fstatat(::dirfd(listing.get()), entry->d_name, &link, AT_SYMLINK_NOFOLLOW) != 0) {`
    //       made `if (true) {` -> red, "S15.77: after three scans the problems are [folder.wpx ...]";
    //       its `dropped.append({name, path, {}, QStringLiteral("It is not a regular file.")});`
    //       deleted -> red, "... [dangle.wpx ...]"; addProblem()'s `if (!m_problems.contains(problem)) {`
    //       made `if (true) {` -> red, the same line with each listed three times; restored.
    Session s(drop, wallpapers);
    bool quiet = true;
    for (int scan = 0; scan < 3 && quiet; ++scan) {
        s.importer().rescan();
        quiet = s.quiesce();
    }
    QStringList problems = s.importer().problems();
    problems.sort();
    expect(quiet && problems == QStringList{dangling, notFile} && recordOf(drop).isEmpty() && s.takeRuns() == 0,
           QStringLiteral("S15.77: after three scans the problems are [%1] and the record [%2]").arg(problems.join(u" | "), recordOf(drop).join(u" | ")));
    if (!writeBytes(target, *package)) {
        fault("S15.77", QStringLiteral("the link's target could not be written"));
        return;
    }
    s.importer().rescan();
    const bool repaired = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
    expect(repaired && s.importer().notices() == QStringList{QStringLiteral("Imported Repaired.")} && recordOf(drop) == QStringList{keyOf(drop + QStringLiteral("/dangle.wpx"))},
           QStringLiteral("S15.77: the link, its target written, gave the notices [%1] and the record [%2]")
               .arg(s.importer().notices().join(u" | "), recordOf(drop).join(u" | ")));
}

// S15.77: the record that cannot be written says why.
void recordUnwritable(const QString& root) {
    const QString drop = root + QStringLiteral("/norecord");
    const QString wallpapers = drop + QStringLiteral("/wallpapers");
    const auto package = zipBytes(root + QStringLiteral("/norecord.scratch"), planEntries(titled("Unrecorded")));
    if (!package || !QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/x.wpx"), *package)) {
        fault("S15.77", QStringLiteral("the drop folder could not be built"));
        return;
    }
    // GOAL: S15.77 -- a record that cannot be written is reported once, with the reason the write
    //       failed, not one a later call left in errno.
    // WHY-THIS-SHAPE: this binary's renameat() refuses the record's final rename into the drop folder
    //       (ENOSPC) after the temporary file beside it was written, so the failure is followed by that
    //       file's removal, which is where a late read of errno would pick up another value. A folder
    //       in the record's place cannot stage it: the record is read before every write, and a folder
    //       there is an unreadable record (S15.332), which writes nothing.
    // CONTROL: app/library/importer.cpp, writeFileAtomically's `const int error = errno;
    //       ::unlinkat(folder, temporary.constData(), 0); return error;` made
    //       `::unlinkat(folder, temporary.constData(), 0); ::unlinkat(folder, temporary.constData(), 0);
    //       return errno;` -> "FAIL: S15.77: the record that cannot be written gave the warnings [... cannot be
    //       written (No such file or directory) ...]". Via control_batch.sh, 2026-10-06. Reverted.
    // COVERAGE-ASSERT: exactly the record's rename must have been refused, and no temporary file left.
    QStringList said;
    bool quiet = false;
    int refused = 0;
    {
        const WarningLog log;
        const RefuseRenameTo refuse(QDir::cleanPath(drop) + QStringLiteral("/.auto-imported"));
        Session s(drop, wallpapers);
        s.importer().rescan();
        quiet = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        said = log.containing(u"cannot be written");
        refused = refuse.refused();
    }
    const QString want = QStringLiteral("wallpaperx: %1/.auto-imported cannot be written (No space left on device), so files in %1 may be imported "
                                        "again")
                             .arg(QString::fromLocal8Bit(QFile::encodeName(QDir::cleanPath(drop))));
    const QStringList left = entriesOf(drop).filter(QRegularExpression(QStringLiteral("^\\.auto-imported")));
    expect(quiet && said == QStringList{want} && refused == 1 && left.isEmpty(),
           QStringLiteral("S15.77: the record that cannot be written gave the warnings [%1] (%2 rename(s) refused, left [%3])")
               .arg(said.join(u" | ")).arg(refused).arg(left.join(u", ")));
}

// Counts opens of a FIFO through inotify -- only this process can open the scratch FIFO, so every open
// counted is this process's -- while holding it open read-write so an open of it never blocks. The
// first opens it counts drop that hold, so a reader that went on to read sees end of file rather than
// hanging; from then on it opens the write end every 20 ms, which releases an open still blocked, and
// counts nothing more -- those opens are mostly its own. A hang guard for the shape a check exists to
// catch, never a verdict. It proves itself on construction: its own open must be counted.
class FifoOpenGuard {
public:
    explicit FifoOpenGuard(const QString& path) : m_path(QFile::encodeName(path)) {
        m_inotify = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_inotify < 0 || ::inotify_add_watch(m_inotify, m_path.constData(), IN_OPEN) < 0) {
            return;
        }
        m_holder = ::open(m_path.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        m_ok = m_holder.load() >= 0 && drain() == 1;
        if (m_ok) {
            m_thread = std::jthread([this](std::stop_token stop) { watch(stop); });
        }
    }
    ~FifoOpenGuard() {
        stop();
        if (const int holder = m_holder.exchange(-1); holder >= 0) {
            ::close(holder);
        }
        if (m_inotify >= 0) {
            ::close(m_inotify);
        }
    }
    FifoOpenGuard(const FifoOpenGuard&) = delete;
    FifoOpenGuard& operator=(const FifoOpenGuard&) = delete;
    bool ok() const { return m_ok; }
    // Opens by anyone else before the guard let go (at least one when any happened). Ends the watch,
    // so call it once, after the work.
    int finish() {
        stop();
        if (m_opens.load() == 0) {
            m_opens += drain();
        }
        return m_opens;
    }

private:
    void stop() {
        if (m_thread.joinable()) {
            m_thread.request_stop();
            m_thread.join();
        }
    }
    int drain() {
        int opens = 0;
        alignas(inotify_event) char buffer[4096];
        for (ssize_t n; (n = ::read(m_inotify, buffer, sizeof buffer)) > 0;) {
            for (ssize_t at = 0; at < n;) {
                const auto* event = reinterpret_cast<const inotify_event*>(buffer + at);
                opens += (event->mask & IN_OPEN) != 0;
                at += ssize_t(sizeof(inotify_event)) + event->len;
            }
        }
        return opens;
    }
    void watch(const std::stop_token& stop) {
        bool released = false;
        while (!stop.stop_requested()) {
            pollfd ready{m_inotify, POLLIN, 0};
            static_cast<void>(::poll(&ready, 1, 20));
            if (const int opened = drain(); opened > 0 && !released) {
                m_opens += opened;
                if (const int holder = m_holder.exchange(-1); holder >= 0) {
                    ::close(holder);
                }
                released = true;
            }
            if (released) {
                if (const int writer = ::open(m_path.constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC); writer >= 0) {
                    ::close(writer);
                }
            }
        }
    }

    QByteArray m_path;
    int m_inotify = -1;
    std::atomic<int> m_holder{-1};
    std::atomic<int> m_opens{0};
    bool m_ok = false;
    std::jthread m_thread;
};

// The record's identity, compared before and after: an unreadable record can be unreadable to this
// process too (mode 000), so its bytes are not what is compared.
QString identityOf(const QString& path) {
    struct stat st {};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0) {
        return QStringLiteral("(none)");
    }
    return QStringLiteral("%1 %2 %3 %4 %5").arg(quint64(st.st_ino)).arg(qint64(st.st_size)).arg(st.st_mode, 0, 8)
        .arg(qint64(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec).arg(qint64(st.st_ctim.tv_sec) * 1'000'000'000 + st.st_ctim.tv_nsec);
}

// S15.332: a record that cannot be read.
void unreadableRecord(const QString& root) {
    const auto first = zipBytes(root + QStringLiteral("/unread-a.scratch"), planEntries(titled("Kept Away")));
    const auto later = zipBytes(root + QStringLiteral("/unread-b.scratch"), planEntries(titled("Dropped Later")));
    if (!first || !later) {
        fault("S15.332", QStringLiteral("the packages could not be built"));
        return;
    }
    struct Kind {
        const char* name;
        const char* why;
    };
    // GOAL: S15.332 -- a drop-folder record that exists but cannot be read (no permission, a folder
    //       or a FIFO in its place, or over 1 MiB) imports nothing from the drop folder, leaves the
    //       record as it was (a FIFO never opened), and is reported -- "<record> cannot be read
    //       (<why>), so files in the drop folder are not imported until it can." -- again at the
    //       next scan after Dismiss. Once it reads again, the next scan uses it: a file it lists stays
    //       unimported (the wallpaper the user deleted stays deleted) and a new file is imported.
    // WHY-THIS-SHAPE: a.wpx sits in the drop folder from the start and is listed in the readable
    //       record written at the end, so "imported nothing" and "the record is used again" are both
    //       about the file a record exists to keep away. Mode 000 is skipped where this process can
    //       read it anyway (root). The FIFO is held open by FifoOpenGuard, so an open that blocks
    //       fails the check instead of hanging the GUI thread, which is the one waiting on it.
    // CONTROL: app/library/importer.cpp: Importer::scanDropFolder
    //       taking an unreadable record as empty after reporting it (`record = QList<DroppedKey>{};` after
    //       addProblem) -> "FAIL: S15.332 (<kind>): 1 import(s) ran, ..." for all four kinds; the
    //       `addProblem(*record.error());` in scanDropFolder deleted -> "FAIL: S15.332 (<kind>): 0 import(s)
    //       ran, reported [] ..." for all four; loadRecordIn()'s openRegularFile(folder, kRecordFileName,
    //       O_RDONLY) replaced by a blocking `::openat(folder, kRecordFileName, O_RDONLY | O_CLOEXEC)` with
    //       the type check after it -> "FAIL: S15.332 (fifo): ..., the FIFO was opened 1 time(s)", alone.
    // COVERAGE-ASSERT: a.wpx's key is in the record each case starts from (or would be, for the
    //       kinds that cannot hold one), and the readable-again half requires the new file imported.
    for (const Kind kind : {Kind{"permission", "Permission denied"}, Kind{"folder", "not a regular file"},
                            Kind{"fifo", "not a regular file"}, Kind{"large", "larger than 1 MiB"}}) {
        const QString label = QStringLiteral("S15.332 (%1)").arg(QLatin1String(kind.name));
        const QString drop = root + QStringLiteral("/unreadable-") + QLatin1String(kind.name);
        const QString wallpapers = drop + QStringLiteral("/wallpapers");
        const QString record = drop + QStringLiteral("/.auto-imported");
        if (!QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/a.wpx"), *first)) {
            fault("S15.332", QStringLiteral("%1: the drop folder could not be built").arg(label));
            return;
        }
        const QByteArray line = keyOf(drop + QStringLiteral("/a.wpx")).toLatin1() + '\n';
        bool made = false;
        if (qstrcmp(kind.name, "permission") == 0) {
            made = writeBytes(record, line) && ::chmod(QFile::encodeName(record).constData(), 0) == 0;
            if (made && ::access(QFile::encodeName(record).constData(), R_OK) == 0) {
                notVerified("S15.332 (permission)", "this process can read a mode-000 file (running as root?)");
                continue;
            }
        } else if (qstrcmp(kind.name, "folder") == 0) {
            made = QDir().mkpath(record);
        } else if (qstrcmp(kind.name, "fifo") == 0) {
            made = ::mkfifo(QFile::encodeName(record).constData(), 0600) == 0;
        } else {
            made = writeBytes(record, line + QByteArray((1 << 20) + 1 - line.size(), '\n'));
        }
        if (!made) {
            fault("S15.332", QStringLiteral("%1: the unreadable record could not be made").arg(label));
            return;
        }
        std::optional<FifoOpenGuard> fifoOpens;
        if (qstrcmp(kind.name, "fifo") == 0) {
            fifoOpens.emplace(record);
            if (!fifoOpens->ok()) {
                harnessFailure("S15.332 (fifo): an inotify watch on the FIFO did not count this test's own open of it");
                return;
            }
        }
        const QString identity = identityOf(record);
        const QString want = QStringLiteral("%1 cannot be read (%2), so files in the drop folder are not imported until it can.")
                                 .arg(QDir::cleanPath(record), QLatin1String(kind.why));
        Session s(drop, wallpapers);
        s.importer().rescan();
        const bool quiet = waitUntil([&] { return !s.importer().problems().isEmpty(); }, kWaitMs) && s.quiesce();
        const int runs = s.takeRuns();
        const QStringList reported = s.importer().problems();
        s.importer().dismissProblems();
        s.importer().rescan();
        const bool again = waitUntil([&] { return !s.importer().problems().isEmpty(); }, kWaitMs) && s.quiesce();
        const int fifoOpened = fifoOpens ? fifoOpens->finish() : 0;
        expect(quiet && again && runs == 0 && s.takeRuns() == 0 && reported == QStringList{want} && s.importer().problems() == QStringList{want} &&
                   entriesOf(wallpapers).isEmpty() && identityOf(record) == identity && fifoOpened == 0,
               QStringLiteral("%1: %2 import(s) ran, reported [%3] and after Dismiss [%4], wallpapers/ holds [%5], the record %6, the FIFO was "
                              "opened %7 time(s)")
                   .arg(label).arg(runs).arg(reported.join(u" | "), s.importer().problems().join(u" | "), entriesOf(wallpapers).join(u", "),
                                             identityOf(record) == identity ? QStringLiteral("unchanged") : QStringLiteral("changed"))
                   .arg(fifoOpened));

        // Readable again, listing a.wpx; b.wpx is new.
        fifoOpens.reset();
        const bool repaired = (QFileInfo(record).isDir() ? QDir().rmdir(record) : QFile::remove(record)) && writeBytes(record, line) &&
                              dropIn(drop, QStringLiteral("b.wpx"), *later);
        if (!repaired) {
            fault("S15.332", QStringLiteral("%1: the record could not be made readable again").arg(label));
            return;
        }
        s.importer().dismissProblems();
        s.importer().rescan();
        const bool used = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        QStringList titles;
        for (const QString& entry : entriesOf(wallpapers)) {
            titles.append(readWallpaperFolder(wallpapers + QLatin1Char('/') + entry).title);
        }
        expect(used && titles == QStringList{QStringLiteral("Dropped Later")} && s.importer().problems().isEmpty(),
               QStringLiteral("%1: once the record read again, wallpapers/ holds [%2] and [%3] was reported").arg(label, titles.join(u", "), s.importer().problems().join(u" | ")));
    }
}

constexpr int kPollMs = 100;

// Waits until a listing of the drop folder counted by `listings` has run, then until the importer
// is quiet. False at the bound.
bool listedAndQuiet(Session& s, OpenCounter& listings) {
    int seen = 0;
    return waitUntil([&] { seen += listings.take().scans; return seen >= 1; }, kWaitMs) && s.quiesce();
}

QString modeName(Importer::Watching watching) {
    return watching == Importer::Watching::Inotify ? QStringLiteral("inotify") : QStringLiteral("polling");
}

// S15.333: a *.wpx link to a FIFO in the drop folder.
void linkToFifo(const QString& root) {
    // GOAL: S15.333 -- the drop-folder watch never opens a FIFO a *.wpx link names: not at the first
    //       scan (inotify), and not by the polling close check when a held file's name is re-pointed
    //       at one. Opening a FIFO or device can act on it.
    // WHY-THIS-SHAPE: opens are counted by an inotify watch on the FIFO itself, proved on this test's
    //       own open first, which also holds the FIFO open so a blocking open fails the check instead
    //       of hanging the run (FifoOpenGuard). The polling case holds y.wpx open for writing so the
    //       close check has a held name to look at, then renames a link to the FIFO over it and closes
    //       the writer.
    // CONTROL: app/library/folder_watcher.cpp, openForWriting()'s openRegularFile() replaced by an
    //       `openat(..., O_RDONLY | O_NONBLOCK)` with no type check; and by a blocking `openat(...,
    //       O_RDONLY)`. Each -> "FAIL: S15.333 (inotify): a .wpx link to a FIFO, there at the start, was
    //       opened 1 time(s) ..." and "FAIL: S15.333 (polling): ... re-pointed at a FIFO, which was opened 1
    //       time(s)". Via control_batch.sh, 2026-10-06. Reverted.
    // COVERAGE-ASSERT: a listing must have run after the link existed, and in the polling case
    //       y.wpx must have been held (not imported) before the rename.
    const auto held = zipBytes(root + QStringLiteral("/held.scratch"), planEntries(titled("Held")));
    for (const auto watching : {Importer::Watching::Inotify, Importer::Watching::Poll}) {
        const QString label = QStringLiteral("S15.333 (%1)").arg(modeName(watching));
        const QString base = root + QStringLiteral("/fifo-") + modeName(watching);
        const QString drop = base + QStringLiteral("/drop");
        const QString fifo = base + QStringLiteral("/elsewhere/pipe");
        if (!held || !QDir().mkpath(drop) || !QDir().mkpath(base + QStringLiteral("/elsewhere")) ||
            ::mkfifo(QFile::encodeName(fifo).constData(), 0600) != 0) {
            fault("S15.333", QStringLiteral("%1: the FIFO could not be made").arg(label));
            return;
        }
        FifoOpenGuard opens(fifo);
        OpenCounter listings(drop);
        if (!opens.ok() || !listings.ok()) {
            harnessFailure((label + QStringLiteral(": the FIFO's open watch or the listing counter did not work")).toStdString());
            return;
        }
        if (watching == Importer::Watching::Inotify) {
            if (!QFile::link(fifo, drop + QStringLiteral("/x.wpx"))) {
                fault("S15.333", QStringLiteral("%1: the link could not be made").arg(label));
                return;
            }
            Session s(drop, drop + QStringLiteral("/wallpapers"), 30000, watching, kPollMs);
            s.importer().rescan();
            const bool listed = listedAndQuiet(s, listings);
            pump(4 * kPollMs);
            const int opened = opens.finish();
            expect(listed && opened == 0, QStringLiteral("%1: a .wpx link to a FIFO, there at the start, was opened %2 time(s) (a listing ran: %3)")
                                              .arg(label).arg(opened).arg(listed));
            continue;
        }
        QFile writer(drop + QStringLiteral("/y.wpx"));
        if (!writer.open(QIODevice::WriteOnly) || writer.write(held->left(held->size() / 2)) <= 0 || !writer.flush()) {
            fault("S15.333", QStringLiteral("%1: the held file could not be written").arg(label));
            return;
        }
        Session s(drop, drop + QStringLiteral("/wallpapers"), 30000, watching, kPollMs);
        s.importer().rescan();
        const bool listed = listedAndQuiet(s, listings);
        pump(4 * kPollMs);
        const int runsWhileHeld = s.takeRuns();
        const QString staged = drop + QStringLiteral("/.link-staging");
        if (!QFile::link(fifo, staged) ||
            ::rename(QFile::encodeName(staged).constData(), QFile::encodeName(drop + QStringLiteral("/y.wpx")).constData()) != 0) {
            fault("S15.333", QStringLiteral("%1: the link could not be renamed over y.wpx").arg(label));
            return;
        }
        writer.close();
        pump(8 * kPollMs);
        static_cast<void>(s.quiesce());
        const int opened = opens.finish();
        expect(listed && runsWhileHeld == 0 && opened == 0,
               QStringLiteral("%1: y.wpx held (%2 import(s) while open, a listing ran: %3), then re-pointed at a FIFO, which was opened %4 time(s)")
                   .arg(label).arg(runsWhileHeld).arg(listed).arg(opened));
    }
}

// S15.338: the record is read at each use, never kept from an earlier read.
void recordReadFresh(const QString& root) {
    const auto first = zipBytes(root + QStringLiteral("/fresh-a.scratch"), planEntries(titled("Listed")));
    const auto later = zipBytes(root + QStringLiteral("/fresh-b.scratch"), planEntries(titled("Unlisted")));
    if (!first || !later) {
        fault("S15.338", QStringLiteral("the packages could not be built"));
        return;
    }
    // GOAL: S15.338 -- a drop folder missing when the importer starts reports nothing; when it then
    //       appears holding a.wpx, b.wpx and a record listing a.wpx only, the next scan imports b.wpx
    //       and not a.wpx.
    // WHY-THIS-SHAPE: the folder is built elsewhere and renamed into place whole, so the one scan
    //       after it sees the files and the record together; a.wpx keeps its inode across the rename,
    //       so the record's line still names it. b.wpx imported shows the scan ran at all.
    // CONTROL: app/library/importer.cpp, each reverted: (a) Importer::loadRecordIn answering an empty
    //       record for a record it has read, so a listed file is not kept away -> "FAIL: S15.338
    //       (appeared): ... gave 1 import(s), wallpapers [Listed, Unlisted] ..."; (b) listDropFolder's
    //       `if (nothingAt(directory, error))` made `if (false)` and Importer::scanDropFolder's
    //       `qWarning(...)` for a listing that failed made `addProblem(...)` -> "FAIL: S15.338 (missing):
    //       ... reported [... cannot be read: No such file or directory.]". The missing folder is reported
    //       by nothing else: no single change of the listing or of the scan alone can put a problem in
    //       front of the user for it.
    {
        const QString base = root + QStringLiteral("/appearing");
        const QString drop = base + QStringLiteral("/drop");
        const QString staged = base + QStringLiteral("/staged");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(staged) || !QDir().mkpath(wallpapers) || !writeBytes(staged + QStringLiteral("/a.wpx"), *first) ||
            !writeBytes(staged + QStringLiteral("/b.wpx"), *later) ||
            !writeBytes(staged + QStringLiteral("/.auto-imported"), keyOf(staged + QStringLiteral("/a.wpx")).toLatin1() + '\n')) {
            fault("S15.338", QStringLiteral("the folder to appear could not be built"));
            return;
        }
        Session s(drop, wallpapers);
        s.importer().rescan();
        const bool quietMissing = s.quiesce();
        expect(quietMissing && s.takeRuns() == 0 && s.importer().problems().isEmpty() && !QFileInfo::exists(drop),
               QStringLiteral("S15.338 (missing): a drop folder missing at the start ran imports or reported [%1]")
                   .arg(s.importer().problems().join(u" | ")));
        if (::rename(QFile::encodeName(staged).constData(), QFile::encodeName(drop).constData()) != 0) {
            fault("S15.338", QStringLiteral("the drop folder could not be moved into place"));
            return;
        }
        s.importer().rescan();
        const bool imported = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        QStringList titles;
        for (const QString& entry : entriesOf(wallpapers)) {
            titles.append(readWallpaperFolder(wallpapers + QLatin1Char('/') + entry).title);
        }
        const int runs = s.takeRuns();
        expect(imported && runs == 1 && titles == QStringList{QStringLiteral("Unlisted")} &&
                   s.importer().notices() == QStringList{QStringLiteral("Imported Unlisted.")},
               QStringLiteral("S15.338 (appeared): the folder that appeared with a record listing a.wpx gave %1 import(s), wallpapers [%2], notices [%3]")
                   .arg(runs).arg(titles.join(u", "), s.importer().notices().join(u" | ")));
    }

    // GOAL: S15.338 -- a drop folder that goes away while the importer runs and comes back is not
    //       imported again: a.wpx, imported and recorded before, is not imported after the return.
    // WHY-THIS-SHAPE: a rename away and back stands in for an unmount and a remount: a scan while it
    //       is away lists nothing, which a record kept in memory would take as "a.wpx is gone" and
    //       forget. Imports are counted by the importer going busy, which an AlreadyPresent run does
    //       too, so a re-import is seen even though the wallpaper is still in the library.
    // CONTROL: Importer::loadRecordIn (app/library/importer.cpp) made to keep the first record it reads
    //       in a static and answer that ever after -> "FAIL: S15.338 (renamed away and back): 1 import(s)
    //       after the folder came back, ...". Via control_batch.sh, 2026-10-06. Reverted.
    {
        const QString base = root + QStringLiteral("/moving");
        const QString drop = base + QStringLiteral("/drop");
        const QString away = base + QStringLiteral("/away");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/a.wpx"), *first)) {
            fault("S15.338", QStringLiteral("the moving drop folder could not be built"));
            return;
        }
        Session s(drop, wallpapers);
        s.importer().rescan();
        const bool importedBefore = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        const QString key = keyOf(drop + QStringLiteral("/a.wpx"));
        if (!importedBefore || s.takeRuns() != 1 || recordOf(drop) != QStringList{key}) {
            fault("S15.338", QStringLiteral("a.wpx was not imported and recorded before the folder moved (record [%1])")
                                 .arg(recordOf(drop).join(u" | ")));
            return;
        }
        const auto move = [](const QString& from, const QString& to) {
            return ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
        };
        if (!move(drop, away)) {
            fault("S15.338", QStringLiteral("the drop folder could not be moved away"));
            return;
        }
        s.importer().rescan();
        const bool quietAway = s.quiesce();
        if (!move(away, drop)) {
            fault("S15.338", QStringLiteral("the drop folder could not be moved back"));
            return;
        }
        s.importer().rescan();
        const bool quietBack = s.quiesce();
        const int runs = s.takeRuns();
        expect(quietAway && quietBack && runs == 0 && recordOf(drop) == QStringList{key} && s.importer().problems().isEmpty(),
               QStringLiteral("S15.338 (renamed away and back): %1 import(s) after the folder came back, the record [%2], reported [%3]")
                   .arg(runs).arg(recordOf(drop).join(u" | "), s.importer().problems().join(u" | ")));
    }
}

// S15.342: a drop folder that comes back between a scan's listing and its record read.
void folderBackMidScan(const QString& root) {
    const auto package = zipBytes(root + QStringLiteral("/midscan.scratch"), planEntries(titled("Kept")));
    const QString base = root + QStringLiteral("/midscan");
    const QString drop = base + QStringLiteral("/drop");
    const QString away = base + QStringLiteral("/away");
    const QString wallpapers = base + QStringLiteral("/wallpapers");
    if (!package || !QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/a.wpx"), *package)) {
        fault("S15.342", QStringLiteral("the drop folder could not be built"));
        return;
    }
    // GOAL: S15.342 -- a scan whose listing found the drop folder missing neither imports nor prunes
    //       the record, even when the folder is back before the scan ends: a.wpx, recorded, whose
    //       wallpaper the user deleted, stays deleted then and at the next scan. False for a prune that
    //       takes "the listing was empty" from a folder that was not there as "every recorded file is
    //       gone".
    // WHY-THIS-SHAPE: this binary's opendir() moves the folder back into place right after the scan's
    //       listing -- a worker's opendir, never the main thread's, whose walks are the watcher's --
    //       found it missing, so anything the scan does afterwards by the folder's path finds
    //       the folder and its record -- the order a mount completing during a scan produces, which no
    //       timing can stage on demand. Imports are counted by the importer going busy, an
    //       AlreadyPresent run included.
    // CONTROL: in Importer::scanDropFolder (app/library/importer.cpp), a listing that found the folder
    //       missing made to open the folder by path, read its record and save it pruned of every key ->
    //       "FAIL: S15.342: a scan listed the drop folder missing and then read its record back: 1
    //       import(s) ran by the next scan's end, and the deleted wallpaper is back".
    // COVERAGE-ASSERT: the move must have happened inside opendir (a harness fault otherwise).
    Session s(drop, wallpapers);
    s.importer().rescan();
    const bool first = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
    const QString key = keyOf(drop + QStringLiteral("/a.wpx"));
    const QString folder = wallpapers + QLatin1Char('/') + expectedName(sha256Of(*package), 0);
    if (!first || s.takeRuns() != 1 || recordOf(drop) != QStringList{key} || !QDir(folder).removeRecursively() ||
        ::rename(QFile::encodeName(drop).constData(), QFile::encodeName(away).constData()) != 0) {
        fault("S15.342", QStringLiteral("a.wpx was not imported and recorded, or its wallpaper deleted, or the folder moved away"));
        return;
    }
    {
        const std::scoped_lock lock(g_returnLock);
        g_returnTo = QFile::encodeName(QDir::cleanPath(drop));
        g_returnFrom = QFile::encodeName(away);
        g_returnsDone = 0;
        g_returnMain = pthread_self();
        g_returnArmed = true;
    }
    setOpendirHook(&returnAfterMissedListing);
    const auto unhook = qScopeGuard([] {
        g_returnArmed = false;
        setOpendirHook(nullptr);
    });
    s.importer().rescan();
    const bool quiet = s.quiesce();
    g_returnArmed = false;
    if (g_returnsDone.load() != 1) {
        harnessFailure("S15.342: the listing never found the drop folder missing, so it was never moved back mid-scan");
        return;
    }
    s.importer().rescan();
    const bool again = s.quiesce();
    const int runs = s.takeRuns();
    expect(quiet && again && runs == 0 && !QFileInfo::exists(folder),
           QStringLiteral("S15.342: a scan listed the drop folder missing and then read its record back: %1 import(s) ran by the next "
                          "scan's end, and the deleted wallpaper is %2")
               .arg(runs).arg(QFileInfo::exists(folder) ? QStringLiteral("back") : QStringLiteral("still gone")));
}

// S15.340: a name re-pointed at a FIFO between its lookup and its open.
void swapToFifo(const QString& root) {
    const auto package = zipBytes(root + QStringLiteral("/swap.scratch"), planEntries(titled("Swapped")));
    const QString base = root + QStringLiteral("/swap");
    const QString fifo = base + QStringLiteral("/elsewhere/pipe");
    if (!package || !QDir().mkpath(base + QStringLiteral("/elsewhere")) || ::mkfifo(QFile::encodeName(fifo).constData(), 0600) != 0) {
        fault("S15.340", QStringLiteral("the FIFO could not be made"));
        return;
    }
    // Stages a link to the FIFO in `folder` and arms its rename over `target` after the next O_PATH
    // lookup of `name`, spelled as the code under test spells it.
    const auto arm = [&fifo](const QString& folder, const QByteArray& name, const QString& target) {
        const QString staged = folder + QStringLiteral("/.swap-staged");
        if (!QFile::link(fifo, staged)) {
            return false;
        }
        const std::scoped_lock lock(g_swapLock);
        g_swapName = name;
        g_swapFrom = QFile::encodeName(staged);
        g_swapTo = QFile::encodeName(target);
        g_swapsDone = 0;
        g_swapArmed = true;
        return true;
    };
    const auto disarm = qScopeGuard([] { g_swapArmed = false; });
    // The race staged, or a harness fault saying why not: a lookup the swap never followed graded nothing.
    const auto staged = [](const QString& label) {
        g_swapArmed = false;
        if (g_swapsDone.load() != 1) {
            harnessFailure((label + QStringLiteral(": the name was never looked up with O_PATH, so the swap never ran")).toStdString());
            return false;
        }
        return true;
    };

    // GOAL: S15.340 -- no FIFO is ever opened through a name that was a regular file when it was
    //       looked up and was re-pointed at a FIFO before the open: not by the import of a .wpx
    //       (unpackPackage), not by the drop-folder watch's check for a writer (openForWriting), and
    //       not by the scan's read of the drop-folder record (Importer::loadRecordIn, reached from
    //       scanDropFolder). Each goes on with the file it looked up, and nothing hangs. The record
    //       read after an import (finish(), the same loadRecordIn) is not reached here: no import runs.
    // WHY-THIS-SHAPE: this binary's openat() renames a link to the FIFO over the name the moment the
    //       code under test has looked it up with O_PATH -- the window no test can hit by timing. Opens
    //       are counted on the FIFO itself (FifoOpenGuard), which also keeps a broken shape from
    //       hanging the run: it holds the FIFO open so an open does not block, and lets go so a read
    //       ends. The import must still be of the file looked up, and the record still used, so
    //       "refused everything" is not a pass.
    // CONTROL: in openRegularFile (app/library/wpx_package.cpp) the reopen through /proc/self/fd made
    //       `::openat(dirfd, name, (flags & ~O_NOFOLLOW) | O_CLOEXEC)` -> one line per site, the helper
    //       being shared: "FAIL: S15.340 (unpackPackage): ... the FIFO was opened 1 time(s)", "FAIL: S15.340
    //       (openForWriting): ..." and "FAIL: S15.340 (loadRecordIn): ...". Via control_batch.sh, 2026-10-06.
    //       Reverted.
    // COVERAGE-ASSERT: the swap must have run once per site (a harness fault otherwise), and the
    //       guard must count its own open first.
    {
        const QString label = QStringLiteral("S15.340 (unpackPackage)");
        const QString dir = base + QStringLiteral("/unpack");
        const QString wallpapers = dir + QStringLiteral("/wallpapers");
        const QString target = dir + QStringLiteral("/x.wpx");
        FifoOpenGuard guard(fifo);
        if (!QDir().mkpath(wallpapers) || !writeBytes(target, *package) || !guard.ok() || !arm(dir, QFile::encodeName(target), target)) {
            fault("S15.340", label + QStringLiteral(": the package, the FIFO's guard or the swap could not be set up"));
            return;
        }
        const auto r = runBounded<ImportResult>([&] { return unpackPackage(target, wallpapers, kNotCancelled); }, fifo, label, 5000);
        const int opened = guard.finish();
        expect(!staged(label) ||
                   (r && r->outcome == ImportResult::Outcome::Imported && readWallpaperFolder(r->folderPath).title == u"Swapped" && opened == 0),
               QStringLiteral("%1: x.wpx re-pointed at a FIFO after its lookup gave %2, and the FIFO was opened %3 time(s)")
                   .arg(label, r ? describe(*r) : QStringLiteral("nothing")).arg(opened));
    }
    {
        const QString label = QStringLiteral("S15.340 (openForWriting)");
        const QString drop = base + QStringLiteral("/watch/drop");
        const QString target = drop + QStringLiteral("/x.wpx");
        FifoOpenGuard guard(fifo);
        if (!QDir().mkpath(drop) || !writeBytes(target, *package) || !guard.ok() || !arm(drop, "x.wpx", target)) {
            fault("S15.340", label + QStringLiteral(": the drop folder, the FIFO's guard or the swap could not be set up"));
            return;
        }
        bool quiet = false;
        {
            Session s(drop, base + QStringLiteral("/watch/wallpapers"));
            s.importer().rescan();
            quiet = s.quiesce();
        }
        const int opened = guard.finish();
        expect(!staged(label) || (quiet && opened == 0), QStringLiteral("%1: x.wpx re-pointed at a FIFO after the watch looked it up; the FIFO was opened %2 time(s)")
                                         .arg(label).arg(opened));
    }
    {
        const QString label = QStringLiteral("S15.340 (loadRecordIn)");
        const QString drop = base + QStringLiteral("/record/drop");
        const QString record = drop + QStringLiteral("/.auto-imported");
        FifoOpenGuard guard(fifo);
        if (!QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/a.wpx"), *package) ||
            !writeBytes(record, keyOf(drop + QStringLiteral("/a.wpx")).toLatin1() + '\n') || !guard.ok() || !arm(drop, ".auto-imported", record)) {
            fault("S15.340", label + QStringLiteral(": the drop folder, the FIFO's guard or the swap could not be set up"));
            return;
        }
        bool quiet = false;
        int runs = -1;
        {
            Session s(drop, base + QStringLiteral("/record/wallpapers"));
            s.importer().rescan();
            quiet = s.quiesce();
            runs = s.takeRuns();
        }
        const int opened = guard.finish();
        expect(!staged(label) || (quiet && opened == 0 && runs == 0),
               QStringLiteral("%1: the record re-pointed at a FIFO after its lookup; the FIFO was opened %2 time(s), and %3 import(s) ran "
                              "of the file the record listed")
                   .arg(label).arg(opened).arg(runs));
    }

    // GOAL: S15.340 -- a .wpx that is a link to a FIFO from the start is refused, "It is not a regular
    //       file.", without the FIFO being opened.
    // CONTROL: in unpackPackage (app/library/importer.cpp) openRegularFile replaced by a plain
    //       `::openat(folder, name, O_RDONLY | O_CLOEXEC)` followed by the type check, re-run via
    //       control_batch.sh, 2026-10-06 -> "FAIL: S15.340 (a link to a FIFO): unpackPackage gave Refused "It is not a
    //       regular file.", and the FIFO was opened 1 time(s)" (beside the harness fault that the
    //       swap above found no O_PATH lookup to follow). Reverted.
    {
        const QString dir = base + QStringLiteral("/linked");
        const QString link = dir + QStringLiteral("/linked.wpx");
        FifoOpenGuard guard(fifo);
        if (!QDir().mkpath(dir + QStringLiteral("/wallpapers")) || !QFile::link(fifo, link) || !guard.ok()) {
            fault("S15.340", QStringLiteral("the link to the FIFO or its guard could not be made"));
            return;
        }
        const auto r = runBounded<ImportResult>([&] { return unpackPackage(link, dir + QStringLiteral("/wallpapers"), kNotCancelled); }, fifo,
                                                QStringLiteral("S15.340 (a link to a FIFO)"), 5000);
        const int opened = guard.finish();
        expect(r && r->outcome == ImportResult::Outcome::Refused && r->problem == u"It is not a regular file." && opened == 0,
               QStringLiteral("S15.340 (a link to a FIFO): unpackPackage gave %1, and the FIFO was opened %2 time(s)")
                   .arg(r ? describe(*r) : QStringLiteral("nothing")).arg(opened));
    }
}

// S15.89: a *.wpx link in the drop folder to a file still being written.
void linkedWhileWritten(const QString& root) {
    const auto linked = zipBytes(root + QStringLiteral("/linked.scratch"), planEntries(titled("Linked")));
    if (!linked) {
        fault("S15.89", QStringLiteral("the package could not be built"));
        return;
    }
    // GOAL: S15.89 -- a *.wpx link in the drop folder to a file outside it that some process has
    //       open and half-written -- linked before the importer starts, or linked in while it runs --
    //       is not imported while the file stays open, whatever listings run; once the writer closes
    //       it, it is imported once, whole: one import, "Imported Linked.", nothing reported, the
    //       record keyed by the whole file. The same when the drop folder is polled.
    // WHY-THIS-SHAPE: the file lives outside the drop folder, so the folder's own watch reports none
    //       of its writes, and the writer pauses while listings run, so only asking the kernel
    //       whether the linked file is open for writing can hold it. A listing is counted after the
    //       link exists (OpenCounter), so "not imported while open" is graded only once one ran; the
    //       half is a broken archive, so importing it would be reported.
    // CONTROL: app/library/folder_watcher.cpp, openForWriting()'s `O_RDONLY | (m_items == Items::Files ? 0 : O_NOFOLLOW)`
    //       made `O_RDONLY | O_NOFOLLOW` (via control_batch.sh 2026-10-05) -> red, all four cases, e.g. "S15.89 (inotify,
    //       linked before the start): while the linked file was open and half-written 1 import(s)
    //       ran, reporting [linked.wpx could not be imported. ...]"; watchTargets()'s
    //       `if (openForWriting(dirFd, entry->d_name)) { hold(tree, name); }` deleted -> red, "S15.89
    //       (inotify, linked in while running): ..." and no other S15.89 case (linked before the
    //       start, placeWatch's first check holds it; polling checks at the end of each scan), with
    //       S15.91, which relies on that check alone; restored.
    for (const auto watching : {Importer::Watching::Inotify, Importer::Watching::Poll}) {
        for (const bool atStart : {true, false}) {
            const QString label = QStringLiteral("S15.89 (%1, %2)").arg(modeName(watching), atStart ? QStringLiteral("linked before the start") : QStringLiteral("linked in while running"));
            const QString base = root + QStringLiteral("/linked-%1-%2").arg(modeName(watching)).arg(atStart ? QStringLiteral("start") : QStringLiteral("later"));
            const QString drop = base + QStringLiteral("/drop");
            const QString target = base + QStringLiteral("/downloads/linked.wpx");
            const QString link = drop + QStringLiteral("/linked.wpx");
            QFile file(target);
            const qsizetype half = linked->size() / 2;
            if (!QDir().mkpath(drop) || !QDir().mkpath(base + QStringLiteral("/downloads")) || !file.open(QIODevice::WriteOnly) ||
                file.write(linked->left(half)) != half || !file.flush() || (atStart && !QFile::link(target, link))) {
                fault("S15.89", QStringLiteral("%1: the half-written file or its link could not be made").arg(label));
                return;
            }
            OpenCounter listings(drop);
            if (!listings.ok()) {
                harnessFailure("S15.89: the open counter was already counting another folder");
                return;
            }
            Session s(drop, drop + QStringLiteral("/wallpapers"), 30000, watching, kPollMs);
            s.importer().rescan();
            bool listed = listedAndQuiet(s, listings);
            if (!atStart) {
                if (!QFile::link(target, link)) {
                    fault("S15.89", QStringLiteral("%1: the link could not be made").arg(label));
                    return;
                }
                listed = listedAndQuiet(s, listings) && listed;
            }
            pump(4 * kPollMs);
            const bool quiet = s.quiesce();
            const int runsWhileOpen = s.takeRuns();
            const QStringList reportedWhileOpen = s.importer().problems();
            const QStringList recordWhileOpen = recordOf(drop);
            const QByteArray rest = linked->mid(half);
            const bool written = file.write(rest) == rest.size() && file.flush();
            file.close();
            if (!written) {
                fault("S15.89", QStringLiteral("%1: the rest of the file could not be written").arg(label));
                return;
            }
            const bool settled = waitUntil([&] { return !s.importer().notices().isEmpty() || !s.importer().problems().isEmpty(); }, kWaitMs) && s.quiesce();
            expect(listed && quiet && runsWhileOpen == 0 && reportedWhileOpen.isEmpty() && recordWhileOpen.isEmpty(),
                   QStringLiteral("%1: while the linked file was open and half-written %2 import(s) ran, reporting [%3] and recording [%4] (a listing ran after the link: %5)")
                       .arg(label).arg(runsWhileOpen).arg(reportedWhileOpen.join(u" | "), recordWhileOpen.join(u" | ")).arg(listed));
            const int runs = s.takeRuns();
            expect(settled && runs == 1 && s.importer().notices() == QStringList{QStringLiteral("Imported Linked.")} && s.importer().problems().isEmpty() &&
                       recordOf(drop) == QStringList{keyOf(link)},
                   QStringLiteral("%1: once the writer closed it, %2 import(s) ran, noticing [%3], reporting [%4], recording [%5] (the whole file is %6)")
                       .arg(label).arg(runs).arg(s.importer().notices().join(u" | "), s.importer().problems().join(u" | "), recordOf(drop).join(u" | "), keyOf(link)));
        }
    }
}

// S15.90: a linked file rewritten in place.
void linkedRewritten(const QString& root) {
    const auto before = zipBytes(root + QStringLiteral("/before.scratch"), planEntries(titled("Before")));
    const auto after = zipBytes(root + QStringLiteral("/rewritten.scratch"), planEntries(titled("Rewritten")));
    if (!before || !after) {
        fault("S15.90", QStringLiteral("the packages could not be built"));
        return;
    }
    // GOAL: S15.90 -- a *.wpx link in the drop folder whose file, outside it, is rewritten in place
    //       after it was imported -- truncated, written in two halves with a pause, closed -- is
    //       imported again once, whole, after the close: "Imported Rewritten.", nothing reported.
    //       Watched and polled.
    // WHY-THIS-SHAPE: the file keeps its inode and lives outside the drop folder, so nothing that
    //       happens in the folder itself changes; only a watch on the file the link names (inotify)
    //       or following the link when looking (polling) can notice. The pause is four polls, so a
    //       reader acting on quiet alone would import the half.
    // CONTROL: app/library/folder_watcher.cpp, handle()'s branch for a link's file
    //       `} else if (const auto target = tree->targets.constFind(event.wd); target != tree->targets.cend()) {...}`
    //       its test prefixed with `false &&` -> red, "S15.90 (inotify): the rewrite gave 0 import(s), noticing []
    //       ..." (and S15.89's two inotify "once the writer closed it" lines); watchTargets()'s
    //       `::inotify_add_watch(m_inotify, (m_directory + '/' + name).constData(), kTargetEvents)`
    //       made `-1` -> red, the same lines and S15.92's; observe()'s `const int follow = m_items == Items::Files ? 0 : AT_SYMLINK_NOFOLLOW;`
    //       made `AT_SYMLINK_NOFOLLOW` -> red, "S15.90 (polling): the rewrite gave 0 import(s) ..." (and
    //       S15.89's two polling cases); restored.
    for (const auto watching : {Importer::Watching::Inotify, Importer::Watching::Poll}) {
        const QString label = QStringLiteral("S15.90 (%1)").arg(modeName(watching));
        const QString base = root + QStringLiteral("/rewritten-%1").arg(modeName(watching));
        const QString drop = base + QStringLiteral("/drop");
        const QString target = base + QStringLiteral("/elsewhere/wallpaper.wpx");
        if (!QDir().mkpath(drop) || !QDir().mkpath(base + QStringLiteral("/elsewhere")) || !writeBytes(target, *before) ||
            !QFile::link(target, drop + QStringLiteral("/linked.wpx"))) {
            fault("S15.90", QStringLiteral("%1: the linked file could not be made").arg(label));
            return;
        }
        Session s(drop, drop + QStringLiteral("/wallpapers"), 30000, watching, kPollMs);
        s.importer().rescan();
        const bool first = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        if (!first || s.importer().notices() != QStringList{QStringLiteral("Imported Before.")}) {
            expect(false, QStringLiteral("%1: the link to a finished file was not imported first: noticing [%2], reporting [%3]")
                              .arg(label, s.importer().notices().join(u" | "), s.importer().problems().join(u" | ")));
            continue;
        }
        s.importer().dismissNotices();
        s.takeRuns();
        QFile file(target);
        const qsizetype half = after->size() / 2;
        bool written = file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(after->left(half)) == half && file.flush();
        pump(4 * kPollMs);
        written = written && file.write(after->mid(half)) == after->size() - half && file.flush();
        file.close();
        if (!written) {
            fault("S15.90", QStringLiteral("%1: the file could not be rewritten").arg(label));
            return;
        }
        const bool settled = waitUntil([&] { return !s.importer().notices().isEmpty() || !s.importer().problems().isEmpty(); }, kWaitMs) && s.quiesce();
        const int runs = s.takeRuns();
        expect(settled && runs == 1 && s.importer().notices() == QStringList{QStringLiteral("Imported Rewritten.")} && s.importer().problems().isEmpty(),
               QStringLiteral("%1: the rewrite gave %2 import(s), noticing [%3], reporting [%4]")
                   .arg(label).arg(runs).arg(s.importer().notices().join(u" | "), s.importer().problems().join(u" | ")));
    }
}

// S15.91: a link whose file's watch cannot be placed.
void linkWatchRefused(const QString& root) {
    constexpr int hold = 300;
    const auto linked = zipBytes(root + QStringLiteral("/unwatched.scratch"), planEntries(titled("Unwatched")));
    const auto later = zipBytes(root + QStringLiteral("/later.scratch"), planEntries(titled("Later")));
    const QString base = root + QStringLiteral("/refused-watch");
    const QString drop = base + QStringLiteral("/drop");
    const QString target = base + QStringLiteral("/downloads/unwatched.wpx");
    const QString link = drop + QStringLiteral("/unwatched.wpx");
    QFile file(target);
    const qsizetype half = linked ? linked->size() / 2 : 0;
    if (!linked || !later || !QDir().mkpath(drop) || !QDir().mkpath(base + QStringLiteral("/downloads")) || !file.open(QIODevice::WriteOnly) ||
        file.write(linked->left(half)) != half || !file.flush()) {
        fault("S15.91", QStringLiteral("the half-written file could not be made"));
        return;
    }
    // GOAL: S15.91 -- a watch on a linked file that the kernel refuses for want of resources does
    //       not switch the drop folder to polling: no warning is logged and a package dropped in
    //       later is imported through the folder's own watch; and the linked file, open and
    //       half-written, is still held by the check made at each scan and at each lapse of the
    //       hold, and imported once, whole, after the close.
    // WHY-THIS-SHAPE: this binary's inotify_add_watch refuses exactly the link's path with ENOSPC
    //       (RefuseWatchOf), a real refusal to the watcher on any machine; the host's watch limit is
    //       never touched. The importer polls only every 600 s, so "imported later" can come only
    //       from a live watch. The hold is 300 ms, so the pause spans several lapses, each of which
    //       must find the file still open.
    // CONTROL: app/library/folder_watcher.cpp, watchTargets() given `if (wd < 0) { pollInstead("inotify_add_watch", errno); return; }`
    //       after its inotify_add_watch -> red, "S15.91: after the link's watch was refused the folder
    //       logged [wallpaperx: inotify_add_watch failed (No space left on device), so ... is polled
    //       every 600000 ms] and a package dropped in later was imported 0"; watchTargets()'s
    //       `if (openForWriting(dirFd, entry->d_name)) { hold(tree, name); }` deleted -> red, "S15.91:
    //       with its watch refused, while the linked file was open 1 import(s) ran ..."; restored.
    const RefuseWatchOf refuse(link);
    OpenCounter listings(drop);
    if (!listings.ok()) {
        harnessFailure("S15.91: the open counter was already counting another folder");
        return;
    }
    const WarningLog log;
    Session s(drop, drop + QStringLiteral("/wallpapers"), hold, Importer::Watching::Inotify, 600000);
    s.importer().rescan();
    const bool started = listedAndQuiet(s, listings);
    if (!QFile::link(target, link)) {
        fault("S15.91", QStringLiteral("the link could not be made"));
        return;
    }
    const bool listed = listedAndQuiet(s, listings);
    pump(4 * hold);
    const bool quiet = s.quiesce();
    const int runsWhileOpen = s.takeRuns();
    const QStringList reportedWhileOpen = s.importer().problems();
    const QByteArray rest = linked->mid(half);
    const bool written = file.write(rest) == rest.size() && file.flush();
    file.close();
    if (!written) {
        fault("S15.91", QStringLiteral("the rest of the file could not be written"));
        return;
    }
    const bool settled = waitUntil([&] { return !s.importer().notices().isEmpty() || !s.importer().problems().isEmpty(); }, kWaitMs) && s.quiesce();
    const int runs = s.takeRuns();
    const QStringList noticed = s.importer().notices();
    const bool laterDropped = dropIn(drop, QStringLiteral("later.wpx"), *later);
    const bool laterImported = laterDropped && waitUntil([&] { return s.importer().notices().size() == 2; }, kWaitMs) && s.quiesce();
    const QStringList warned = log.containing(u"failed (");
    if (refuse.refused() == 0) {
        notVerified("S15.91", "the watch of the link's file was never asked for, so its refusal was not staged");
        return;
    }
    expect(started && listed && quiet && runsWhileOpen == 0 && reportedWhileOpen.isEmpty(),
           QStringLiteral("S15.91: with its watch refused, while the linked file was open %1 import(s) ran, reporting [%2] (listed %3)")
               .arg(runsWhileOpen).arg(reportedWhileOpen.join(u" | ")).arg(started && listed));
    expect(settled && runs == 1 && noticed == QStringList{QStringLiteral("Imported Unwatched.")} && s.importer().problems().isEmpty(),
           QStringLiteral("S15.91: with its watch refused, once the writer closed it %1 import(s) ran, noticing [%2], reporting [%3]")
               .arg(runs).arg(noticed.join(u" | "), s.importer().problems().join(u" | ")));
    expect(warned.isEmpty() && laterImported,
           QStringLiteral("S15.91: after the link's watch was refused the folder logged [%1] and a package dropped in later was imported %2")
               .arg(warned.join(u" | ")).arg(laterImported));
}

// This process's inotify descriptors.
QList<int> inotifyDescriptors() {
    QList<int> found;
    DIR* fds = ::opendir("/proc/self/fd");
    if (fds == nullptr) {
        return found;
    }
    while (const dirent* entry = ::readdir(fds)) {
        char target[64] = {};
        if (::readlinkat(::dirfd(fds), entry->d_name, target, sizeof target - 1) > 0 && std::strcmp(target, "anon_inode:inotify") == 0) {
            found.append(std::atoi(entry->d_name));
        }
    }
    ::closedir(fds);
    return found;
}

// The watches the inotify descriptor `fd` holds, as the kernel lists them; -1 when unreadable.
int watchesOn(int fd) {
    const auto info = readBytes(QStringLiteral("/proc/self/fdinfo/%1").arg(fd));
    return info ? int(info->count("inotify wd:")) : -1;
}

// S15.92: a removed link's watch.
void linkWatchDropped(const QString& root) {
    const auto watched = zipBytes(root + QStringLiteral("/watched.scratch"), planEntries(titled("Watched")));
    const QString base = root + QStringLiteral("/dropped-watch");
    const QString drop = base + QStringLiteral("/drop");
    const QString link = drop + QStringLiteral("/watched.wpx");
    if (!watched || !QDir().mkpath(drop) || !writeBytes(base + QStringLiteral("/watched.wpx"), *watched) ||
        !QFile::link(base + QStringLiteral("/watched.wpx"), link)) {
        fault("S15.92", QStringLiteral("the linked file could not be made"));
        return;
    }
    // GOAL: S15.92 -- once a *.wpx link is removed from the drop folder and a scan has run, the
    //       watch on the file it named is gone: the importer's inotify descriptor holds the
    //       folder's watch alone, where it held that and the file's while the link was there.
    // WHY-THIS-SHAPE: the kernel lists every watch of a descriptor in /proc/self/fdinfo, which is
    //       the only view of watches that nothing in the importer can misreport. The importer's
    //       descriptor is the one inotify descriptor that appeared as it was built.
    // CONTROL: app/library/folder_watcher.cpp, watchTargets()'s removal loop
    //       `for (auto it = tree.targets.keyBegin(); ...) { if (!targets.contains(*it)) { ::inotify_rm_watch(m_inotify, *it); } }`
    //       deleted -> red, "S15.92: the importer's descriptor held 2 watch(es) with the link and 2
    //       once it was removed"; restored.
    const QList<int> before = inotifyDescriptors();
    Session s(drop, drop + QStringLiteral("/wallpapers"));
    QList<int> added = inotifyDescriptors();
    added.removeIf([&before](int fd) { return before.contains(fd); });
    if (added.size() != 1) {
        fault("S15.92", QStringLiteral("the importer's inotify descriptor could not be told apart: %1 appeared").arg(added.size()));
        return;
    }
    s.importer().rescan();
    const bool imported = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
    const int withLink = watchesOn(added.constFirst());
    OpenCounter listings(drop);
    if (!listings.ok() || !QFile::remove(link)) {
        fault("S15.92", QStringLiteral("the open counter was busy, or the link could not be removed"));
        return;
    }
    const bool listed = listedAndQuiet(s, listings);
    const int withoutLink = watchesOn(added.constFirst());
    expect(imported && listed && withLink == 2 && withoutLink == 1,
           QStringLiteral("S15.92: the importer's descriptor held %1 watch(es) with the link and %2 once it was removed (imported %3, listed after %4)")
               .arg(withLink).arg(withoutLink).arg(imported).arg(listed));
}

// S15.93: a file: URL whose path holds %00, given as a file dialog gives it.
void nulInFileUrl(const QString& root) {
    const QString dir = root + QStringLiteral("/nul-url");
    const QString wallpapers = dir + QStringLiteral("/wallpapers");
    const auto named = zipBytes(root + QStringLiteral("/named.scratch"), planEntries(titled("Named")));
    if (!named || !QDir().mkpath(wallpapers) || !writeBytes(dir + QStringLiteral("/named.wpx"), *named)) {
        fault("S15.93", QStringLiteral("named.wpx could not be written"));
        return;
    }
    // GOAL: S15.93 -- a file: URL whose path decodes to a NUL byte (".../named.wpx%00.jpg") is
    //       refused as "not a file on this computer", and nothing is imported, though the path
    //       before the NUL names a real package; the same URL without %00 imports it.
    // WHY-THIS-SHAPE: importFile() is what the Import dialog calls; the importer's drop folder is
    //       never listed here (no rescan()), so any import is this URL's. Every system call would
    //       end the path at the NUL, naming named.wpx.
    // CONTROL: app/library/wpx_package.cpp, localPathOfFileUrl's `if (!path.startsWith('/') || path.contains('\0')) {`
    //       made `if (!path.startsWith('/')) {` -> red, "S15.93: file://.../named.wpx%00.jpg gave 1
    //       import(s), the problems [] and wallpapers/ holding [<name>]", the only line red;
    //       app/library/importer.cpp, importFile()'s `if (!path) {` made `if (true) {` -> red, "S15.93:
    //       the same URL without the NUL left wallpapers/ holding [], not one Named", the only line
    //       red; restored.
    const QByteArray folderUrl = "file://" + QFile::encodeName(dir).toPercentEncoding("/");
    Session s(dir + QStringLiteral("/drop"), wallpapers);
    s.importer().importFile(QUrl::fromEncoded(folderUrl + "/named.wpx%00.jpg"));
    const bool quiet = s.quiesce();
    const int runs = s.takeRuns();
    const QStringList problems = s.importer().problems();
    // One arg() call: "%00" in the format, or in text a chained arg() inserted, is a placeholder.
    expect(quiet && runs == 0 && problems.size() == 1 && problems.constFirst().endsWith(QLatin1String(" is not a file on this computer, so it cannot be imported.")) &&
               entriesOf(wallpapers).isEmpty(),
           QStringLiteral("S15.93: %1 gave %2 import(s), the problems [%3] and wallpapers/ holding [%4]")
               .arg(QStringLiteral("file://.../named.wpx%00.jpg"), QString::number(runs), problems.join(u" | "), entriesOf(wallpapers).join(u", ")));
    s.importer().importFile(QUrl::fromEncoded(folderUrl + "/named.wpx"));
    const bool plain = s.quiesce();
    const QStringList made = entriesOf(wallpapers);
    expect(plain && made.size() == 1 && readWallpaperFolder(wallpapers + QLatin1Char('/') + made.value(0)).title == u"Named",
           QStringLiteral("S15.93: the same URL without the NUL left wallpapers/ holding [%1], not one Named").arg(made.join(u", ")));
}

// A drop folder given as a link, re-pointed at another folder while a scan or an import is under way:
// a mount changing under the importer. Re-pointed by a rename over the link, so it is never missing.
[[nodiscard]] bool pointLink(const QByteArray& link, const QByteArray& at) {
    const QByteArray staged = link + ".repoint";
    ::unlink(staged.constData());
    return ::symlink(at.constData(), staged.constData()) == 0 && ::rename(staged.constData(), link.constData()) == 0;
}

std::mutex g_repointLock;
QByteArray g_repointLink;
QByteArray g_repointTo;
QByteArray g_repointName;  // for the lookup hook: the file name whose lookup triggers it
pthread_t g_repointMain{};
std::atomic<bool> g_repointArmed{false};
std::atomic<int> g_repointsDone{0};

void armRepoint(const QString& link, const QString& to, const QByteArray& name = {}) {
    const std::scoped_lock lock(g_repointLock);
    g_repointLink = QFile::encodeName(QDir::cleanPath(link));
    g_repointTo = QFile::encodeName(to);
    g_repointName = name;
    g_repointMain = pthread_self();
    g_repointsDone = 0;
    g_repointArmed = true;
}

void repointNow() {
    g_repointArmed = false;
    if (pointLink(g_repointLink, g_repointTo)) {
        ++g_repointsDone;
    }
}

// After a worker -- never the main thread, whose walks are the watcher's -- has listed the link.
void repointAfterListing(const char* path, DIR* result) {
    if (result == nullptr || !g_repointArmed.load() || path == nullptr || pthread_equal(pthread_self(), g_repointMain)) {
        return;
    }
    const std::scoped_lock lock(g_repointLock);
    if (g_repointArmed.load() && g_repointLink == path) {
        repointNow();
    }
}

// Just before a worker first looks up a file of that name with O_PATH, by its name or by a path
// ending in it: after the scan's listing, so the watcher sees no change while the scan runs.
void repointBeforeLookup(int, const char* path, int flags) {
    if (!g_repointArmed.load() || path == nullptr || (flags & O_PATH) == 0 || pthread_equal(pthread_self(), g_repointMain)) {
        return;
    }
    const std::scoped_lock lock(g_repointLock);
    const QByteArray looked(path);
    if (g_repointArmed.load() && !g_repointName.isEmpty() && (looked == g_repointName || looked.endsWith('/' + g_repointName))) {
        repointNow();
    }
}

// S15.352: the drop folder's record is written to the folder whose files were listed.
void recordThroughListedFolder(const QString& root) {
    const auto package = zipBytes(root + QStringLiteral("/listed.scratch"), planEntries(titled("Listed")));
    if (!package) {
        fault("S15.352", QStringLiteral("the package could not be built"));
        return;
    }
    setOpendirHook(&repointAfterListing);
    const auto unhook = qScopeGuard([] {
        g_repointArmed = false;
        setOpendirHook(nullptr);
    });
    // GOAL: S15.352 -- a scan's record write goes to the folder the scan listed: when another folder is
    //       at the drop folder's path by then (a mount changed), that folder's record is left as it was.
    // WHY-THIS-SHAPE: the drop folder is a link, re-pointed from A to B by this binary's opendir()
    //       right after the scan's worker has listed A -- no timing can put a mount change there on
    //       demand. B's record ("7 8 9") names no file, so any save into B would prune it to nothing.
    //       Polled with a long period, so no second scan runs on its own.
    // CONTROL: in Importer::scanDropFolder (app/library/importer.cpp), the prune's
    //       `saveRecord(folder, identity, *record)` made `saveRecord(openDropFolder()->get(), identity,
    //       *record)`, saving by path -> "FAIL: S15.352 (prune): the folder listed has the record [1 2 3], the
    //       folder at the path []". Re-run via control_batch.sh, 2026-10-07.
    // COVERAGE-ASSERT: the link must have been re-pointed exactly once, by the scan's worker.
    {
        const QString label = QStringLiteral("S15.352 (prune)");
        const QString base = root + QStringLiteral("/listed-prune");
        const QString a = base + QStringLiteral("/A");
        const QString b = base + QStringLiteral("/B");
        const QString link = base + QStringLiteral("/drop");
        const bool built = QDir().mkpath(a) && QDir().mkpath(b) && writeBytes(b + QStringLiteral("/.auto-imported"), "7 8 9\n") &&
                           writeBytes(a + QStringLiteral("/.auto-imported"), "1 2 3\n");
        if (!built || !pointLink(QFile::encodeName(link), QFile::encodeName(a))) {
            fault("S15.352", label + QStringLiteral(": the two folders and the link could not be built"));
            return;
        }
        Session s(link, base + QStringLiteral("/wallpapers"), 30000, Importer::Watching::Poll, 600000);
        armRepoint(link, b);
        s.importer().rescan();
        const bool quiet = s.quiesce();
        g_repointArmed = false;
        if (g_repointsDone.load() != 1) {
            harnessFailure((label + QStringLiteral(": the scan's worker never listed the drop folder, so it was never re-pointed")).toStdString());
            return;
        }
        expect(quiet && recordOf(a).isEmpty() && recordOf(b) == QStringList{QStringLiteral("7 8 9")},
               QStringLiteral("%1: the folder listed has the record [%2], the folder at the path [%3]")
                   .arg(label, recordOf(a).join(u" | "), recordOf(b).join(u" | ")));
    }

    // GOAL: S15.352 -- a file a scan listed is recorded in the folder it was listed in as soon as its
    //       import ends, whatever is at the drop path by then -- nothing, or another folder -- and
    //       whether or not the user also asked for it; another folder at the path keeps its record.
    // WHY-THIS-SHAPE: the import is held in its mkdtemp, its source open, while the user asks for the
    //       same file (which makes it the user's) and the drop path is unlinked or re-pointed to B; then
    //       released. The records are read at once, before any scan, so "recorded later" cannot pass.
    // CONTROL: in Importer::finish (app/library/importer.cpp), `if (job.listedIn)` made `if (false &&
    //       job.listedIn)` -> all four "FAIL: S15.352 (...): after the import the listed folder's record is []
    //       ...", another folder's "[<key> | 7 8 9]". Re-run via control_batch.sh, 2026-10-07.
    // COVERAGE-ASSERT: each import must have been held, and the user's ask merged into it (one run).
    //
    // GOAL: S15.367 -- a key recorded is not also owed: B at the path, holding the same file (a hard link,
    //       the same key), has it imported by its next scan, not taken as owed.
    // WHY-THIS-SHAPE: a debt owed to A alone could not keep x out of B (S15.370), so this grades the debt
    //       that could: one with no folder, as a filesystem without f_fsid owes. It cannot catch a debt
    //       owed to A after a good write.
    // CONTROL: in Importer::finish (app/library/importer.cpp), `owe(std::nullopt);` added before the last
    //       `addOnce`, after the successful saveRecord, standing in for a no-f_fsid host -> "FAIL: S15.367: a key recorded at once
    //       is not also owed: with B at the path, holding the same file, B's scan ran 0 import(s) and wrote
    //       [<key>]". Via control_batch.sh, 2026-10-07.
    for (const bool anotherFolder : {false, true}) {
        for (const bool userToo : {false, true}) {
            const QString label = QStringLiteral("S15.352 (%1, %2)")
                                      .arg(anotherFolder ? QStringLiteral("another folder at the path") : QStringLiteral("nothing at the path"),
                                           userToo ? QStringLiteral("asked by the user") : QStringLiteral("found by the scan"));
            const QString base = root + QStringLiteral("/listed-at-once-%1-%2").arg(int(anotherFolder)).arg(int(userToo));
            const QString a = base + QStringLiteral("/A");
            const QString b = base + QStringLiteral("/B");
            const QString link = base + QStringLiteral("/drop");
            const QString wallpapers = base + QStringLiteral("/wallpapers");
            const bool built = QDir().mkpath(a) && QDir().mkpath(b) && writeBytes(b + QStringLiteral("/.auto-imported"), "7 8 9\n") &&
                               writeBytes(a + QStringLiteral("/x.wpx"), *package) &&
                               ::link(QFile::encodeName(a + QStringLiteral("/x.wpx")).constData(),
                                      QFile::encodeName(b + QStringLiteral("/x.wpx")).constData()) == 0 &&
                               pointLink(QFile::encodeName(link), QFile::encodeName(a));
            if (!built) {
                fault("S15.352", label + QStringLiteral(": the two folders and the link could not be built"));
                return;
            }
            const QString key = keyOf(a + QStringLiteral("/x.wpx"));
            Session s(link, wallpapers, 30000, Importer::Watching::Poll, 600000);
            g_holdImports = true;
            const auto release = qScopeGuard([] { g_holdImports = false; });
            s.importer().rescan();
            const bool held = waitUntil([] { return g_importHeld.load(); }, kWaitMs);
            if (userToo) {
                s.importer().importPath(link + QStringLiteral("/x.wpx"));
            }
            const bool moved = anotherFolder ? pointLink(QFile::encodeName(link), QFile::encodeName(b))
                                             : ::unlink(QFile::encodeName(link).constData()) == 0;
            g_holdImports = false;
            const bool done = waitUntil([&] { return !s.importer().busy(); }, kWaitMs);
            const int runs = s.takeRuns();
            if (!held || !moved || !done || runs != 1) {
                harnessFailure((label + QStringLiteral(": the import was not held (%1), the path not changed (%2), or it "
                                                       "ran %3 time(s)")
                                            .arg(held)
                                            .arg(moved)
                                            .arg(runs))
                                   .toStdString());
                return;
            }
            const QStringList recordA = recordOf(a);
            const QStringList recordB = recordOf(b);
            const bool told = userToo ? s.takeImported().size() == 1 && s.importer().notices().isEmpty()
                                      : s.importer().notices() == QStringList{QStringLiteral("Imported Listed.")};
            expect(recordA == QStringList{key} && recordB == QStringList{QStringLiteral("7 8 9")} && told,
                   QStringLiteral("%1: after the import the listed folder's record is [%2], the other folder's [%3]; told %4")
                       .arg(label, recordA.join(u" | "), recordB.join(u" | "))
                       .arg(told));
            if (!anotherFolder || userToo) {
                continue;
            }
            s.importer().rescan();
            const bool scanned = s.quiesce();
            const int bRuns = s.takeRuns();
            expect(scanned && bRuns == 1 && recordOf(b) == QStringList{key},
                   QStringLiteral("S15.367: a key recorded at once is not also owed: with B at the path, holding the same "
                                  "file, B's scan ran %1 import(s) and wrote [%2]")
                       .arg(bRuns)
                       .arg(recordOf(b).join(u" | ")));
        }
    }
}

// A record that cannot be read: a folder in its place.
[[nodiscard]] bool spoilRecord(const QString& drop) {
    const QByteArray record = QFile::encodeName(drop + QStringLiteral("/.auto-imported"));
    ::unlink(record.constData());
    return ::mkdir(record.constData(), 0700) == 0;
}

[[nodiscard]] bool repairRecord(const QString& drop) {
    return QDir().rmdir(drop + QStringLiteral("/.auto-imported"));
}

// Whether Importer::identityOf can tell folders at `path` apart: a filesystem ID that is neither zero nor
// only the device number. Asked through fstatfs, so the fake above answers too.
bool foldersTellApart(const QString& path) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct statfs fs {};
    struct stat st {};
    const bool got = ::fstatfs(fd, &fs) == 0 && ::fstat(fd, &st) == 0;
    ::close(fd);
    const quint32 first = quint32(fs.f_fsid.__val[0]);
    const quint32 second = quint32(fs.f_fsid.__val[1]);
    return got && (first != 0 || second != 0) && !(second == 0 && first == quint32(st.st_dev));
}

// GOAL: S15.370, S15.378, S15.380 (another folder), S15.381 are reported not verified, never failed, where the
//       scratch filesystem gives folders no identity: there key-only matching is the design (S15.385).
// WHY-THIS-SHAPE: judged by the test's own reading of fstatfs, not by asking the importer, so a break in
//       Importer::identityOf still reaches those checks and goes red (S15.378's control (b)).
// CONTROL: by hand, 2026-10-07: this binary's fake (Device) forced on for the whole scratch folder ->
//       owedKeys and owedByFolder print NOT VERIFIED for exactly those four and ctest says Skipped, no FAIL;
//       with this guard also forced true -> FAIL for S15.370, S15.381, S15.378, S15.380 (another folder).
bool foldersToldApart(const char* goals, const QString& root) {
    if (foldersTellApart(root)) {
        return true;
    }
    notVerified(goals, "the scratch filesystem at " + root.toStdString() +
                           " reports a filesystem ID that is zero or only the device number (XFS, FAT, exFAT, f2fs), where "
                           "debts are matched by key alone by design");
    return false;
}

// The user imports x.wpx while the record cannot be read: its key is owed to this folder.
[[nodiscard]] bool oweX(Session& s, const QString& drop) {
    s.importer().importPath(drop + QStringLiteral("/x.wpx"));
    return s.quiesce() && s.takeImported().size() == 1 && !s.importer().problems().filter(QStringLiteral("cannot be read")).isEmpty();
}

// The user imports `file` while this binary's open() refuses the drop folder with O_PATH: its key is owed
// with no folder to name.
[[nodiscard]] bool oweWithNoFolder(Session& s, const QString& drop, const QString& file) {
    {
        const std::scoped_lock lock(g_refusedOpenLock);
        g_refusedOpen = QFile::encodeName(QDir::cleanPath(QDir(drop).absolutePath()));
    }
    g_refusingOpen = true;
    const auto stopRefusing = qScopeGuard([] { g_refusingOpen = false; });
    s.importer().importPath(file);
    const bool done = s.quiesce();
    g_refusingOpen = false;
    return done && s.takeImported().size() == 1 && !s.importer().problems().filter(QStringLiteral("cannot be opened")).isEmpty();
}

// Another folder at the drop path, as a volume mounted or unmounted there: the folder moved aside and an
// empty one made in its place, on the same filesystem with another inode. And back again.
[[nodiscard]] bool swapOut(const QString& drop, const QString& aside) {
    return ::rename(QFile::encodeName(drop).constData(), QFile::encodeName(aside).constData()) == 0 && QDir().mkdir(drop);
}

[[nodiscard]] bool swapBack(const QString& drop, const QString& aside) {
    return QDir(drop).removeRecursively() && ::rename(QFile::encodeName(aside).constData(), QFile::encodeName(drop).constData()) == 0;
}

// S15.368, S15.369, S15.370, S15.381: what is owed, and for how long.
void owedKeys(const QString& root) {
    const auto package = zipBytes(root + QStringLiteral("/owed-keys.scratch"), planEntries(titled("Owed")));
    if (!package) {
        fault("S15.368", QStringLiteral("the package could not be built"));
        return;
    }

    // GOAL: S15.368 -- a key owed for a file that has since left the drop folder is no longer owed once
    //       a listing no longer shows the file: the file put back (the same key), with the record lacking
    //       it and its wallpaper deleted, is imported again. The debt is pruned whether or not the record
    //       can be read at that listing.
    // WHY-THIS-SHAPE: the record is a folder when the user's import finishes, so the key is owed; x.wpx is
    //       then moved out (rename: the key stays), a scan lists the folder without it, and x.wpx comes
    //       back. Twice: with the record repaired before that scan, and with it still unreadable then.
    // CONTROL: in Importer::scanDropFolder (app/library/importer.cpp), (a) the debt prune
    //       `m_unrecorded.removeIf(...)` made a no-op -> "FAIL: S15.368 (record readable at the prune): once a
    //       listing no longer showed the owed file, the file put back ran 0 import(s), notices []" and the
    //       unreadable line; (b) that prune made conditional on `if (record)` -> the unreadable line alone. Re-run
    //       via control_batch.sh, 2026-10-07.
    // COVERAGE-ASSERT: the key must have been owed -- the import done and the record unreadable then.
    for (const bool readableAtPrune : {true, false}) {
        const QString label = readableAtPrune ? QStringLiteral("S15.368 (record readable at the prune)")
                                              : QStringLiteral("S15.368 (record unreadable at the prune)");
        const QString base = root + (readableAtPrune ? QStringLiteral("/owed-gone-readable") : QStringLiteral("/owed-gone-unreadable"));
        const QString drop = base + QStringLiteral("/drop");
        const QString away = base + QStringLiteral("/away.wpx");
        const QString file = drop + QStringLiteral("/x.wpx");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !writeBytes(file, *package) || !spoilRecord(drop)) {
            fault("S15.368", label + QStringLiteral(": the drop folder could not be built"));
            return;
        }
        Session s(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
        s.importer().importPath(file);
        const bool imported = s.quiesce() && s.takeImported().size() == 1;
        const bool movedOut = ::rename(QFile::encodeName(file).constData(), QFile::encodeName(away).constData()) == 0;
        if (!imported || !movedOut || !s.importer().problems().filter(QStringLiteral("cannot be read")).size()) {
            harnessFailure((label + QStringLiteral(": the import did not finish owed, or the file could not be moved out")).toStdString());
            return;
        }
        if (readableAtPrune && !repairRecord(drop)) {
            fault("S15.368", label + QStringLiteral(": the record could not be repaired"));
            return;
        }
        s.importer().rescan();
        const bool pruned = s.quiesce();
        const QStringList made = entriesOf(wallpapers);
        const bool back = (readableAtPrune || repairRecord(drop)) && made.size() == 1 &&
                          QDir(wallpapers + QLatin1Char('/') + made.constFirst()).removeRecursively() &&
                          ::rename(QFile::encodeName(away).constData(), QFile::encodeName(file).constData()) == 0;
        if (!back) {
            fault("S15.368", label + QStringLiteral(": the wallpaper could not be deleted or the file put back"));
            return;
        }
        s.importer().dismissNotices();
        s.takeRuns();
        s.importer().rescan();
        const bool again = s.quiesce();
        const int runs = s.takeRuns();
        expect(pruned && again && runs == 1 && entriesOf(wallpapers).size() == 1 &&
                   s.importer().notices() == QStringList{QStringLiteral("Imported Owed.")},
               QStringLiteral("%1: once a listing no longer showed the owed file, the file put back ran %2 import(s), notices [%3]")
                   .arg(label)
                   .arg(runs)
                   .arg(s.importer().notices().join(u" | ")));
    }

    // GOAL: S15.369 -- a key owed while a scan's listing is out, for a file that listing could not show,
    //       survives that scan, and the next listing that holds the file writes it to the record: the
    //       file is not imported again.
    // WHY-THIS-SHAPE: this binary's readdir() holds the scan's worker once it has read the folder to its
    //       end, before the scan's continuation runs; x.wpx arrives then and the user's import of it ends
    //       owed (the record is a folder). Released, the scan prunes against a listing without x.wpx.
    // CONTROL: in Importer::finish (app/library/importer.cpp), `addOnce(m_finishedSinceListing);`
    //       deleted from `owe` -> "FAIL: S15.369: a key owed while a listing was out survived it: the next scan
    //       ran 1 import(s) and wrote [<key>]". Re-run via control_batch.sh, 2026-10-07.
    // COVERAGE-ASSERT: the listing must have been held, and the import owed while it was.
    {
        const QString base = root + QStringLiteral("/owed-mid-listing");
        const QString drop = base + QStringLiteral("/drop");
        const QString file = drop + QStringLiteral("/x.wpx");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !spoilRecord(drop)) {
            fault("S15.369", QStringLiteral("the drop folder could not be built"));
            return;
        }
        Session s(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
        {
            const std::scoped_lock lock(g_listingLock);
            g_heldListingPath = QFile::encodeName(QDir::cleanPath(drop));
            g_heldListingDir = nullptr;
            g_listingMain = pthread_self();
        }
        g_holdListing = true;
        setOpendirHook(&noteHeldListing);
        const auto unhook = qScopeGuard([] {
            g_holdListing = false;
            setOpendirHook(nullptr);
        });
        s.importer().rescan();
        const bool listingHeld = waitUntil([] { return g_listingHeld.load(); }, kWaitMs);
        bool owedMeanwhile = false;
        if (listingHeld && writeBytes(file, *package)) {
            s.importer().importPath(file);
            owedMeanwhile = waitUntil([&] { return !s.takeImported().isEmpty(); }, kWaitMs) &&
                            !s.importer().problems().filter(QStringLiteral("cannot be read")).isEmpty();
        }
        g_holdListing = false;
        const bool settled = s.quiesce();
        const QStringList made = entriesOf(wallpapers);
        if (!listingHeld || !owedMeanwhile || !settled || made.size() != 1 || !repairRecord(drop) ||
            !QDir(wallpapers + QLatin1Char('/') + made.constFirst()).removeRecursively()) {
            harnessFailure("S15.369: the listing was not held, or the import did not end owed while it was");
            return;
        }
        s.takeRuns();
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(quiet && runs == 0 && entriesOf(wallpapers).isEmpty() && recordOf(drop) == QStringList{keyOf(file)},
               QStringLiteral("S15.369: a key owed while a listing was out survived it: the next scan ran %1 import(s) and wrote [%2]")
                   .arg(runs)
                   .arg(recordOf(drop).join(u" | ")));
    }

    // GOAL: S15.369 -- a key recorded while a scan's listing is out, for a file that listing does not
    //       show (moved away meanwhile), is not pruned by that scan: put back, the file is not imported
    //       again.
    // WHY-THIS-SHAPE: the scan's own import of x.wpx is held in mkdtemp, its source open; x.wpx is moved
    //       away and a second scan's listing is held at its end; the import is let go and records the key;
    //       then the listing, which lacks x.wpx, is let go. Put back, x.wpx must not be imported.
    // CONTROL: in Importer::scanDropFolder (app/library/importer.cpp), `&& !m_finishedSinceListing.contains(key)`
    //       deleted from `gone` -> "FAIL: S15.369 (recorded while listed): the record after the scan listed before
    //       it is []; 1 import(s) once the file was back", and the owed-while-listed line above. Re-run via
    //       control_batch.sh, 2026-10-07.
    // COVERAGE-ASSERT: the import and the second listing must both have been held.
    {
        const QString base = root + QStringLiteral("/recorded-mid-listing");
        const QString drop = base + QStringLiteral("/drop");
        const QString file = drop + QStringLiteral("/x.wpx");
        const QString away = base + QStringLiteral("/away.wpx");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !writeBytes(file, *package)) {
            fault("S15.369", QStringLiteral("the drop folder could not be built"));
            return;
        }
        const QString key = keyOf(file);
        Session s(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
        g_holdImports = true;
        const auto releaseImport = qScopeGuard([] { g_holdImports = false; });
        s.importer().rescan();
        const bool importHeld = waitUntil([] { return g_importHeld.load(); }, kWaitMs);
        const bool movedAway = ::rename(QFile::encodeName(file).constData(), QFile::encodeName(away).constData()) == 0;
        {
            const std::scoped_lock lock(g_listingLock);
            g_heldListingPath = QFile::encodeName(QDir::cleanPath(drop));
            g_heldListingDir = nullptr;
            g_listingMain = pthread_self();
        }
        g_holdListing = true;
        setOpendirHook(&noteHeldListing);
        const auto unhook = qScopeGuard([] {
            g_holdListing = false;
            setOpendirHook(nullptr);
        });
        s.importer().rescan();
        const bool listingHeld = waitUntil([] { return g_listingHeld.load(); }, kWaitMs);
        g_holdImports = false;
        const bool recorded = waitUntil([&] { return recordOf(drop) == QStringList{key}; }, kWaitMs);
        g_holdListing = false;
        const bool settled = s.quiesce();
        const QStringList afterScan = recordOf(drop);
        if (!importHeld || !movedAway || !listingHeld || !recorded || !settled) {
            harnessFailure(QStringLiteral("S15.369 (recorded while listed): the import held %1, moved %2, listing held %3, "
                                          "recorded %4")
                               .arg(importHeld)
                               .arg(movedAway)
                               .arg(listingHeld)
                               .arg(recorded)
                               .toStdString());
            return;
        }
        s.takeRuns();
        const bool putBack = ::rename(QFile::encodeName(away).constData(), QFile::encodeName(file).constData()) == 0;
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(putBack && quiet && afterScan == QStringList{key} && runs == 0,
               QStringLiteral("S15.369 (recorded while listed): the record after the scan listed before it is [%1]; %2 import(s) "
                              "once the file was back")
                   .arg(afterScan.join(u" | "))
                   .arg(runs));
    }

    // GOAL: S15.370 -- a key owed to folder A does not keep the same file out of another folder B at the
    //       drop path: B's scan imports it once and writes the key to B's record.
    // WHY-THIS-SHAPE: the record is turned into a folder as the scan's import looks its file up, so the
    //       key is owed to A; then the drop path is re-pointed at B, which holds a hard link to the same file
    //       (the same key), so only the folder can tell the debt from B's file.
    // CONTROL: in Importer::scanDropFolder (app/library/importer.cpp), the absorb predicate made to ignore
    //       the folder, `(true || owedTo(owed, identity)) && owed.key == key` -> "FAIL: S15.370: a key owed to
    //       another folder kept the same file out of B: B's scan ran 0 import(s), notices [], B's record
    //       [<key>]". Via control_batch.sh, 2026-10-07.
    // COVERAGE-ASSERT: the record must have been spoiled exactly once, by the import's lookup.
    //
    // GOAL: S15.381 -- writing B's record settles nothing owed to A: once A is back at the path with a
    //       readable record, its scan imports nothing and writes the key to A's record.
    // WHY-THIS-SHAPE: continues S15.370, whose import in B writes a record holding the very key A is owed --
    //       the one case where a record written elsewhere could settle A's debt. A's record is repaired
    //       (empty) and the wallpaper deleted, so only the debt keeps x out of A's import queue.
    // CONTROL: in Importer::settle (app/library/importer.cpp), which saveRecord calls, the predicate made to
    //       ignore the folder, `(true || owedTo(owed, identity)) && record.contains(owed.key)` -> "FAIL: S15.381:
    //       B's record settled a debt owed to A: once A was back, its scan ran 1 import(s), notices [Imported
    //       Owed.], A's record [<key>]", the S15.370 line staying green. Via control_run.sh, 2026-10-07.
    if (foldersToldApart("S15.370, S15.381", root)) {
        const QString base = root + QStringLiteral("/owed-elsewhere");
        const QString a = base + QStringLiteral("/A");
        const QString b = base + QStringLiteral("/B");
        const QString link = base + QStringLiteral("/drop");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(a) || !QDir().mkpath(b) || !writeBytes(a + QStringLiteral("/x.wpx"), *package) ||
            !pointLink(QFile::encodeName(link), QFile::encodeName(a))) {
            fault("S15.370", QStringLiteral("the two folders and the link could not be built"));
            return;
        }
        static std::atomic<bool> s_spoilArmed{false};
        static std::atomic<int> s_spoiled{0};
        static QByteArray s_record;
        s_record = QFile::encodeName(a + QStringLiteral("/.auto-imported"));
        s_spoiled = 0;
        s_spoilArmed = true;
        g_repointMain = pthread_self();
        g_beforeOpenat = [](int, const char* path, int flags) {
            if (!s_spoilArmed.load() || path == nullptr || (flags & O_PATH) == 0 || pthread_equal(pthread_self(), g_repointMain) ||
                std::strcmp(path, "x.wpx") != 0 || !s_spoilArmed.exchange(false)) {
                return;
            }
            ::unlink(s_record.constData());
            if (::mkdir(s_record.constData(), 0700) == 0) {
                ++s_spoiled;
            }
        };
        const auto unhook = qScopeGuard([] {
            s_spoilArmed = false;
            g_beforeOpenat = nullptr;
        });
        Session s(link, wallpapers, 30000, Importer::Watching::Poll, 600000);
        s.importer().rescan();
        const bool imported = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        if (!imported || s_spoiled.load() != 1 ||
            ::link(QFile::encodeName(a + QStringLiteral("/x.wpx")).constData(), QFile::encodeName(b + QStringLiteral("/x.wpx")).constData()) != 0 ||
            !pointLink(QFile::encodeName(link), QFile::encodeName(b))) {
            harnessFailure(QStringLiteral("S15.370: the import ran %1, the record was spoiled %2 time(s), or B could not be set up")
                               .arg(imported)
                               .arg(s_spoiled.load())
                               .toStdString());
            return;
        }
        const QString key = keyOf(b + QStringLiteral("/x.wpx"));
        if (!deleteWallpapers(wallpapers)) {
            fault("S15.370", QStringLiteral("the wallpaper could not be deleted"));
            return;
        }
        s.takeRuns();
        s.importer().dismissNotices();
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(quiet && runs == 1 && s.importer().notices() == QStringList{QStringLiteral("Imported Owed.")} &&
                   recordOf(b) == QStringList{key},
               QStringLiteral("S15.370: a key owed to another folder kept the same file out of B: B's scan ran %1 import(s), "
                              "notices [%2], B's record [%3]")
                   .arg(runs)
                   .arg(s.importer().notices().join(u" | "), recordOf(b).join(u" | ")));

        if (!deleteWallpapers(wallpapers) || !repairRecord(a) || !pointLink(QFile::encodeName(link), QFile::encodeName(a))) {
            fault("S15.381", QStringLiteral("the wallpaper could not be deleted, A's record repaired, or the drop path "
                                            "pointed back at A"));
            return;
        }
        s.takeRuns();
        s.importer().dismissNotices();
        s.importer().rescan();
        const bool back = s.quiesce();
        const int runsBack = s.takeRuns();
        expect(back && runsBack == 0 && s.importer().notices().isEmpty() && recordOf(a) == QStringList{key},
               QStringLiteral("S15.381: B's record settled a debt owed to A: once A was back, its scan ran %1 import(s), "
                              "notices [%2], A's record [%3]")
                   .arg(runsBack)
                   .arg(s.importer().notices().join(u" | "), recordOf(a).join(u" | ")));
    }
}

// S15.378, S15.379, S15.380, S15.384: a debt belongs to the folder it is owed to.
void owedByFolder(const QString& root) {
    const auto owedX = zipBytes(root + QStringLiteral("/owed-folder-x.scratch"), planEntries(titled("Owed X")));
    const auto laterY = zipBytes(root + QStringLiteral("/owed-folder-y.scratch"), planEntries(titled("Later Y")));
    if (!owedX || !laterY) {
        fault("S15.378", QStringLiteral("the packages could not be built"));
        return;
    }

    // GOAL: S15.378 -- a debt is pruned only by a listing of its own folder: a key owed to the drop
    //       folder survives a scan that lists another folder at the path without the file, and once its
    //       folder is back the file is not imported again (its wallpaper deleted meanwhile).
    // WHY-THIS-SHAPE: the drop folder is renamed aside and an empty folder made at the path, which stands in
    //       for an unmounted drive's mount point: the same filesystem, so the two differ by inode alone. A
    //       rename, not a link re-pointed as S15.370 does, so the debt's own folder comes back with its
    //       identity, as a remounted one does.
    // CONTROL: in app/library/importer.cpp, (a) Importer::scanDropFolder's debt prune made to ignore the
    //       folder, `(true || owedTo(owed, identity)) && gone(owed.key)` -> "FAIL: S15.378: a debt survived a
    //       listing of another folder at the drop path: once its own folder was back, its scan ran 1 import(s),
    //       notices [Imported Owed X.], record [<key>]"; (b) Importer::identityOf made to answer nothing for
    //       every folder (its whole no-identity condition made `true`) -> the same line, and S15.380 (another
    //       folder) beside it: the identity is really taken on this filesystem, not the fallback. Via
    //       control_run.sh, 2026-10-07.
    // COVERAGE-ASSERT: the other folder's record ("7 8 9", naming no file there) must have been pruned to
    //       nothing, so its listing provably ran to the prune; and the key must have been owed.
    if (foldersToldApart("S15.378", root)) {
        const QString base = root + QStringLiteral("/owed-other-listing");
        const QString drop = base + QStringLiteral("/drop");
        const QString aside = base + QStringLiteral("/aside");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/x.wpx"), *owedX) || !spoilRecord(drop)) {
            fault("S15.378", QStringLiteral("the drop folder could not be built"));
            return;
        }
        const QString key = keyOf(drop + QStringLiteral("/x.wpx"));
        Session s(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
        const bool owed = oweX(s, drop);
        const bool swapped = swapOut(drop, aside) && writeBytes(drop + QStringLiteral("/.auto-imported"), "7 8 9\n");
        s.importer().rescan();
        const bool listed = s.quiesce();
        const QFileInfo otherRecord(drop + QStringLiteral("/.auto-imported"));
        if (!owed || !swapped || !listed || !otherRecord.exists() || otherRecord.size() != 0) {
            harnessFailure(QStringLiteral("S15.378: the import ended owed %1, the other folder was put in place %2, its listing "
                                          "pruned its record to [%3]")
                               .arg(owed)
                               .arg(swapped)
                               .arg(recordOf(drop).join(u" | "))
                               .toStdString());
            return;
        }
        if (!swapBack(drop, aside) || !repairRecord(drop) || !deleteWallpapers(wallpapers)) {
            fault("S15.378", QStringLiteral("the drop folder could not be put back and repaired, or the wallpaper deleted"));
            return;
        }
        s.takeRuns();
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(quiet && runs == 0 && s.importer().notices().isEmpty() && recordOf(drop) == QStringList{key},
               QStringLiteral("S15.378: a debt survived a listing of another folder at the drop path: once its own folder was back, "
                              "its scan ran %1 import(s), notices [%2], record [%3]")
                   .arg(runs)
                   .arg(s.importer().notices().join(u" | "), recordOf(drop).join(u" | ")));
    }

    // GOAL: S15.379 -- a debt owed with no folder to name -- the drop folder could not be opened when the
    //       import ended -- is still pruned by a listing that no longer shows its file: put back with its
    //       wallpaper deleted, the file is imported again.
    // WHY-THIS-SHAPE: oweWithNoFolder: this binary's open() refuses the drop folder with O_PATH during the user's import, the
    //       one way a debt is owed with no identity that this host can stage; the other, a filesystem with
    //       no f_fsid, needs FUSE. x.wpx is then moved out, the folder listed, and x.wpx put back.
    // CONTROL: in Importer::scanDropFolder (app/library/importer.cpp), the debt prune made to spare debts with
    //       no folder, `owed.folder && owedTo(owed, identity) && gone(owed.key)` -> "FAIL: S15.379: a debt owed
    //       with no folder was pruned by a listing without its file: put back, it ran 0 import(s), notices []".
    //       Via control_batch.sh, 2026-10-07. Deleting
    //       Importer::owedTo's `!owed.folder ||` leaves this line green: such a debt is then neither pruned nor
    //       used by any folder, which looks the same here; S15.384 grades that break.
    // COVERAGE-ASSERT: the import must have said the drop folder could not be opened, which is the branch
    //       that owes the key with no folder.
    {
        const QString base = root + QStringLiteral("/owed-no-folder");
        const QString drop = base + QStringLiteral("/drop");
        const QString file = drop + QStringLiteral("/x.wpx");
        const QString away = base + QStringLiteral("/away.wpx");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !writeBytes(file, *owedX)) {
            fault("S15.379", QStringLiteral("the drop folder could not be built"));
            return;
        }
        Session s(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
        const bool owed = oweWithNoFolder(s, drop, file);
        const bool movedOut = ::rename(QFile::encodeName(file).constData(), QFile::encodeName(away).constData()) == 0;
        if (!owed || !movedOut) {
            harnessFailure(QStringLiteral("S15.379: the import ended owed with no folder %1 (problems [%2]), the file moved out %3")
                               .arg(owed)
                               .arg(s.importer().problems().join(u" | "))
                               .arg(movedOut)
                               .toStdString());
            return;
        }
        s.importer().rescan();
        const bool pruned = s.quiesce();
        if (!deleteWallpapers(wallpapers) || ::rename(QFile::encodeName(away).constData(), QFile::encodeName(file).constData()) != 0) {
            fault("S15.379", QStringLiteral("the wallpaper could not be deleted or the file put back"));
            return;
        }
        s.importer().dismissProblems();
        s.takeRuns();
        s.importer().rescan();
        const bool again = s.quiesce();
        const int runs = s.takeRuns();
        expect(pruned && again && runs == 1 && s.importer().notices() == QStringList{QStringLiteral("Imported Owed X.")},
               QStringLiteral("S15.379: a debt owed with no folder was pruned by a listing without its file: put back, it ran %1 "
                              "import(s), notices [%2]")
                   .arg(runs)
                   .arg(s.importer().notices().join(u" | ")));
    }

    // GOAL: S15.384 -- a debt owed with no folder to name is used by any listing that shows its file: with its
    //       wallpaper deleted, the drop folder's next scan imports nothing and writes the key to its record.
    // WHY-THIS-SHAPE: owed through oweWithNoFolder (the drop folder refused during the user's import), then listed with
    //       the file still there. Every folder speaks for such a debt, so the folder's identity, which the
    //       debt lacks, must not stand between them.
    // CONTROL: Importer::owedTo's `!owed.folder ||` deleted (app/library/importer.cpp) -> "FAIL: S15.384: a debt
    //       owed with no folder was not used by the drop folder's listing: its scan ran 1 import(s), notices
    //       [Imported Owed X.], record [<key>]", the S15.379 line staying green. Via control_batch.sh, 2026-10-07.
    // COVERAGE-ASSERT: the import must have said the drop folder could not be opened.
    {
        const QString base = root + QStringLiteral("/owed-no-folder-used");
        const QString drop = base + QStringLiteral("/drop");
        const QString file = drop + QStringLiteral("/x.wpx");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !writeBytes(file, *owedX)) {
            fault("S15.384", QStringLiteral("the drop folder could not be built"));
            return;
        }
        Session s(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
        const bool owed = oweWithNoFolder(s, drop, file);
        if (!owed || !deleteWallpapers(wallpapers)) {
            harnessFailure(QStringLiteral("S15.384: the import ended owed with no folder %1 (problems [%2]), or the wallpaper "
                                          "could not be deleted")
                               .arg(owed)
                               .arg(s.importer().problems().join(u" | "))
                               .toStdString());
            return;
        }
        s.importer().dismissProblems();
        s.takeRuns();
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(quiet && runs == 0 && s.importer().notices().isEmpty() && recordOf(drop) == QStringList{keyOf(file)},
               QStringLiteral("S15.384: a debt owed with no folder was not used by the drop folder's listing: its scan ran %1 "
                              "import(s), notices [%2], record [%3]")
                   .arg(runs)
                   .arg(s.importer().notices().join(u" | "), recordOf(drop).join(u" | ")));
    }

    // GOAL: S15.380 -- the next record an import writes in a debt's own folder holds the debt too: after x.wpx
    //       ends owed and y.wpx is imported into the repaired record, that record holds both keys, and an app
    //       restarted with both wallpapers deleted imports nothing -- with no scan in between to write x's key.
    // WHY-THIS-SHAPE: both imports are the user's and the drop folder is polled every 10 minutes, so no scan
    //       runs before the restart; a scan would write x's key itself (S15.369) and hide the fold. The record
    //       is read straight after y's import, and the importer destroyed, as the app exiting does.
    // CONTROL: in Importer::finish (app/library/importer.cpp), the fold's `if (owedTo(owed, identity))` made
    //       `if (false && owedTo(owed, identity))` -> "FAIL: S15.380 (same folder): the record written for y.wpx is
    //       [<y>] where [<x> | <y>] was due; after a restart with the wallpapers deleted the scan ran 1 import(s),
    //       notices [Imported Owed X.]". Via control_batch.sh, 2026-10-07.
    {
        const QString base = root + QStringLiteral("/owed-folded");
        const QString drop = base + QStringLiteral("/drop");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/x.wpx"), *owedX) || !spoilRecord(drop)) {
            fault("S15.380", QStringLiteral("the drop folder could not be built"));
            return;
        }
        QStringList want{keyOf(drop + QStringLiteral("/x.wpx"))};
        bool owed = false;
        bool wroteY = false;
        QStringList afterY;
        {
            Session s(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
            owed = oweX(s, drop);
            if (owed && repairRecord(drop) && dropIn(drop, QStringLiteral("y.wpx"), *laterY)) {
                want.append(keyOf(drop + QStringLiteral("/y.wpx")));
                s.importer().importPath(drop + QStringLiteral("/y.wpx"));
                wroteY = s.quiesce() && s.takeImported().size() == 1 && s.takeRuns() == 2;
                afterY = recordOf(drop);
            }
        }
        want.sort();
        if (!owed || !wroteY || !deleteWallpapers(wallpapers)) {
            harnessFailure(QStringLiteral("S15.380 (same folder): the import ended owed %1, y.wpx was imported alone after it %2, or "
                                          "the wallpapers could not be deleted")
                               .arg(owed)
                               .arg(wroteY)
                               .toStdString());
            return;
        }
        Session restarted(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
        restarted.importer().rescan();
        const bool quiet = restarted.quiesce();
        const int runs = restarted.takeRuns();
        expect(afterY == want && quiet && runs == 0,
               QStringLiteral("S15.380 (same folder): the record written for y.wpx is [%1] where [%2] was due; after a restart with the "
                              "wallpapers deleted the scan ran %3 import(s), notices [%4]")
                   .arg(afterY.join(u" | "), want.join(u" | "))
                   .arg(runs)
                   .arg(restarted.importer().notices().join(u" | ")));
    }

    // GOAL: S15.380 -- a record written in another folder at the drop path does not take, and so does not
    //       settle, a debt owed to the drop folder: that record holds only its own key, and once the debt's
    //       folder is back (its wallpaper deleted) the file is not imported again.
    // WHY-THIS-SHAPE: x.wpx ends owed to the drop folder; the folder is moved aside, an empty one made at the
    //       path and y.wpx imported there by the user, which writes that folder's record. A write that took
    //       every debt would settle x's there, where x is not, and x would be imported again once its own
    //       folder returned.
    // CONTROL: in Importer::finish (app/library/importer.cpp), the fold made to take every debt, `if (true ||
    //       owedTo(owed, identity))` -> "FAIL: S15.380 (another folder): the record written in another folder at
    //       the path is [<x> | <y>]; once the debt's own folder was back, its scan ran 0 import(s), notices [],
    //       record [<x>]" -- red on that record alone, since writing it no longer settles x's debt (S15.381
    //       grades that). Re-run via control_batch.sh, 2026-10-07.
    if (foldersToldApart("S15.380 (another folder)", root)) {
        const QString base = root + QStringLiteral("/owed-fold-other");
        const QString drop = base + QStringLiteral("/drop");
        const QString aside = base + QStringLiteral("/aside");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/x.wpx"), *owedX) || !spoilRecord(drop)) {
            fault("S15.380", QStringLiteral("the drop folder could not be built"));
            return;
        }
        const QString keyX = keyOf(drop + QStringLiteral("/x.wpx"));
        Session s(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
        const bool owed = oweX(s, drop);
        const bool swapped = swapOut(drop, aside) && dropIn(drop, QStringLiteral("y.wpx"), *laterY);
        const QString keyY = keyOf(drop + QStringLiteral("/y.wpx"));
        s.importer().importPath(drop + QStringLiteral("/y.wpx"));
        const bool wroteY = s.quiesce() && s.takeImported().size() == 1;
        const QStringList otherRecord = recordOf(drop);
        if (!owed || !swapped || !wroteY) {
            harnessFailure(QStringLiteral("S15.380 (another folder): the import ended owed %1, the other folder was put in place %2, "
                                          "y.wpx was imported there %3")
                               .arg(owed)
                               .arg(swapped)
                               .arg(wroteY)
                               .toStdString());
            return;
        }
        if (!swapBack(drop, aside) || !repairRecord(drop) || !deleteWallpapers(wallpapers)) {
            fault("S15.380", QStringLiteral("the drop folder could not be put back and repaired, or the wallpapers deleted"));
            return;
        }
        s.takeRuns();
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(otherRecord == QStringList{keyY} && quiet && runs == 0 && recordOf(drop) == QStringList{keyX},
               QStringLiteral("S15.380 (another folder): the record written in another folder at the path is [%1]; once the debt's "
                              "own folder was back, its scan ran %2 import(s), notices [%3], record [%4]")
                   .arg(otherRecord.join(u" | "))
                   .arg(runs)
                   .arg(s.importer().notices().join(u" | "), recordOf(drop).join(u" | ")));
    }
}

// S15.385, S15.386: a filesystem ID that is only the device number gives a folder no identity.
void deviceFsid(const QString& root) {
    const auto owedX = zipBytes(root + QStringLiteral("/device-fsid.scratch"), planEntries(titled("Owed X")));
    if (!owedX) {
        fault("S15.385", QStringLiteral("the package could not be built"));
        return;
    }
    const auto deviceOf = [](const QString& path) {
        struct stat st {};
        return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? QString::number(quint32(st.st_dev), 16) : QStringLiteral("?");
    };
    // GOAL: S15.385 -- a folder whose filesystem ID is only its device number (as vfat, XFS, exFAT and f2fs
    //       report it) has no identity, so its debts go by key alone: (absorb) another folder at the drop path
    //       holding the owed file runs no import and writes the key; (prune) a listing of another folder
    //       without the file ends the debt, so the file is imported again once its own folder is back.
    //       S15.386 -- an ID that differs from it by one word ({device, 1}, {device + 1, 0}) is an identity:
    //       the same two run folder by folder (one import; no import).
    // WHY-THIS-SHAPE: no XFS or FAT can be mounted here, so this binary's fstatfs reports the ID built from
    //       the folder's own st_dev, under this phase's folders only. The two near misses run the same
    //       scenarios, so the device rule cannot pass by taking every ID as none.
    // CONTROL: in Importer::identityOf (app/library/importer.cpp), via control_batch.sh, 2026-10-07: (1) the
    //       device comparison removed (`if (fsid == 0)`) -> "FAIL: S15.385 (device number), absorb: ... its
    //       scan ran 1 import(s) where 0 was due"; (2) major and minor swapped in the encoding -> the same
    //       line; (3) `quint32(fs.f_fsid.__val[1]) == 0 &&` deleted -> "FAIL: S15.386 (second word 1),
    //       absorb: ... 0 import(s) where 1 was due"; (4) `&& quint32(fs.f_fsid.__val[0]) == device` deleted
    //       -> the same for "S15.386 (device + 1)".
    // COVERAGE-ASSERT: the owing import and each graded scan must have met the fake (g_fsidsFaked rises).
    struct Case {
        FakeFsid fake;
        const char* label;
    };
    for (const Case& c : {Case{FakeFsid::Device, "S15.385 (device number)"}, Case{FakeFsid::DeviceSecondWordSet, "S15.386 (second word 1)"},
                          Case{FakeFsid::NextDevice, "S15.386 (device + 1)"}}) {
        const bool keyOnly = c.fake == FakeFsid::Device;
        const QString label = QString::fromLatin1(c.label);
        const QString base = root + QStringLiteral("/device-fsid-%1").arg(int(c.fake));
        if (!QDir().mkpath(base)) {
            fault(c.label, QStringLiteral("the scratch folder could not be made"));
            return;
        }
        {
            const std::scoped_lock lock(g_fakeFsidLock);
            g_fakeFsidUnder = QFile::encodeName(QFileInfo(base).canonicalFilePath());
        }
        g_fakeFsid = c.fake;
        const auto unfake = qScopeGuard([] { g_fakeFsid = FakeFsid::Off; });
        for (const bool absorb : {true, false}) {
            const QString part = label + (absorb ? QStringLiteral(", absorb") : QStringLiteral(", prune"));
            const QString drop = base + (absorb ? QStringLiteral("/absorb/drop") : QStringLiteral("/prune/drop"));
            const QString aside = base + (absorb ? QStringLiteral("/absorb/aside") : QStringLiteral("/prune/aside"));
            const QString wallpapers = base + (absorb ? QStringLiteral("/absorb/wallpapers") : QStringLiteral("/prune/wallpapers"));
            if (!QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/x.wpx"), *owedX) || !spoilRecord(drop)) {
                fault(c.label, part + QStringLiteral(": the drop folder could not be built"));
                return;
            }
            const QString key = keyOf(drop + QStringLiteral("/x.wpx"));
            const QString device = deviceOf(drop);
            Session s(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
            const int beforeOwe = g_fsidsFaked.load();
            const bool owed = oweX(s, drop) && g_fsidsFaked.load() > beforeOwe;
            // The other folder: with a hard link to the owed file (absorb), or with a record naming no file
            // there, which its listing must prune to nothing (prune).
            bool other = swapOut(drop, aside);
            if (absorb) {
                other = other &&
                        ::link(QFile::encodeName(aside + QStringLiteral("/x.wpx")).constData(), QFile::encodeName(drop + QStringLiteral("/x.wpx")).constData()) == 0;
            } else {
                other = other && writeBytes(drop + QStringLiteral("/.auto-imported"), "7 8 9\n");
                s.importer().rescan();
                const bool listed = s.quiesce();
                const QFileInfo otherRecord(drop + QStringLiteral("/.auto-imported"));
                other = other && listed && otherRecord.exists() && otherRecord.size() == 0 && swapBack(drop, aside) && repairRecord(drop);
            }
            if (!owed || !other || !deleteWallpapers(wallpapers)) {
                harnessFailure((part + QStringLiteral(": the import ended owed through the fake %1, the other folder was set up%2 %3"))
                                   .arg(owed)
                                   .arg(absorb ? QString() : QStringLiteral(", listed and swapped back"))
                                   .arg(other)
                                   .toStdString());
                return;
            }
            s.takeRuns();
            s.importer().dismissNotices();
            const int beforeScan = g_fsidsFaked.load();
            s.importer().rescan();
            const bool quiet = s.quiesce();
            const int runs = s.takeRuns();
            if (g_fsidsFaked.load() == beforeScan) {
                harnessFailure((part + QStringLiteral(": the graded scan never met the faked filesystem ID")).toStdString());
                return;
            }
            const int due = absorb == keyOnly ? 0 : 1;
            expect(quiet && runs == due && recordOf(drop) == QStringList{key},
                   QStringLiteral("%1: %2 (device %3): its scan ran %4 import(s) where %5 was due, notices [%6], record [%7]")
                       .arg(part,
                            absorb ? QStringLiteral("another folder at the drop path holding the owed file")
                                   : QStringLiteral("the debt's own folder back after a listing of another folder without the file"),
                            device)
                       .arg(runs)
                       .arg(due)
                       .arg(s.importer().notices().join(u" | "), recordOf(drop).join(u" | ")));
        }
    }
}

// S15.387: a record read that already holds an owed key settles that debt.
void readRecordSettles(const QString& root) {
    const auto owedX = zipBytes(root + QStringLiteral("/settle-x.scratch"), planEntries(titled("Owed X")));
    const auto laterY = zipBytes(root + QStringLiteral("/settle-y.scratch"), planEntries(titled("Later Y")));
    if (!owedX || !laterY) {
        fault("S15.387", QStringLiteral("the packages could not be built"));
        return;
    }
    const auto inodeOf = [](const QString& path) {
        struct stat st {};
        return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? quint64(st.st_ino) : quint64(0);
    };
    // GOAL: S15.387 -- a debt whose key a record read in its folder already holds is settled by that read:
    //       with the record then deleted and the wallpaper gone, the next scan imports the file again. Read
    //       by a scan, and by the user's import of a file the record already holds.
    // WHY-THIS-SHAPE: x ends owed (the record a folder); another writer then puts x's key (and y's) in the
    //       record. The read must write nothing -- y already recorded, x present -- or the write would settle
    //       the debt and hide the read. Deleting the record leaves only the debt to keep x out.
    // CONTROL: via control_batch.sh, 2026-10-07: Importer::scanDropFolder's `settle(identity, *record);`
    //       deleted (app/library/importer.cpp) -> "FAIL: S15.387 (read by a scan): a debt the record read
    //       already held was not settled: ... notices []", the import line green; Importer::finish's deleted
    //       -> the same for "(read by the user's import of a recorded file)", notices [Imported Later Y.].
    // COVERAGE-ASSERT: the key must have been owed, and the record not rewritten by the read (same inode).
    for (const bool byImport : {false, true}) {
        const QString label = byImport ? QStringLiteral("S15.387 (read by the user's import of a recorded file)")
                                       : QStringLiteral("S15.387 (read by a scan)");
        const QString base = root + (byImport ? QStringLiteral("/settled-by-import") : QStringLiteral("/settled-by-scan"));
        const QString drop = base + QStringLiteral("/drop");
        const QString record = drop + QStringLiteral("/.auto-imported");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/x.wpx"), *owedX) || !spoilRecord(drop) ||
            (byImport && !dropIn(drop, QStringLiteral("y.wpx"), *laterY))) {
            fault("S15.387", label + QStringLiteral(": the drop folder could not be built"));
            return;
        }
        QStringList outside{keyOf(drop + QStringLiteral("/x.wpx"))};
        if (byImport) {
            outside.append(keyOf(drop + QStringLiteral("/y.wpx")));
        }
        outside.sort();
        Session s(drop, wallpapers, 30000, Importer::Watching::Poll, 600000);
        const bool owed = oweX(s, drop);
        const bool written = owed && repairRecord(drop) && writeBytes(record, (outside.join(u'\n') + u'\n').toLatin1());
        const quint64 inode = inodeOf(record);
        bool read = false;
        if (written && byImport) {
            s.importer().importPath(drop + QStringLiteral("/y.wpx"));
            read = s.quiesce() && s.takeImported().size() == 1;
        } else if (written) {
            s.importer().rescan();
            read = s.quiesce();
        }
        if (!owed || !written || !read || inodeOf(record) != inode || recordOf(drop) != outside) {
            harnessFailure((label + QStringLiteral(": the import ended owed %1, the record was written by the other writer %2, read %3; "
                                                   "after the read it is [%4], rewritten %5"))
                               .arg(owed)
                               .arg(written)
                               .arg(read)
                               .arg(recordOf(drop).join(u" | "))
                               .arg(inodeOf(record) != inode)
                               .toStdString());
            return;
        }
        if (!QFile::remove(record) || !deleteWallpapers(wallpapers)) {
            fault("S15.387", label + QStringLiteral(": the record or the wallpapers could not be deleted"));
            return;
        }
        s.importer().dismissNotices();
        s.importer().rescan();
        const bool quiet = s.quiesce();
        expect(quiet && s.importer().notices().contains(QStringLiteral("Imported Owed X.")),
               QStringLiteral("%1: a debt the record read already held was not settled: with the record deleted, the next scan's "
                              "notices are [%2]")
                   .arg(label, s.importer().notices().join(u" | ")));
    }
}

// S15.371: a refusal that never opened a regular file records nothing.
void stamplessRefusal(const QString& root) {
    const auto package = zipBytes(root + QStringLiteral("/stampless.scratch"), planEntries(titled("Listed")));
    const QString base = root + QStringLiteral("/stampless");
    const QString drop = base + QStringLiteral("/drop");
    const QString file = drop + QStringLiteral("/x.wpx");
    const QString away = base + QStringLiteral("/away.wpx");
    const QString fifo = base + QStringLiteral("/fifo");
    if (!package || !QDir().mkpath(drop) || !writeBytes(file, *package) ||
        ::mkfifo(QFile::encodeName(fifo).constData(), 0600) != 0) {
        fault("S15.371", QStringLiteral("the drop folder could not be built"));
        return;
    }
    // GOAL: S15.371 -- a listed file that is no longer a regular file when its import looks it up is
    //       refused, and nothing is recorded for it: put back, the file is imported.
    // WHY-THIS-SHAPE: x.wpx is swapped for a FIFO just before the import's lookup of it, after the scan
    //       listed it as a regular file, so the refusal has no stamp of the file opened. Recording the key
    //       as listed would keep the real file, put back, from ever being imported.
    // CONTROL: in Importer::finish (app/library/importer.cpp), the stampless early return made to record
    //       `job.listedIn->key` instead -> "FAIL: S15.371: the record after the refusal is [<key>]; once the
    //       file was back, notices []".
    // COVERAGE-ASSERT: the swap must have happened exactly once, at the import's lookup.
    static std::atomic<bool> s_swapArmed{false};
    static std::atomic<int> s_swapped{0};
    static QByteArray s_file, s_away, s_fifo;
    s_file = QFile::encodeName(file);
    s_away = QFile::encodeName(away);
    s_fifo = QFile::encodeName(fifo);
    s_swapped = 0;
    s_swapArmed = true;
    g_repointMain = pthread_self();
    g_beforeOpenat = [](int, const char* path, int flags) {
        if (!s_swapArmed.load() || path == nullptr || (flags & O_PATH) == 0 || pthread_equal(pthread_self(), g_repointMain) ||
            std::strcmp(path, "x.wpx") != 0 || !s_swapArmed.exchange(false)) {
            return;
        }
        if (::rename(s_file.constData(), s_away.constData()) == 0 && ::rename(s_fifo.constData(), s_file.constData()) == 0) {
            ++s_swapped;
        }
    };
    const auto unhook = qScopeGuard([] {
        s_swapArmed = false;
        g_beforeOpenat = nullptr;
    });
    Session s(drop, base + QStringLiteral("/wallpapers"), 30000, Importer::Watching::Poll, 600000);
    s.importer().rescan();
    const bool refused = waitUntil([&] { return !s.importer().problems().isEmpty(); }, kWaitMs) && s.quiesce();
    const QStringList afterRefusal = recordOf(drop);
    if (!refused || s_swapped.load() != 1 || ::unlink(s_file.constData()) != 0 ||
        ::rename(s_away.constData(), s_file.constData()) != 0) {
        harnessFailure(QStringLiteral("S15.371: the refusal came %1 after %2 swap(s), or the file could not be put back")
                           .arg(refused)
                           .arg(s_swapped.load())
                           .toStdString());
        return;
    }
    s.importer().rescan();
    const bool imported = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
    expect(afterRefusal.isEmpty() && imported && s.importer().notices() == QStringList{QStringLiteral("Imported Listed.")},
           QStringLiteral("S15.371: the record after the refusal is [%1]; once the file was back, notices [%2]")
               .arg(afterRefusal.join(u" | "), s.importer().notices().join(u" | ")));
}

// S15.372 and S15.373: what is said when the drop folder cannot be listed or opened.
void dropFolderUnreadable(const QString& root) {
    const QString base = root + QStringLiteral("/drop-unreadable");
    const QString plain = base + QStringLiteral("/plain");
    if (!QDir().mkpath(base) || !writeBytes(plain, "not a folder")) {
        fault("S15.372", QStringLiteral("the file standing in for the drop folder could not be written"));
        return;
    }
    // GOAL: S15.372 -- a drop path that is something other than a folder (here a regular file) is said
    //       ("... cannot be read: Not a directory."), while a drop path under a file -- a folder that is not
    //       there -- says nothing, as a missing folder says nothing.
    // WHY-THIS-SHAPE: both are ENOTDIR from opendir; only a stat of the path itself tells them apart. Read
    //       from the importer's warnings, which is where a scan's listing failure goes.
    // CONTROL: in nothingAt (app/library/importer.cpp), (a) ENOTDIR made always "nothing there" -> "FAIL:
    //       S15.372: a drop path that is a file logged []"; (b) ENOTDIR made never "nothing there" -> "FAIL:
    //       S15.372: ... one under a file logged [... cannot be read: Not a directory.]".
    QStringList atFile;
    QStringList underFile;
    for (const bool under : {false, true}) {
        const WarningLog log;
        Session s(under ? plain + QStringLiteral("/sub") : plain, base + QStringLiteral("/wallpapers"), 30000,
                  Importer::Watching::Poll, 600000);
        s.importer().rescan();
        static_cast<void>(s.quiesce());
        (under ? underFile : atFile) = log.containing(u"cannot be read");
    }
    expect(atFile.size() == 1 && atFile.constFirst().endsWith(QStringLiteral("cannot be read: Not a directory.")) &&
               underFile.isEmpty(),
           QStringLiteral("S15.372: a drop path that is a file logged [%1]; one under a file logged [%2]")
               .arg(atFile.join(u" | "), underFile.join(u" | ")));

    // GOAL: S15.373 -- a drop folder that cannot be opened when a hand import of one of its files ends is
    //       said in words that name the drop folder, not its record, which was never reached.
    // WHY-THIS-SHAPE: this binary's open() refuses the drop folder with O_PATH (EACCES) during the import;
    //       the file itself is read by its own path, so the import succeeds and only the record is lost.
    // CONTROL: in Importer::openDropFolder (app/library/importer.cpp), the message made
    //       `recordUnreadable(m_dropDirectory, systemError(error))` -> "FAIL: S15.373: ... [.../.auto-imported
    //       cannot be read (Permission denied) ...]".
    const auto package = zipBytes(root + QStringLiteral("/drop-open.scratch"), planEntries(titled("Asked")));
    const QString drop = base + QStringLiteral("/drop");
    if (!package || !QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/x.wpx"), *package)) {
        fault("S15.373", QStringLiteral("the drop folder could not be built"));
        return;
    }
    Session s(drop, base + QStringLiteral("/wallpapers-open"), 30000, Importer::Watching::Poll, 600000);
    {
        const std::scoped_lock lock(g_refusedOpenLock);
        g_refusedOpen = QFile::encodeName(QDir::cleanPath(QDir(drop).absolutePath()));
    }
    g_refusingOpen = true;
    s.importer().importPath(drop + QStringLiteral("/x.wpx"));
    const bool done = s.quiesce();
    g_refusingOpen = false;
    const QString want = QStringLiteral("The drop folder %1 cannot be opened (Permission denied), so the files imported from it "
                                        "are recorded there once it can.")
                             .arg(QDir::cleanPath(QDir(drop).absolutePath()));
    expect(done && s.importer().problems() == QStringList{want} && s.takeImported().size() == 1,
           QStringLiteral("S15.373: a drop folder that could not be opened was reported as [%1]").arg(s.importer().problems().join(u" | ")));
}

// S15.353: an imported file's key is kept until a record holding it is written.
void recordOwedKept(const QString& root) {
    const auto package = zipBytes(root + QStringLiteral("/owed.scratch"), planEntries(titled("Owed")));
    if (!package) {
        fault("S15.353", QStringLiteral("the package could not be built"));
        return;
    }
    // GOAL: S15.353 -- a drop-folder import whose record could not be written is not imported again:
    //       after the record's write is refused and the user deletes the wallpaper, the next scan
    //       imports nothing and writes the record holding the file's key.
    // WHY-THIS-SHAPE: this binary's renameat() refuses the first rename onto the record, the last step
    //       of its write; deleting the wallpaper means an import would make it again, so "nothing
    //       imported" is about the key, not about the library already holding the bytes.
    // CONTROL: `m_unrecorded.append(owed);` deleted from the `owe` lambda in Importer::finish
    //       (app/library/importer.cpp) -> "FAIL: S15.353 (write refused): after the record's write failed
    //       and the wallpaper was deleted, the next scan ran 1 import(s) and wrote the record [<key>]", the
    //       read-failed line beside it. Re-run via control_batch.sh, 2026-10-07.
    // COVERAGE-ASSERT: after the import the record must lack the file's key, its write refused at most
    //       once -- refused once on this code, and not attempted at all when nothing is owed, which is
    //       the defect this grades -- or the staging failed.
    {
        const QString drop = root + QStringLiteral("/owed-write");
        const QString wallpapers = drop + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/x.wpx"), *package)) {
            fault("S15.353", QStringLiteral("the drop folder could not be built"));
            return;
        }
        Session s(drop, wallpapers);
        bool imported = false;
        int refused = 0;
        {
            const WarningLog log;
            const RefuseRenameTo refuse(drop + QStringLiteral("/.auto-imported"), 1);
            s.importer().rescan();
            imported = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
            refused = refuse.refused();
        }
        const QStringList made = entriesOf(wallpapers);
        const bool unrecorded = !recordOf(drop).contains(keyOf(drop + QStringLiteral("/x.wpx")));
        if (!imported || refused > 1 || !unrecorded || made.size() != 1 ||
            !QDir(wallpapers + QLatin1Char('/') + made.constFirst()).removeRecursively()) {
            harnessFailure(QStringLiteral("S15.353 (write refused): the import ran %1, %2 record write(s) were refused, the "
                                          "record %3 the key, and its wallpaper could not be deleted")
                               .arg(imported)
                               .arg(refused)
                               .arg(unrecorded ? QStringLiteral("lacks") : QStringLiteral("holds"))
                               .toStdString());
            return;
        }
        s.takeRuns();
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(quiet && runs == 0 && recordOf(drop) == QStringList{keyOf(drop + QStringLiteral("/x.wpx"))} && entriesOf(wallpapers).isEmpty(),
               QStringLiteral("S15.353 (write refused): after the record's write failed and the wallpaper was deleted, the next scan ran %1 "
                              "import(s) and wrote the record [%2]")
                   .arg(runs)
                   .arg(recordOf(drop).join(u" | ")));
    }

    // GOAL: S15.353 -- a record that cannot be read when an import finishes is said, as at a scan, and
    //       the key is still kept: once the record reads again, the next scan imports nothing and
    //       writes it.
    // WHY-THIS-SHAPE: the record is turned into a folder just as the import's worker looks its file
    //       up, after the scan's own read of it; only the import's read meets the folder.
    // CONTROL: the `addProblem(*record.error());` after loadRecordIn in Importer::finish
    //       (app/library/importer.cpp) deleted -> "FAIL: S15.353 (read failed): when the import finished
    //       the importer said []; ...". Re-run via control_batch.sh, 2026-10-07.
    // COVERAGE-ASSERT: the record must have been turned into a folder exactly once, by the import.
    {
        const QString drop = root + QStringLiteral("/owed-read");
        const QString wallpapers = drop + QStringLiteral("/wallpapers");
        const QString record = drop + QStringLiteral("/.auto-imported");
        if (!QDir().mkpath(drop) || !writeBytes(drop + QStringLiteral("/x.wpx"), *package)) {
            fault("S15.353", QStringLiteral("the drop folder could not be built"));
            return;
        }
        static std::atomic<bool> s_spoilArmed{false};
        static std::atomic<int> s_spoiled{0};
        static QByteArray s_record;
        s_record = QFile::encodeName(record);
        s_spoiled = 0;
        s_spoilArmed = true;
        g_repointMain = pthread_self();
        g_beforeOpenat = [](int, const char* path, int flags) {
            if (!s_spoilArmed.load() || path == nullptr || (flags & O_PATH) == 0 || pthread_equal(pthread_self(), g_repointMain) ||
                std::strcmp(path, "x.wpx") != 0 || !s_spoilArmed.exchange(false)) {
                return;
            }
            ::unlink(s_record.constData());
            if (::mkdir(s_record.constData(), 0700) == 0) {
                ++s_spoiled;
            }
        };
        const auto unhook = qScopeGuard([] {
            s_spoilArmed = false;
            g_beforeOpenat = nullptr;
        });
        Session s(drop, wallpapers);
        s.importer().rescan();
        const bool imported = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        if (!imported || s_spoiled.load() != 1) {
            harnessFailure(QStringLiteral("S15.353 (read failed): the import ran %1 and the record was spoiled %2 time(s)")
                               .arg(imported)
                               .arg(s_spoiled.load())
                               .toStdString());
            return;
        }
        const QString want = QStringLiteral("%1 cannot be read (not a regular file), so files in the drop folder are not imported "
                                            "until it can.")
                                 .arg(QDir::cleanPath(record));
        const QStringList said = s.importer().problems();
        const QStringList made = entriesOf(wallpapers);
        const bool repaired = QDir().rmdir(record) && made.size() == 1 && QDir(wallpapers + QLatin1Char('/') + made.constFirst()).removeRecursively();
        s.importer().dismissProblems();
        s.takeRuns();
        s.importer().rescan();
        const bool quiet = s.quiesce();
        const int runs = s.takeRuns();
        expect(said == QStringList{want} && repaired && quiet && runs == 0 &&
                   recordOf(drop) == QStringList{keyOf(drop + QStringLiteral("/x.wpx"))},
               QStringLiteral("S15.353 (read failed): when the import finished the importer said [%1]; once the record read again the "
                              "next scan ran %2 import(s) and wrote [%3]")
                   .arg(said.join(u" | "))
                   .arg(runs)
                   .arg(recordOf(drop).join(u" | ")));
    }
}

// S15.355: a dropped file's bytes are read through the folder that was listed.
void bytesThroughListedFolder(const QString& root) {
    const auto listed = zipBytes(root + QStringLiteral("/bytes-a.scratch"), planEntries(titled("Listed")));
    const auto swapped = zipBytes(root + QStringLiteral("/bytes-b.scratch"), planEntries(titled("Swapped")));
    const auto linked = zipBytes(root + QStringLiteral("/bytes-c.scratch"), planEntries(titled("Linked In")));
    const auto elsewhere = zipBytes(root + QStringLiteral("/bytes-d.scratch"), planEntries(titled("By Hand")));
    if (!listed || !swapped || !linked || !elsewhere) {
        fault("S15.355", QStringLiteral("the packages could not be built"));
        return;
    }
    // GOAL: S15.355 -- the bytes a scan's import reads are those of the file the scan listed: when the
    //       drop folder's path is re-pointed at another folder holding a file of the same name between
    //       the listing and the import's open, the listed file is the one imported.
    // WHY-THIS-SHAPE: the drop folder is a link from A to B; this binary's openat() re-points it at B
    //       just before the import's worker first looks x.wpx up (by name or by a path ending in it),
    //       which is after the scan's listing, so the watcher sees no change during the scan. The two
    //       files carry different titles, which the notice names.
    // CONTROL: in Importer::runNext (app/library/importer.cpp), the job always imported by path
    //       (`unpackPackage(path, directory, *cancelled)` whatever `listed` holds) -> "FAIL: S15.355
    //       (swapped): ... the notices are [Imported Swapped.], ...". Via control_batch.sh, 2026-10-06.
    //       Reverted.
    // COVERAGE-ASSERT: the link must have been re-pointed exactly once, by the import's lookup.
    {
        const QString base = root + QStringLiteral("/bytes-swap");
        const QString a = base + QStringLiteral("/A");
        const QString b = base + QStringLiteral("/B");
        const QString link = base + QStringLiteral("/drop");
        if (!QDir().mkpath(a) || !QDir().mkpath(b) || !writeBytes(a + QStringLiteral("/x.wpx"), *listed) ||
            !writeBytes(b + QStringLiteral("/x.wpx"), *swapped) || !pointLink(QFile::encodeName(link), QFile::encodeName(a))) {
            fault("S15.355", QStringLiteral("the two folders and the link could not be built"));
            return;
        }
        Session s(link, base + QStringLiteral("/wallpapers"), 30000, Importer::Watching::Poll, 600000);
        armRepoint(link, b, "x.wpx");
        g_beforeOpenat = &repointBeforeLookup;
        const auto unhook = qScopeGuard([] {
            g_repointArmed = false;
            g_beforeOpenat = nullptr;
        });
        s.importer().rescan();
        const bool imported = waitUntil([&] { return !s.importer().notices().isEmpty() || !s.importer().problems().isEmpty(); }, kWaitMs) &&
                              s.quiesce();
        if (g_repointsDone.load() != 1) {
            harnessFailure("S15.355 (swapped): the import never looked x.wpx up, so the folder was never re-pointed");
            return;
        }
        expect(imported && s.importer().notices() == QStringList{QStringLiteral("Imported Listed.")},
               QStringLiteral("S15.355 (swapped): with the drop folder re-pointed before the import opened x.wpx, the notices are [%1], "
                              "the problems [%2]")
                   .arg(s.importer().notices().join(u" | "), s.importer().problems().join(u" | ")));
    }

    // GOAL: S15.355 -- a .wpx in the drop folder that is a link, relative to the folder holding it, is
    //       still imported through the listed folder.
    // CONTROL: in unpackPackage's folder form (app/library/importer.cpp), the open made
    //       `openRegularFile(folder, name.constData(), O_RDONLY | O_NOFOLLOW)` -> "FAIL: S15.355 (a link in
    //       the drop folder): the notices are [], the problems [y.wpx could not be imported. It is not a
    //       regular file.]". Via control_batch.sh, 2026-10-06. Reverted.
    {
        const QString base = root + QStringLiteral("/bytes-link");
        const QString drop = base + QStringLiteral("/drop");
        if (!QDir().mkpath(drop) || !QDir().mkpath(base + QStringLiteral("/store")) ||
            !writeBytes(base + QStringLiteral("/store/y.wpx"), *linked) ||
            ::symlink("../store/y.wpx", QFile::encodeName(drop + QStringLiteral("/y.wpx")).constData()) != 0) {
            fault("S15.355", QStringLiteral("the drop folder and its link could not be built"));
            return;
        }
        Session s(drop, base + QStringLiteral("/wallpapers"));
        s.importer().rescan();
        const bool imported = waitUntil([&] { return !s.importer().notices().isEmpty() || !s.importer().problems().isEmpty(); }, kWaitMs) &&
                              s.quiesce();
        expect(imported && s.importer().notices() == QStringList{QStringLiteral("Imported Linked In.")} && s.importer().problems().isEmpty(),
               QStringLiteral("S15.355 (a link in the drop folder): the notices are [%1], the problems [%2]")
                   .arg(s.importer().notices().join(u" | "), s.importer().problems().join(u" | ")));
    }

    // GOAL: S15.355 -- a hand import of a file outside the drop folder is read by its path as before.
    // CONTROL: in unpackPackage's path form (app/library/importer.cpp), the name passed made the file's
    //       name alone (`encodeFileName(QFileInfo(archivePath).fileName())`), so it is not found by its
    //       path -> "FAIL: S15.355 (by hand, outside the drop folder): the import gave [], the problems [z.wpx
    //       could not be imported. It cannot be opened: No such file or directory.]". Via control_batch.sh,
    //       2026-10-06. Reverted.
    {
        const QString base = root + QStringLiteral("/bytes-hand");
        const QString file = base + QStringLiteral("/elsewhere/z.wpx");
        const QString wallpapers = base + QStringLiteral("/wallpapers");
        if (!QDir().mkpath(base + QStringLiteral("/elsewhere")) || !writeBytes(file, *elsewhere)) {
            fault("S15.355", QStringLiteral("the file outside the drop folder could not be written"));
            return;
        }
        Session s(base + QStringLiteral("/drop"), wallpapers);
        s.importer().importPath(file);
        const bool quiet = s.quiesce();
        const QStringList folders = s.takeImported();
        expect(quiet && folders.size() == 1 && readWallpaperFolder(folders.value(0)).title == u"By Hand" && s.importer().problems().isEmpty(),
               QStringLiteral("S15.355 (by hand, outside the drop folder): the import gave [%1], the problems [%2]")
                   .arg(folders.join(u" | "), s.importer().problems().join(u" | ")));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1 && std::strcmp(argv[1], "--small-disk") == 0) {
        QCoreApplication app(argc, argv);
        return smallDiskChild(QCoreApplication::arguments().mid(2));
    }
    wallpaperx::testing::g_testName = "importer";
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified("S15.58, S15.75-S15.81", "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    QCoreApplication app(argc, argv);

    const QString root = scratch.path();
    runPhase("imports", [&] { imports(root); });
    runPhase("refusals", [&] { refusals(root); });
    runPhase("staleImports", [&] { staleImports(root); });
    runPhase("cancelledImport", [&] { cancelledImport(root); });
    runPhase("freeSpace", [&] { freeSpace(root); });
    runPhase("autoImport", [&] { autoImport(root); });
    runPhase("failuresAndMarking", [&] { failuresAndMarking(root); });
    runPhase("arrivals", [&] { arrivals(root); });
    runPhase("queuedListings", [&] { queuedListings(root); });
    runPhase("nesting", [&] { nesting(root); });
    runPhase("stuckImport", [&] { stuckImport(root); });
    runPhase("unreadableSources", [&] { unreadableSources(root); });
    runPhase("renameFallback", [&] { renameFallback(root); });
    runPhase("askedWhileDropped", [&] { askedWhileDropped(root); });
    runPhase("unusableDrops", [&] { unusableDrops(root); });
    runPhase("recordUnwritable", [&] { recordUnwritable(root); });
    runPhase("unreadableRecord", [&] { unreadableRecord(root); });
    runPhase("linkToFifo", [&] { linkToFifo(root); });
    runPhase("recordReadFresh", [&] { recordReadFresh(root); });
    runPhase("folderBackMidScan", [&] { folderBackMidScan(root); });
    runPhase("swapToFifo", [&] { swapToFifo(root); });
    runPhase("linkedWhileWritten", [&] { linkedWhileWritten(root); });
    runPhase("linkedRewritten", [&] { linkedRewritten(root); });
    runPhase("linkWatchRefused", [&] { linkWatchRefused(root); });
    runPhase("linkWatchDropped", [&] { linkWatchDropped(root); });
    runPhase("nulInFileUrl", [&] { nulInFileUrl(root); });
    runPhase("recordThroughListedFolder", [&] { recordThroughListedFolder(root); });
    runPhase("owedKeys", [&] { owedKeys(root); });
    runPhase("owedByFolder", [&] { owedByFolder(root); });
    runPhase("deviceFsid", [&] { deviceFsid(root); });
    runPhase("readRecordSettles", [&] { readRecordSettles(root); });
    runPhase("stamplessRefusal", [&] { stamplessRefusal(root); });
    runPhase("dropFolderUnreadable", [&] { dropFolderUnreadable(root); });
    runPhase("recordOwedKept", [&] { recordOwedKept(root); });
    runPhase("bytesThroughListedFolder", [&] { bytesThroughListedFolder(root); });
    // No import may still be writing into the scratch folder when it is removed.
    static_cast<void>(QThreadPool::globalInstance()->waitForDone(30000));
    return verdict();
}
