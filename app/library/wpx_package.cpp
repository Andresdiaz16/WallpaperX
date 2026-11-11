// SPDX-License-Identifier: GPL-2.0-or-later

#include "wpx_package.h"

#include "audio_probe.h"
#include "unique_fd.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QStandardPaths>
#include <QStringDecoder>

#include <zip.h>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wallpaperx {

namespace {

QString systemError(int error)
{
    return qt_error_string(error);
}

// `value` as a double when it holds a number of any width; QML hands an integer literal over as an int.
std::optional<double> numberOf(const QVariant& value)
{
    switch (value.typeId()) {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return value.toDouble();
    default:
        return std::nullopt;
    }
}

// What QML hands over, as the shared rules take it: a number of any width, a bool, text, or a QColor,
// which is a colour once its alpha is dropped. Nothing for anything else, which no rule accepts.
std::optional<manifest::Value> sharedValue(const QVariant& value)
{
    if (const std::optional<double> number = numberOf(value)) {
        return *number;
    }
    switch (value.typeId()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::QString:
        return value.toString().toStdString();
    case QMetaType::QColor:
        if (const QColor color = value.value<QColor>(); color.isValid()) {
            return color.name(QColor::HexRgb).toStdString();
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

QVariant variantOf(const manifest::Value& value)
{
    return std::visit(
        [](const auto& v) {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) {
                return QVariant(QString::fromStdString(v));
            } else {
                return QVariant(v);
            }
        },
        value);
}

// The rules are the shared ones; the default is not one of them.
manifest::Property sharedProperty(const WallpaperProperty& property)
{
    manifest::Property shared;
    shared.key = property.key.toStdString();
    shared.type = property.type;
    shared.minimum = property.minimum;
    shared.maximum = property.maximum;
    return shared;
}

WallpaperProperty appProperty(const manifest::Property& shared)
{
    WallpaperProperty property;
    property.key = QString::fromStdString(shared.key);
    property.label = QString::fromStdString(shared.label);
    property.type = shared.type;
    property.defaultValue = variantOf(shared.fallback);
    property.minimum = shared.minimum;
    property.maximum = shared.maximum;
    return property;
}

// The manifest fields of `parsed`; the file fields are left empty.
Wallpaper appWallpaper(const manifest::Manifest& parsed)
{
    Wallpaper wallpaper;
    wallpaper.type = QString::fromStdString(parsed.type);
    wallpaper.title = QString::fromStdString(parsed.title);
    wallpaper.author = QString::fromStdString(parsed.author);
    for (const std::string& tag : parsed.tags) {
        wallpaper.tags.append(QString::fromStdString(tag));
    }
    wallpaper.previewName = QString::fromStdString(parsed.previewName);
    wallpaper.assetName = QString::fromStdString(parsed.assetName);
    for (const manifest::Property& property : parsed.properties) {
        wallpaper.properties.append(appProperty(property));
    }
    return wallpaper;
}

struct ZipDiscard {
    void operator()(zip_t* zip) const { zip_discard(zip); }
};
using ZipHandle = std::unique_ptr<zip_t, ZipDiscard>;

struct OpenedArchive {
    ZipHandle zip;
    struct stat st {};
};

// Takes `fd`, closing it on failure too.
std::expected<OpenedArchive, QString> openArchive(int fd)
{
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
    return OpenedArchive{ZipHandle(zip), st};
}

std::optional<zip_uint64_t> locate(zip_t* zip, const std::string& name)
{
    const zip_int64_t index = zip_name_locate(zip, name.c_str(), 0);
    return index < 0 ? std::nullopt : std::optional<zip_uint64_t>(static_cast<zip_uint64_t>(index));
}

std::expected<std::string, std::string> readEntry(zip_t* zip, const std::string& name, std::size_t maxBytes)
{
    const auto index = locate(zip, name);
    if (!index) {
        return std::unexpected("The archive has no " + name + ".");
    }
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat_index(zip, *index, 0, &st) != 0 || (st.valid & ZIP_STAT_SIZE) == 0) {
        return std::unexpected("The size of " + name + " in the archive cannot be read.");
    }
    if (st.size > static_cast<zip_uint64_t>(maxBytes)) {
        return std::unexpected(name + " is larger than " + std::to_string(maxBytes >> 20) + " MiB.");
    }
    zip_file_t* file = zip_fopen_index(zip, *index, 0);
    if (file == nullptr) {
        return std::unexpected(name + " cannot be opened in the archive: " + zip_strerror(zip) + ".");
    }
    std::string bytes(static_cast<std::size_t>(st.size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const zip_int64_t got = zip_fread(file, bytes.data() + done, static_cast<zip_uint64_t>(bytes.size() - done));
        if (got <= 0) {
            break;
        }
        done += static_cast<std::size_t>(got);
    }
    const std::string failure = done == bytes.size() ? std::string() : std::string(zip_file_strerror(file));
    zip_fclose(file);
    if (done != bytes.size()) {
        return std::unexpected(name + " cannot be read from the archive: " + failure + ".");
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

Wallpaper broken(Wallpaper file, const manifest::Problem& problem)
{
    if (problem.manifestStat) {
        file.stamp = stampFrom(*problem.manifestStat);
    }
    return broken(std::move(file), QString::fromStdString(problem.text));
}

// `file`'s name, as a usable wallpaper read as `read` says.
Wallpaper usable(Wallpaper file, const manifest::Read& read)
{
    Wallpaper wallpaper = appWallpaper(read.manifest);
    wallpaper.filePath = std::move(file.filePath);
    wallpaper.fileName = std::move(file.fileName);
    wallpaper.stamp = stampFrom(read.manifestStat);
    wallpaper.previewStamp = stampFrom(read.previewStat);
    wallpaper.assetStamp = stampFrom(read.asset.st);
    return wallpaper;
}

class ArchiveContents final : public manifest::Contents {
public:
    ArchiveContents(zip_t* zip, const struct stat& st) : m_zip(zip), m_st(st) {}

    std::expected<manifest::FileRead, std::string> read(const std::string& name, std::size_t maxBytes) override
    {
        auto bytes = readEntry(m_zip, name, maxBytes);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return manifest::FileRead{std::move(*bytes), m_st};
    }

    std::expected<manifest::FileFound, std::string> find(const std::string& name) override
    {
        if (!locate(m_zip, name)) {
            return std::unexpected("The archive has no " + name + ", which wallpaper.json names.");
        }
        return manifest::FileFound{m_st, {}};
    }

private:
    zip_t* m_zip;
    struct stat m_st;
};

Wallpaper namedFile(const QString& path)
{
    Wallpaper file;
    file.filePath = path;
    file.fileName = QFileInfo(path).fileName();
    return file;
}

// The folder `name` in `parent` (a descriptor, or AT_FDCWD), shown as `path`.
Wallpaper readFolderAt(int parent, const QByteArray& name, const QString& path, const KnownAudio& known = {})
{
    const auto audio = [&path, &known](const manifest::Read& video) {
        return audioOf(known, path + u'/' + QString::fromStdString(video.manifest.assetName), video);
    };
    auto read = manifest::readFolder(parent, name.toStdString(), encodeFileName(path).toStdString(), audio);
    if (!read) {
        return broken(namedFile(path), read.error());
    }
    return usableFolder(path, read->read, read->audio);
}

} // namespace

KnownAudio knownAudio(const QList<Wallpaper>& previous)
{
    KnownAudio known;
    for (const Wallpaper& wallpaper : previous) {
        // Only a probe's answer: any other type's Absent says nothing of the file. Unknown is asked
        // again, since what kept the probe from answering may have passed.
        if (wallpaper.usable() && wallpaper.type == u"video" && wallpaper.audio != AudioStreams::Unknown &&
            !wallpaper.assetPath.isEmpty()) {
            known.insert(wallpaper.assetPath, {wallpaper.assetStamp, wallpaper.audio});
        }
    }
    return known;
}

AudioStreams audioOf(const KnownAudio& known, const QString& assetPath, const manifest::Read& video)
{
    if (const auto it = known.constFind(assetPath); it != known.cend() && it->first == stampFrom(video.asset.st)) {
        return it->second;
    }
    return probeAudioStreams(video.asset.fd.get());
}

Wallpaper usableFolder(const QString& path, const manifest::Read& read, AudioStreams audio)
{
    Wallpaper wallpaper = usable(namedFile(path), read);
    wallpaper.assetPath = wallpaper.filePath + u'/' + wallpaper.assetName;
    wallpaper.audio = audio;
    return wallpaper;
}

Wallpaper brokenFolder(const QString& path, QString problem, std::optional<FileStamp> stamp)
{
    Wallpaper wallpaper = broken(namedFile(path), std::move(problem));
    if (stamp) {
        wallpaper.stamp = *stamp;
    }
    return wallpaper;
}

bool isPlainRelativePath(const QString& name)
{
    return manifest::isPlainRelativePath(name.toStdString());
}

QString WallpaperProperty::typeName() const
{
    return QString::fromLatin1(sharedProperty(*this).typeName());
}

QList<double> WallpaperProperty::choices() const
{
    const std::vector<double> within = sharedProperty(*this).choices();
    return QList<double>(within.begin(), within.end());
}

std::optional<QVariant> WallpaperProperty::accept(const QVariant& value) const
{
    const std::optional<manifest::Value> given = sharedValue(value);
    if (!given) {
        return std::nullopt;
    }
    const std::optional<manifest::Value> kept = sharedProperty(*this).accept(*given);
    return kept ? std::optional<QVariant>(variantOf(*kept)) : std::nullopt;
}

QUrl Wallpaper::previewSource() const
{
    if (filePath.isEmpty() || previewName.isEmpty()) {
        return {};
    }
    return QUrl(QStringLiteral("image://%1/%2")
                    .arg(QString::fromLatin1(kPreviewProviderId), previewImageId(filePath, previewName, previewStamp)));
}

QStringList Wallpaper::positionModes() const
{
    QStringList names;
    const std::string kind = type.toStdString();
    for (const PositionMode mode : kPositionModes) {
        if (positionModeAllowed(kind, mode)) {
            names.append(QString::fromLatin1(toString(mode)));
        }
    }
    return names;
}

QVariantMap Wallpaper::defaultValues() const
{
    QVariantMap values;
    if (!usable()) {
        return values;
    }
    for (const WallpaperProperty& property : properties) {
        values.insert(property.key, property.defaultValue);
    }
    const Placement standard;
    values.insert(QString::fromLatin1(kPositionKey), QString::fromLatin1(toString(standard.mode)));
    values.insert(QString::fromLatin1(kPositionZoomKey), standard.zoom);
    values.insert(QString::fromLatin1(kPositionXKey), standard.x);
    values.insert(QString::fromLatin1(kPositionYKey), standard.y);
    return values;
}

std::optional<QVariant> Wallpaper::accept(const QString& key, const QVariant& value) const
{
    if (!usable()) {
        return std::nullopt;
    }
    const std::optional<manifest::Value> given = sharedValue(value);
    if (!given) {
        return std::nullopt;
    }
    const auto it = std::ranges::find(properties, key, &WallpaperProperty::key);
    const std::optional<manifest::Property> declared =
        it == properties.end() ? std::nullopt : std::optional(sharedProperty(*it));
    const std::optional<manifest::Value> kept =
        manifest::acceptValue(type.toStdString(), declared ? &*declared : nullptr, key.toStdString(), *given);
    return kept ? std::optional<QVariant>(variantOf(*kept)) : std::nullopt;
}

std::expected<Wallpaper, QString> parseManifest(const QByteArray& json)
{
    const auto parsed = manifest::parse(std::string_view(json.constData(), size_t(json.size())));
    if (!parsed) {
        return std::unexpected(QString::fromStdString(parsed.error()));
    }
    return appWallpaper(*parsed);
}

Wallpaper readWallpaper(int fd, const QString& path)
{
    Wallpaper file = namedFile(path);
    const int own = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (own < 0) {
        return broken(std::move(file), QStringLiteral("It cannot be opened: %1.").arg(systemError(errno)));
    }
    auto archive = openArchive(own);
    if (!archive) {
        return broken(std::move(file), archive.error());
    }
    file.stamp = stampFrom(archive->st);
    ArchiveContents contents(archive->zip.get(), archive->st);
    const auto read = manifest::readFrom(contents);
    return read ? usable(std::move(file), *read) : broken(std::move(file), read.error());
}

Wallpaper readWallpaperFolder(const QString& path)
{
    return readFolderAt(AT_FDCWD, encodeFileName(path), path);
}

std::expected<QByteArray, QString> readWallpaperEntry(const QString& folderPath, const QString& name,
                                                      qint64 maxBytes)
{
    UniqueFd dir(::open(encodeFileName(folderPath).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir) {
        return std::unexpected(QStringLiteral("%1 cannot be opened: %2.").arg(folderPath, systemError(errno)));
    }
    manifest::FolderContents contents(dir.get());
    auto file = contents.read(name.toStdString(), static_cast<std::size_t>(std::max<qint64>(maxBytes, 0)));
    if (!file) {
        return std::unexpected(QString::fromStdString(file.error()));
    }
    return QByteArray::fromStdString(file->bytes);
}

std::expected<QList<Wallpaper>, QString> scanLibrary(const QString& directory, const QList<Wallpaper>& previous)
{
    const KnownAudio known = knownAudio(previous);
    auto wallpapers = readFolders(
        directory,
        [&known](int dirFd, const QByteArray& name, const QString& path) { return readFolderAt(dirFd, name, path, known); },
        QStringLiteral("It is a link to a folder. A wallpaper has to be a folder inside the library itself."));
    if (wallpapers) {
        std::ranges::sort(*wallpapers, listedBefore);
    }
    return wallpapers;
}

std::expected<QList<Wallpaper>, QString> readFolders(const QString& directory, const FolderReader& read,
                                                     const QString& linkProblem)
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
    const int dirFd = ::dirfd(listing.get());
    for (;;) {
        errno = 0;
        const dirent* entry = ::readdir(listing.get());
        if (entry == nullptr) {
            break;
        }
        // Every call below takes the entry's own bytes; `path` is only what the row shows.
        const QByteArray name(entry->d_name);
        // Also skips "." and "..", and the importer's unfinished folders.
        if (name.startsWith('.')) {
            continue;
        }
        const QString path = dir.absoluteFilePath(decodeFileName(name));
        struct stat st {};
        if (::fstatat(dirFd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            const int error = errno;
            // Removed since it was listed.
            if (error == ENOENT) {
                continue;
            }
            wallpapers.append(broken(namedFile(path), QStringLiteral("It cannot be read: %1.").arg(systemError(error))));
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            // Not followed: what changes inside a folder is watched, and the watch stops at the link.
            if (struct stat target {}; ::fstatat(dirFd, name.constData(), &target, 0) == 0 && S_ISDIR(target.st_mode)) {
                wallpapers.append(broken(namedFile(path), linkProblem));
            }
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            wallpapers.append(read(dirFd, name, path));
        }
    }
    // readdir's end and its failure differ only in errno.
    if (const int error = errno; error != 0) {
        return std::unexpected(QStringLiteral("%1 cannot be read: %2.").arg(dir.absolutePath(), systemError(error)));
    }
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

QString wallpapersDirectory(const QString& libraryDirectory)
{
    return libraryDirectory + QStringLiteral("/wallpapers");
}

FileStamp stampFrom(const struct stat& st)
{
    return FileStamp{static_cast<quint64>(st.st_dev), static_cast<quint64>(st.st_ino), static_cast<qint64>(st.st_size),
                     static_cast<qint64>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec};
}

std::expected<UniqueFd, int> openRegularFile(int dirfd, const char* name, int flags)
{
    const UniqueFd found(::openat(dirfd, name, O_PATH | O_CLOEXEC | (flags & O_NOFOLLOW)));
    struct stat st {};
    if (!found || ::fstat(found.get(), &st) != 0) {
        return std::unexpected(errno);
    }
    if (!S_ISREG(st.st_mode)) {
        return std::unexpected(kNotRegularFile);
    }
    // The magic link is a link, which O_NOFOLLOW would refuse. O_NONBLOCK, because some callers are
    // on the GUI thread and a lease break can take lease-break-time (45 s by default).
    UniqueFd fd(::open(("/proc/self/fd/" + QByteArray::number(found.get())).constData(),
                       (flags & ~O_NOFOLLOW) | O_CLOEXEC | O_NONBLOCK));
    if (!fd) {
        // `found` shows the file is there: ENOENT can only be a /proc not mounted, and must not read
        // as a missing file.
        return std::unexpected(errno == ENOENT ? EOPNOTSUPP : errno);
    }
    return fd;
}

std::expected<QByteArray, int> readToEnd(int fd, qint64 maxBytes)
{
    auto bytes = manifest::readToEnd(fd, static_cast<std::size_t>(std::max<qint64>(maxBytes, 0)));
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return QByteArray::fromStdString(*bytes);
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

std::optional<QByteArray> localPathOfFileUrl(const QUrl& url)
{
    if (!url.isLocalFile() || !(url.host().isEmpty() || url.host() == u"localhost")) {
        return std::nullopt;
    }
    // Through the encoded path, so a name that is not UTF-8 keeps its bytes.
    QByteArray path = QByteArray::fromPercentEncoding(url.path(QUrl::FullyEncoded).toLatin1());
    // A %00 would end the path early at every system call, naming another file.
    if (!path.startsWith('/') || path.contains('\0')) {
        return std::nullopt;
    }
    return path;
}

namespace {

constexpr auto kIdEncoding = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

} // namespace

// Base64url so a path's '/', '%', '?' and '#' survive the trip through a URL unchanged.
QString previewImageId(const QString& folderPath, const QString& entryName, const FileStamp& stamp)
{
    return QStringLiteral("%1-%2-%3-%4/%5/%6")
        .arg(QString::number(stamp.device), QString::number(stamp.inode), QString::number(stamp.size),
             QString::number(stamp.modifiedNs), QString::fromLatin1(folderPath.toUtf8().toBase64(kIdEncoding)),
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
