// SPDX-License-Identifier: GPL-2.0-or-later

// HARNESS ONLY at this point. The six-state checks S10.10 asks for are not here yet; this
// file establishes that the plugin's real main.qml can be loaded and driven headless, which
// is what was missing. Written deliberately as the smallest thing that would go red if the
// wiring broke, so the harness itself is not taken on trust.

import QtQuick
import QtTest

TestCase {
    id: suite

    name: "WallpaperStates"

    // The plugin's own file, not a copy of its logic: a copy would drift, and the states are
    // derived by a binding that only exists there.
    Component {
        id: plugin
        Loader {
            source: "../../kde/kpackage/contents/ui/main.qml"
        }
    }

    function loadPlugin(connector) {
        const host = plugin.createObject(suite);
        verify(host !== null, "the plugin component was created: " + plugin.errorString());
        verify(host.status !== Loader.Error, "main.qml loaded: " + host.sourceComponent);
        const item = host.item;
        verify(item !== null, "main.qml produced an item");
        // The seam: no headless Qt platform reports a screen name, so every state below the
        // first would otherwise be unreachable.
        item.connector = connector;
        return item;
    }

    // GOAL: the harness can load the plugin's real main.qml headless and set the connector it
    //       keys every state off. Stated so it can fail: if WallpaperItem needed a Plasma shell,
    //       or the offscreen platform refused, this goes red rather than silently skipping.
    // WHY-THIS-SHAPE: a Loader rather than a direct instantiation, because main.qml's root type
    //       comes from a Plasma import and a failed import inside a Component is reported as a
    //       load error on the Loader rather than as a thrown QML error.
    // CONTROL: not yet run -- this file is harness scaffolding, and the checks that establish
    //       S10.10 are to be authored with their own controls. Do not read this block as a
    //       claim that anything was observed red.
    function test_harness_loads_the_plugin() {
        const item = loadPlugin("DP-3");
        compare(item.connector, "DP-3", "the connector seam takes the value a test sets");
        verify(typeof item.describe === "function", "the plugin exposes the message it renders");
        verify(item.screenState !== undefined, "the plugin exposes the state it derives");
    }
}
