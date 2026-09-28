// SPDX-License-Identifier: GPL-2.0-or-later

#include "wpx_package.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringDecoder>

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <memory>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wallpaperx {

namespace {

using Json = nlohmann::ordered_json;

constexpr const char* kManifestName = "wallpaper.json";

QString systemError(int error)
{
    return qt_error_string(error);
}

QString fromJson(const Json& value)
{
    return QString::fromStdString(value.get<std::string>());
}

// Refuses what could reach outside a directory the archive is unpacked into.
bool isPlainRelativePath(const QString& name)
{
    if (name.isEmpty() || name.contains(u'\\') || name.contains(QChar(0))) {
        return false;
    }
    const auto parts = QStringView(name).split(u'/');
    return std::ranges::none_of(parts, [](QStringView part) { return part.isEmpty() || part == u"." || part == u".."; });
}

const QRegularExpression& colorPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return pattern;
}

std::expected<WallpaperProperty, QString> parseProperty(const QString& key, const Json& spec)
{
    const auto refuse = [&key](const QString& what) {
        return std::unexpected(QStringLiteral("Property \"%1\" in wallpaper.json %2").arg(key, what));
    };
    if (key.isEmpty()) {
        return std::unexpected(QStringLiteral("wallpaper.json has a property with an empty name."));
    }
    if (!spec.is_object()) {
        return refuse(QStringLiteral("is not an object."));
    }

    WallpaperProperty property;
    property.key = key;
    property.label = key;
    if (spec.contains("label")) {
        if (!spec["label"].is_string()) {
            return refuse(QStringLiteral("has a label that is not text."));
        }
        property.label = fromJson(spec["label"]);
    }

    const auto type = spec.find("type");
    if (type == spec.end() || !type->is_string()) {
        return refuse(QStringLiteral("has no type."));
    }
    const std::string typeName = type->get<std::string>();
    if (typeName == "float") {
        property.type = WallpaperProperty::Type::Float;
        if (!spec.contains("min") || !spec["min"].is_number() || !spec.contains("max") || !spec["max"].is_number()) {
            return refuse(QStringLiteral("needs a numeric min and max."));
        }
        property.minimum = spec["min"].get<double>();
        property.maximum = spec["max"].get<double>();
        if (!std::isfinite(property.minimum) || !std::isfinite(property.maximum) || !(property.minimum < property.maximum)) {
            return refuse(QStringLiteral("has a min that is not below its max."));
        }
    } else if (typeName == "bool") {
        property.type = WallpaperProperty::Type::Bool;
    } else if (typeName == "color") {
        property.type = WallpaperProperty::Type::Color;
    } else {
        return refuse(QStringLiteral("has type \"%1\", which is not float, bool or color.")
                          .arg(QString::fromStdString(typeName)));
    }

    const auto fallback = spec.find("default");
    if (fallback == spec.end()) {
        return refuse(QStringLiteral("has no default."));
    }
    QVariant value;
    if (fallback->is_number()) {
        value = fallback->get<double>();
    } else if (fallback->is_boolean()) {
        value = fallback->get<bool>();
    } else if (fallback->is_string()) {
        value = fromJson(*fallback);
    }
    const auto accepted = property.accept(value);
    if (!accepted) {
        return refuse(property.type == WallpaperProperty::Type::Float
                          ? QStringLiteral("has a default outside its min and max.")
                          : QStringLiteral("has a default that is not a %1.").arg(property.typeName()));
    }
    property.defaultValue = *accepted;
    return property;
}

struct ZipDiscard {
    void operator()(zip_t* zip) const { zip_discard(zip); }
};
using ZipHandle = std::unique_ptr<zip_t, ZipDiscard>;

struct OpenedArchive {
    ZipHandle zip;
    FileStamp stamp;
};

std::expected<OpenedArchive, QString> openArchive(const QString& path)
{
    // O_NONBLOCK: something other than a regular file put in its place since it was listed, a FIFO
    // say, is refused below instead of blocking the open.
    const int fd = ::open(encodeFileName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return std::unexpected(QStringLiteral("It cannot be opened: %1.").arg(systemError(errno)));
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return std::unexpected(QStringLiteral("It is not a regular file."));
    }
    int error = 0;
    zip_t* zip = zip_fdopen(fd, 0, &error);
    if (zip == nullptr) {
        ::close(fd);
        if (error == ZIP_ER_NOZIP) {
            return std::unexpected(QStringLiteral("It is not a ZIP archive, so not a WallpaperX package."));
        }
        zip_error_t described;
        zip_error_init_with_code(&described, error);
        const QString text = QString::fromUtf8(zip_error_strerror(&described));
        zip_error_fini(&described);
        return std::unexpected(QStringLiteral("It cannot be read as a ZIP archive: %1.").arg(text));
    }
    return OpenedArchive{ZipHandle(zip), stampFrom(st)};
}

std::optional<zip_uint64_t> locate(zip_t* zip, const QString& name)
{
    const zip_int64_t index = zip_name_locate(zip, name.toUtf8().constData(), 0);
    return index < 0 ? std::nullopt : std::optional<zip_uint64_t>(static_cast<zip_uint64_t>(index));
}

std::expected<QByteArray, QString> readEntry(zip_t* zip, const QString& name, qint64 maxBytes)
{
    const auto index = locate(zip, name);
    if (!index) {
        return std::unexpected(QStringLiteral("The archive has no %1.").arg(name));
    }
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat_index(zip, *index, 0, &st) != 0 || (st.valid & ZIP_STAT_SIZE) == 0) {
        return std::unexpected(QStringLiteral("The size of %1 in the archive cannot be read.").arg(name));
    }
    if (st.size > static_cast<zip_uint64_t>(maxBytes)) {
        return std::unexpected(QStringLiteral("%1 is larger than %2 MiB.").arg(name, QString::number(maxBytes >> 20)));
    }
    zip_file_t* file = zip_fopen_index(zip, *index, 0);
    if (file == nullptr) {
        return std::unexpected(QStringLiteral("%1 cannot be opened in the archive: %2.")
                                   .arg(name, QString::fromUtf8(zip_strerror(zip))));
    }
    QByteArray bytes(static_cast<qsizetype>(st.size), Qt::Uninitialized);
    qsizetype done = 0;
    while (done < bytes.size()) {
        const zip_int64_t got = zip_fread(file, bytes.data() + done, static_cast<zip_uint64_t>(bytes.size() - done));
        if (got <= 0) {
            break;
        }
        done += static_cast<qsizetype>(got);
    }
    const QString failure = done == bytes.size() ? QString() : QString::fromUtf8(zip_file_strerror(file));
    zip_fclose(file);
    if (done != bytes.size()) {
        return std::unexpected(QStringLiteral("%1 cannot be read from the archive: %2.").arg(name, failure));
    }
    return bytes;
}

Wallpaper broken(Wallpaper wallpaper, QString problem)
{
    Wallpaper result;
    result.filePath = std::move(wallpaper.filePath);
    result.fileName = std::move(wallpaper.fileName);
    result.stamp = wallpaper.stamp;
    result.problem = std::move(problem);
    return result;
}

} // namespace

QString WallpaperProperty::typeName() const
{
    switch (type) {
    case Type::Float:
        return QStringLiteral("float");
    case Type::Bool:
        return QStringLiteral("bool");
    case Type::Color:
        return QStringLiteral("color");
    }
    return {};
}

std::optional<QVariant> WallpaperProperty::accept(const QVariant& value) const
{
    switch (type) {
    case Type::Float: {
        switch (value.typeId()) {
        case QMetaType::Double:
        case QMetaType::Float:
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            break;
        default:
            return std::nullopt;
        }
        const double number = value.toDouble();
        if (!std::isfinite(number) || number < minimum || number > maximum) {
            return std::nullopt;
        }
        return QVariant(number);
    }
    case Type::Bool:
        if (value.typeId() != QMetaType::Bool) {
            return std::nullopt;
        }
        return value;
    case Type::Color: {
        if (value.typeId() == QMetaType::QColor) {
            const QColor color = value.value<QColor>();
            return color.isValid() ? std::optional<QVariant>(color.name(QColor::HexRgb)) : std::nullopt;
        }
        if (value.typeId() != QMetaType::QString) {
            return std::nullopt;
        }
        const QString text = value.toString();
        if (!colorPattern().match(text).hasMatch()) {
            return std::nullopt;
        }
        return QVariant(text.toLower());
    }
    }
    return std::nullopt;
}

QUrl Wallpaper::previewSource() const
{
    if (!usable()) {
        return {};
    }
    return QUrl(QStringLiteral("image://%1/%2")
                    .arg(QString::fromLatin1(kPreviewProviderId), previewImageId(filePath, previewName, stamp)));
}

std::expected<Wallpaper, QString> parseManifest(const QByteArray& json)
{
    Json doc;
    try {
        doc = Json::parse(json.cbegin(), json.cend());
    } catch (const Json::parse_error& e) {
        return std::unexpected(QStringLiteral("wallpaper.json is not valid JSON (at byte %1).").arg(e.byte));
    } catch (const Json::out_of_range&) {
        // How the parser reports a number literal too large for a double, such as 1e999.
        return std::unexpected(QStringLiteral("wallpaper.json holds a number too large to read."));
    } catch (const Json::exception& e) {
        return std::unexpected(QStringLiteral("wallpaper.json cannot be read: %1.").arg(QString::fromUtf8(e.what())));
    }
    if (!doc.is_object()) {
        return std::unexpected(QStringLiteral("wallpaper.json is not a JSON object."));
    }

    const auto version = doc.find("schema_version");
    if (version == doc.end()) {
        return std::unexpected(QStringLiteral("wallpaper.json has no schema_version."));
    }
    const bool versionOne = (version->is_string() && version->get<std::string>() == "1")
                            || (version->is_number_integer() && version->get<long long>() == 1);
    if (!versionOne) {
        return std::unexpected(QStringLiteral("wallpaper.json is for schema version %1, which this WallpaperX does "
                                              "not read.")
                                   .arg(QString::fromStdString(version->dump())));
    }

    // Each is required, a string, and non-empty.
    const auto text = [&doc](const char* key) -> std::optional<QString> {
        const auto it = doc.find(key);
        if (it == doc.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
            return std::nullopt;
        }
        return fromJson(*it);
    };

    Wallpaper wallpaper;
    const auto type = text("type");
    if (!type || (*type != u"static" && *type != u"video" && *type != u"shader")) {
        return std::unexpected(QStringLiteral("wallpaper.json needs a type of static, video or shader."));
    }
    wallpaper.type = *type;

    const auto title = text("title");
    if (!title) {
        return std::unexpected(QStringLiteral("wallpaper.json has no title."));
    }
    wallpaper.title = *title;

    if (doc.contains("author")) {
        if (!doc["author"].is_string()) {
            return std::unexpected(QStringLiteral("wallpaper.json has an author that is not text."));
        }
        wallpaper.author = fromJson(doc["author"]);
    }

    for (const char* key : {"preview", "file"}) {
        const auto name = text(key);
        if (!name) {
            return std::unexpected(QStringLiteral("wallpaper.json has no \"%1\".").arg(QLatin1StringView(key)));
        }
        if (!isPlainRelativePath(*name)) {
            return std::unexpected(QStringLiteral("wallpaper.json's \"%1\" is not a plain path inside the archive.")
                                       .arg(QLatin1StringView(key)));
        }
        (std::string_view(key) == "preview" ? wallpaper.previewName : wallpaper.assetName) = *name;
    }

    if (doc.contains("tags")) {
        const Json& tags = doc["tags"];
        if (!tags.is_array() || !std::ranges::all_of(tags, &Json::is_string)) {
            return std::unexpected(QStringLiteral("wallpaper.json's tags are not a list of text."));
        }
        for (const Json& tag : tags) {
            wallpaper.tags.append(fromJson(tag));
        }
    }

    if (doc.contains("properties")) {
        const Json& properties = doc["properties"];
        if (!properties.is_object()) {
            return std::unexpected(QStringLiteral("wallpaper.json's properties are not an object."));
        }
        for (const auto& [key, spec] : properties.items()) {
            auto property = parseProperty(QString::fromStdString(key), spec);
            if (!property) {
                return std::unexpected(property.error());
            }
            wallpaper.properties.append(std::move(*property));
        }
    }
    return wallpaper;
}

Wallpaper readWallpaper(const QString& path)
{
    Wallpaper file;
    file.filePath = path;
    file.fileName = QFileInfo(path).fileName();
    // The daemon and Plasma take a path as a D-Bus string, which must be UTF-8.
    if (!QStringView(path).isValidUtf16()) {
        return broken(std::move(file), QStringLiteral("Its path is not valid UTF-8, so it cannot be set as a wallpaper. "
                                                      "Rename the file."));
    }

    auto archive = openArchive(path);
    if (!archive) {
        return broken(std::move(file), archive.error());
    }
    file.stamp = archive->stamp;

    const auto manifest = readEntry(archive->zip.get(), QString::fromLatin1(kManifestName), kMaxManifestBytes);
    if (!manifest) {
        return broken(std::move(file), manifest.error());
    }
    auto parsed = parseManifest(*manifest);
    if (!parsed) {
        return broken(std::move(file), parsed.error());
    }
    for (const QString& name : {parsed->previewName, parsed->assetName}) {
        if (!locate(archive->zip.get(), name)) {
            return broken(std::move(file), QStringLiteral("The archive has no %1, which wallpaper.json names.").arg(name));
        }
    }
    parsed->filePath = std::move(file.filePath);
    parsed->fileName = std::move(file.fileName);
    parsed->stamp = file.stamp;
    return std::move(*parsed);
}

std::expected<QByteArray, QString> readPackageEntry(const QString& archivePath, const QString& entryName,
                                                    qint64 maxBytes)
{
    auto archive = openArchive(archivePath);
    if (!archive) {
        return std::unexpected(archive.error());
    }
    return readEntry(archive->zip.get(), entryName, maxBytes);
}

std::expected<QList<Wallpaper>, QString> scanLibrary(const QString& directory)
{
    const QDir dir(directory);
    std::unique_ptr<DIR, int (*)(DIR*)> listing(::opendir(encodeFileName(dir.absolutePath()).constData()), ::closedir);
    if (!listing) {
        const int error = errno;
        if (error == ENOENT) {
            return QList<Wallpaper>();
        }
        if (error == ENOTDIR) {
            return std::unexpected(QStringLiteral("%1 is not a folder.").arg(dir.absolutePath()));
        }
        return std::unexpected(QStringLiteral("%1 cannot be read: %2.").arg(dir.absolutePath(), systemError(error)));
    }

    QList<Wallpaper> wallpapers;
    for (;;) {
        errno = 0;
        const dirent* entry = ::readdir(listing.get());
        if (entry == nullptr) {
            break;
        }
        // Every call below takes the entry's own bytes; `path` is only what the row shows.
        const char* name = entry->d_name;
        if (!isPackageFileName(name)) {
            continue;
        }
        const QString path = dir.absoluteFilePath(decodeFileName(name));
        struct stat st {};
        // Followed, so a link to a package counts; a subfolder or FIFO named *.wpx is not one.
        if (::fstatat(::dirfd(listing.get()), name, &st, 0) != 0) {
            const int error = errno;
            struct stat link {};
            // ENOENT is also an entry removed since it was listed, which is not shown.
            if (error == ENOENT && ::fstatat(::dirfd(listing.get()), name, &link, AT_SYMLINK_NOFOLLOW) != 0) {
                continue;
            }
            Wallpaper unreadable;
            unreadable.filePath = path;
            unreadable.fileName = QFileInfo(path).fileName();
            unreadable.problem = error == ENOENT ? QStringLiteral("It is a link to a file that does not exist.")
                                                 : QStringLiteral("It cannot be read: %1.").arg(systemError(error));
            wallpapers.append(std::move(unreadable));
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        // An empty file is one not yet written, never a package. Through a link it is listed, broken:
        // the link's watch is the only thing that notices its target filled in outside the folder.
        if (struct stat link {}; st.st_size == 0 && ::fstatat(::dirfd(listing.get()), name, &link, AT_SYMLINK_NOFOLLOW) == 0
                                 && !S_ISLNK(link.st_mode)) {
            continue;
        }
        Wallpaper wallpaper = readWallpaper(path);
        // Taken before the read, and set on a file that could not be opened too, so a caller
        // comparing it with the file now sees any change made while or since it was read.
        wallpaper.stamp = stampFrom(st);
        wallpapers.append(std::move(wallpaper));
    }
    // readdir's end and its failure differ only in errno.
    if (const int error = errno; error != 0) {
        return std::unexpected(QStringLiteral("%1 cannot be read: %2.").arg(dir.absolutePath(), systemError(error)));
    }

    std::ranges::sort(wallpapers, listedBefore);
    return wallpapers;
}

bool listedBefore(const Wallpaper& a, const Wallpaper& b)
{
    if (a.usable() != b.usable()) {
        return a.usable();
    }
    if (const int order = a.displayTitle().localeAwareCompare(b.displayTitle()); order != 0) {
        return order < 0;
    }
    return a.filePath < b.filePath;
}

QString defaultLibraryDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/wallpaperx");
}

FileStamp stampFrom(const struct stat& st)
{
    return FileStamp{static_cast<quint64>(st.st_dev), static_cast<quint64>(st.st_ino), static_cast<qint64>(st.st_size),
                     static_cast<qint64>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec};
}

std::optional<FileStamp> stampOf(const QString& path)
{
    struct stat st {};
    if (::stat(encodeFileName(path).constData(), &st) != 0) {
        return std::nullopt;
    }
    return stampFrom(st);
}

bool isPackageFileName(QByteArrayView name)
{
    return !name.startsWith('.') && name.size() > 4 && name.last(4).compare(".wpx", Qt::CaseInsensitive) == 0;
}

QString decodeFileName(QByteArrayView bytes)
{
    // ConvertInitialBom: by default a leading U+FEFF is dropped, which would name another file.
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom);
    QString text = utf8(bytes);
    if (!utf8.hasError()) {
        return text;
    }
    QString escaped;
    escaped.reserve(bytes.size());
    for (const char byte : bytes) {
        const auto value = static_cast<uchar>(byte);
        escaped.append(value < 0x80 ? QChar(value) : QChar(char16_t(0xDC00 + value)));
    }
    return escaped;
}

QByteArray encodeFileName(QStringView name)
{
    if (name.isValidUtf16()) {
        return name.toUtf8();
    }
    QByteArray bytes;
    bytes.reserve(name.size());
    for (const QChar c : name) {
        const char16_t unit = c.unicode();
        if (unit < 0x80 || (unit >= 0xDC80 && unit <= 0xDCFF)) {
            bytes.append(static_cast<char>(unit & 0xFF));
        } else {
            bytes.append(QStringView(&c, 1).toUtf8());
        }
    }
    return bytes;
}

namespace {

constexpr auto kIdEncoding = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

} // namespace

// Base64url so a path's '/', '%', '?' and '#' survive the trip through a URL unchanged.
QString previewImageId(const QString& archivePath, const QString& entryName, const FileStamp& stamp)
{
    return QStringLiteral("%1-%2-%3-%4/%5/%6")
        .arg(QString::number(stamp.device), QString::number(stamp.inode), QString::number(stamp.size),
             QString::number(stamp.modifiedNs), QString::fromLatin1(archivePath.toUtf8().toBase64(kIdEncoding)),
             QString::fromLatin1(entryName.toUtf8().toBase64(kIdEncoding)));
}

std::optional<PreviewLocation> parsePreviewImageId(const QString& id)
{
    const QStringList parts = id.split(u'/');
    if (parts.size() != 3) {
        return std::nullopt;
    }
    const auto decode = [](const QString& part) {
        return QByteArray::fromBase64Encoding(part.toLatin1(), kIdEncoding | QByteArray::AbortOnBase64DecodingErrors);
    };
    const auto path = decode(parts[1]);
    const auto entry = decode(parts[2]);
    if (!path || !entry || (*path).isEmpty() || (*entry).isEmpty()) {
        return std::nullopt;
    }
    return PreviewLocation{QString::fromUtf8(*path), QString::fromUtf8(*entry)};
}

} // namespace wallpaperx
