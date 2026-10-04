// SPDX-License-Identifier: GPL-2.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import WallpaperX

// Settings of the selected screen, kept by the daemon whatever wallpaper it shows.
ColumnLayout {
    id: root

    required property KScreenClient kscreen
    required property DaemonClient daemon
    required property ApplyController applier

    readonly property string output: kscreen.selectedOutput
    // A screen the daemon has no setting for plays; "" is a mode a newer WallpaperX set.
    readonly property string shownMode: {
        const mode = root.applier.pendingPauseMode !== "" ? root.applier.pendingPauseMode
                                                          : root.daemon.pauseModes[root.output];
        return mode === undefined ? "play" : mode;
    }

    spacing: 4

    RowLayout {
        Layout.fillWidth: true

        Label {
            textFormat: Text.PlainText
            text: qsTr("Pause video on this screen:")
        }
        ComboBox {
            id: pause

            Layout.preferredWidth: 320
            textRole: "text"
            valueRole: "mode"
            model: [
                { mode: "play", text: qsTr("Never") },
                { mode: "always_paused", text: qsTr("Always") },
                { mode: "paused_while_focused", text: qsTr("While a window has the focus") },
                { mode: "paused_while_maximized", text: qsTr("While a window is maximized or fullscreen") }
            ]
            enabled: root.daemon.state === DaemonClient.Ready && root.applier.pauseState !== ApplyController.Working
            currentIndex: pause.indexOfValue(root.shownMode)
            displayText: pause.currentIndex < 0 ? qsTr("Set by a newer WallpaperX") : pause.currentText
            onActivated: {
                root.applier.setPauseMode(pause.currentValue);
                // The choice shows only once the daemon has it, through the binding.
                pause.currentIndex = Qt.binding(() => pause.indexOfValue(root.shownMode));
            }
        }
        BusyIndicator {
            Layout.preferredHeight: pause.implicitHeight
            Layout.preferredWidth: pause.implicitHeight
            visible: root.applier.pauseState === ApplyController.Working
            running: visible
        }
        Item {
            Layout.fillWidth: true
        }
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        visible: text !== ""
        text: {
            if (root.daemon.state === DaemonClient.Unavailable)
                return qsTr("The pause setting cannot be shown. %1").arg(root.daemon.unavailableReason);
            if (root.applier.pauseState === ApplyController.Failed)
                return root.applier.pauseMessage;
            return "";
        }
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        visible: text !== ""
        text: root.daemon.persistenceProblem
    }
}
