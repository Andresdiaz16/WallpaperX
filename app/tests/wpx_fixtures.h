// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Builds wallpaper folders, and .wpx archives with libzip, for the library tests, and the
// event-loop helpers they share.
// Manifests are raw JSON text or nlohmann::ordered_json, never QJsonObject: QJsonObject sorts keys,
// which would erase the property order the tests exist to check.

#include "tests/test_phase.h"
#include "tests/test_report.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QPair>
#include <QProcess>
#include <QSemaphore>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <nlohmann/json.hpp>
#include <zip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wallpaperx::apptest {

using OrderedJson = nlohmann::ordered_json;

// PLAN.md's wallpaper.json example with its colour in upper case. The property keys are listed
// speed, mute, tint -- not alphabetical -- so an implementation that sorts them is caught.
inline constexpr const char* kPlanManifest = R"({
  "schema_version": "1",
  "type": "video",
  "title": "Ocean Waves",
  "author": "someone",
  "preview": "preview.png",
  "file": "assets/video.mp4",
  "tags": ["nature", "relaxing"],
  "properties": {
    "speed": { "type": "float", "default": 1.0, "min": 0.1, "max": 3.0, "label": "Speed" },
    "mute":  { "type": "bool",  "default": true,              "label": "Mute"  },
    "tint":  { "type": "color", "default": "#FFFFFF",         "label": "Tint"  }
  }
})";

inline OrderedJson planManifest() {
    return OrderedJson::parse(kPlanManifest);
}

inline QByteArray jsonBytes(const OrderedJson& json) {
    return QByteArray::fromStdString(json.dump());
}

// PLAN's manifest with another title.
inline QByteArray titled(const char* title) {
    OrderedJson json = planManifest();
    json["title"] = title;
    return jsonBytes(json);
}

inline void expect(bool cond, const QString& msg) {
    testing::check(cond, msg.toStdString().c_str());
}

// A fault in the test's own fixtures, never in the code under test.
inline void fault(const char* goal, const QString& what) {
    testing::harnessFailure(std::string(goal) + ": " + what.toStdString());
}

inline QByteArray pngBytes(int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(qRgb(30, 90, 160));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

using Entries = QList<QPair<QString, QByteArray>>;

// The entries PLAN's manifest names, with a 1280x720 preview.
inline Entries planEntries(const QByteArray& manifest) {
    return {{QStringLiteral("wallpaper.json"), manifest},
            {QStringLiteral("preview.png"), pngBytes(1280, 720)},
            {QStringLiteral("assets/video.mp4"), QByteArrayLiteral("not really a video")}};
}

// Writes a ZIP archive holding `entries` at `path`, letting `each` adjust every entry once added
// (false: give up). Empty string on success, else why not.
// libzip deletes an archive with no entries on close, so every caller passes at least one.
// libzip writes a temporary file beside `path` and renames it into place, so in a watched folder
// the archive arrives whole, as a move.
[[nodiscard]] inline QString writeZipWith(const QString& path, const Entries& entries,
                                          const std::function<bool(zip_t*, zip_uint64_t, const QString&)>& each) {
    int error = 0;
    zip_t* zip = zip_open(QFile::encodeName(path).constData(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (zip == nullptr) {
        return QStringLiteral("zip_open(%1) failed with libzip error %2").arg(path).arg(error);
    }
    for (const auto& [name, bytes] : entries) {
        zip_source_t* source = zip_source_buffer(zip, bytes.constData(), static_cast<zip_uint64_t>(bytes.size()), 0);
        const zip_int64_t index = source == nullptr ? -1 : zip_file_add(zip, name.toUtf8().constData(), source, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
        if (index < 0 || !each(zip, zip_uint64_t(index), name)) {
            const QString why = QString::fromUtf8(zip_strerror(zip));
            // Once added, the source belongs to the archive, and zip_discard frees it.
            if (source != nullptr && index < 0) {
                zip_source_free(source);
            }
            zip_discard(zip);
            return QStringLiteral("adding %1 to %2 failed: %3").arg(name, path, why);
        }
    }
    if (zip_close(zip) != 0) {
        const QString why = QString::fromUtf8(zip_strerror(zip));
        zip_discard(zip);
        return QStringLiteral("closing %1 failed: %2").arg(path, why);
    }
    return {};
}

[[nodiscard]] inline QString writeZip(const QString& path, const Entries& entries) {
    return writeZipWith(path, entries, [](zip_t*, zip_uint64_t, const QString&) { return true; });
}

// The bytes of a .wpx holding `entries`. Built by writing `scratchPath` itself, which is
// overwritten and then removed, so it must name a file nothing else uses or watches.
[[nodiscard]] inline std::optional<QByteArray> zipBytes(const QString& scratchPath, const Entries& entries) {
    const bool written = writeZip(scratchPath, entries).isEmpty();
    QFile file(scratchPath);
    std::optional<QByteArray> bytes;
    if (written && file.open(QIODevice::ReadOnly)) {
        bytes = file.readAll();
        file.close();
    }
    QFile::remove(scratchPath);
    return bytes && !bytes->isEmpty() ? bytes : std::nullopt;
}

[[nodiscard]] inline bool writeBytes(const QString& path, const QByteArray& bytes,
                                     QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Truncate) {
    QFile file(path);
    return file.open(mode) && file.write(bytes) == bytes.size() && file.flush();
}

// Writes each entry as a file below `dir`, making the folders on the way. Empty string on success.
// Every file is closed before this returns, so a watch placed later finds nothing open.
[[nodiscard]] inline QString writeFolder(const QString& dir, const Entries& entries) {
    for (const auto& [name, bytes] : entries) {
        const QString path = dir + QLatin1Char('/') + name;
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !writeBytes(path, bytes)) {
            return QStringLiteral("%1 could not be written").arg(path);
        }
    }
    if (entries.isEmpty() && !QDir().mkpath(dir)) {
        return QStringLiteral("%1 could not be made").arg(dir);
    }
    return {};
}

// A wallpaper folder `name` holding `entries`, built in `staging` and renamed into `library`, so
// a watched library sees it arrive whole, as an import does. Empty string on success.
[[nodiscard]] inline QString moveInFolder(const QString& staging, const QString& library, const QString& name,
                                          const Entries& entries) {
    const QString built = staging + QLatin1Char('/') + name;
    if (const QString why = writeFolder(built, entries); !why.isEmpty()) {
        return why;
    }
    if (!QDir().mkpath(library) || ::rename(QFile::encodeName(built).constData(), QFile::encodeName(library + QLatin1Char('/') + name).constData()) != 0) {
        return QStringLiteral("%1 could not be moved into %2").arg(built, library);
    }
    return {};
}

// Removes `name` in the folder `parentFd` and everything below it by raw bytes. QTemporaryDir
// removes its tree by path text, which cannot name an entry that is not UTF-8.
inline void removeRawTree(int parentFd, const QByteArray& name) {
    const int fd = ::openat(parentFd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        if (DIR* listing = ::fdopendir(fd)) {
            QList<QByteArray> inside;
            while (const dirent* entry = ::readdir(listing)) {
                if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
                    inside.append(QByteArray(entry->d_name));
                }
            }
            for (const QByteArray& child : std::as_const(inside)) {
                if (::unlinkat(::dirfd(listing), child.constData(), 0) != 0) {
                    removeRawTree(::dirfd(listing), child);
                }
            }
            ::closedir(listing);
        } else {
            ::close(fd);
        }
    }
    ::unlinkat(parentFd, name.constData(), AT_REMOVEDIR);
}

// The whole file, or nothing when it cannot be read or is empty.
[[nodiscard]] inline std::optional<QByteArray> readBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QByteArray bytes = file.readAll();
    return bytes.isEmpty() ? std::nullopt : std::optional<QByteArray>(std::move(bytes));
}

// chmod that says whether it worked; a fixture whose mode was not set must not be graded.
[[nodiscard]] inline bool setMode(const QString& path, unsigned mode) {
    return ::chmod(QFile::encodeName(path).constData(), mode) == 0;
}

// Writes `bytes` to the file named by the raw bytes `name` inside `dir`: a name that is not UTF-8
// has no QString QFile could open it by.
[[nodiscard]] inline bool writeRawName(const QString& dir, const QByteArray& name, const QByteArray& bytes) {
    const QByteArray path = QFile::encodeName(dir) + '/' + name;
    const int fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    const bool whole = ::write(fd, bytes.constData(), size_t(bytes.size())) == bytes.size();
    return ::close(fd) == 0 && whole;
}

// Removes entries named by raw bytes, files or folders, when it goes out of scope. QTemporaryDir
// removes its tree by path text, which cannot name one that is not UTF-8, so without this the
// folder is left behind.
class RawNamesRemover {
public:
    RawNamesRemover(const QString& dir, QList<QByteArray> names) : m_dir(QFile::encodeName(dir)), m_names(std::move(names)) {}
    ~RawNamesRemover() {
        const int dirFd = ::open(m_dir.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        for (const QByteArray& name : std::as_const(m_names)) {
            if (::unlink((m_dir + '/' + name).constData()) != 0 && dirFd >= 0) {
                removeRawTree(dirFd, name);
            }
        }
        if (dirFd >= 0) {
            ::close(dirFd);
        }
    }
    RawNamesRemover(const RawNamesRemover&) = delete;
    RawNamesRemover& operator=(const RawNamesRemover&) = delete;

private:
    QByteArray m_dir;
    QList<QByteArray> m_names;
};

// Runs the event loop until `pred` holds. False at the deadline: every caller treats that as the
// check failing or the phase being abandoned, never as a result.
template <typename Pred>
bool waitUntil(Pred pred, int timeoutMs) {
    const QDeadlineTimer deadline(timeoutMs);
    QTimer tick; // guarantees a wake-up, so WaitForMoreEvents cannot sleep past the deadline
    tick.start(5);
    while (!pred()) {
        if (deadline.hasExpired()) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents);
    }
    return true;
}

// Runs the event loop for `ms`, so timers and file-watch events are serviced meanwhile.
inline void pump(int ms) {
    static_cast<void>(waitUntil([] { return false; }, ms));
}

// Runs one phase of a test's main(), so an exception out of it fails the run with its text instead
// of ending it in std::terminate, and the later phases still run. It may come from the code under
// test or from the file's own fixtures, and the line says so rather than guessing.
template <typename Phase>
void runPhase(const char* name, Phase phase) {
    if (!testing::phaseSelected(name)) return;
    std::string escaped;
    try {
        phase();
        return;
    } catch (const std::exception& e) {
        escaped = e.what();
    } catch (...) {
        escaped = "an exception that is not a std::exception";
    }
    testing::check(false, (std::string("an exception escaped ") + name + ", from the code under test or this file's fixtures: " + escaped).c_str());
}

// Keeps every warning logged, from any thread, while alive, passing each on to the handler that
// was installed before. One at a time.
class WarningLog {
public:
    WarningLog() {
        s_current.store(this);
        m_previous = qInstallMessageHandler(&WarningLog::handle);
    }
    ~WarningLog() {
        qInstallMessageHandler(m_previous);
        s_current.store(nullptr);
    }
    WarningLog(const WarningLog&) = delete;
    WarningLog& operator=(const WarningLog&) = delete;

    QStringList lines() const {
        const QMutexLocker lock(&m_lock);
        return m_lines;
    }
    // The lines holding `text`.
    QStringList containing(QStringView text) const { return lines().filter(text); }

private:
    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message) {
        if (WarningLog* log = s_current.load()) {
            if (type == QtWarningMsg) {
                const QMutexLocker lock(&log->m_lock);
                log->m_lines.append(message);
            }
            if (log->m_previous != nullptr) {
                log->m_previous(type, context, message);
            }
        }
    }

    inline static std::atomic<WarningLog*> s_current{nullptr};
    QtMessageHandler m_previous = nullptr;
    mutable QMutex m_lock;
    QStringList m_lines;
};

// Runs `work`, described by `what`, on its own thread and waits at most `timeoutMs`. Nothing is
// returned, with a failed check, when it threw or blocked. On a timeout the FIFO at `fifo` is
// opened for writing, which releases a reader blocked opening it. A worker still blocked after
// that cannot be joined, so the run reports it and exits at once rather than hang in the thread's
// destructor.
// CONTROL: by hand, test-side: importer_test.cpp's FIFO work made `std::this_thread::sleep_for(20s)`
//       -> "FAIL: S15.76: unpackPackage on a FIFO blocked for 3000 ms, and opening the FIFO's write
//       end did not release it", exit 1 after about 5 s instead of 20; restored.
template <typename Result>
std::optional<Result> runBounded(const std::function<Result()>& work, const QString& fifo, const QString& what, int timeoutMs) {
    std::promise<Result> promise;
    std::future<Result> result = promise.get_future();
    std::jthread worker([&] {
        try {
            promise.set_value(work());
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    const QString blocked = QStringLiteral("%1 blocked for %2 ms").arg(what).arg(timeoutMs);
    if (result.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready) {
        try {
            return result.get();
        } catch (const std::exception& e) {
            expect(false, QStringLiteral("%1 threw: %2").arg(what, QString::fromLocal8Bit(e.what())));
        } catch (...) {
            expect(false, QStringLiteral("%1 threw something that is not a std::exception").arg(what));
        }
        return std::nullopt;
    }
    for (int attempt = 0; attempt < 200 && result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready; ++attempt) {
        const int fd = ::open(QFile::encodeName(fifo).constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            ::close(fd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        expect(false, blocked + QStringLiteral(", and opening the FIFO's write end did not release it"));
        static_cast<void>(testing::reportCheckFailures());
        std::fflush(stderr);
        std::_Exit(1);
    }
    expect(false, blocked);
    return std::nullopt;
}

// Holds the global pool's only thread, so work started meanwhile waits until release(). The thread
// limit is restored on every path out, a throw included.
class PoolHold {
public:
    explicit PoolHold(int waitMs) : m_pool(*QThreadPool::globalInstance()), m_saved(m_pool.maxThreadCount()), m_waitMs(waitMs) {
        m_pool.setMaxThreadCount(1);
        // Shared, so a blocker outliving this object (release never seen) touches nothing freed.
        m_pool.start([gates = m_gates, waitMs] {
            gates->started.release();
            static_cast<void>(gates->release.tryAcquire(1, waitMs));
        });
        m_ok = m_gates->started.tryAcquire(1, waitMs);
    }
    ~PoolHold() {
        release();
        static_cast<void>(m_pool.waitForDone(m_waitMs));
        m_pool.setMaxThreadCount(m_saved);
    }
    PoolHold(const PoolHold&) = delete;
    PoolHold& operator=(const PoolHold&) = delete;

    // Whether the pool's thread is provably held: the blocker has started.
    bool ok() const { return m_ok; }
    void release() { m_gates->release.release(); }

private:
    struct Gates {
        QSemaphore started;
        QSemaphore release;
    };
    QThreadPool& m_pool;
    int m_saved;
    int m_waitMs;
    std::shared_ptr<Gates> m_gates = std::make_shared<Gates>();
    bool m_ok = false;
};

// This process's user namespace, as /proc names it; empty when it cannot be read.
inline QByteArray userNamespace() {
    char target[64] = {};
    const ssize_t got = ::readlink("/proc/self/ns/user", target, sizeof target - 1);
    return got > 0 ? QByteArray(target, int(got)) : QByteArray();
}

// A run's exit status, after printing its verdict lines (which inNamespace() relays from a child).
inline int verdict() {
    const bool failed = testing::reportCheckFailures();
    const bool faulted = testing::reportHarnessFaults("in this test file's fixtures, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return testing::reportNotVerifiedOrPassed();
}

// The start of a child run by inNamespace(): refuses to go on, reporting it, unless this process
// is in a user namespace of its own, and only then prints the start line. Nothing when it may go
// on, else its exit status.
inline std::optional<int> refuseUnlessOwnNamespace(const QString& goal, const QString& parentNamespace) {
    const QByteArray own = userNamespace();
    if (own.isEmpty() || own == parentNamespace.toUtf8()) {
        testing::harnessFailure(goal.toStdString() + ": not in a user namespace of its own (" + own.toStdString() + "), so nothing was changed");
        return verdict();
    }
    std::printf("namespace child started\n");
    std::fflush(stdout);
    return std::nullopt;
}

// Printed by a namespace child once its fixtures are built, just before its first call into the
// code under test, so inNamespace() can tell a crash or hang in setup from one in the app.
inline void childReady() {
    std::printf("namespace child ready\n");
    std::fflush(stdout);
}

// How long inNamespace() lets one child run. Each takes under half a second; the ctest TIMEOUT of
// every entry running children leaves room for all of them to reach this bound.
inline constexpr int kNamespaceChildMs = 10000;

// Runs this binary again as `flag args... <this user namespace>` under unshare with
// `unshareArgs`, and relays its verdicts. unshare runs without --fork, so the process that ends is
// the child itself.
// GOAL: a child that never printed its start line could not be put in namespaces of its own (not
//       verified); one that crashed or hung after its ready line did so in the code under test (a
//       failure naming the goal); one that did so between the two, in this file's setup (a
//       harness fault).
// WHY-THIS-SHAPE: one condition `!started || crashed -> not verified` is the obvious shape, and it
//       files a crash of the code under test as a skip; a single start line printed before setup
//       would blame the app for a crash in mount or writeFolder. A hang is bounded per child
//       (kNamespaceChildMs) so every verdict prints before ctest's TIMEOUT kills the run.
// CONTROL: app/library/importer.cpp, the free-space refusal given `std::abort();` -> red, "S15.76
//       (free space): the child crashed after its setup"; the same refusal given `for (;;) { ::pause(); }`
//       -> red, "S15.76 (free space): the child hung after its setup"; app/library/folder_watcher.cpp,
//       leasesShowWriters()'s `default: return false;` made `default: ::raise(SIGSEGV); return false;`
//       -> red, "S15.74: the child crashed after its setup" and the same for "S15.87 (holds kept)";
//       restored. By hand, test-side: importer_test.cpp's smallDiskChild given `std::abort();`
//       before childReady() -> "FAIL (harness): S15.76 (free space): the child crashed while
//       setting up its fixtures", and no FAIL line; removed.
inline void inNamespace(const std::string& goal, const QStringList& unshareArgs, const QString& flag, const QStringList& args) {
    QProcess child;
    child.setProgram(QStringLiteral("unshare"));
    child.setArguments(unshareArgs + QStringList{QCoreApplication::applicationFilePath(), flag} + args + QStringList{QString::fromUtf8(userNamespace())});
    child.start();
    if (!child.waitForStarted(10000)) {
        testing::notVerified(goal.c_str(), "unshare could not be started: " + child.errorString().toStdString());
        return;
    }
    const bool hung = !child.waitForFinished(kNamespaceChildMs);
    if (hung) {
        child.kill();
        static_cast<void>(child.waitForFinished(3000));
    }
    const QString said = QString::fromLocal8Bit(child.readAllStandardOutput() + child.readAllStandardError());
    const QStringList lines = said.split(u'\n', Qt::SkipEmptyParts);
    const bool ready = lines.contains(QStringLiteral("namespace child ready"));
    const QString qgoal = QString::fromStdString(goal);
    if (hung) {
        if (ready) {
            expect(false, QStringLiteral("%1: the child hung after its setup, in the code under test, for %2 ms; it said: %3")
                              .arg(qgoal).arg(kNamespaceChildMs).arg(said.trimmed().right(600)));
        } else {
            testing::harnessFailure(goal + ": the child hung before its setup was done, for " + std::to_string(kNamespaceChildMs) + " ms; it said: " + said.trimmed().toStdString());
        }
        return;
    }
    if (!lines.contains(QStringLiteral("namespace child started"))) {
        bool faulted = false;
        for (const QString& line : lines) {
            if (line.startsWith(QLatin1String("FAIL (harness): "))) {
                testing::harnessFailure(line.mid(16).toStdString());
                faulted = true;
            }
        }
        if (!faulted) {
            testing::notVerified(goal.c_str(), "unshare could not run this test in namespaces of its own: " + said.trimmed().toStdString());
        }
        return;
    }
    bool relayed = child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
    for (const QString& line : lines) {
        if (line.startsWith(QLatin1String("FAIL (harness): "))) {
            testing::harnessFailure(line.mid(16).toStdString());
            relayed = true;
        } else if (line.startsWith(QLatin1String("FAIL: "))) {
            expect(false, line.mid(6));
            relayed = true;
        } else if (line.contains(QLatin1String("NOT VERIFIED"))) {
            testing::notVerified(goal.c_str(), "the child said: " + line.toStdString());
            relayed = true;
        }
    }
    if (child.exitStatus() != QProcess::NormalExit && ready) {
        expect(false, QStringLiteral("%1: the child crashed after its setup, in the code under test; it said: %2").arg(qgoal, said.trimmed().right(600)));
    } else if (child.exitStatus() != QProcess::NormalExit) {
        testing::harnessFailure(goal + ": the child crashed while setting up its fixtures; it said: " + said.trimmed().toStdString());
    } else if (!relayed) {
        testing::harnessFailure(goal + ": the child exited " + std::to_string(child.exitCode()) + " with no verdict: " + said.toStdString());
    }
}

// Points every XDG base directory into `root`, so nothing this process does can land in the user's
// own ~/.config or ~/.local.
inline void isolateXdg(const QString& root) {
    for (const char* name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"}) {
        qputenv(name, QFile::encodeName(root + QLatin1Char('/') + QString::fromLatin1(name)));
    }
}

} // namespace wallpaperx::apptest
