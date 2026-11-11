// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Wallpaper Engine's Workshop items, found through Steam's own list of its library folders and read
// where Steam keeps them. Nothing here writes to a Steam folder.

#include "wpx_package.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <expected>
#include <optional>

namespace wallpaperx {

// The folders this user's Steam may live in, existing or not, each once: $XDG_DATA_HOME/Steam,
// ~/.local/share/Steam, ~/.steam/steam and ~/.steam/root, links resolved. Flatpak's Steam is not
// among them.
QStringList defaultSteamRoots();

// <root>/steamapps, which holds libraryfolders.vdf.
QString steamAppsFolder(const QString& root);
// <root>/steamapps/libraryfolders.vdf.
QString libraryListOf(const QString& root);
bool isLibraryListName(QByteArrayView name);

// The library folders `root`'s libraryfolders.vdf lists, `root` first and as given; only `root` when
// it has no list. Each once, links resolved before they are compared, those listed given resolved
// when they exist. Reads the disk, so never on the GUI thread. The error is a sentence naming the
// list: it is there and cannot be read.
std::expected<QStringList, QString> steamLibrariesOf(const QString& root);

// <library>/steamapps/workshop/content/431960, where Steam keeps Wallpaper Engine's Workshop items.
QString workshopItemsOf(const QString& library);
// <library>/steamapps/common/wallpaper_engine, Wallpaper Engine itself when it is installed there.
QString wallpaperEngineInstallOf(const QString& library);

// The Workshop item folder `name` in `parent` (a descriptor, or AT_FDCWD), shown as `path`. A video
// is usable as a WallpaperX video, its sound probed unless `known` has the answer, a malformed setting
// only logged, and not logged again when `lastRead` is the stamp of the project.json it was logged
// for; any other kind is listed with its title and preview, and why it cannot be used.
Wallpaper readWorkshopItem(int parent, const QByteArray& name, const QString& path, const KnownAudio& known = {},
                           std::optional<FileStamp> lastRead = std::nullopt);
struct WorkshopScan {
    struct Unread {
        QString folder;
        QString problem;
    };
    // Not sorted.
    QList<Wallpaper> items;
    // Each folder that could not be listed, which adds no item, with a sentence saying why.
    QList<Unread> unread;
};
// Every item in each of `itemFolders`; a setting problem `previous` already listed it usable with is
// not logged again.
WorkshopScan scanWorkshop(const QStringList& itemFolders, const QList<Wallpaper>& previous = {});

} // namespace wallpaperx
