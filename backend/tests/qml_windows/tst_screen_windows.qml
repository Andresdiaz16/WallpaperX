// SPDX-License-Identifier: GPL-2.0-or-later

// ScreenWindows.qml, the plugin's answer to "is a window on this screen focused, or maximized?",
// driven through its real file. Plasma's task manager model is a stand-in here
// (tests/qml_standins/): it serves the rows a case gives it and records what the file asked it to
// filter by. So these cases grade the file's half of the contract -- what it asks the model for, and
// how it reads the rows it gets back -- and not Plasma's filtering, which is Plasma's.
//
// The renderer is a stand-in too: an object holding each property the file writes.

import QtQuick
import QtTest
import org.kde.kwindowsystem
import org.kde.taskmanager as TaskManager

TestCase {
    id: suite

    name: "ScreenWindows"
    // Effective visibility, as tst_wallpaper_states.qml explains; and on a screen, so the file's own
    // screen geometry is a real one.
    visible: true
    when: windowShown

    readonly property string file: "../../kde/kpackage/contents/ui/ScreenWindows.qml"
    // Somewhere on the screen, so a row counts unless a case says otherwise.
    readonly property rect placed: Qt.rect(10, 10, 300, 200)

    Component {
        id: rendererStandIn
        QtObject {
            property bool windowFocused: false
            property bool desktopFocused: false
            property bool windowMaximized: false
            property bool desktopCovered: false
        }
    }

    Component {
        id: host
        Loader {}
    }

    // A top-level window of its own, to take the activation from the test's window.
    Component {
        id: otherWindow
        Window {
            width: 50
            height: 50
            visible: true
            // Not transient to the test's window: an active transient child counts as its parent's
            // activation, which is what keeps a desktop menu "desktop".
            transientParent: null
        }
    }

    function cleanup() {
        KWindowSystem.showingDesktop = false;
    }

    // Loads the real file onto `renderer`, under `parentItem` (the suite, so on the screen, unless a
    // case wants it off), and returns it with the stand-in objects it made.
    function load(renderer, parentItem) {
        const loader = createTemporaryObject(host, parentItem);
        verify(loader !== null, "a Loader for the file was created");
        loader.setSource(suite.file, { "renderer": renderer });
        compare(loader.status, Loader.Ready, "ScreenWindows.qml loaded (Loader status " + loader.status + ")");
        const watcher = loader.item;
        let tasks = null;
        let desktops = null;
        let activities = null;
        for (let i = 0; i < watcher.resources.length; ++i) {
            const o = watcher.resources[i];
            if (typeof o.resetWindows === "function") tasks = o;
            else if (o.currentDesktop !== undefined) desktops = o;
            else if (o.currentActivity !== undefined) activities = o;
        }
        // The stand-in's own test interface, which Plasma's model has not: proof that the file was
        // handed the stand-in and not the host's module.
        verify(tasks !== null, "the file's task model is this harness's stand-in");
        verify(desktops !== null && activities !== null, "and so are its desktop and activity sources");
        return { watcher: watcher, tasks: tasks, desktops: desktops, activities: activities };
    }

    function loaded() {
        const renderer = createTemporaryObject(rendererStandIn, suite);
        const parts = load(renderer, suite);
        parts.renderer = renderer;
        return parts;
    }

    function compareFacts(renderer, focused, maximized, what) {
        compare(renderer.windowFocused, focused, what + ": focused");
        compare(renderer.windowMaximized, maximized, what + ": maximized");
    }

    // GOAL: the file asks the model for exactly this screen's windows, one row per window, without
    //       minimized ones, on the current activity and the current virtual desktop. Each is a filter
    //       Plasma applies only when asked: unasked, a window on another screen, a minimized one, or
    //       one on another desktop or activity would pause this screen's video.
    // WHY-THIS-SHAPE: read off the model the file configured, because whether those windows are
    //       then left out is Plasma's filtering, which no stand-in can grade. The screen geometry is
    //       compared with the test window's own screen, not with the file's expression for it.
    //       The desktop clause accepts either path the file can take: the per-screen filter where
    //       the model has it (Plasma 6.7 and later, as this stand-in does), or the global desktop.
    // CONTROL: each a one-line change to backend/kde/kpackage/contents/ui/ScreenWindows.qml.
    //       `filterByScreen: false` -> "the model is asked for this screen's windows only" red.
    //       `filterMinimized: false` -> "...not for minimized ones" red. `filterByVirtualDesktop: false`
    //       with the Component.onCompleted switch deleted -> "...only on the current virtual desktop"
    //       red. `filterByActivity: false` -> "...only on the current activity" red. `groupMode`
    //       deleted -> "one row per window" red. Width and height swapped in the file's
    //       screenGeometry -> "the screen it names" red, on the wide screen the entry configures (on
    //       the offscreen default, which is square, it stayed green). Every control in this file
    //       observed 2026-10-01 through control_batch.sh and reverted.
    function test_the_model_is_asked_for_this_screens_windows() {
        const p = loaded();
        compare(p.tasks.groupMode, 0, "the model is asked for one row per window, not grouped");
        verify(p.tasks.filterByScreen, "the model is asked for this screen's windows only");
        verify(suite.Screen.width > 0 && suite.Screen.height > 0, "the test window is on a screen");
        // The offscreen platform's default screen is square, which a transposed rect passes; the
        // entry configures a wide one.
        verify(suite.Screen.width !== suite.Screen.height, "and it is not square, so a transposed rect is caught");
        const named = p.tasks.screenGeometry;
        compare([named.x, named.y, named.width, named.height],
                [suite.Screen.virtualX, suite.Screen.virtualY, suite.Screen.width, suite.Screen.height],
                "and the screen it names is the one the item is on");
        verify(p.tasks.filterMinimized, "the model is asked for no minimized windows");
        verify(p.tasks.filterByCurrentVirtualDesktop ||
               (p.tasks.filterByVirtualDesktop && p.tasks.virtualDesktop === p.desktops.currentDesktop),
               "the model is asked for windows only on the current virtual desktop");
        verify(p.tasks.filterByActivity, "the model is asked for windows only on the current activity");
        p.activities.currentActivity = "standin-activity-2";
        compare(p.tasks.activity, "standin-activity-2", "and the activity it names follows the current one");
    }

    function test_the_rows_decide_both_facts_data() {
        const placed = suite.placed;
        return [
            { tag: "no window", rows: [], focused: false, maximized: false },
            { tag: "a focused window", rows: [{ active: true }], focused: true, maximized: false },
            { tag: "a maximized window, not focused", rows: [{ maximized: true }], focused: false, maximized: true },
            { tag: "a fullscreen window counts as maximized", rows: [{ fullScreen: true }], focused: false,
              maximized: true },
            { tag: "focused and maximized", rows: [{ active: true, maximized: true }], focused: true, maximized: true },
            { tag: "any row decides", rows: [{}, { active: true }, { fullScreen: true }], focused: true,
              maximized: true },
            { tag: "a window not placed yet counts nowhere",
              rows: [{ active: true, maximized: true, fullScreen: true, geometry: Qt.rect(0, 0, 0, 0) }],
              focused: false, maximized: false },
        ];
    }

    // GOAL: focused is "some counted row is active"; maximized is "some counted row is maximized or
    //       fullscreen"; and a row the model cannot place yet -- empty geometry, which Plasma's model
    //       lets through on every screen for a new window's first milliseconds -- counts for neither.
    // WHY-THIS-SHAPE: each row through the file's own recount, read on the renderer it writes to;
    //       data-driven so a row that should count and one that must not are graded by the same code.
    // CONTROL: in ScreenWindows.qml, the `IsFullScreen` term dropped from the maximized expression
    //       -> rows "a fullscreen window counts as maximized" and "any row decides" red; the
    //       `placed` test deleted from recount() -> "a window not placed yet counts nowhere" red;
    //       IsActive and IsMaximized swapped in recount() -> four rows red.
    function test_the_rows_decide_both_facts(row) {
        const p = loaded();
        p.tasks.resetWindows(row.rows);
        compareFacts(p.renderer, row.focused, row.maximized, row.tag);
    }

    // GOAL: while "show desktop" is on, both facts are false: windows are hidden then, not
    //       minimized, so the model still lists them.
    // CONTROL: `!KWindowSystem.showingDesktop &&` dropped from both bindings in ScreenWindows.qml ->
    //       "showing the desktop: focused" red.
    function test_showing_the_desktop_clears_both() {
        const p = loaded();
        p.tasks.resetWindows([{ active: true, maximized: true }]);
        compareFacts(p.renderer, true, true, "a focused maximized window");
        KWindowSystem.showingDesktop = true;
        compareFacts(p.renderer, false, false, "showing the desktop");
        KWindowSystem.showingDesktop = false;
        compareFacts(p.renderer, true, true, "the desktop no longer shown");
    }

    // GOAL: every way the model can say its rows changed is recounted: a window opening (rows
    //       inserted), changing state (data changed), closing (rows removed), the model rebuilt
    //       (reset) and its rows re-sorted (layout changed). One missed signal leaves the screen
    //       reported as the model was before it -- a video paused under a window that has closed.
    // WHY-THIS-SHAPE: one step per signal, each moving the answer, so each handler is the only thing
    //       that can produce the value its step asserts.
    // CONTROL: each handler deleted in turn from ScreenWindows.qml's TasksModel: onRowsInserted ->
    //       "a window opening" red; the recount() in the Connections' onDataChanged -> "a window
    //       changing state" red; onRowsRemoved -> "a window closing" red; onModelReset -> "the model
    //       rebuilt" red; onLayoutChanged -> "rows re-sorted" red.
    function test_every_model_change_is_recounted() {
        const p = loaded();
        p.tasks.appendWindow({ active: true });
        compareFacts(p.renderer, true, false, "a window opening");
        p.tasks.changeWindow(0, { active: true, maximized: true });
        compareFacts(p.renderer, true, true, "a window changing state");
        p.tasks.removeWindow(0);
        compareFacts(p.renderer, false, false, "a window closing");
        p.tasks.resetWindows([{ maximized: true }]);
        compareFacts(p.renderer, false, true, "the model rebuilt");
        p.tasks.changeWindowByLayout(0, { active: true });
        compareFacts(p.renderer, true, false, "rows re-sorted");
    }

    // GOAL: a change the model announces under roles recount() never reads -- a title, an icon --
    //       does not recount; one naming any of the four it reads, or naming no role (any may have
    //       changed), does. Every desktop's watcher otherwise rescans its rows on each title change.
    // WHY-THIS-SHAPE: the row itself changes under the unrelated announcement, so an answer that
    //       did not move means the file did not recount, not that there was nothing to see; the
    //       empty-list step then recounts the same row and must move it.
    // CONTROL: in ScreenWindows.qml, readsAny made `return true;` -> "announced only as a display
    //       role" red. Each of the four roles dropped from countedRoles in turn -> that role's row of
    //       test_each_counted_role_recounts red.
    function test_unrelated_roles_do_not_recount() {
        const p = loaded();
        p.tasks.resetWindows([{}]);
        compareFacts(p.renderer, false, false, "one window, neither focused nor maximized");
        p.tasks.changeWindowRoles(0, { active: true, maximized: true }, [Qt.DisplayRole, Qt.DecorationRole]);
        compareFacts(p.renderer, false, false, "the row changed but announced only as a display role");
        p.tasks.changeWindowRoles(0, { active: true, maximized: true }, []);
        compareFacts(p.renderer, true, true, "the same row announced with no roles, meaning any");
    }

    function test_each_counted_role_recounts_data() {
        const unplaced = Qt.rect(0, 0, 0, 0);
        return [
            { tag: "ScreenGeometry", role: TaskManager.AbstractTasksModel.ScreenGeometry,
              from: { active: true, geometry: unplaced }, to: { active: true }, focused: true, maximized: false },
            { tag: "IsActive", role: TaskManager.AbstractTasksModel.IsActive,
              from: {}, to: { active: true }, focused: true, maximized: false },
            { tag: "IsMaximized", role: TaskManager.AbstractTasksModel.IsMaximized,
              from: {}, to: { maximized: true }, focused: false, maximized: true },
            { tag: "IsFullScreen", role: TaskManager.AbstractTasksModel.IsFullScreen,
              from: {}, to: { fullScreen: true }, focused: false, maximized: true },
        ];
    }

    function test_each_counted_role_recounts(row) {
        const p = loaded();
        p.tasks.resetWindows([row.from]);
        compareFacts(p.renderer, false, false, row.tag + ": before the change");
        p.tasks.changeWindowRoles(0, row.to, [Qt.DisplayRole, row.role]);
        compareFacts(p.renderer, row.focused, row.maximized, row.tag + ": a change announced under it");
    }

    // GOAL: a watcher states its answer as it starts, even when the answer is "no window", so a
    //       renderer left holding an older answer is corrected rather than kept.
    // CONTROL: the writes in ScreenWindows.qml's root Component.onCompleted deleted -> "a new watcher
    //       with no windows: focused" red.
    function test_a_new_watcher_states_its_answer() {
        const renderer = createTemporaryObject(rendererStandIn, suite, { windowFocused: true, windowMaximized: true });
        load(renderer, suite);
        compareFacts(renderer, false, false, "a new watcher with no windows");
    }

    // GOAL: S15.470 -- desktopFocused is written to the renderer as the window the file is in gains and
    //       loses the activation: true while that window is active, false once another top-level window
    //       takes it; and it never sets windowFocused.
    // WHY-THIS-SHAPE: the activation is moved by a real second window, as KWin moves it away from the
    //       desktop; the value is read on the renderer, so both the binding and the change handler that
    //       carries it there are graded.
    // CONTROL: via control_batch.sh, 2026-10-11, each alone, reverted, in
    //       backend/kde/kpackage/contents/ui/ScreenWindows.qml: `&& root.Window.active` dropped from
    //       desktopFocused, and the onDesktopFocusedChanged handler deleted -> each "FAIL! ...
    //       test_the_desktop_focus_follows_the_windows_activation() 'the desktop is not reported focused
    //       once another window is active' returned FALSE".
    function test_the_desktop_focus_follows_the_windows_activation() {
        const p = loaded();
        suite.Window.window.requestActivate();
        tryVerify(() => suite.Window.active, 3000, "the test's window became active (the premise)");
        tryVerify(() => p.renderer.desktopFocused === true, 3000, "the active window's desktop is reported focused");
        compare(p.renderer.windowFocused, false, "and no window is reported focused for it");
        const other = createTemporaryObject(otherWindow, suite);
        other.requestActivate();
        tryVerify(() => !suite.Window.active, 3000, "another window took the activation (the premise)");
        tryVerify(() => p.renderer.desktopFocused === false, 3000, "the desktop is not reported focused once another window is active");
        suite.Window.window.requestActivate();
        tryVerify(() => p.renderer.desktopFocused === true, 3000, "and is again once its window is active again");
    }
}
