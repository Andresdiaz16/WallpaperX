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

    // The executable runner, rather than a desktop-file launch: the app has no desktop entry
    // yet -- that arrives with packaging -- and this reports an exit code, which is what tells
    // the button whether to say anything.
    Plasma5Support.DataSource {
        id: launcher

        engine: "executable"
        connectedSources: []

        onNewData: function (source, data) {
            disconnectSource(source);
            const code = data["exit code"];
            // A launcher that reports success for a command that was never found is worse than
            // one that reports nothing: 127 is the shell's own "not found".
            if (code !== 0) {
                root.launchError = (code === 127)
                    ? qsTr("The WallpaperX app is not installed.")
                    : qsTr("The WallpaperX app could not be started (exit %1).").arg(code);
            }
        }

        function launch() {
            root.launchError = "";
            connectSource("wallpaperx");
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
