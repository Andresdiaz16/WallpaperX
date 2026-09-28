// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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
    // `value` as this property stores it, or nothing when it cannot hold it: a finite number in
    // range for a float, a bool for a bool, "#rrggbb" for a color. A valid QColor is taken with its
    // alpha dropped, since the format has none.
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

// A .wpx file in the library. Either usable, with every manifest field read and checked against
// the archive, or broken: `problem` says why, and only the file fields are set.
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
    Q_PROPERTY(QUrl previewSource READ previewSource CONSTANT)
    Q_PROPERTY(QList<wallpaperx::WallpaperProperty> properties MEMBER properties CONSTANT)

public:
    // Absolute path of the .wpx archive, as decodeFileName gives it.
    QString filePath;
    QString fileName;
    // The file as it was read; not given to QML.
    FileStamp stamp;
    QString problem;

    // "static", "video" or "shader": the values SetWallpaper takes.
    QString type;
    QString title;
    QString author;
    QStringList tags;
    // Names of entries inside the archive, checked to exist and to be plain relative paths.
    QString assetName;
    QString previewName;
    // In the order wallpaper.json lists them.
    QList<WallpaperProperty> properties;

    // False for a default-constructed Wallpaper, which stands for none.
    bool usable() const { return !filePath.isEmpty() && problem.isEmpty(); }
    QString displayTitle() const { return title.isEmpty() ? fileName : title; }
    // An image://wallpaperx-preview URL naming this version of the file, so an edited archive
    // gets a new URL instead of a cached image. Empty for a broken wallpaper.
    QUrl previewSource() const;

    friend bool operator==(const Wallpaper&, const Wallpaper&) = default;
};

inline constexpr const char* kPreviewProviderId = "wallpaperx-preview";
inline constexpr qint64 kMaxManifestBytes = 1 << 20;
inline constexpr qint64 kMaxPreviewBytes = 32 << 20;

// The manifest fields of a wallpaper.json; the file fields are left empty. The error is a
// sentence for the user.
std::expected<Wallpaper, QString> parseManifest(const QByteArray& json);

// Reads one .wpx archive. Never fails: an archive that cannot be used comes back broken.
Wallpaper readWallpaper(const QString& path);

// The uncompressed bytes of one archive entry, refused when larger than `maxBytes`.
std::expected<QByteArray, QString> readPackageEntry(const QString& archivePath, const QString& entryName,
                                                    qint64 maxBytes);

// Every package entry (isPackageFileName) directly in `directory` that is a regular file, not
// empty unless through a link, or cannot be stat'ed; usable ones first, each group by title. A
// directory that does not exist is an empty library; one that cannot be listed is the error.
std::expected<QList<Wallpaper>, QString> scanLibrary(const QString& directory);
// The order scanLibrary lists in.
bool listedBefore(const Wallpaper& a, const Wallpaper& b);

// Whether a directory entry named `name` is listed: *.wpx in any case, not hidden.
bool isPackageFileName(QByteArrayView name);

// A file-system path as text and back, losing nothing: bytes that are not UTF-8 become lone
// surrogates (U+DC80..U+DCFF), so such a path is not valid UTF-16 and cannot be mistaken for one
// that is. QFile::decodeName would turn them into U+FFFD, naming another file or none.
QString decodeFileName(QByteArrayView bytes);
QByteArray encodeFileName(QStringView name);

// $XDG_DATA_HOME/wallpaperx, or ~/.local/share/wallpaperx when that is unset.
QString defaultLibraryDirectory();

FileStamp stampFrom(const struct stat& st);
// Nothing when `path` cannot be stat'ed.
std::optional<FileStamp> stampOf(const QString& path);

// The id previewSource() puts after "image://wallpaperx-preview/", and its inverse.
QString previewImageId(const QString& archivePath, const QString& entryName, const FileStamp& stamp);
struct PreviewLocation {
    QString archivePath;
    QString entryName;
};
std::optional<PreviewLocation> parsePreviewImageId(const QString& id);

} // namespace wallpaperx
