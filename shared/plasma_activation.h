// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The script that puts the WallpaperX wallpaper on a screen's Plasma desktops, through
// org.kde.PlasmaShell.evaluateScript, and how to read Plasma's answer to it: one definition for the
// app and wpxctl. Header-only and standard library only, since neither links the other.

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace wallpaperx {

// Where evaluateScript is sent, and how long to wait: switching every activity's desktop can take a while.
inline constexpr const char* kPlasmaShellService = "org.kde.plasmashell";
inline constexpr const char* kPlasmaShellPath = "/PlasmaShell";
inline constexpr const char* kPlasmaShellInterface = "org.kde.PlasmaShell";
inline constexpr int kPlasmaScriptTimeoutMs = 25000;

// The plugin id Plasma knows the WallpaperX wallpaper by (backend/kde/kpackage/metadata.json).
inline constexpr std::string_view kWallpaperPluginId = "com.wallpaperx.wallpaper";
// knownWallpaperPlugins() lists plugins by display name only: metadata.json's KPlugin.Name.
inline constexpr std::string_view kWallpaperPluginName = "WallpaperX";

// Whether `text` can stand between the quotes of a JavaScript string literal as it is.
constexpr bool isPlainScriptText(std::string_view text) {
    for (const char c : text) {
        if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\') return false;
    }
    return true;
}
static_assert(isPlainScriptText(kWallpaperPluginId) && isPlainScriptText(kWallpaperPluginName),
              "the plugin's name and id are written into the script unescaped");

// The script throws its own sentence between these, and prints kActivationSwitched and a count.
inline constexpr std::string_view kActivationFailOpen = "[wallpaperx]";
inline constexpr std::string_view kActivationFailClose = "[/wallpaperx]";
inline constexpr std::string_view kActivationSwitched = "wallpaperx-switched=";

// `text` as a JavaScript string literal: everything outside printable ASCII escaped by UTF-16 code
// unit, so no connector name can end the literal or the line.
inline std::string plasmaScriptString(std::u16string_view text) {
    std::string literal = "\"";
    for (const char16_t c : text) {
        if (c == u'"' || c == u'\\') {
            literal += '\\';
            literal += static_cast<char>(c);
        } else if (c >= 0x20 && c < 0x7f) {
            literal += static_cast<char>(c);
        } else {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
            literal += escaped;
        }
    }
    return literal + '"';
}

// What evaluateScript runs to put the WallpaperX wallpaper on every desktop Plasma keeps for
// `connector`, on every activity. It switches the plugin on and writes nothing else: what to show is
// the daemon's to say. It throws, failing the call, when Plasma shows no desktop of its own there or
// the plugin is not installed, and prints how many desktops it switched.
//
// screenForConnector is the shell's own ScreenPool::idForName, the index desktopForScreen and a
// desktop's `screen` use: KWin's output order less mirrored and fake outputs, so it cannot be rebuilt
// from KScreen's list. desktopForScreen is not used: it returns current-activity desktops only, and
// for a screen without one it wraps a null containment, which crashes plasmashell 6.7. A desktop of
// another activity has `screen` -1 and is matched by its saved lastScreen, the key Plasma itself
// matches it by when that activity is shown. The plugin change is applied when the script's wrappers
// are destroyed, after the script and before the reply, so no reloadConfig().
inline std::string plasmaActivationScript(std::u16string_view connector) {
    const std::string open(kActivationFailOpen);
    const std::string close(kActivationFailClose);
    return "(function () {\n"
           "    var connector = " + plasmaScriptString(connector) + ";\n"
           "    function fail(why) { throw new Error(\"" + open + "\" + why + \"" + close + "\"); }\n"
           "    if (!(\"" + std::string(kWallpaperPluginName) + "\" in knownWallpaperPlugins()))\n"
           "        fail(\"The WallpaperX wallpaper plugin is not installed for Plasma\");\n"
           "    if (typeof screenForConnector !== \"function\")\n"
           "        fail(\"This Plasma cannot find a screen by its connector name\");\n"
           "    var screen = screenForConnector(connector);\n"
           "    if (typeof screen !== \"number\" || screen < 0)\n"
           "        fail(\"Plasma shows no desktop of its own on \" + connector);\n"
           "    var all = desktops();\n"
           "    var switched = 0;\n"
           "    for (var i = 0; i < all.length; ++i) {\n"
           "        var d = all[i];\n"
           "        var on = d.screen >= 0 ? d.screen : parseInt(d.readConfig(\"lastScreen\", -1), 10);\n"
           "        if (on !== screen)\n"
           "            continue;\n"
           "        d.wallpaperPlugin = \"" + std::string(kWallpaperPluginId) + "\";\n"
           "        ++switched;\n"
           "    }\n"
           "    if (switched === 0)\n"
           "        fail(\"Plasma has no desktop on \" + connector + \" yet\");\n"
           "    print(\"" + std::string(kActivationSwitched) + "\" + switched);\n"
           "})();\n";
}

// The sentence the script threw, out of the error Plasma answered with: Plasma wraps it as
// "Error: <it> at line N" plus a backtrace, translated, and the markers find it in any language.
// Nothing when the error carries no such sentence.
inline std::optional<std::string> plasmaActivationFailure(std::string_view message) {
    const std::size_t open = message.find(kActivationFailOpen);
    const std::size_t close = message.find(kActivationFailClose);
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open) return std::nullopt;
    const std::size_t from = open + kActivationFailOpen.size();
    return std::string(message.substr(from, close - from));
}

// How many desktops the script's printed answer says it switched: the first kActivationSwitched
// followed by a digit, read to at most six digits. 0 when the answer says nothing of the kind, which
// is a failure: a shell that runs nothing is never reported as done.
inline int plasmaActivationSwitched(std::string_view printed) {
    for (std::size_t at = printed.find(kActivationSwitched); at != std::string_view::npos;
         at = printed.find(kActivationSwitched, at + 1)) {
        int count = 0;
        std::size_t digits = 0;
        for (std::size_t i = at + kActivationSwitched.size(); i < printed.size() && digits < 6; ++i, ++digits) {
            if (printed[i] < '0' || printed[i] > '9') break;
            count = count * 10 + (printed[i] - '0');
        }
        if (digits > 0) return count;
    }
    return 0;
}

}  // namespace wallpaperx
