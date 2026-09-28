// SPDX-License-Identifier: GPL-2.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import WallpaperX

// The selected wallpaper: its preview, a control per property writing to the library, and Apply.
Item {
    id: root

    required property LibraryManager library

    readonly property wallpaper chosen: library.selectedWallpaper

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true

            Button {
                text: qsTr("Back")
                onClicked: root.library.clearSelection()
            }
            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.pointSize: 16
                text: root.chosen.displayTitle
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 3
                color: palette.base
                border.width: 1
                border.color: palette.mid

                Image {
                    id: preview
                    anchors.fill: parent
                    anchors.margins: 1
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(1920, 1080)
                    source: root.chosen.previewSource
                }
                Label {
                    anchors.centerIn: parent
                    visible: preview.status === Image.Error
                    textFormat: Text.PlainText
                    color: palette.placeholderText
                    text: qsTr("No preview")
                }
            }

            ColumnLayout {
                Layout.fillHeight: true
                Layout.fillWidth: true
                Layout.preferredWidth: 2
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    text: {
                        let parts = [root.chosen.type];
                        if (root.chosen.author !== "")
                            parts.push(qsTr("by %1").arg(root.chosen.author));
                        if (root.chosen.tags.length > 0)
                            parts.push(root.chosen.tags.join(", "));
                        return parts.join(" · ");
                    }
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth

                    ColumnLayout {
                        width: parent.width
                        spacing: 8

                        Label {
                            visible: root.chosen.properties.length === 0
                            textFormat: Text.PlainText
                            color: palette.placeholderText
                            text: qsTr("This wallpaper has no settings.")
                        }

                        Repeater {
                            model: root.chosen.properties

                            delegate: RowLayout {
                                id: row

                                required property wallpaperProperty modelData
                                readonly property var current: root.library.propertyValues[row.modelData.key]

                                Layout.fillWidth: true

                                Label {
                                    Layout.preferredWidth: 120
                                    elide: Text.ElideRight
                                    textFormat: Text.PlainText
                                    text: row.modelData.label
                                }
                                Slider {
                                    Layout.fillWidth: true
                                    visible: row.modelData.type === "float"
                                    from: row.modelData.minimum
                                    to: row.modelData.maximum
                                    value: row.modelData.type === "float" ? row.current : 0
                                    onMoved: root.library.setPropertyValue(row.modelData.key, value)
                                }
                                Label {
                                    visible: row.modelData.type === "float"
                                    textFormat: Text.PlainText
                                    text: row.modelData.type === "float" ? Number(row.current).toFixed(2) : ""
                                }
                                Switch {
                                    visible: row.modelData.type === "bool"
                                    checked: row.modelData.type === "bool" && row.current === true
                                    onToggled: root.library.setPropertyValue(row.modelData.key, checked)
                                }
                                Rectangle {
                                    visible: row.modelData.type === "color"
                                    implicitWidth: 32
                                    implicitHeight: 20
                                    color: row.modelData.type === "color" ? row.current : "transparent"
                                    border.width: 1
                                    border.color: palette.mid
                                }
                                Button {
                                    visible: row.modelData.type === "color"
                                    text: qsTr("Change…")
                                    onClicked: {
                                        colorDialog.key = row.modelData.key;
                                        colorDialog.selectedColor = row.current;
                                        colorDialog.open();
                                    }
                                }
                                Item {
                                    Layout.fillWidth: row.modelData.type !== "float"
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        text: qsTr("Reset to defaults")
                        enabled: root.chosen.properties.length > 0
                        onClicked: root.library.resetPropertyValues()
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    // Inert until applying to the daemon and Plasma is wired.
                    Button {
                        text: qsTr("Apply")
                        enabled: false
                    }
                }
            }
        }
    }

    ColorDialog {
        id: colorDialog

        property string key

        onAccepted: root.library.setPropertyValue(colorDialog.key, colorDialog.selectedColor)
    }
}
