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

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/stat.h>
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
// Opens of the one path armed here, by any thread: how many times an import read that package.
std::mutex g_countedLock;
QByteArray g_countedPath;
std::atomic<bool> g_counting{false};
std::atomic<int> g_countedOpens{0};

void noteOpen(const char* path) {
    if (path != nullptr && g_counting.load()) {
        const std::scoped_lock lock(g_countedLock);
        if (g_countedPath == path) {
            ++g_countedOpens;
        }
    }
}

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

extern "C" char* mkdtemp(char* pattern) noexcept {
    static const auto real = reinterpret_cast<char* (*)(char*)>(::dlsym(RTLD_NEXT, "mkdtemp"));
    if (std::strstr(pattern, "/.import-") != nullptr) {
        ++g_unfinishedMade;
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
    return real(path, flags, mode);
}

extern "C" int open64(const char* path, int flags, ...) {
    static const auto real = reinterpret_cast<int (*)(const char*, int, ...)>(::dlsym(RTLD_NEXT, "open64"));
    va_list args;
    va_start(args, flags);
    const int mode = modeOf(flags, args);
    va_end(args);
    noteOpen(path);
    return real(path, flags, mode);
}

namespace {

// Counts the opens of `path` while alive. One at a time.
class OpensOf {
public:
    explicit OpensOf(const QString& path) {
        const std::scoped_lock lock(g_countedLock);
        g_countedPath = QFile::encodeName(path);
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
    static_assert(std::size(cases) == 11, "a refusal row was added or dropped");
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
    // CONTROL: app/library/importer.cpp, finish()'s Imported case `if (job.dropped) {` made
    //       `if (!job.dropped) {` -> red, "S15.78: the first session ran 1 import(s), noticed [] ..."
    //       and "a .wpx dropped in while the importer ran gave the notices []"; dismissNotices()'s
    //       `m_notices.clear();` deleted -> red, "S15.78: after Dismiss the notices are [Imported
    //       Present. | Imported Second.]"; restored.
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
    // CONTROL: app/library/importer.cpp, scanDropFolder()'s `&& !m_record.contains(key)` deleted ->
    //       red, "S15.77: after a restart 1 import(s) ran ..." (both files in one busy stretch);
    //       loadRecord()'s first line made `return;` -> red, the same line; restored.
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
    //       restart: 1"; restored.
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
        // CONTROL: app/library/importer.cpp, scanDropFolder()'s `m_record.removeIf(...)` prune made
        //       `if (false)` -> red, "S15.77: with present.wpx removed the record still holds its line";
        //       restored.
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
    //       and Rescan pressed, [] was noticed"; restored. (Importer::rescan()'s `scanDropFolder();`
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
    // CONTROL: app/library/importer.cpp, finish()'s `|| isInFolder(job.path, m_dropDirectory)`
    //       deleted -> red, "S15.79: after the explicit import the record holds []" and "after the
    //       user deleted it and restarted, 1 import(s) ran and the folder is back"; isInFolder()'s body made `return true;`
    //       -> red, "S15.79: an explicit import from outside the drop folder recorded [...]"; restored.
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
    // CONTROL: app/library/importer.cpp, unpackPackage's source open `O_RDONLY | O_CLOEXEC | O_NONBLOCK`
    //       made `O_RDONLY | O_CLOEXEC` -> red, "S15.76: unpackPackage on a FIFO blocked for 3000 ms"; restored.
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
    //       second hold when the hand import is asked for. Imports are counted as opens of the
    //       .wpx, since both would end in the same folder.
    // CONTROL: app/library/importer.cpp, jobFor()'s hand-queued branch
    //       `return isInFolder(job.path, m_dropDirectory) && ::stat(...) == 0 && keyOf(stampFrom(st)) == key;`
    //       made `return false;` -> red, "S15.77: asked by hand, then found by the scan: 2 import(s) ..."
    //       only; importPath()'s take-over block deleted -> red, "S15.77: found by the scan, then asked
    //       by hand: 2 import(s) ..., notices [Imported Asked.]" only; restored.
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
    if (!package || !QDir().mkpath(drop + QStringLiteral("/.auto-imported")) || !writeBytes(drop + QStringLiteral("/x.wpx"), *package)) {
        fault("S15.77", QStringLiteral("the drop folder with a folder for a record could not be built"));
        return;
    }
    // GOAL: S15.77 -- a record that cannot be written is reported once, with the reason the write
    //       failed, not one a later call left in errno.
    // WHY-THIS-SHAPE: a folder where the record goes makes the final rename fail (EISDIR) after the
    //       temporary file was written, so the failure is followed by that file's removal, which is
    //       where a late read of errno would pick up another value.
    // CONTROL: app/library/importer.cpp, writeFileAtomically's `const int error = errno;
    //       ::unlink(name.constData()); return error;` made `::unlink(name.constData());
    //       ::unlink(name.constData()); return errno;` -> red, "S15.77: the record that cannot be
    //       written gave the warnings [... (No such file or directory) ...]"; restored.
    QStringList said;
    bool quiet = false;
    {
        const WarningLog log;
        Session s(drop, wallpapers);
        s.importer().rescan();
        quiet = waitUntil([&] { return !s.importer().notices().isEmpty(); }, kWaitMs) && s.quiesce();
        said = log.containing(u"cannot be written");
    }
    const QString want = QStringLiteral("wallpaperx: %1/.auto-imported cannot be written (Is a directory), so files in %1 may be imported again")
                             .arg(QString::fromLocal8Bit(QFile::encodeName(QDir::cleanPath(drop))));
    expect(quiet && said == QStringList{want}, QStringLiteral("S15.77: the record that cannot be written gave the warnings [%1]").arg(said.join(u" | ")));
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
    // CONTROL: app/library/folder_watcher.cpp, openForWriting()'s `const int follow = m_items == Items::Files ? 0 : O_NOFOLLOW;`
    //       made `const int follow = O_NOFOLLOW;` -> red, all four cases, e.g. "S15.89 (inotify,
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
    runPhase("linkedWhileWritten", [&] { linkedWhileWritten(root); });
    runPhase("linkedRewritten", [&] { linkedRewritten(root); });
    runPhase("linkWatchRefused", [&] { linkWatchRefused(root); });
    runPhase("linkWatchDropped", [&] { linkWatchDropped(root); });
    runPhase("nulInFileUrl", [&] { nulInFileUrl(root); });
    // No import may still be writing into the scratch folder when it is removed.
    static_cast<void>(QThreadPool::globalInstance()->waitForDone(30000));
    return verdict();
}
