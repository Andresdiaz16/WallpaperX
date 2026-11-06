// SPDX-License-Identifier: GPL-2.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import WallpaperX

// The first run's progress, and any run's refusal or failure.
ApplicationWindow {
    id: window

    required property FirstRunSetup setup
    // The first run: nothing else is open, so leaving here quits.
    required property bool blocking

    width: 560
    height: 420
    visible: true
    title: qsTr("WallpaperX setup")

    // A close is never refused: Qt's quit, as on SIGTERM at logout, asks every window first, and a
    // compositor's close looks the same as the user's. A run under way carries on behind the closed
    // window (FirstRunSetup).

    readonly property var steps: [
        { step: FirstRunSetup.Library, text: qsTr("Create the wallpaper library") },
        { step: FirstRunSetup.Daemon, text: qsTr("Copy the WallpaperX service") },
        { step: FirstRunSetup.Service, text: qsTr("Install and start the service") },
        { step: FirstRunSetup.OldCopy, text: qsTr("Remove the previous copy") },
        { step: FirstRunSetup.Plugin, text: qsTr("Install the Plasma wallpaper plugin") },
        { step: FirstRunSetup.Stamp, text: qsTr("Finish") }
    ]

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            font.pointSize: 16
            text: window.setup.state === FirstRunSetup.Refused ? qsTr("WallpaperX cannot run here")
                : window.setup.state === FirstRunSetup.Failed ? qsTr("Setup did not finish")
                : window.setup.state === FirstRunSetup.Done ? qsTr("WallpaperX is set up")
                : qsTr("Setting up WallpaperX")
        }
        Repeater {
            model: window.setup.state === FirstRunSetup.Refused ? [] : window.steps

            delegate: RowLayout {
                id: row

                required property var modelData
                readonly property bool current: window.setup.step === row.modelData.step
                readonly property bool past: window.setup.state === FirstRunSetup.Done || window.setup.step > row.modelData.step

                Layout.fillWidth: true
                BusyIndicator {
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    running: row.current && window.setup.state === FirstRunSetup.Running
                    opacity: running ? 1 : 0
                }
                Label {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    opacity: row.past || row.current ? 1 : 0.5
                    font.bold: row.current && window.setup.state === FirstRunSetup.Failed
                    text: row.past ? qsTr("%1 (done)").arg(row.modelData.text) : row.modelData.text
                }
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            verticalAlignment: Text.AlignTop
            text: window.setup.message
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight

            Button {
                visible: window.setup.state === FirstRunSetup.Failed
                text: qsTr("Try again")
                onClicked: window.setup.start()
            }
            Button {
                visible: window.setup.state !== FirstRunSetup.Running
                text: window.blocking ? qsTr("Quit") : qsTr("Close")
                onClicked: window.close()
            }
        }
    }
}
