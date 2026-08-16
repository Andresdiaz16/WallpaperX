// SPDX-License-Identifier: GPL-2.0-or-later

// The file Plasma loads for this wallpaper. It asks the daemon over loopback HTTP what this
// screen should show, identifying itself by connector name -- the one fact both sides derive
// from the hardware, so nothing has to stay in sync.
//
// No renderer yet: WallpaperXRenderer arrives in Step 12, and naming a QML type that does not
// exist makes the whole file fail to load, which is a black desktop with nothing logged.
import QtQuick
import org.kde.plasma.plasmoid

WallpaperItem {
    id: root

    anchors.fill: parent

    // What the last poll established; everything shown derives from these in bindings. The
    // linter checks a property read and a declarative binding but not an assignment target
    // inside a function body, so the logic lives where it can be seen.
    property var reply: null
    property bool answered: false
    property string refusalReason: ""

    // The request still awaiting a reply, so a later tick supersedes it instead of racing it.
    property var pending: null

    readonly property string screenName:
        (root.reply && typeof root.reply.output === "string") ? root.reply.output : ""

    // Never path_literal: that is the raw bytes to act on, and putting them on the desktop
    // would render control characters and bidirectional overrides out of a filename.
    readonly property string wallpaperPath:
        (root.reply && typeof root.reply.path === "string") ? root.reply.path : ""

    // Each state needs a different action from whoever reads it, so they are told apart
    // rather than merged. Order matters: a refusal only means anything once something answered.
    readonly property string screenState:
        root.Screen.name === "" ? "unnamed"
        : !root.answered ? "unreachable"
        : root.refusalReason !== "" ? "refused"
        : !root.reply ? "unreadable"
        : root.reply.known !== true ? "unknown"
        : root.wallpaperPath === "" ? "unset"
        : "set"

    function describe(): string {
        if (root.screenState === "unnamed") {
            return qsTr("This screen has not reported a connector name.\nWallpaperX identifies screens by that name.");
        }
        if (root.screenState === "unreachable") {
            return qsTr("WallpaperX is not running.\nStart the wallpaperx-daemon service.");
        }
        if (root.screenState === "refused") {
            return qsTr("WallpaperX is running, but refused this screen's request:\n%1")
                .arg(root.refusalReason);
        }
        if (root.screenState === "unreadable") {
            return qsTr("WallpaperX answered with something this plugin cannot read.\nCheck the wallpaperx-daemon version.");
        }
        if (root.screenState === "unknown") {
            return qsTr("WallpaperX does not know this screen yet (%1).\nRun the WallpaperX app once.")
                .arg(root.screenName);
        }
        if (root.screenState === "unset") {
            return qsTr("No wallpaper chosen for this screen (%1).\nPick one in the WallpaperX app.")
                .arg(root.screenName);
        }
        return qsTr("WallpaperX will draw:\n%1").arg(root.wallpaperPath);
    }

    function refresh() {
        // An empty output= is a request the daemon rightly refuses, so asking anyway would
        // report a refusal this plugin caused as though the daemon had rejected the screen.
        if (root.Screen.name === "") {
            return;
        }

        const previous = root.pending;
        root.pending = null;
        if (previous !== null) {
            previous.abort();
        }

        const request = new XMLHttpRequest();
        root.pending = request;
        // The timestamp defeats any cache between here and the daemon: a wallpaper the user
        // just chose must not wait on a stale 200.
        const url = "http://127.0.0.1:18462/config?output=" +
                    encodeURIComponent(root.Screen.name) + "&t=" + Date.now();
        request.onreadystatechange = function () {
            if (request.readyState !== XMLHttpRequest.DONE) {
                return;
            }
            // Superseded or aborted: its answer describes a question already replaced.
            if (root.pending !== request) {
                return;
            }
            root.pending = null;
            root.apply(request.status, request.responseText);
        };
        request.open("GET", url);
        request.send();
    }

    function apply(status: int, body: string) {
        // status 0 is nothing answering, the ordinary state before the daemon is started.
        if (status === 0) {
            root.answered = false;
            root.refusalReason = "";
            root.reply = null;
            return;
        }
        root.answered = true;

        let parsed = null;
        try {
            parsed = JSON.parse(body);
        } catch (error) {
            parsed = null;
        }
        // Valid JSON need not be an object: a bare null or number would throw on the first
        // member access, inside a handler with nothing to catch it.
        const object = (typeof parsed === "object" && parsed !== null) ? parsed : null;

        // Any other non-200 is the daemon answering and refusing, and its body says why.
        // Reporting that as "not running" sends the user to restart a service that is up.
        if (status !== 200) {
            const why = (object && typeof object.error === "string")
                ? object.error
                : qsTr("HTTP status %1").arg(status);
            // Clamped: this comes from whatever holds the port, and it lands on the desktop.
            root.refusalReason = why.substring(0, 256);
            root.reply = null;
            return;
        }
        root.refusalReason = "";
        root.reply = object;
    }

    // Opaque, because a message drawn over an unknown desktop may be unreadable, and a
    // transparent plugin that failed is indistinguishable from no plugin at all.
    Rectangle {
        id: backdrop

        anchors.fill: parent
        color: "#1b1b1b"

        Text {
            id: message

            anchors.centerIn: parent
            width: Math.min(backdrop.width * 0.8, 900)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: "#f0f0f0"
            font.pointSize: 16
            text: root.describe()
        }
    }

    Timer {
        interval: 1000
        repeat: true
        running: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }
}
