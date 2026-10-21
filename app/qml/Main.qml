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

    // Sound on every screen, as being set or else as the daemon has it.
    readonly property bool shownSound: window.applier.pendingSound !== undefined ? window.applier.pendingSound
                                                                                  : window.daemon.sound
    // 0 is a rate a newer WallpaperX set.
    readonly property int shownFrameRate: window.applier.pendingFrameRate !== undefined ? window.applier.pendingFrameRate
                                                                                        : window.daemon.frameRate
    readonly property bool shownKeepHigher: window.applier.pendingKeepHigher !== undefined
        ? window.applier.pendingKeepHigher : window.daemon.keepHigherFrameRate

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            Label {
                textFormat: Text.PlainText
                font.bold: true
                text: qsTr("General")
            }
            Switch {
                id: sound

                text: qsTr("Sound on every screen")
                enabled: window.daemon.state === DaemonClient.Ready && window.applier.soundState !== ApplyController.Working
                checked: window.shownSound
                onToggled: {
                    window.applier.setSound(sound.checked);
                    sound.checked = Qt.binding(() => window.shownSound);
                }
            }
            BusyIndicator {
                Layout.preferredHeight: sound.implicitHeight
                Layout.preferredWidth: sound.implicitHeight
                visible: window.applier.soundState === ApplyController.Working
                running: visible
            }
            Label {
                textFormat: Text.PlainText
                text: qsTr("Frame rate on every screen:")
            }
            ComboBox {
                id: frameRate

                Layout.preferredWidth: 220
                textRole: "text"
                valueRole: "rate"
                model: [
                    { rate: 30, text: qsTr("30 frames a second") },
                    { rate: 60, text: qsTr("60 frames a second") },
                    { rate: 120, text: qsTr("120 frames a second") }
                ]
                enabled: window.daemon.state === DaemonClient.Ready && window.applier.frameRateState !== ApplyController.Working
                currentIndex: frameRate.indexOfValue(window.shownFrameRate)
                displayText: frameRate.currentIndex < 0 ? qsTr("Set by a newer WallpaperX") : frameRate.currentText
                onActivated: {
                    window.applier.setFrameRate(frameRate.currentValue);
                    frameRate.currentIndex = Qt.binding(() => frameRate.indexOfValue(window.shownFrameRate));
                }
            }
            BusyIndicator {
                Layout.preferredHeight: frameRate.implicitHeight
                Layout.preferredWidth: frameRate.implicitHeight
                visible: window.applier.frameRateState === ApplyController.Working
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
            text: window.applier.soundState === ApplyController.Failed ? window.applier.soundMessage : ""
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            visible: text !== ""
            text: window.applier.frameRateState === ApplyController.Failed ? window.applier.frameRateMessage : ""
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            visible: text !== ""
            text: window.applier.frameRateNotice
        }
        RowLayout {
            Layout.fillWidth: true
            // Kept while the choice matters, so a rate kept above the screens can be given up again.
            visible: window.applier.frameRateAboveScreens

            CheckBox {
                id: keepHigher

                text: qsTr("Keep a higher rate anyway")
                enabled: window.daemon.state === DaemonClient.Ready && window.applier.keepHigherState !== ApplyController.Working
                checked: window.shownKeepHigher
                onToggled: {
                    window.applier.setKeepHigherFrameRate(keepHigher.checked);
                    keepHigher.checked = Qt.binding(() => window.shownKeepHigher);
                }
            }
            BusyIndicator {
                Layout.preferredHeight: keepHigher.implicitHeight
                Layout.preferredWidth: keepHigher.implicitHeight
                visible: window.applier.keepHigherState === ApplyController.Working
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
            text: window.applier.keepHigherState === ApplyController.Failed ? window.applier.keepHigherMessage : ""
        }

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
