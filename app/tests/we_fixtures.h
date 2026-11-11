// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Fake Steam folders holding fake Wallpaper Engine Workshop items, written in a test's own scratch
// folder. Shaped after docs/phase4/spec_project_json.md and Steam's libraryfolders.vdf; never a copy
// of anyone's real items.

#include "wpx_fixtures.h"

#include <QByteArray>
#include <QDir>
#include <QString>
#include <QStringList>

namespace wallpaperx::apptest {

inline QString steamItemFolder(const QString& library) {
    return library + QStringLiteral("/steamapps/workshop/content/431960");
}

inline QString steamInstallFolder(const QString& library) {
    return library + QStringLiteral("/steamapps/common/wallpaper_engine");
}

// libraryfolders.vdf as Steam writes it, tab-separated and quoted, one block per library.
inline QByteArray libraryListBytes(const QStringList& libraries) {
    QByteArray out("\"libraryfolders\"\n{\n");
    for (qsizetype i = 0; i < libraries.size(); ++i) {
        out += "\t\"" + QByteArray::number(i) + "\"\n\t{\n";
        out += "\t\t\"path\"\t\t\"" + QFile::encodeName(libraries[i]) + "\"\n";
        out += "\t\t\"label\"\t\t\"\"\n\t\t\"contentid\"\t\t\"123\"\n";
        out += "\t\t\"apps\"\n\t\t{\n\t\t\t\"431960\"\t\t\"1000\"\n\t\t}\n\t}\n";
    }
    out += "}\n";
    return out;
}

// <root>/steamapps/libraryfolders.vdf listing `libraries`. Empty string on success.
[[nodiscard]] inline QString writeSteamRoot(const QString& root, const QStringList& libraries) {
    const QString apps = root + QStringLiteral("/steamapps");
    if (!QDir().mkpath(apps) || !writeBytes(apps + QStringLiteral("/libraryfolders.vdf"), libraryListBytes(libraries))) {
        return QStringLiteral("%1 could not be written").arg(apps);
    }
    return {};
}

// A project.json of `type` naming `file`, with its accent colour as every item has it.
inline QByteArray projectBytes(const char* type, const QString& title, const QString& file,
                               const QString& preview = QStringLiteral("preview.jpg")) {
    OrderedJson json;
    json["file"] = file.toStdString();
    json["general"]["properties"]["schemecolor"] = {
        {"order", 0}, {"text", "ui_browse_properties_scheme_color"}, {"type", "color"}, {"value", "0.1 0.2 0.3"}};
    if (!preview.isEmpty()) {
        json["preview"] = preview.toStdString();
    }
    json["tags"] = {"Landscape"};
    json["title"] = title.toStdString();
    json["type"] = type;
    json["workshopid"] = 1234;
    return QByteArray::fromStdString(json.dump(1, '\t'));
}

// Item `id` in `library`'s Workshop folder, holding `entries`. Empty string on success.
[[nodiscard]] inline QString writeItem(const QString& library, const QString& id, const Entries& entries) {
    return writeFolder(steamItemFolder(library) + u'/' + id, entries);
}

// A video item: project.json, its preview and a short video with sound named `file`.
inline Entries videoItem(const QString& title, const QString& file = QStringLiteral("clip [1] it's.flv")) {
    return {{QStringLiteral("project.json"), projectBytes("video", title, file)},
            {QStringLiteral("preview.jpg"), pngBytes(16, 9)},
            {file, soundVideo()}};
}

// A scene item: project.json and its preview; its package is not needed to list it.
inline Entries sceneItem(const QString& title) {
    return {{QStringLiteral("project.json"), projectBytes("Scene", title, QStringLiteral("scene.json"))},
            {QStringLiteral("preview.jpg"), pngBytes(16, 9)},
            {QStringLiteral("scene.pkg"), QByteArray("PKGV0001")}};
}

} // namespace wallpaperx::apptest
