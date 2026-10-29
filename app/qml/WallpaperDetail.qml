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
    required property ApplyController applier

    readonly property wallpaper chosen: library.selectedWallpaper
    readonly property string positionMode: library.propertyValues[Positioning.modeKey] ?? ""
    readonly property real zoom: Number(library.propertyValues[Positioning.zoomKey] ?? 1)
    readonly property var modeNames: ({
        cover: qsTr("Cover"),
        fit: qsTr("Fit"),
        center: qsTr("Center"),
        tile: qsTr("Tile"),
        stretch: qsTr("Stretch"),
        free: qsTr("Free")
    })

    // How far the picture's centre may move from the screen's, in screens: the picture is at least
    // `zoom` screens wide and high, so a tenth of a screen of it stays in view at the limit.
    function offsetReach(zoom) {
        return Math.min(Positioning.maximumOffset, zoom / 2 + 0.4);
    }

    // Brings the offsets inside the new zoom's reach, so a smaller picture is not left off screen.
    function setZoom(zoom) {
        const z = Math.min(Positioning.maximumZoom, Math.max(Positioning.minimumZoom, zoom));
        const values = { [Positioning.zoomKey]: z };
        const reach = root.offsetReach(z);
        for (const key of [Positioning.xKey, Positioning.yKey]) {
            const offset = Number(root.library.propertyValues[key]);
            if (Math.abs(offset) > reach)
                values[key] = Math.sign(offset) * reach;
        }
        root.library.setPropertyValues(values);
    }

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
                    id: settingsView

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth

                    ColumnLayout {
                        width: parent.width
                        spacing: 8

                        // The wheel moves the settings at once, as a desktop list does. Left to the view,
                        // each notch sets them gliding, and a press while they glide only stops them: the
                        // first click on a control just scrolled to (Frame as Cover, at the bottom) was lost.
                        WheelHandler {
                            onWheel: event => {
                                const view = settingsView.contentItem;
                                const step = event.pixelDelta.y !== 0
                                    ? event.pixelDelta.y
                                    : event.angleDelta.y / 120 * Qt.styleHints.wheelScrollLines * 20;
                                view.contentY = Math.max(0, Math.min(view.contentHeight - view.height, view.contentY - step));
                                event.accepted = true;
                            }
                        }

                        Label {
                            visible: root.chosen.properties.length === 0
                            textFormat: Text.PlainText
                            color: palette.placeholderText
                            text: qsTr("This wallpaper has no settings of its own.")
                        }

                        Repeater {
                            model: root.chosen.properties

                            delegate: RowLayout {
                                id: row

                                required property wallpaperProperty modelData
                                readonly property var current: root.library.propertyValues[row.modelData.key]
                                // A video's own sound, shown as sound on and a percentage whatever
                                // wallpaper.json calls them.
                                readonly property bool isMute: row.modelData.key === "mute"
                                readonly property bool isVolume: row.modelData.key === "volume"
                                // A float that takes only a few values, the speed: one button for each.
                                readonly property bool isChoice: row.modelData.choices.length > 0
                                readonly property bool isSlider: row.modelData.type === "float" && !row.isChoice

                                Layout.fillWidth: true

                                Label {
                                    Layout.preferredWidth: 120
                                    elide: Text.ElideRight
                                    textFormat: Text.PlainText
                                    text: row.isMute ? qsTr("Sound") : row.isVolume ? qsTr("Volume") : row.modelData.label
                                }
                                Slider {
                                    Layout.fillWidth: true
                                    visible: row.isSlider
                                    from: row.modelData.minimum
                                    to: row.modelData.maximum
                                    value: row.isSlider ? row.current : 0
                                    onMoved: root.library.setPropertyValue(row.modelData.key, value)
                                }
                                Label {
                                    visible: row.isSlider
                                    textFormat: Text.PlainText
                                    text: !row.isSlider ? ""
                                        : row.isVolume ? qsTr("%1 %").arg(Math.round(Number(row.current) * 100))
                                        : Number(row.current).toFixed(2)
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    visible: row.isChoice

                                    Repeater {
                                        model: row.modelData.choices

                                        delegate: Button {
                                            required property real modelData

                                            text: qsTr("%1×").arg(modelData)
                                            checkable: true
                                            // Exclusive, so a click on the speed already chosen leaves it chosen.
                                            autoExclusive: true
                                            checked: row.isChoice && Number(row.current) === modelData
                                            onClicked: root.library.setPropertyValue(row.modelData.key, modelData)
                                        }
                                    }
                                }
                                Switch {
                                    visible: row.modelData.type === "bool"
                                    checked: row.modelData.type === "bool" && (row.current === true) !== row.isMute
                                    onToggled: root.library.setPropertyValue(row.modelData.key, checked !== row.isMute)
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
                                    Layout.fillWidth: !row.isSlider && !row.isChoice
                                }
                            }
                        }

                        // Holds the video on its picture on the selected screen, whatever that screen's pause mode.
                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.chosen.type === "video"

                            Label {
                                Layout.preferredWidth: 120
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: qsTr("Paused")
                            }
                            Switch {
                                checked: root.library.propertyValues["paused"] === true
                                onToggled: root.library.setPropertyValue("paused", checked)
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                        }

                        // Where the picture sits on the chosen screen, one button per mode, as the speed.
                        RowLayout {
                            Layout.fillWidth: true

                            Label {
                                Layout.preferredWidth: 120
                                Layout.alignment: Qt.AlignTop
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: qsTr("Position")
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: 4

                                Repeater {
                                    model: root.chosen.positionModes

                                    delegate: Button {
                                        required property string modelData

                                        text: root.modeNames[modelData] ?? modelData
                                        checkable: true
                                        autoExclusive: true
                                        checked: root.positionMode === modelData
                                        onClicked: root.library.setPropertyValue(Positioning.modeKey, modelData)
                                    }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: root.positionMode === "free"

                            Label {
                                Layout.preferredWidth: 120
                                textFormat: Text.PlainText
                                text: qsTr("Zoom")
                            }
                            // On a log scale, so 1, Cover's size, sits halfway between 0.1 and 10.
                            Slider {
                                Layout.fillWidth: true
                                from: Math.log10(Positioning.minimumZoom)
                                to: Math.log10(Positioning.maximumZoom)
                                value: Math.log10(root.zoom)
                                onMoved: root.setZoom(Math.pow(10, value))
                            }
                            Label {
                                textFormat: Text.PlainText
                                text: qsTr("%1×").arg(root.zoom.toFixed(2))
                            }
                        }
                        Repeater {
                            model: [
                                { key: Positioning.xKey, label: qsTr("Horizontal") },
                                { key: Positioning.yKey, label: qsTr("Vertical") }
                            ]

                            // Fractions of the screen's width or height, shown as percentages.
                            delegate: RowLayout {
                                id: offsetRow

                                required property var modelData
                                readonly property real offset: Number(root.library.propertyValues[offsetRow.modelData.key] ?? 0)

                                Layout.fillWidth: true
                                visible: root.positionMode === "free"

                                Label {
                                    Layout.preferredWidth: 120
                                    textFormat: Text.PlainText
                                    text: offsetRow.modelData.label
                                }
                                Slider {
                                    Layout.fillWidth: true
                                    from: -root.offsetReach(root.zoom)
                                    to: root.offsetReach(root.zoom)
                                    value: offsetRow.offset
                                    onMoved: root.library.setPropertyValue(offsetRow.modelData.key, value)
                                }
                                Label {
                                    textFormat: Text.PlainText
                                    text: qsTr("%1 %").arg(Math.round(offsetRow.offset * 100))
                                }
                            }
                        }
                        Button {
                            Layout.leftMargin: 120
                            visible: root.positionMode === "free"
                            text: qsTr("Frame as Cover")
                            onClicked: root.library.setPropertyValues({
                                [Positioning.zoomKey]: 1.0,
                                [Positioning.xKey]: 0.0,
                                [Positioning.yKey]: 0.0
                            })
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.applier.liveOutput !== ""
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    color: palette.placeholderText
                    text: qsTr("Changes to these settings reach %1 as you make them.").arg(root.applier.liveOutput)
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.applier.liveError !== ""
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    text: root.applier.liveError
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        text: qsTr("Reset to defaults")
                        onClicked: root.library.resetPropertyValues()
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    BusyIndicator {
                        Layout.preferredHeight: applyButton.implicitHeight
                        Layout.preferredWidth: applyButton.implicitHeight
                        visible: root.applier.applyState === ApplyController.Working
                        running: visible
                    }
                    Button {
                        id: applyButton
                        text: qsTr("Apply")
                        enabled: root.applier.blockedReason === "" && root.applier.applyState !== ApplyController.Working
                        onClicked: root.applier.apply()
                    }
                }
                // Why Apply is unavailable, else how the last one went.
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    visible: text !== ""
                    text: root.applier.blockedReason !== "" && root.applier.applyState !== ApplyController.Working
                        ? root.applier.blockedReason
                        : root.applier.applyMessage
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    visible: text !== ""
                    text: root.applier.processorNotice
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
