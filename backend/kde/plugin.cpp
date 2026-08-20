// SPDX-License-Identifier: GPL-2.0-or-later

// Makes WallpaperXItem reachable from QML as `WallpaperXRenderer`.
//
// A QML extension plugin rather than a declarative type registration, because this .so is loaded
// out of the wallpaper package's own lib directory by Plasma's QML engine, not linked into
// anything at build time. The module name here, the qmldir beside it, and the import line in
// main.qml are three spellings of one string; kpackage_metadata_test compares them so a rename
// cannot break the desktop silently.

#include "wallpaperx_item.h"

#include <QQmlEngine>
#include <QQmlExtensionPlugin>

namespace {

// The QML module this plugin provides. Matches `module com.wallpaperx` in qmldir.
constexpr const char* kExpectedUri = "com.wallpaperx";

}  // namespace

class WallpaperXQmlPlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char* uri) override {
        // The engine passes the uri it resolved the plugin under. Asserted rather than trusted:
        // registering under a different one produces a type that exists but that no import can
        // reach, which surfaces as a blank desktop with nothing logged.
        Q_ASSERT(QString::fromUtf8(uri) == QString::fromUtf8(kExpectedUri));

        qmlRegisterType<wallpaperx::WallpaperXItem>(uri, 1, 0, "WallpaperXRenderer");
    }
};

#include "plugin.moc"
