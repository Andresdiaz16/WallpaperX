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
            const code = data["exit code"];
            // 127 is the shell's own "not found", and the command here is kstart -- so that
            // code says the launcher is missing, not the app. Anything else non-zero is kstart
            // reporting it could not start the entry, which is the case that means the app.
            if (code !== 0) {
                root.launchError = (code === 127)
                    ? qsTr("The desktop launcher (kstart) is missing, so the app could not be opened.")
                    : qsTr("The WallpaperX app could not be started. It may not be installed.");
            }
        }

        // The id, without the .desktop suffix, is what kstart resolves through the desktop
        // environment's own lookup -- so this finds the app wherever it was installed rather
        // than assuming a prefix.
        readonly property string desktopId: "com.wallpaperx.app"

        function launch() {
            root.launchError = "";
            connectSource("kstart --application " + launcher.desktopId);
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
