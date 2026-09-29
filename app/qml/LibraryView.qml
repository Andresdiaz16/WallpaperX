// SPDX-License-Identifier: GPL-2.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import WallpaperX

// The wallpapers in the library folder; clicking a usable one selects it on the library.
Item {
    id: root

    required property LibraryManager library
    required property Importer importer

    readonly property bool listing: library.state === LibraryManager.Ready && grid.count > 0

    // Lines for the user above the grid, shown until dismissed.
    component Banner: Frame {
        id: banner

        property string text
        signal dismissed

        Layout.fillWidth: true
        visible: banner.text.length > 0

        RowLayout {
            anchors.fill: parent

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: banner.text
            }
            Button {
                text: qsTr("Dismiss")
                onClicked: banner.dismissed()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: qsTr("Wallpapers")
                font.pointSize: 16
            }
            Label {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                textFormat: Text.PlainText
                color: palette.placeholderText
                text: root.library.directory
            }
            BusyIndicator {
                Layout.preferredHeight: importButton.implicitHeight
                Layout.preferredWidth: importButton.implicitHeight
                visible: root.importer.busy
                running: visible
            }
            Button {
                id: importButton
                text: qsTr("Import…")
                onClicked: importDialog.open()
            }
            Button {
                text: qsTr("Rescan")
                onClicked: {
                    root.library.rescan();
                    root.importer.rescan();
                }
            }
        }

        Banner {
            text: root.importer.notices.join("\n")
            onDismissed: root.importer.dismissNotices()
        }

        Banner {
            text: root.importer.problems.join("\n")
            onDismissed: root.importer.dismissProblems()
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.listing
            clip: true
            model: root.library.wallpaperModel

            readonly property real tileWidth: 240
            cellWidth: Math.max(tileWidth, width / Math.max(1, Math.floor(width / tileWidth)))
            cellHeight: tileWidth * 9 / 16 + 48

            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: tile

                required property wallpaper modelData

                width: grid.cellWidth
                height: grid.cellHeight

                Accessible.role: Accessible.Button
                // In one pass: chained arg() would fill a "%2" inside the file name too.
                Accessible.name: tile.modelData.usable
                    ? tile.modelData.displayTitle
                    : qsTr("%1, cannot be used: %2").replace(/%[12]/g, p => p === "%1" ? tile.modelData.fileName
                                                                                        : tile.modelData.problem)
                Accessible.onPressAction: root.library.select(tile.modelData.filePath)

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 4

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: palette.base
                        border.width: 1
                        border.color: palette.mid
                        clip: true

                        Image {
                            id: preview
                            anchors.fill: parent
                            anchors.margins: 1
                            visible: tile.modelData.usable
                            asynchronous: true
                            fillMode: Image.PreserveAspectCrop
                            // Fixed, so resizing the window does not decode every preview again.
                            sourceSize: Qt.size(2 * grid.tileWidth, 2 * grid.tileWidth * 9 / 16)
                            source: tile.modelData.previewSource
                        }
                        Label {
                            anchors.fill: parent
                            anchors.margins: 8
                            visible: !tile.modelData.usable || preview.status === Image.Error
                            wrapMode: Text.Wrap
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            textFormat: Text.PlainText
                            color: palette.placeholderText
                            text: tile.modelData.usable ? qsTr("No preview") : tile.modelData.problem
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        font.bold: tile.modelData.usable
                        font.italic: !tile.modelData.usable
                        text: tile.modelData.displayTitle
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: tile.modelData.usable
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.library.select(tile.modelData.filePath)
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.listing
            spacing: 12

            Item { Layout.fillHeight: true }
            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                visible: root.library.state === LibraryManager.Loading
                running: visible
            }
            Label {
                Layout.alignment: Qt.AlignHCenter
                Layout.maximumWidth: root.width
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                textFormat: Text.PlainText
                text: {
                    switch (root.library.state) {
                    case LibraryManager.Loading:
                        return qsTr("Looking for wallpapers…");
                    case LibraryManager.Unavailable:
                        return qsTr("Wallpapers could not be listed. %1").arg(root.library.unavailableReason);
                    default:
                        return qsTr("No wallpapers yet. Import a .wpx file, or put one in %1.").arg(root.importer.dropDirectory);
                    }
                }
            }
            Item { Layout.fillHeight: true }
        }
    }

    FileDialog {
        id: importDialog

        title: qsTr("Import wallpapers")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("WallpaperX wallpapers (*.wpx)")]
        onAccepted: {
            for (const file of importDialog.selectedFiles)
                root.importer.importFile(file);
        }
    }
}
