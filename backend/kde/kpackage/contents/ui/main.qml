// SPDX-License-Identifier: GPL-2.0-or-later

// The file Plasma loads for this wallpaper. It asks the daemon over loopback HTTP what this
// screen should show, identifying itself by connector name -- the one fact both sides derive
// from the hardware, so nothing has to stay in sync.
//
// This file must never import com.wallpaperx: naming a QML type that fails to resolve takes the
// whole entry file down, which is a black desktop with nothing logged. The renderer is reached
// through the Loader below -- see WallpaperSurface.qml.
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

    // The request still awaiting a reply, so a later tick supersedes it instead of racing it,
    // and how many polls it has been waiting. A reply is not late until several have passed:
    // the poll interval was doubling as the deadline, so a daemon answering slower than one
    // tick was aborted every time and never once heard from.
    property var pending: null
    property int pendingPolls: 0
    readonly property int lateAfterPolls: 5

    // What this will parse. The reply comes from whatever holds the port, and nothing else
    // bounds what the shell acts on. Not what it buffers: see the handler below.
    readonly property int maxBodyBytes: 65536

    // Checked on the way back as well as set on the way out: a redirect would otherwise make
    // every screen's plugin fetch from wherever it names, once a second, from inside the shell.
    readonly property string daemonOrigin: "http://127.0.0.1:18462"

    // No headless Qt platform reports a screen name, and an empty one pins every state below to
    // "unnamed", so a test needs a way in. It gets this rather than a writable `connector`: an
    // assignment there would destroy the binding, leaving the plugin asking about a screen that
    // a re-plug has since renamed. Empty in production.
    property string connectorOverride: ""

    // The screen this instance sits on, by connector name -- the one fact the daemon also
    // derives from the hardware.
    readonly property string connector:
        root.connectorOverride !== "" ? root.connectorOverride : root.Screen.name

    // Clamped for the same reason refusalReason is: this comes from whatever holds the port,
    // and it lands on the desktop. The count is UTF-16 units and the daemon's limit is 256
    // bytes, which is the safe direction -- no string is fewer bytes than units, so nothing the
    // daemon can send is truncated here.
    readonly property string screenName:
        (root.reply && typeof root.reply.output === "string")
            ? root.reply.output.substring(0, 256)
            : ""

    // Never path_literal: that is the raw bytes to act on, and putting them on the desktop
    // would render control characters and bidirectional overrides out of a filename.
    // Clamped like the two strings above, against the 4096 bytes the daemon caps this field to:
    // it lands in a text layout on plasmashell's GUI thread.
    readonly property string wallpaperPath:
        (root.reply && typeof root.reply.path === "string")
            ? root.reply.path.substring(0, 4096)
            : ""

    // The id the renderer asks the daemon for frames by. Null in the reply until the daemon
    // knows the screen, and null is deliberate on its side rather than 0 -- an id handed back
    // as 0 would put monitor 0's wallpaper on a screen that is not monitor 0. -1 here means the
    // same thing and is what the renderer refuses to draw for.
    readonly property int monitorId:
        (root.reply && typeof root.reply.monitor_id === "number") ? root.reply.monitor_id : -1

    // Each state needs a different action from whoever reads it, so they are told apart
    // rather than merged. Order matters: a refusal only means anything once something answered.
    readonly property string screenState:
        root.connector === "" ? "unnamed"
        : !root.answered ? "unreachable"
        : root.refusalReason !== "" ? "refused"
        : !root.reply ? "unreadable"
        : root.reply.known !== true ? "unknown"
        : root.wallpaperPath === "" ? "unset"
        : "set"

    // Why the renderer is not drawing, when it has loaded and still is not. Kept on root rather
    // than read through surface.item, which qmllint can only see as a QtObject.
    property string rendererReason: ""

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
        // Clamped like the strings above: it can carry a D-Bus error message from off-process.
        if (root.rendererReason !== "") {
            return qsTr("WallpaperX cannot draw on this screen:\n%1").arg(root.rendererReason.substring(0, 256));
        }
        return qsTr("WallpaperX will draw:\n%1").arg(root.wallpaperPath);
    }

    function refresh() {
        // An empty output= is a request the daemon rightly refuses, so asking anyway would
        // report a refusal this plugin caused as though the daemon had rejected the screen.
        if (root.connector === "") {
            return;
        }

        if (root.pending !== null) {
            root.pendingPolls += 1;
            if (root.pendingPolls < root.lateAfterPolls) {
                return;
            }
            const late = root.pending;
            root.pending = null;
            root.pendingPolls = 0;
            late.abort();
            // Nothing answered within the deadline, which is what "not running" describes.
            root.apply(0, "");
            return;
        }

        const request = new XMLHttpRequest();
        root.pending = request;
        root.pendingPolls = 0;
        // The timestamp defeats any cache between here and the daemon: a wallpaper the user
        // just chose must not wait on a stale 200.
        const url = root.daemonOrigin + "/config?output=" +
                    encodeURIComponent(root.connector) + "&t=" + Date.now();
        request.onreadystatechange = function () {
            // Superseded or aborted: its answer describes a question already replaced.
            if (root.pending !== request) {
                return;
            }
            // A declared Content-Length is deliberately NOT acted on here. Refusing at
            // HEADERS_RECEIVED means abort()ing from inside this handler, and on Qt 6.11.2 that
            // segfaults QQmlXMLHttpRequest on the next network event -- so the defence against
            // an oversized reply became a way for whatever holds the port to kill plasmashell.
            // The bound below catches the same reply a moment later, having buffered it.
            if (request.readyState !== XMLHttpRequest.DONE) {
                return;
            }
            root.pending = null;
            root.pendingPolls = 0;
            // Redirected away from the daemon, or oversized: neither is ours whatever it holds,
            // so both read as unreadable. An empty responseURL means Qt did not report one.
            const from = request.responseURL;
            const elsewhere = typeof from === "string" && from !== "" &&
                              from.indexOf(root.daemonOrigin + "/") !== 0;
            const body = request.responseText;
            const usable = !elsewhere && body.length <= root.maxBodyBytes;
            root.apply(request.status, usable ? body : "");
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

    // The wallpaper itself, loaded rather than imported. A module that fails to load leaves this
    // Loader in Error and the message below still describes the screen, instead of taking the
    // whole plugin down with it.
    Loader {
        id: surface

        // Named so a check can find it without depending on declaration order.
        objectName: "wallpaperxSurface"
        anchors.fill: parent
        // Only once there is something to draw AND a screen to ask about. Without the id test
        // the renderer would poll for frames belonging to no monitor.
        active: root.screenState === "set" && root.monitorId >= 0
        source: "WallpaperSurface.qml"
        onLoaded: {
            item.monitorId = root.monitorId;
            root.rendererReason = item.unavailableReason;
        }
        // A reason from an item that is going away would otherwise outlive it.
        onStatusChanged: if (surface.status !== Loader.Ready) root.rendererReason = ""

        // A later reply can move the screen to a different id -- a re-plug, or the daemon
        // learning the name after the fact -- and the loaded item would otherwise keep drawing
        // the frames of the id it was created with.
        Connections {
            target: root
            function onMonitorIdChanged() {
                if (surface.item !== null) surface.item.monitorId = root.monitorId;
            }
        }

        Connections {
            target: surface.item
            ignoreUnknownSignals: true
            function onUnavailableReasonChanged() {
                root.rendererReason = surface.item.unavailableReason;
            }
        }
    }

    Rectangle {
        id: backdrop

        objectName: "wallpaperxBackdrop"
        anchors.fill: parent
        // Opaque, because a message drawn over an unknown desktop may be unreadable, and a
        // transparent plugin that failed is indistinguishable from no plugin at all. So it is
        // hidden rather than drawn under the wallpaper, and shown again the moment anything goes
        // wrong -- keyed on the renderer drawing, not merely on its module having loaded.
        visible: !(surface.status === Loader.Ready && root.screenState === "set" && root.rendererReason === "")
        color: "#1b1b1b"

        Text {
            id: message

            anchors.centerIn: parent
            width: Math.min(backdrop.width * 0.8, 900)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: "#f0f0f0"
            font.pointSize: 16

            // Plain, not the default AutoText: the refusal message carries a string copied
            // verbatim from whatever holds port 18462, and auto-detection would render markup
            // in it -- including images Qt would fetch. Same reason the path above is shown
            // escaped rather than raw.
            textFormat: Text.PlainText
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
