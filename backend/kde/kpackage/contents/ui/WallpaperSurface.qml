// SPDX-License-Identifier: GPL-2.0-or-later

// The drawing half of the plugin, kept in its own file so that main.qml never imports
// com.wallpaperx directly.
//
// That separation is the point. A failed import at the top of main.qml takes the whole file
// down, and a wallpaper plugin that fails to load is a black desktop with nothing logged -- the
// exact failure its own header warns about. Loaded through a Loader instead, a missing or
// unloadable module is a Loader error: main.qml keeps running and keeps showing the message
// states, which is a desktop that explains itself rather than one that is simply black.
//
// So this file is allowed to fail. main.qml is not.
import QtQuick
import com.wallpaperx

WallpaperXRenderer {
    id: renderer

    // Set by the Loader through its own binding; -1 until the daemon has named the screen, and
    // the item draws nothing until it is a real id.
    monitorId: -1

    // Loaded for the reason this file is: if Plasma's task manager module fails, the wallpaper
    // still draws and the screen is reported as having no window.
    Loader {
        id: windows

        objectName: "wallpaperxWindows"
        Component.onCompleted: windows.setSource("ScreenWindows.qml", { "renderer": renderer })
    }
}
