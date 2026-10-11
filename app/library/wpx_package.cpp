// SPDX-License-Identifier: GPL-2.0-or-later

#include "wpx_package.h"

#include "audio_probe.h"
#include "unique_fd.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringDecoder>

#include <nlohmann/json.hpp>
#include <zip.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <memory>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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

const QRegularExpression& colorPattern()
{
    // anchoredPattern, not ^...$: `$` also matches before a final "\n", which the daemon refuses.
    static const QRegularExpression pattern(QRegularExpression::anchoredPattern(QStringLiteral("#[0-9a-fA-F]{6}")));
    return pattern;
}

bool isPropertyKey(const QString& key)
{
    static const QRegularExpression pattern(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9_.-]{1,%1}").arg(kMaxPropertyKeyBytes)));
    return pattern.match(key).hasMatch();
}

QString propertyProblem(const QString& key, const QString& what)
{
    return QStringLiteral("Property \"%1\" in wallpaper.json %2").arg(key, what);
}

std::expected<WallpaperProperty, QString> parseProperty(const QString& key, const Json& spec)
{
    const auto refuse = [&key](const QString& what) { return std::unexpected(propertyProblem(key, what)); };
    // Not quoted back: it can hold anything, a bidirectional override included.
    if (!isPropertyKey(key)) {
        return std::unexpected(QStringLiteral("wallpaper.json has a property whose name is not 1 to %1 letters, "
                                              "digits, \"_\", \"-\" or \".\".")
                                   .arg(kMaxPropertyKeyBytes));
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

    // A key the daemon gives a meaning, refused rather than clamped: a clamped range is one the
    // author never declared. The sound keys are checked in settleSoundProperties(), which knows
    // whether they are kept.
    if (key == u"speed") {
        if (property.type != WallpaperProperty::Type::Float) {
            return refuse(QStringLiteral("is not a float, and WallpaperX reads a video's speed as one."));
        }
        if (!(property.minimum > 0.0) || property.maximum > kMaxPlaybackSpeed) {
            return refuse(QStringLiteral("allows %1 to %2, and WallpaperX plays video above 0 and at most %3 times "
                                         "its speed.")
                              .arg(property.minimum)
                              .arg(property.maximum)
                              .arg(kMaxPlaybackSpeed));
        }
    }
    return property;
}

// Why the daemon would refuse a declared `mute` or `volume`, for the same reason as `speed`.
std::optional<QString> soundPropertyProblem(const WallpaperProperty& property)
{
    if (property.key == QLatin1StringView(kMuteKey) && property.type != WallpaperProperty::Type::Bool) {
        return propertyProblem(property.key, QStringLiteral("is not a bool, and WallpaperX reads \"mute\" as one."));
    }
    if (property.key != QLatin1StringView(kVolumeKey)) {
        return std::nullopt;
    }
    if (property.type != WallpaperProperty::Type::Float) {
        return propertyProblem(property.key,
                               QStringLiteral("is not a float, and WallpaperX reads a video's volume as one."));
    }
    if (property.minimum < 0.0 || property.maximum > kMaxVolume) {
        return propertyProblem(property.key,
                               QStringLiteral("allows %1 to %2, and WallpaperX plays a video's volume from 0 to %3.")
                                   .arg(property.minimum)
                                   .arg(property.maximum)
                                   .arg(kMaxVolume));
    }
    return std::nullopt;
}

struct ZipDiscard {
    void operator()(zip_t* zip) const { zip_discard(zip); }
};
using ZipHandle = std::unique_ptr<zip_t, ZipDiscard>;

struct OpenedArchive {
    ZipHandle zip;
    FileStamp stamp;
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

struct ReadFile {
    QByteArray bytes;
    FileStamp stamp;
};

struct FoundFile {
    FileStamp stamp;
    // The file, open, in a folder; none in an archive.
    UniqueFd fd;
};

// The files of one wallpaper, wherever they are kept. Messages are sentences for the user.
class Contents {
public:
    virtual ~Contents() = default;
    virtual std::expected<ReadFile, QString> read(const QString& name, qint64 maxBytes) = 0;
    // The file wallpaper.json names as `name`.
    virtual std::expected<FoundFile, QString> find(const QString& name) = 0;
};

class ArchiveContents final : public Contents {
public:
    ArchiveContents(zip_t* zip, FileStamp stamp) : m_zip(zip), m_stamp(stamp) {}

    std::expected<ReadFile, QString> read(const QString& name, qint64 maxBytes) override
    {
        auto bytes = readEntry(m_zip, name, maxBytes);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return ReadFile{std::move(*bytes), m_stamp};
    }

    std::expected<FoundFile, QString> find(const QString& name) override
    {
        if (!locate(m_zip, name)) {
            return std::unexpected(QStringLiteral("The archive has no %1, which wallpaper.json names.").arg(name));
        }
        return FoundFile{m_stamp, {}};
    }

private:
    zip_t* m_zip;
    FileStamp m_stamp;
};

// Where /proc says `fd` is open; empty when it cannot tell.
QByteArray openedPath(int fd)
{
    const QByteArray link = "/proc/self/fd/" + QByteArray::number(fd);
    QByteArray target(PATH_MAX, Qt::Uninitialized);
    const ssize_t length = ::readlink(link.constData(), target.data(), size_t(target.size()));
    if (length <= 0 || length >= target.size()) {
        return {};
    }
    target.truncate(length);
    return target;
}

// Opens `name` below the folder `dirfd`, refusing with EXDEV a file that, links followed, is not
// inside it.
int openBeneath(int dirfd, const QByteArray& name, int flags)
{
    open_how how{};
    how.flags = static_cast<quint64>(flags | O_CLOEXEC);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
    const int fd = static_cast<int>(::syscall(SYS_openat2, dirfd, name.constData(), &how, sizeof how));
    // EXDEV also refuses an absolute link, or a ".." out and back in, that lands inside; ENOSYS is
    // a kernel before 5.6, and a sandbox filtering openat2 answers ENOSYS or EPERM.
    if (fd >= 0 || (errno != EXDEV && errno != ENOSYS && errno != EPERM)) {
        return fd;
    }
    // Found with O_PATH, which opens nothing, then checked, then opened through its own descriptor:
    // a device or FIFO outside the folder is never opened, even to be refused.
    const UniqueFd found(::openat(dirfd, name.constData(), O_PATH | O_CLOEXEC));
    if (!found) {
        return -1;
    }
    const QByteArray folder = openedPath(dirfd);
    const QByteArray file = openedPath(found.get());
    if (folder.isEmpty() || !file.startsWith(folder == "/" ? folder : folder + '/')) {
        errno = EXDEV;
        return -1;
    }
    return ::open(("/proc/self/fd/" + QByteArray::number(found.get())).constData(), flags | O_CLOEXEC);
}

class FolderContents final : public Contents {
public:
    explicit FolderContents(int dirfd) : m_dir(dirfd) {}

    std::expected<ReadFile, QString> read(const QString& name, qint64 maxBytes) override
    {
        struct stat st {};
        auto fd = open(name, QStringLiteral("The folder has no %1.").arg(name), st);
        if (!fd) {
            return std::unexpected(fd.error());
        }
        auto bytes = readToEnd(fd->get(), maxBytes);
        if (!bytes) {
            return std::unexpected(bytes.error() == EFBIG
                                       ? QStringLiteral("%1 is larger than %2 MiB.").arg(name, QString::number(maxBytes >> 20))
                                       : QStringLiteral("%1 cannot be read: %2.").arg(name, systemError(bytes.error())));
        }
        return ReadFile{std::move(*bytes), stampFrom(st)};
    }

    std::expected<FoundFile, QString> find(const QString& name) override
    {
        struct stat st {};
        auto fd = open(name, QStringLiteral("The folder has no %1, which wallpaper.json names.").arg(name), st);
        if (!fd) {
            return std::unexpected(fd.error());
        }
        return FoundFile{stampFrom(st), std::move(*fd)};
    }

private:
    std::expected<UniqueFd, QString> open(const QString& name, QString missing, struct stat& st)
    {
        // O_NONBLOCK: a FIFO under the name is refused below instead of blocking the open.
        UniqueFd fd(openBeneath(m_dir, name.toUtf8(), O_RDONLY | O_NONBLOCK | O_NOCTTY));
        if (!fd) {
            switch (const int error = errno) {
            case ENOENT:
            case ENOTDIR:
                return std::unexpected(std::move(missing));
            case EXDEV:
                return std::unexpected(QStringLiteral("%1 leads outside the wallpaper's folder.").arg(name));
            default:
                return std::unexpected(QStringLiteral("%1 cannot be opened: %2.").arg(name, systemError(error)));
            }
        }
        if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
            return std::unexpected(QStringLiteral("%1 is not a regular file.").arg(name));
        }
        return fd;
    }

    int m_dir;
};

struct ReadContents {
    Wallpaper wallpaper;
    // The asset, still open, when `wallpaper` is usable and read from a folder.
    UniqueFd asset;
};

// The one validator for a folder and an archive: the manifest, then every file it names.
ReadContents readFrom(Wallpaper file, Contents& contents)
{
    const auto manifest = contents.read(QString::fromLatin1(kManifestName), kMaxManifestBytes);
    if (!manifest) {
        return {broken(std::move(file), manifest.error())};
    }
    file.stamp = manifest->stamp;
    auto parsed = parseManifest(manifest->bytes);
    if (!parsed) {
        return {broken(std::move(file), parsed.error())};
    }
    const auto preview = contents.find(parsed->previewName);
    if (!preview) {
        return {broken(std::move(file), preview.error())};
    }
    auto asset = contents.find(parsed->assetName);
    if (!asset) {
        return {broken(std::move(file), asset.error())};
    }
    parsed->filePath = std::move(file.filePath);
    parsed->fileName = std::move(file.fileName);
    parsed->stamp = file.stamp;
    parsed->previewStamp = preview->stamp;
    parsed->assetStamp = asset->stamp;
    return {std::move(*parsed), std::move(asset->fd)};
}

// Gives a video with sound its own sound controls, taking the declared ones where there are, and
// takes them from a wallpaper known to have none: they would set nothing anyone can hear. One the
// probe could not answer for keeps them as declared. What is kept is what Apply sends, so it is
// held to the daemon's rule; the error is a sentence for the user.
std::optional<QString> settleSoundProperties(Wallpaper& wallpaper)
{
    if (wallpaper.audio == AudioStreams::Absent) {
        wallpaper.properties.removeIf([](const WallpaperProperty& property) {
            return property.key == QLatin1StringView(kMuteKey) || property.key == QLatin1StringView(kVolumeKey);
        });
        return std::nullopt;
    }
    for (const WallpaperProperty& property : std::as_const(wallpaper.properties)) {
        if (auto problem = soundPropertyProblem(property)) {
            return problem;
        }
    }
    if (wallpaper.audio == AudioStreams::Unknown) {
        return std::nullopt;
    }
    // Taken as declared once the daemon's property cap is reached: one more would cost the
    // wallpaper its Apply.
    const auto declared = [&wallpaper](const char* key) {
        return wallpaper.properties.size() >= kMaxProperties ||
               std::ranges::contains(wallpaper.properties, QLatin1StringView(key), &WallpaperProperty::key);
    };
    if (!declared(kMuteKey)) {
        WallpaperProperty mute;
        mute.key = mute.label = QString::fromLatin1(kMuteKey);
        mute.type = WallpaperProperty::Type::Bool;
        mute.defaultValue = false;
        wallpaper.properties.append(mute);
    }
    if (!declared(kVolumeKey)) {
        WallpaperProperty volume;
        volume.key = volume.label = QString::fromLatin1(kVolumeKey);
        volume.type = WallpaperProperty::Type::Float;
        volume.defaultValue = kMaxVolume;
        volume.minimum = 0.0;
        volume.maximum = kMaxVolume;
        wallpaper.properties.append(volume);
    }
    return std::nullopt;
}

// Probe answers from an earlier scan, by asset path, each with the version of the asset it is for.
using KnownAudio = QHash<QString, std::pair<FileStamp, AudioStreams>>;

AudioStreams audioOf(const Wallpaper& wallpaper, int assetFd, const KnownAudio& known)
{
    if (wallpaper.type != u"video") {
        return AudioStreams::Absent;
    }
    if (const auto it = known.constFind(wallpaper.assetPath); it != known.cend() && it->first == wallpaper.assetStamp) {
        return it->second;
    }
    return probeAudioStreams(assetFd);
}

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
    Wallpaper folder = namedFile(path);
    // The daemon and Plasma take a path as a D-Bus string, which must be UTF-8.
    if (!QStringView(path).isValidUtf16()) {
        return broken(std::move(folder), QStringLiteral("Its path is not valid UTF-8, so it cannot be set as a wallpaper. "
                                                        "Rename the folder."));
    }
    UniqueFd dir(::openat(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!dir) {
        const int error = errno;
        struct stat link {};
        if (::fstatat(parent, name.constData(), &link, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(link.st_mode)) {
            return broken(std::move(folder), QStringLiteral("It is a link, not a folder."));
        }
        return broken(std::move(folder), error == ENOTDIR ? QStringLiteral("It is not a folder.")
                                                          : QStringLiteral("It cannot be opened: %1.").arg(systemError(error)));
    }
    FolderContents contents(dir.get());
    ReadContents read = readFrom(std::move(folder), contents);
    Wallpaper& wallpaper = read.wallpaper;
    if (wallpaper.usable()) {
        wallpaper.assetPath = wallpaper.filePath + u'/' + wallpaper.assetName;
        wallpaper.audio = audioOf(wallpaper, read.asset.get(), known);
        if (auto problem = settleSoundProperties(wallpaper)) {
            return broken(std::move(wallpaper), std::move(*problem));
        }
    }
    return std::move(wallpaper);
}

} // namespace

bool isPlainRelativePath(const QString& name)
{
    if (name.isEmpty() || name.contains(u'\\') || name.contains(QChar(0))) {
        return false;
    }
    const auto parts = QStringView(name).split(u'/');
    return std::ranges::none_of(parts, [](QStringView part) { return part.isEmpty() || part == u"." || part == u".."; });
}

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
                    .arg(QString::fromLatin1(kPreviewProviderId), previewImageId(filePath, previewName, previewStamp)));
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
            return std::unexpected(QStringLiteral("wallpaper.json's \"%1\" is not a plain path inside the wallpaper.")
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
        if (qsizetype(properties.size()) > kMaxProperties) {
            return std::unexpected(QStringLiteral("wallpaper.json has %1 properties; WallpaperX takes at most %2.")
                                       .arg(properties.size())
                                       .arg(kMaxProperties));
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
    file.stamp = archive->stamp;
    ArchiveContents contents(archive->zip.get(), archive->stamp);
    return readFrom(std::move(file), contents).wallpaper;
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
    FolderContents contents(dir.get());
    auto file = contents.read(name, maxBytes);
    if (!file) {
        return std::unexpected(file.error());
    }
    return std::move(file->bytes);
}

std::expected<QList<Wallpaper>, QString> scanLibrary(const QString& directory, const QList<Wallpaper>& previous)
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
                wallpapers.append(broken(namedFile(path), QStringLiteral("It is a link to a folder. A wallpaper has to be "
                                                                         "a folder inside the library itself.")));
            }
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            wallpapers.append(readFolderAt(dirFd, name, path, known));
        }
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
    struct stat st {};
    const qint64 size = ::fstat(fd, &st) == 0 ? qint64(st.st_size) : 0;
    if (size > maxBytes) {
        return std::unexpected(EFBIG);
    }
    // To the end, not to st_size: the file may be growing. One byte over says it is too large.
    QByteArray bytes(qsizetype(std::max<qint64>(size, 0)) + 1, Qt::Uninitialized);
    qsizetype done = 0;
    for (;;) {
        if (done == bytes.size()) {
            if (bytes.size() > maxBytes) {
                return std::unexpected(EFBIG);
            }
            bytes.resize(std::min<qsizetype>(bytes.size() * 2, maxBytes + 1));
        }
        const ssize_t got = ::read(fd, bytes.data() + done, size_t(bytes.size() - done));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            return std::unexpected(errno);
        }
        if (got == 0) {
            break;
        }
        done += got;
    }
    bytes.truncate(done);
    return bytes;
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
