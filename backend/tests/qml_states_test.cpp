// SPDX-License-Identifier: GPL-2.0-or-later

// Entry point for the Qt Quick Test cases in tests/qml/, which drive the Plasma wallpaper
// plugin's main.qml through the states it can show (S10.10). The checks are QML because the
// thing under test is QML: the states are derived in a binding and rendered by describe(),
// and nothing in C++ can observe either.
//
// Runs headless under the offscreen platform, set by the ctest entry. No Plasma shell is
// needed -- WallpaperItem instantiates on its own -- but no headless platform reports a screen
// name, which is why main.qml takes its connector through an assignable property.

#include <QtQuickTest/quicktest.h>

QUICK_TEST_MAIN(wallpaperx_qml_states)
