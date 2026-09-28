// SPDX-License-Identifier: GPL-2.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import WallpaperX

// The monitors laid out as KScreen places them; clicking one selects it on the client.
Item {
    id: root

    required property KScreenClient kscreen

    readonly property var placed: kscreen.monitors.filter(m => m.selectable)
    // Replicas drawn with the monitor they copy. One whose source has no tile is listed with the rest.
    readonly property var mirrors: kscreen.monitors.filter(m => m.mirrorOf !== "" && m.enabled && m.connected
                                                           && placed.some(p => p.outputName === m.mirrorOf))
    // Attached monitors with no tile of their own; an empty connector is not a monitor.
    readonly property var unplaced: kscreen.monitors.filter(m => m.connected && !m.selectable
                                                            && !mirrors.some(r => r.outputName === m.outputName))

    // Where each tile goes, in logical coordinates, by connector name: { area, sharedWith }.
    // Placed monitors whose areas overlap split their combined box into strips, one each, so none
    // hides another; the rest keep their own. A replica takes a band along the bottom of its
    // source's area, so it sits with the picture it shows and covers nothing clickable.
    readonly property var tiles: {
        // More than a pixel each way, so fractional scaling's rounding between neighbours is not
        // taken for overlap.
        const overlaps = (a, b) => Math.min(a.x + a.width, b.x + b.width) - Math.max(a.x, b.x) > 1
            && Math.min(a.y + a.height, b.y + b.height) - Math.max(a.y, b.y) > 1;
        const union = (a, b) => {
            const left = Math.min(a.x, b.x), top = Math.min(a.y, b.y);
            return Qt.rect(left, top, Math.max(a.x + a.width, b.x + b.width) - left,
                           Math.max(a.y + a.height, b.y + b.height) - top);
        };
        // Merged until no two boxes overlap, since a merged box can reach a third monitor.
        let groups = placed.map(m => ({ members: [m], box: m.geometry }));
        for (let merged = true; merged; ) {
            merged = false;
            for (let i = 0; i < groups.length && !merged; ++i) {
                for (let j = i + 1; j < groups.length && !merged; ++j) {
                    if (overlaps(groups[i].box, groups[j].box)) {
                        groups[i] = { members: groups[i].members.concat(groups[j].members),
                                      box: union(groups[i].box, groups[j].box) };
                        groups.splice(j, 1);
                        merged = true;
                    }
                }
            }
        }
        const result = {};
        for (const g of groups) {
            const n = g.members.length;
            const across = g.box.width >= g.box.height;
            const names = g.members.map(m => m.outputName).sort();
            names.forEach((name, k) => {
                result[name] = {
                    area: across ? Qt.rect(g.box.x + k * g.box.width / n, g.box.y, g.box.width / n, g.box.height)
                                 : Qt.rect(g.box.x, g.box.y + k * g.box.height / n, g.box.width, g.box.height / n),
                    sharedWith: names.filter(other => other !== name)
                };
            });
        }
        for (const source of placed) {
            const copies = mirrors.filter(r => r.mirrorOf === source.outputName).map(r => r.outputName).sort();
            if (copies.length === 0)
                continue;
            const a = result[source.outputName].area;
            const band = a.height / 3;
            result[source.outputName].area = Qt.rect(a.x, a.y, a.width, a.height - band);
            copies.forEach((name, k) => {
                result[name] = {
                    area: Qt.rect(a.x + k * a.width / copies.length, a.y + a.height - band, a.width / copies.length, band),
                    sharedWith: []
                };
            });
        }
        return result;
    }

    // The desktop's bounding box in logical coordinates.
    readonly property rect bounds: {
        if (placed.length === 0)
            return Qt.rect(0, 0, 0, 0);
        let left = Infinity, top = Infinity, right = -Infinity, bottom = -Infinity;
        for (const m of placed) {
            left = Math.min(left, m.geometry.x);
            top = Math.min(top, m.geometry.y);
            right = Math.max(right, m.geometry.x + m.geometry.width);
            bottom = Math.max(bottom, m.geometry.y + m.geometry.height);
        }
        return Qt.rect(left, top, right - left, bottom - top);
    }

    readonly property real margin: 24
    readonly property real layoutScale: bounds.width > 0 && bounds.height > 0
        ? Math.min((width - 2 * margin) / bounds.width, (height - 2 * margin - footer.height) / bounds.height)
        : 0
    readonly property real originX: (width - bounds.width * layoutScale) / 2
    readonly property real originY: (height - footer.height - bounds.height * layoutScale) / 2

    Item {
        id: desk
        anchors.fill: parent
        visible: root.kscreen.state === KScreenClient.Ready && root.placed.length > 0

        Repeater {
            model: root.placed

            delegate: Rectangle {
                id: tile

                required property monitor modelData
                readonly property bool selected: tile.modelData.outputName === root.kscreen.selectedOutput
                // The fallback covers a delegate evaluated before `tiles` has caught up with `placed`.
                readonly property var placement: root.tiles[tile.modelData.outputName]
                    ?? { area: tile.modelData.geometry, sharedWith: [] }

                // A two-pixel gap keeps adjacent screens distinct at small scales.
                x: root.originX + (tile.placement.area.x - root.bounds.x) * root.layoutScale + 2
                y: root.originY + (tile.placement.area.y - root.bounds.y) * root.layoutScale + 2
                width: Math.max(tile.placement.area.width * root.layoutScale - 4, 1)
                height: Math.max(tile.placement.area.height * root.layoutScale - 4, 1)
                radius: 4
                color: tile.selected ? palette.highlight : palette.button
                border.width: tile.selected ? 3 : 1
                border.color: tile.selected ? palette.highlightedText : palette.mid

                Accessible.role: Accessible.RadioButton
                Accessible.name: tile.modelData.displayName + " (" + tile.modelData.outputName + ")"
                Accessible.checked: tile.selected
                Accessible.onPressAction: root.kscreen.select(tile.modelData.outputName)

                ColumnLayout {
                    anchors.centerIn: parent
                    width: parent.width - 8
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        font.bold: true
                        text: tile.modelData.displayName
                        color: tile.selected ? palette.highlightedText : palette.buttonText
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: {
                            const m = tile.modelData;
                            let parts = [m.outputName, m.pixelSize.width + "×" + m.pixelSize.height];
                            if (m.rotation !== 0)
                                parts.push(qsTr("rotated %1°").arg(m.rotation));
                            if (m.primary)
                                parts.push(qsTr("primary"));
                            if (tile.placement.sharedWith.length > 0)
                                parts.push(qsTr("same area as %1").arg(tile.placement.sharedWith.join(", ")));
                            return parts.join(" · ");
                        }
                        color: tile.selected ? palette.highlightedText : palette.buttonText
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.kscreen.select(tile.modelData.outputName)
                }
            }
        }

        // No MouseArea: a replica cannot be selected, and nothing selectable lies beneath it.
        Repeater {
            model: root.mirrors

            delegate: Rectangle {
                id: copy

                required property monitor modelData
                readonly property var placement: root.tiles[copy.modelData.outputName]
                    ?? { area: Qt.rect(0, 0, 0, 0), sharedWith: [] }

                x: root.originX + (copy.placement.area.x - root.bounds.x) * root.layoutScale + 2
                y: root.originY + (copy.placement.area.y - root.bounds.y) * root.layoutScale + 2
                width: Math.max(copy.placement.area.width * root.layoutScale - 4, 1)
                height: Math.max(copy.placement.area.height * root.layoutScale - 4, 1)
                radius: 4
                clip: true
                color: palette.window
                border.width: 1
                border.color: palette.mid

                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("%1 (%2), mirrors %3").arg(copy.modelData.displayName)
                                                            .arg(copy.modelData.outputName)
                                                            .arg(copy.modelData.mirrorOf)

                ColumnLayout {
                    anchors.centerIn: parent
                    width: parent.width - 8
                    spacing: 0

                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: copy.modelData.displayName
                        color: palette.placeholderText
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        font.italic: true
                        text: copy.modelData.outputName + " · " + qsTr("mirrors %1").arg(copy.modelData.mirrorOf)
                        color: palette.placeholderText
                    }
                }
            }
        }
    }

    Label {
        id: footer
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        bottomPadding: 8
        visible: root.kscreen.state === KScreenClient.Ready && root.unplaced.length > 0
        height: visible ? implicitHeight : 0
        textFormat: Text.PlainText
        text: qsTr("Not showing a desktop: %1").arg(root.unplaced.map(
            m => m.mirrorOf === "" ? m.outputName : qsTr("%1 (mirrors %2)").arg(m.outputName).arg(m.mirrorOf)).join(", "))
    }

    ColumnLayout {
        anchors.centerIn: parent
        visible: !desk.visible
        spacing: 12

        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            visible: root.kscreen.state === KScreenClient.Loading
            running: visible
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: root.width - 2 * root.margin
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            textFormat: Text.PlainText
            text: {
                switch (root.kscreen.state) {
                case KScreenClient.Loading:
                    return qsTr("Looking for monitors…");
                case KScreenClient.Unavailable:
                    return qsTr("Monitors could not be listed. %1").arg(root.kscreen.unavailableReason);
                default:
                    return qsTr("KScreen reports no monitor showing a desktop.");
                }
            }
        }
        Button {
            Layout.alignment: Qt.AlignHCenter
            visible: root.kscreen.state !== KScreenClient.Loading
            text: qsTr("Try again")
            onClicked: root.kscreen.refresh()
        }
    }
}
