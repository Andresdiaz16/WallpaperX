// SPDX-License-Identifier: GPL-2.0-or-later

#include "wallpaper_engine.h"

#include "current_exception.h"
#include "steam_libraries.h"
#include "wallpaper_manifest.h"
#include "we_project.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>

#include <fcntl.h>

namespace wallpaperx {

namespace {

Wallpaper brokenItem(const QString& path, QString problem, std::optional<FileStamp> stamp = std::nullopt)
{
    Wallpaper item = brokenFolder(path, std::move(problem), stamp);
    item.source = Wallpaper::Source::WallpaperEngine;
    return item;
}

QString whyNotShown(const we::Project& project)
{
    switch (project.kind) {
    case we::Project::Kind::Scene:
        return QStringLiteral("Wallpaper Engine scenes are not supported yet.");
    case we::Project::Kind::Web:
        return QStringLiteral("Wallpaper Engine web wallpapers are not supported.");
    case we::Project::Kind::Video:
        return {};
    case we::Project::Kind::Other:
        break;
    }
    // Quoted only when it cannot carry anything but plain letters into the sentence.
    const std::string& type = project.typeName;
    if (type.size() <= 32 && std::ranges::all_of(type, [](char c) { return c > 0x20 && c < 0x7f; })) {
        return QStringLiteral("It is a Wallpaper Engine \"%1\" wallpaper, which WallpaperX does not know.")
            .arg(QString::fromStdString(type));
    }
    return QStringLiteral("It is a kind of Wallpaper Engine wallpaper WallpaperX does not know.");
}

// Links resolved when it exists, so one folder written two ways compares equal.
QString resolvedPath(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() ? info.canonicalFilePath() : QDir::cleanPath(path);
}

} // namespace

QStringList defaultSteamRoots()
{
    const QString home = QDir::homePath();
    const QStringList candidates{
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Steam"),
        home + QStringLiteral("/.local/share/Steam"),
        home + QStringLiteral("/.steam/steam"),
        home + QStringLiteral("/.steam/root"),
    };
    QStringList roots;
    for (const QString& candidate : candidates) {
        if (const QString resolved = resolvedPath(candidate); !roots.contains(resolved)) {
            roots.append(resolved);
        }
    }
    return roots;
}

QString steamAppsFolder(const QString& root)
{
    return root + QStringLiteral("/steamapps");
}

QString libraryListOf(const QString& root)
{
    return steamAppsFolder(root) + u'/' + QString::fromLatin1(steam::kLibraryListName);
}

bool isLibraryListName(QByteArrayView name)
{
    return name == steam::kLibraryListName;
}

std::expected<QStringList, QString> steamLibrariesOf(const QString& root)
{
    const QString list = libraryListOf(root);
    auto fd = openRegularFile(AT_FDCWD, encodeFileName(list).constData(), O_RDONLY);
    if (!fd) {
        if (fd.error() == ENOENT || fd.error() == ENOTDIR) {
            return QStringList{root};
        }
        if (fd.error() == kNotRegularFile) {
            return std::unexpected(QStringLiteral("%1 is not a file.").arg(list));
        }
        return std::unexpected(QStringLiteral("%1 cannot be opened: %2.").arg(list, qt_error_string(fd.error())));
    }
    const auto bytes = readToEnd(fd->get(), qint64(steam::kMaxLibraryListBytes));
    if (!bytes) {
        return std::unexpected(bytes.error() == EFBIG
                                   ? QStringLiteral("%1 is larger than %2 MiB.").arg(list).arg(steam::kMaxLibraryListBytes >> 20)
                                   : QStringLiteral("%1 cannot be read: %2.").arg(list, qt_error_string(bytes.error())));
    }
    const auto paths = steam::libraryFolders(std::string_view(bytes->constData(), std::size_t(bytes->size())));
    if (!paths) {
        return std::unexpected(QStringLiteral("%1 cannot be used: %2").arg(list, QString::fromStdString(paths.error())));
    }
    // The root as given, so its own library is one folder whether the list names it or not.
    const QString self = resolvedPath(root);
    QStringList libraries{root};
    for (const std::string& path : *paths) {
        const QString library = resolvedPath(decodeFileName(QByteArrayView(path.data(), qsizetype(path.size()))));
        if (library != self && !libraries.contains(library)) {
            libraries.append(library);
        }
    }
    return libraries;
}

QString workshopItemsOf(const QString& library)
{
    return library + QStringLiteral("/steamapps/workshop/content/") + QString::fromLatin1(we::kAppId);
}

QString wallpaperEngineInstallOf(const QString& library)
{
    return library + QStringLiteral("/steamapps/common/wallpaper_engine");
}

Wallpaper readWorkshopItem(int parent, const QByteArray& name, const QString& path, const KnownAudio& known,
                           std::optional<FileStamp> lastRead)
{
    // The daemon takes the path as a D-Bus string.
    if (!manifest::isValidUtf8(encodeFileName(path).toStdString())) {
        return brokenItem(path, QStringLiteral("Its path is not valid UTF-8, so it cannot be set as a wallpaper."));
    }
    const UniqueFd dir(::openat(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!dir) {
        return brokenItem(path, QStringLiteral("It cannot be opened: %1.").arg(qt_error_string(errno)));
    }
    manifest::FolderContents contents(dir.get(), we::kProjectName);
    const auto file = contents.read(we::kProjectName, we::kMaxProjectBytes);
    if (!file) {
        return brokenItem(path, QString::fromStdString(file.error()));
    }
    const FileStamp stamp = stampFrom(file->st);
    const auto project = we::parseProject(file->bytes);
    if (!project) {
        return brokenItem(path, QString::fromStdString(project.error()), stamp);
    }

    manifest::Read read;
    read.manifest.type = "video";
    read.manifest.title = project->title;
    read.manifest.tags = project->tags;
    read.manifestStat = file->st;
    // A missing preview costs the item only its picture in the library.
    if (!project->preview.empty()) {
        if (const auto preview = contents.find(project->preview)) {
            read.manifest.previewName = project->preview;
            read.previewStat = preview->st;
        }
    }

    if (const QString problem = whyNotShown(*project); !problem.isEmpty()) {
        Wallpaper item = brokenItem(path, problem, stamp);
        item.title = QString::fromStdString(project->title);
        for (const std::string& tag : project->tags) {
            item.tags.append(QString::fromStdString(tag));
        }
        item.previewName = QString::fromStdString(read.manifest.previewName);
        if (!item.previewName.isEmpty()) {
            item.previewStamp = stampFrom(read.previewStat);
        }
        return item;
    }

    auto asset = contents.find(project->file);
    if (!asset) {
        return brokenItem(path, QString::fromStdString(asset.error()), stamp);
    }
    read.manifest.assetName = project->file;
    read.asset = std::move(*asset);
    // Its settings are all dropped: none is one WallpaperX has (spec_project_json.md: a video declares
    // only its accent colour). Sound gets WallpaperX's own mute and volume, as any video does.
    if (lastRead != stamp) {
        for (const std::string& problem : project->settingProblems) {
            qWarning("wallpaperx: ignored in %s: %s", encodeFileName(path).constData(), problem.c_str());
        }
    }
    const QString assetPath = path + u'/' + QString::fromStdString(project->file);
    const auto audio = manifest::settleSoundFor(read.manifest, [&] { return audioOf(known, assetPath, read); });
    if (!audio) {
        return brokenItem(path, QString::fromStdString(audio.error()), stamp);
    }
    Wallpaper item = usableFolder(path, read, *audio);
    item.source = Wallpaper::Source::WallpaperEngine;
    return item;
}

WorkshopScan scanWorkshop(const QStringList& itemFolders, const QList<Wallpaper>& previous)
{
    const KnownAudio known = knownAudio(previous);
    // A usable row read its settings, and logged their problems, from this version of project.json.
    QHash<QString, FileStamp> logged;
    for (const Wallpaper& item : previous) {
        if (item.fromWallpaperEngine() && item.usable()) {
            logged.insert(item.filePath, item.stamp);
        }
    }
    WorkshopScan scan;
    for (const QString& folder : itemFolders) {
        std::expected<QList<Wallpaper>, QString> found;
        try {
            found = readFolders(
                folder,
                [&known, &logged](int dirFd, const QByteArray& name, const QString& path) {
                    const auto last = logged.constFind(path);
                    return readWorkshopItem(dirFd, name, path, known,
                                            last == logged.cend() ? std::nullopt : std::optional(*last));
                },
                QStringLiteral("It is a link to a folder, which WallpaperX does not follow among Workshop items."));
        } catch (...) {
            found = std::unexpected(readingFailed(folder));
        }
        if (!found) {
            scan.unread.append({folder, QStringLiteral("Wallpaper Engine's items cannot be listed. %1").arg(found.error())});
            continue;
        }
        for (Wallpaper& item : *found) {
            item.source = Wallpaper::Source::WallpaperEngine;
        }
        scan.items.append(std::move(*found));
    }
    return scan;
}

} // namespace wallpaperx
