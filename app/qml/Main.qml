// SPDX-License-Identifier: GPL-2.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import WallpaperX

ApplicationWindow {
    id: window

    required property KScreenClient kscreen

    width: 1280
    height: 720
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
            Layout.fillWidth: true
            Layout.fillHeight: true
            kscreen: window.kscreen
        }
        Label {
            textFormat: Text.PlainText
            text: window.kscreen.selectedOutput === ""
                ? qsTr("No monitor selected")
                : qsTr("Selected: %1 (%2)").arg(window.kscreen.selectedMonitor.displayName)
                                          .arg(window.kscreen.selectedOutput)
        }
    }
}
