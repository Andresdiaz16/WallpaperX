// SPDX-License-Identifier: GPL-2.0-or-later

// What tells main.qml to ask the daemon again. Its own file for WallpaperSurface.qml's reason:
// main.qml must never import com.wallpaperx, and a module that fails to load here is a Loader error
// there rather than a plugin that does not load at all.
import com.wallpaperx

WallpaperXConfigWatch {}
