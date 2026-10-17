// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "audio_probe.h"
#include "playback_speed.h"
#include "unique_fd.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include <expected>
#include <optional>

struct stat;

namespace wallpaperx {

// One editable property a wallpaper declares in wallpaper.json.
struct WallpaperProperty {
    Q_GADGET
    QML_VALUE_TYPE(wallpaperProperty)
    Q_PROPERTY(QString key MEMBER key CONSTANT)
    Q_PROPERTY(QString label MEMBER label CONSTANT)
    Q_PROPERTY(QString type READ typeName CONSTANT)
    Q_PROPERTY(QVariant defaultValue MEMBER defaultValue CONSTANT)
    Q_PROPERTY(double minimum MEMBER minimum CONSTANT)
    Q_PROPERTY(double maximum MEMBER maximum CONSTANT)
    Q_PROPERTY(QList<double> choices READ choices CONSTANT)

public:
    enum class Type { Float, Bool, Color };

    QString key;
    QString label;
    Type type = Type::Float;
    // A double, a bool, or a lower-case "#rrggbb" string, by `type`.
    QVariant defaultValue;
    // Float only; minimum < maximum.
    double minimum = 0.0;
    double maximum = 0.0;

    // The format's own name for `type`: "float", "bool" or "color".
    QString typeName() const;
    // The only values a float may take, when it has a fixed few: for "speed", the playback speeds
    // inside its declared range. Empty for any other property.
    QList<double> choices() const;
    // `value` as this property stores it, or nothing when it cannot hold it: one of choices() for a
    // float that has them, else a finite number in range for a float, a bool for a bool, "#rrggbb"
    // for a color. A valid QColor is taken with its alpha dropped, since the format has none.
    std::optional<QVariant> accept(const QVariant& value) const;

    friend bool operator==(const WallpaperProperty&, const WallpaperProperty&) = default;
};

// What identifies one version of a file's bytes, so a changed file is noticed without re-reading it.
struct FileStamp {
    quint64 device = 0;
    quint64 inode = 0;
    qint64 size = -1;
    qint64 modifiedNs = 0;

    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

// One wallpaper: a folder in the library, or a .wpx archive being checked before it is unpacked.
// Either usable, with every manifest field read and checked against the files beside it, or
// broken: `problem` says why, and only the file fields are set.
struct Wallpaper {
    Q_GADGET
    QML_VALUE_TYPE(wallpaper)
    Q_PROPERTY(QString filePath MEMBER filePath CONSTANT)
    Q_PROPERTY(QString fileName MEMBER fileName CONSTANT)
    Q_PROPERTY(QString problem MEMBER problem CONSTANT)
    Q_PROPERTY(bool usable READ usable CONSTANT)
    Q_PROPERTY(QString displayTitle READ displayTitle CONSTANT)
    Q_PROPERTY(QString type MEMBER type CONSTANT)
    Q_PROPERTY(QString title MEMBER title CONSTANT)
    Q_PROPERTY(QString author MEMBER author CONSTANT)
    Q_PROPERTY(QStringList tags MEMBER tags CONSTANT)
    Q_PROPERTY(QString assetName MEMBER assetName CONSTANT)
    Q_PROPERTY(QString assetPath MEMBER assetPath CONSTANT)
    Q_PROPERTY(QUrl previewSource READ previewSource CONSTANT)
    Q_PROPERTY(QList<wallpaperx::WallpaperProperty> properties MEMBER properties CONSTANT)

public:
    // Absolute path of the wallpaper's folder, or of the archive, as decodeFileName gives it.
    QString filePath;
    QString fileName;
    // wallpaper.json in a folder, the whole file for an archive; not given to QML.
    FileStamp stamp;
    QString problem;

    // "static", "video" or "shader": the values SetWallpaper takes.
    QString type;
    QString title;
    QString author;
    QStringList tags;
    // Plain relative paths inside the folder or archive, checked to name a file there.
    QString assetName;
    QString previewName;
    // The file the daemon is to open: `assetName` inside a usable folder, checked to resolve
    // inside it. Empty for an archive, which has nothing on disk to open.
    QString assetPath;
    // The files as they were read; for an archive, the archive's own.
    FileStamp previewStamp;
    FileStamp assetStamp;
    // In the order wallpaper.json lists them. In a folder, a video with an audio stream has `mute`
    // and `volume` whether declared or not, a wallpaper known to have none has neither, and a video
    // the probe could not answer for has them as declared.
    QList<WallpaperProperty> properties;
    // What the asset holds for sound, in a folder: Absent for a static or shader wallpaper, the
    // probe's answer for a video. Unknown in an archive, which is not probed; not given to QML.
    AudioStreams audio = AudioStreams::Unknown;

    // False for a default-constructed Wallpaper, which stands for none.
    bool usable() const { return !filePath.isEmpty() && problem.isEmpty(); }
    QString displayTitle() const { return title.isEmpty() ? fileName : title; }
    // An image://wallpaperx-preview URL naming this version of the preview, so a rewritten one
    // gets a new URL instead of a cached image. Empty for a broken wallpaper.
    QUrl previewSource() const;

    friend bool operator==(const Wallpaper&, const Wallpaper&) = default;
};

inline constexpr const char* kPreviewProviderId = "wallpaperx-preview";
inline constexpr qint64 kMaxManifestBytes = 1 << 20;
inline constexpr qint64 kMaxPreviewBytes = 32 << 20;
// The daemon's own bounds on what SetWallpaperWithProperties accepts (wallpaper_properties.h), so a
// wallpaper it would refuse is listed broken instead of failing at Apply. Not `inline`, as in
// daemon_client.h: internal linkage keeps a copy from merging with a daemon variable of its name.
constexpr qsizetype kMaxProperties = 64;
constexpr qsizetype kMaxPropertyKeyBytes = 64;
constexpr double kMaxVolume = 1.0;

// The keys of a video's own sound: offered for every video with sound, declared or not, and
// removed from a wallpaper known to have none.
constexpr const char* kMuteKey = "mute";
constexpr const char* kVolumeKey = "volume";

// The manifest fields of a wallpaper.json; the file fields are left empty. The error is a
// sentence for the user.
std::expected<Wallpaper, QString> parseManifest(const QByteArray& json);

// Reads the .wpx archive open as `fd`, which stays the caller's, shown as `path`. Never fails: an
// archive that cannot be used comes back broken. The path need not be UTF-8: nothing is ever set
// from it. The sound keys are not judged: that needs the asset probed for sound, which only a
// folder read does.
Wallpaper readWallpaper(int fd, const QString& path);
// Reads one wallpaper folder by the same rules, and judges the sound keys. The folder itself must
// not be a link, and every file wallpaper.json names must, its links followed, be a file inside it.
Wallpaper readWallpaperFolder(const QString& path);

// The bytes of `name` inside a wallpaper folder, refused when larger than `maxBytes`, or when it
// is not inside the folder by readWallpaperFolder's rule.
std::expected<QByteArray, QString> readWallpaperEntry(const QString& folderPath, const QString& name,
                                                      qint64 maxBytes);

// Every wallpaper folder directly in `directory`: each subfolder not hidden, and each link to a
// folder, which is listed broken. Usable ones first, each group by title. A directory that does
// not exist is an empty library; one that cannot be listed is the error. A video whose asset has
// the path and stamp it had in `previous`, an earlier scan, is not probed again.
std::expected<QList<Wallpaper>, QString> scanLibrary(const QString& directory, const QList<Wallpaper>& previous = {});
// The order scanLibrary lists in.
bool listedBefore(const Wallpaper& a, const Wallpaper& b);

// Whether a directory entry named `name` is a package: *.wpx in any case, not hidden.
bool isPackageFileName(QByteArrayView name);
// Refuses what could reach outside a folder: absolute, empty parts, ".", "..", backslashes.
bool isPlainRelativePath(const QString& name);

// A file-system path as text and back, losing nothing: bytes that are not UTF-8 become lone
// surrogates (U+DC80..U+DCFF), so such a path is not valid UTF-16 and cannot be mistaken for one
// that is. QFile::decodeName would turn them into U+FFFD, naming another file or none.
QString decodeFileName(QByteArrayView bytes);
QByteArray encodeFileName(QStringView name);
// The absolute path, as bytes, of a file URL naming this computer (no host, or localhost).
// Nothing for any other URL, for file:name, which has no absolute path, and for a path holding a
// NUL byte, which no file has.
std::optional<QByteArray> localPathOfFileUrl(const QUrl& url);

// $XDG_DATA_HOME/wallpaperx, or ~/.local/share/wallpaperx when that is unset: where a .wpx put
// by hand is imported from, and the parent of wallpapersDirectory.
QString defaultLibraryDirectory();
// Where the wallpaper folders live, inside `libraryDirectory`.
QString wallpapersDirectory(const QString& libraryDirectory);

FileStamp stampFrom(const struct stat& st);
// What `fd` holds from its offset to the end: EFBIG when that is more than `maxBytes`, else the
// errno of a failed read.
std::expected<QByteArray, int> readToEnd(int fd, qint64 maxBytes);

// openRegularFile's error for a name that is there but is not a regular file; never an errno.
inline constexpr int kNotRegularFile = -1;
// `name`, relative to `dirfd` or AT_FDCWD, opened with `flags` only once it is known to be a regular
// file; O_NOFOLLOW in `flags` refuses a link. Found with O_PATH, which opens nothing, checked through
// that descriptor and opened through it, so a device or FIFO put under the name meanwhile is never
// opened. Always non-blocking, so a file under another process's write lease is EWOULDBLOCK rather
// than a wait. The error is an errno or kNotRegularFile.
std::expected<UniqueFd, int> openRegularFile(int dirfd, const char* name, int flags);
// Nothing when `path` cannot be stat'ed.
std::optional<FileStamp> stampOf(const QString& path);

// The id previewSource() puts after "image://wallpaperx-preview/", and its inverse.
QString previewImageId(const QString& folderPath, const QString& entryName, const FileStamp& stamp);
struct PreviewLocation {
    QString folderPath;
    QString entryName;
};
std::optional<PreviewLocation> parsePreviewImageId(const QString& id);

} // namespace wallpaperx
