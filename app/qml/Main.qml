// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import WallpaperX

ApplicationWindow {
    id: window

    required property KScreenClient kscreen
    required property LibraryManager library
    required property Importer importer
    required property DaemonClient daemon
    required property ApplyController applier

    width: 1280
    height: 800
    visible: true
    title: qsTr("WallpaperX")

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            text: qsTr("Choose a monitor")
            font.pointSize: 16
        }
        MonitorPicker {
            id: picker
            Layout.fillWidth: true
            // What the desktop's shape asks for, capped so the library below keeps its room.
            Layout.preferredHeight: Math.max(220, Math.min(picker.implicitHeight, window.height * 0.45))
            kscreen: window.kscreen
        }
        Label {
            textFormat: Text.PlainText
            text: window.kscreen.selectedOutput === ""
                ? qsTr("No monitor selected")
                : picker.fill(qsTr("Selected: %1 (%2)"),
                              [window.kscreen.selectedMonitor.displayName, window.kscreen.selectedOutput])
        }
        ScreenSettings {
            Layout.fillWidth: true
            visible: window.kscreen.selectedOutput !== ""
            kscreen: window.kscreen
            daemon: window.daemon
            applier: window.applier
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: window.library.selectedPath === "" ? 0 : 1

            LibraryView {
                library: window.library
                importer: window.importer
            }
            WallpaperDetail {
                library: window.library
                applier: window.applier
            }
        }
    }
}
