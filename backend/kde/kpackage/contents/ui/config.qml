// SPDX-License-Identifier: GPL-2.0-or-later

// The pane Plasma shows under Wallpaper settings for a WallpaperX screen. It deliberately
// offers no wallpaper controls: the daemon is the single source of truth and main.qml polls it,
// so a picker here would be a second writer with nothing arbitrating between them -- a screen
// could show one thing while Plasma believed another. What it does instead is say where the
// configuration lives, and open it.
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasma5support as Plasma5Support

ColumnLayout {
    id: root

    // Plasma's settings dialog assigns this to align a pane's labels with its neighbours. It is
    // read by the dialog, not here, and every shipped wallpaper declares it.
    property var parentLayout

    // Set when a launch has been attempted and failed. A button that silently does nothing
    // leaves the user believing the app is not installed, which is the one thing the pane
    // exists to rule out.
    property string launchError: ""

    // How long a launch may take before it is called failed. kstart waits for the application's
    // window to appear, and on an id it cannot resolve it waits forever -- measured, not
    // assumed: `kstart --application` on an unknown entry was still running at 60 s. Unbounded,
    // the most likely failure of all (the app is not installed) would leave the button doing
    // nothing with no exit code ever arriving, which is the one outcome this pane exists to
    // prevent. Generous, because a cold start on a slow disk is not a failure.
    readonly property int launchTimeoutSeconds: 20

    // What a given exit code means, as a function rather than inline in the handler: the
    // handler can only be reached by running a real process, and this is the part worth
    // grading.
    function launchFailureMessage(code: int): string {
        if (code === 0) {
            return "";
        }
        // 127 is the shell's own "not found", and the command is the timeout/kstart pair, so
        // that code says the launcher is missing rather than the app.
        if (code === 127) {
            return qsTr("The desktop launcher (kstart) is missing, so the app could not be opened.");
        }
        // 124 is timeout(1) reporting it ended a command that had not finished -- here, kstart
        // still waiting for a window that never appeared.
        if (code === 124) {
            return qsTr("The WallpaperX app did not start. It may not be installed.");
        }
        return qsTr("The WallpaperX app could not be started.");
    }

    spacing: Kirigami.Units.largeSpacing

    // kstart, not the binary directly: run as a plain command the app becomes a child of
    // plasmashell, and Plasma restarts its shell routinely -- the app would go with it. kstart
    // launches the desktop entry into its own systemd scope, detached, with startup
    // notification. The runner is still used rather than an openUrlExternally because it
    // reports an exit code, which is the only reason this pane can say a launch failed.
    Plasma5Support.DataSource {
        id: launcher

        engine: "executable"
        connectedSources: []

        onNewData: function (source, data) {
            disconnectSource(source);
            root.launchError = root.launchFailureMessage(data["exit code"]);
        }

        // The id, without the .desktop suffix, is what kstart resolves through the desktop
        // environment's own lookup -- so this finds the app wherever it was installed rather
        // than assuming a prefix.
        readonly property string desktopId: "com.wallpaperx.app"

        function launch() {
            root.launchError = "";
            // Bounded by timeout(1): see launchTimeoutSeconds. Without it a kstart that never
            // returns leaves this source connected and the pane silent forever.
            connectSource("timeout " + root.launchTimeoutSeconds
                          + " kstart --application " + launcher.desktopId);
        }
    }

    Kirigami.InlineMessage {
        Layout.fillWidth: true
        visible: root.launchError !== ""
        type: Kirigami.MessageType.Error
        // Plain, not the default: this carries a string composed around an exit code, and
        // nothing downstream should be free to render markup in a settings dialog.
        text: root.launchError
    }

    // Colour comes from the theme rather than being written here. The pane shipped with
    // color: "#ffffff", which is white text on the light Breeze dialog.
    Kirigami.Heading {
        // Named so a check can find it by identity rather than by position among its siblings,
        // which would make adding a row above it a test failure.
        objectName: "wallpaperxConfigHeading"
        Layout.fillWidth: true
        level: 3
        wrapMode: Text.Wrap
        text: qsTr("Wallpapers for this screen are managed in the WallpaperX app.")
    }

    Kirigami.SelectableLabel {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("Choose a wallpaper there and it appears on this screen. There is nothing to "
                    + "set here, so that Plasma and the app cannot disagree about what a screen shows.")
    }

    // The only action, and it writes nothing: it opens the app that does the writing.
    QQC2.Button {
        Layout.alignment: Qt.AlignLeft
        icon.name: "preferences-desktop-wallpaper"
        text: qsTr("Open WallpaperX…")
        onClicked: launcher.launch()
    }

    Item { Layout.fillHeight: true }
}
