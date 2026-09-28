// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Builds .wpx archives for the library tests with libzip, and the event-loop helpers they share.
// Manifests are raw JSON text or nlohmann::ordered_json, never QJsonObject: QJsonObject sorts keys,
// which would erase the property order the tests exist to check.

#include "tests/test_report.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QImage>
#include <QList>
#include <QPair>
#include <QString>
#include <QTimer>

#include <nlohmann/json.hpp>
#include <zip.h>

#include <cstdlib>
#include <exception>
#include <optional>
#include <string>

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

// Writes a ZIP archive holding `entries` at `path`. Empty string on success, else why not.
// libzip deletes an archive with no entries on close, so every caller passes at least one.
// libzip writes a temporary file beside `path` and renames it into place, so in a watched folder
// the archive arrives whole, as a move.
[[nodiscard]] inline QString writeZip(const QString& path, const Entries& entries) {
    int error = 0;
    zip_t* zip = zip_open(QFile::encodeName(path).constData(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (zip == nullptr) {
        return QStringLiteral("zip_open(%1) failed with libzip error %2").arg(path).arg(error);
    }
    for (const auto& [name, bytes] : entries) {
        zip_source_t* source = zip_source_buffer(zip, bytes.constData(), static_cast<zip_uint64_t>(bytes.size()), 0);
        if (source == nullptr || zip_file_add(zip, name.toUtf8().constData(), source, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0) {
            const QString why = QString::fromUtf8(zip_strerror(zip));
            if (source != nullptr) {
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

// Unlinks files named by raw bytes when it goes out of scope. QTemporaryDir removes its tree by
// path text, which cannot name a file that is not UTF-8, so without this the folder is left behind.
class RawNamesRemover {
public:
    RawNamesRemover(const QString& dir, QList<QByteArray> names) : m_dir(QFile::encodeName(dir)), m_names(std::move(names)) {}
    ~RawNamesRemover() {
        for (const QByteArray& name : std::as_const(m_names)) {
            ::unlink((m_dir + '/' + name).constData());
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

// Points every XDG base directory into `root`, so nothing this process does can land in the user's
// own ~/.config or ~/.local.
inline void isolateXdg(const QString& root) {
    for (const char* name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"}) {
        qputenv(name, QFile::encodeName(root + QLatin1Char('/') + QString::fromLatin1(name)));
    }
}

} // namespace wallpaperx::apptest
