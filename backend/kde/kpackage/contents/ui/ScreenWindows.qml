// SPDX-License-Identifier: GPL-2.0-or-later

// What the windows on this screen are doing, read from Plasma's task manager model -- the one its
// panels and its own image wallpaper use for the same question -- and whether the desktop has the
// focus here. Loaded, never imported: a Plasma whose task manager module will not load loses these
// facts and nothing else, and the daemon then takes the screen to have nothing focused.
import QtQuick
import org.kde.kwindowsystem
import org.kde.taskmanager as TaskManager

Item {
    id: root

    // The renderer these facts are written to. Its type is not named here: that would import
    // com.wallpaperx, tying this file's loading to the renderer module's. var, not QtObject: qmllint
    // checks the writes below against a declared type, and QObject has neither property.
    required property var renderer

    // The geometry the task model files each window under: the screen holding the window's centre,
    // so one spanning two screens counts on one of them. An empty one would turn the model's screen
    // filter off rather than match nothing.
    readonly property rect screenGeometry: Qt.rect(root.Screen.virtualX, root.Screen.virtualY, root.Screen.width,
                                                   root.Screen.height)
    readonly property bool onScreen: root.screenGeometry.width > 0 && root.screenGeometry.height > 0

    property bool anyActive: false
    property bool anyMaximized: false

    // Showing the desktop hides windows without minimizing them.
    readonly property bool focused: root.onScreen && !KWindowSystem.showingDesktop && root.anyActive
    readonly property bool maximized: root.onScreen && !KWindowSystem.showingDesktop && root.anyMaximized
    // The window this item is in is the screen's desktop window, which the task model never lists.
    // KWin focuses it when it is clicked, when the desktop is shown, and when no window on the screen
    // is left to take the focus.
    readonly property bool desktopFocused: root.onScreen && root.Window.active

    onFocusedChanged: root.renderer.windowFocused = root.focused
    onDesktopFocusedChanged: root.renderer.desktopFocused = root.desktopFocused
    onMaximizedChanged: root.renderer.windowMaximized = root.maximized
    // A value already true when the bindings first ran raised no change.
    Component.onCompleted: {
        root.renderer.windowFocused = root.focused;
        root.renderer.desktopFocused = root.desktopFocused;
        root.renderer.windowMaximized = root.maximized;
    }

    // The roles recount() reads; a change to no other can alter its answer. A window title or icon
    // changes far more often than these. An empty list is the model saying any role may have changed.
    readonly property list<int> countedRoles: [TaskManager.AbstractTasksModel.ScreenGeometry,
        TaskManager.AbstractTasksModel.IsActive, TaskManager.AbstractTasksModel.IsMaximized,
        TaskManager.AbstractTasksModel.IsFullScreen]
    function readsAny(roles: list<int>): bool {
        return roles.length === 0 || roles.some(role => root.countedRoles.includes(role));
    }

    function recount() {
        let active = false;
        let maximized = false;
        for (let row = 0; row < tasks.count; ++row) {
            const index = tasks.index(row, 0);
            // The model lets a window it cannot place yet through on every screen -- a new one has
            // no geometry for its first few milliseconds -- so it counts nowhere until placed.
            const placed = tasks.data(index, TaskManager.AbstractTasksModel.ScreenGeometry);
            if (!placed || placed.width <= 0 || placed.height <= 0) {
                continue;
            }
            active = active || tasks.data(index, TaskManager.AbstractTasksModel.IsActive) === true;
            maximized = maximized || tasks.data(index, TaskManager.AbstractTasksModel.IsMaximized) === true ||
                        tasks.data(index, TaskManager.AbstractTasksModel.IsFullScreen) === true;
        }
        root.anyActive = active;
        root.anyMaximized = maximized;
    }

    TaskManager.VirtualDesktopInfo {
        id: desktops
    }

    TaskManager.ActivityInfo {
        id: activities
    }

    // Minimized windows, and those on another activity or virtual desktop, are not counted. A window
    // on every desktop or activity is.
    TaskManager.TasksModel {
        id: tasks

        // Grouped, a row would describe several windows at once.
        groupMode: TaskManager.TasksModel.GroupDisabled
        filterByScreen: true
        screenGeometry: root.screenGeometry
        filterByVirtualDesktop: true
        virtualDesktop: desktops.currentDesktop
        filterByActivity: true
        activity: activities.currentActivity
        filterMinimized: true

        // A window moving between screens or desktops leaves or joins the rows; one changing
        // state changes its data, which the Connections below hears.
        onRowsInserted: root.recount()
        onRowsRemoved: root.recount()
        onModelReset: root.recount()
        onLayoutChanged: root.recount()

        Component.onCompleted: {
            // With KWin's per-screen virtual desktops, the global current desktop can be one this
            // screen is not showing. Plasma 6.7 added the filter for that; asked for by name because
            // a binding to it would fail to load on an older Plasma.
            if ("filterByCurrentVirtualDesktop" in tasks) {
                tasks.filterByVirtualDesktop = false;
                tasks.filterByCurrentVirtualDesktop = true;
            }
            root.recount();
        }
    }

    // Here rather than as the model's onDataChanged: qmllint types that handler by the signal's
    // two-argument overload and rejects the roles parameter, which Qt does pass.
    Connections {
        target: tasks

        function onDataChanged(topLeft, bottomRight, roles) {
            if (root.readsAny(roles)) {
                root.recount();
            }
        }
    }
}
