// SPDX-License-Identifier: GPL-2.0-or-later

// S10.10: whatever has gone wrong, the plugin says something visible and says which cause it
// is. The seven cases main.qml derives are driven here through its real file -- not a copy of
// its logic, which would drift -- and both halves are asserted for each: the state the binding
// produces and the message describe() hands to the Text that lands on the desktop.
//
// The plugin is driven, never polled: apply(status, body) is the same entry point its own
// XMLHttpRequest handler calls, so no stub HTTP server is needed and no check waits on anything.

import QtQuick
import QtTest

TestCase {
    id: suite

    name: "WallpaperStates"

    // Qt Quick Test leaves its TestCase item hidden, and `visible` is the effective value: with
    // it left alone every item below is invisible whatever the plugin declares, and the check
    // that the message is not hidden would pass on the harness rather than on main.qml.
    visible: true

    // A named screen for every case but the first: no headless Qt platform reports a screen
    // name, which is why main.qml takes its connector through an assignable property.
    readonly property string namedScreen: "DP-3"

    readonly property string goodReply:
        '{"output":"DP-3","known":true,"path":"/home/u/Pictures/beach.png"}'

    // The plugin's own file, loaded through a Loader rather than instantiated directly: its
    // root type comes from a Plasma import, and a failed import inside a Component surfaces as
    // a load error on the Loader instead of as a thrown QML error nothing here would catch.
    Component {
        id: plugin
        Loader {
            width: 800
            height: 600
            source: "../../kde/kpackage/contents/ui/main.qml"
        }
    }

    // The seven cases, each with the drive that reaches it, the state main.qml must derive, and
    // a fragment of the message that names *that* cause and no other. Fragments, not whole
    // strings, so a wording fix does not fail the suite -- but see the uniqueness check below,
    // which is what makes a fragment worth asserting.
    function causeCases() {
        return [
            {
                tag: "1-unnamed",
                connector: "",
                drive: null,
                state: "unnamed",
                says: "has not reported a connector name"
            },
            {
                // Same cause, but reached with an answer already applied: the connector test has
                // to come first in the chain, or a screen with no name is described by whatever
                // the daemon last said about some other screen. Controlled by demoting that arm
                // below the reply arms, which reds this row alone with 'Actual (): set'.
                tag: "1-unnamed-outranks-an-answer",
                connector: "",
                drive: [200, suite.goodReply],
                state: "unnamed",
                says: "has not reported a connector name"
            },
            {
                tag: "2-unreachable",
                connector: suite.namedScreen,
                drive: [0, ""],
                state: "unreachable",
                says: "Start the wallpaperx-daemon service"
            },
            {
                tag: "3-refused",
                connector: suite.namedScreen,
                drive: [400, '{"error":"output name is empty"}'],
                state: "refused",
                says: "refused this screen's request"
            },
            {
                tag: "4-unreadable-not-json",
                connector: suite.namedScreen,
                drive: [200, "<html>502 Bad Gateway</html>"],
                state: "unreadable",
                says: "cannot read"
            },
            {
                // JSON, but not an object: parses fine and then has no members, which is a
                // different way into the same cause and the one that would throw if it reached
                // the binding.
                tag: "4-unreadable-json-but-not-an-object",
                connector: suite.namedScreen,
                drive: [200, "42"],
                state: "unreadable",
                says: "cannot read"
            },
            {
                tag: "5-unknown",
                connector: suite.namedScreen,
                drive: [200, '{"output":"DP-3","known":false}'],
                state: "unknown",
                says: "does not know this screen"
            },
            {
                tag: "6-unset",
                connector: suite.namedScreen,
                drive: [200, '{"output":"DP-3","known":true,"path":""}'],
                state: "unset",
                says: "No wallpaper chosen"
            },
            {
                tag: "7-set",
                connector: suite.namedScreen,
                drive: [200, suite.goodReply],
                state: "set",
                says: "will draw"
            }
        ];
    }

    // GOAL: a driven check must observe the state it drove, not one the plugin's own 1s poll
    //       overwrote. Falsifiable: with the poll left running, a case driven to "set" reads
    //       back "unreachable" the moment the event loop turns (measured, not assumed).
    // WHY-THIS-SHAPE: the timer is found by the property only a Timer has rather than by index
    //       into data[], because the index depends on declaration order in a production file
    //       these tests must not constrain.
    // CONTROL: leaving the poll running was observed first -- an item driven to "set" read back
    //       "unreachable" after wait(1500). The count assert was seen red by misspelling the
    //       property the scan looks for: 'the plugin has exactly one poll to quiesce. Actual
    //       (): 0. Expected (): 1'. Both reverted.
    // COVERAGE-ASSERT: the count is asserted, so a rename that makes this scan find nothing --
    //       and quietly restores the clobbering the whole suite depends on avoiding -- fails.
    function quiesceThePoll(item) {
        let stopped = 0;
        for (let i = 0; i < item.data.length; ++i) {
            if (item.data[i].triggeredOnStart !== undefined) {
                item.data[i].running = false;
                stopped += 1;
            }
        }
        compare(stopped, 1, "the plugin has exactly one poll to quiesce");
    }

    // Loads the real main.qml, stops its poll, and puts it on the named screen the case wants.
    // createTemporaryObject so each case gets a plugin with no history from the last one.
    function loadPlugin(connector) {
        const host = createTemporaryObject(plugin, suite);
        verify(host !== null, "the plugin component was created: " + plugin.errorString());
        compare(host.status, Loader.Ready,
                "main.qml loaded (Loader status " + host.status + ")");
        const item = host.item;
        verify(item !== null, "main.qml produced an item");
        quiesceThePoll(item);
        item.connector = connector;
        return item;
    }

    function drivenPlugin(testCase) {
        const item = loadPlugin(testCase.connector);
        if (testCase.drive !== null) {
            item.apply(testCase.drive[0], testCase.drive[1]);
        }
        return item;
    }

    // GOAL: the harness can load the plugin's real main.qml headless. False if WallpaperItem
    //       needed a Plasma shell, or the offscreen platform refused -- in which case every
    //       check below would be reporting on nothing.
    // CONTROL: the Loader's source changed to a file that does not exist -- red with 'main.qml
    //       loaded (Loader status 3). Actual (): 3. Expected (): 1', i.e. Loader.Error rather
    //       than Loader.Ready, on every check in the file. Reverted.
    function test_harness_loads_the_plugin() {
        const item = loadPlugin(suite.namedScreen);
        compare(item.connector, suite.namedScreen, "the connector seam takes a test's value");
        verify(typeof item.describe === "function", "the plugin exposes the message it renders");
        verify(typeof item.apply === "function", "the plugin exposes the reply entry point");
    }

    function test_each_cause_is_told_apart_data() {
        return causeCases();
    }

    // GOAL: each of the six failure causes (plus the wallpaper case) derives its own state AND
    //       renders a message that names that cause. False if any drive lands on another
    //       cause's state, or renders nothing, or renders text that does not mention its cause.
    // WHY-THIS-SHAPE: both halves are asserted because they can disagree -- describe() reads
    //       screenState, so a correct state with a mis-cased comparison in describe() falls
    //       through to the "will draw" arm and shows a wallpaper message on a broken screen.
    //       Asserting the state alone would call that green.
    // CONTROL: one mutation of main.qml per cause, each reverted. `root.reply.known !== true`
    //       replaced by `false` reds row 5-unknown with 'the state this drive derives. Actual
    //       (): unset. Expected (): unknown'; the unreadable arm of describe() made to return
    //       the not-running text reds both row 4s on the message half instead, with 'looked for
    //       [cannot read] in [WallpaperX is not running....]'. Per-cause list in the report.
    function test_each_cause_is_told_apart(testCase) {
        const item = drivenPlugin(testCase);
        compare(item.screenState, testCase.state, "the state this drive derives");
        const shown = item.describe();
        verify(shown.length > 0, "the message is not blank: a blank desktop is what a plugin "
                                 + "that is not installed looks like");
        verify(shown.indexOf(testCase.says) !== -1,
               "the message names this cause -- looked for [" + testCase.says
               + "] in [" + shown + "]");
    }

    // GOAL: no two causes share a message. False if any pair is equal -- which is the S10.10
    //       defect exactly: a refusal collapsed onto "not running" tells the user to start a
    //       service that is already up, while the daemon had supplied the real reason.
    // WHY-THIS-SHAPE: every pair, not just the refused/unreachable pair the defect came from;
    //       a per-pair loop is what makes a *future* collapse of any other two arms fail here.
    // CONTROL: the unreadable arm of describe() changed to return the not-running string -- red
    //       with 'two causes share one message: 2-unreachable and 4-unreadable-not-json both
    //       show [WallpaperX is not running.\nStart the wallpaperx-daemon service.]'. Reverted.
    // COVERAGE-ASSERT: the pair count is asserted, so trimming the case table (or a `continue`
    //       that skips rows) fails instead of quietly comparing less. Seen red by shortening
    //       the inner loop by one: 'every distinct-cause pair was compared. Actual (): 26.
    //       Expected (): 34'. Reverted.
    function test_no_two_causes_share_a_message() {
        const cases = causeCases();
        const shown = [];
        for (let i = 0; i < cases.length; ++i) {
            const message = drivenPlugin(cases[i]).describe();
            verify(message.length > 0, cases[i].tag + " renders a blank desktop");
            shown.push(message);
        }

        let compared = 0;
        for (let a = 0; a < shown.length; ++a) {
            for (let b = a + 1; b < shown.length; ++b) {
                // The two rows that drive the same cause on purpose are the one pair allowed
                // to match; every other pair is two remedies and must read differently.
                if (cases[a].state === cases[b].state) {
                    continue;
                }
                compared += 1;
                verify(shown[a] !== shown[b],
                       "two causes share one message: " + cases[a].tag + " and " + cases[b].tag
                       + " both show [" + shown[a] + "]");
            }
        }
        compare(compared, 34, "every distinct-cause pair was compared");
    }

    // GOAL: a message names its own cause and no other, so the user is not handed two remedies
    //       at once. False if a fragment that identifies one cause appears in another's message.
    // WHY-THIS-SHAPE: the fragment-per-message matrix, rather than each row checking only its
    //       own fragment, is what stops the fragments from going vacuous: a describe() that
    //       concatenated every explanation would satisfy "contains my fragment" for all seven.
    // CONTROL: `root.answered = false` added to apply()'s non-200 path, the historical collapse
    //       -- red with 'the fragment [Start the wallpaperx-daemon service] names 2 causes
    //       (unreachable, refused), not 1'. Reverted.
    // COVERAGE-ASSERT: the number of fragment/message tests is asserted at cases squared. Seen
    //       red by shortening the inner loop by one, which drops the last message and takes the
    //       "will draw" fragment to 0 causes before the count is even reached. Reverted.
    function test_a_message_names_only_its_own_cause() {
        const cases = causeCases();
        const shown = [];
        for (let i = 0; i < cases.length; ++i) {
            shown.push(drivenPlugin(cases[i]).describe());
        }

        let tested = 0;
        for (let f = 0; f < cases.length; ++f) {
            // Which causes this fragment turns up in. Counted by state, not by row: two rows
            // may drive the same cause on purpose and then share its message by construction.
            const named = [];
            for (let m = 0; m < shown.length; ++m) {
                tested += 1;
                if (shown[m].indexOf(cases[f].says) !== -1
                        && named.indexOf(cases[m].state) === -1) {
                    named.push(cases[m].state);
                }
            }
            compare(named.length, 1, "the fragment [" + cases[f].says + "] names "
                    + named.length + " causes (" + named.join(", ") + "), not 1");
        }
        compare(tested, cases.length * cases.length, "every fragment was tried on every message");
    }

    // GOAL: when the daemon refuses, the reason it put in the reply body is what the desktop
    //       shows. False if the message drops it -- the user is then told a request was refused
    //       with no way to learn why, and the daemon had already said.
    // WHY-THIS-SHAPE: an unmistakable reason string is used so `indexOf` cannot pass on a
    //       coincidence with the plugin's own wording, and the no-reason path is driven too:
    //       without it, a describe() that ignored refusalReason and printed the status would
    //       still look right on the first case.
    // CONTROL: the refused arm reduced to a message with no `.arg(root.refusalReason)` -- red
    //       with 'the daemon's own reason reaches the desktop -- looked for [output "DP-9" is
    //       not a connector this daemon knows] in [WallpaperX is running, but refused this
    //       screen's request.]'. Reverted.
    function test_the_refusal_message_carries_the_daemon_reason() {
        const reason = 'output "DP-9" is not a connector this daemon knows';
        const item = loadPlugin(suite.namedScreen);
        item.apply(404, JSON.stringify({ error: reason }));

        compare(item.screenState, "refused", "a non-200 with a reason is a refusal");
        const shown = item.describe();
        verify(shown.indexOf(reason) !== -1,
               "the daemon's own reason reaches the desktop -- looked for [" + reason
               + "] in [" + shown + "]");

        // A refusal whose body says nothing usable still has to be a refusal, and still has to
        // carry the one fact there is: the status.
        const bare = loadPlugin(suite.namedScreen);
        bare.apply(503, "");
        compare(bare.screenState, "refused", "a non-200 with no readable body is still a refusal");
        verify(bare.describe().indexOf("503") !== -1,
               "the status stands in when the body carries no reason: [" + bare.describe() + "]");
    }

    // GOAL: a refusal is never reported as "the daemon is not running". This is the defect the
    //       goal was written from, stated on its own so a regression prints as itself.
    // WHY-THIS-SHAPE: asserted as the absence of the other cause's remedy rather than only as
    //       state equality, because the state name is not what the user reads; a describe()
    //       that mapped "refused" onto the not-running text would pass a state-only check.
    // CONTROL: `root.answered = false` added to apply()'s non-200 path -- red with 'a refusal
    //       must not send the user to start a daemon that answered it: [WallpaperX is not
    //       running.\nStart the wallpaperx-daemon service.]'. Reverted.
    function test_a_refusal_is_not_reported_as_a_dead_daemon() {
        const item = loadPlugin(suite.namedScreen);
        item.apply(409, '{"error":"another client holds this output"}');

        const shown = item.describe();
        verify(shown.indexOf("Start the wallpaperx-daemon service") === -1,
               "a refusal must not send the user to start a daemon that answered it: [" + shown + "]");
        verify(shown.indexOf("is not running") === -1,
               "a refusal must not report the daemon as down: [" + shown + "]");
    }

    // GOAL: a cause stops being shown once it stops applying. False if a refusal's reason
    //       outlives the refusal -- the desktop would then explain a request that succeeded, or
    //       blame a daemon that has since gone away, with the wrong remedy either way.
    // WHY-THIS-SHAPE: driven as sequences on one instance rather than as fresh instances per
    //       state, because the fields describe() reads are carried across polls; a fresh
    //       instance per case can never observe a stale one.
    // CONTROL: `root.refusalReason = "";` deleted from apply()'s 200 path -- red with 'a served
    //       wallpaper after a refusal. Actual (): refused. Expected (): set'. The second half
    //       was seen red separately by apply()'s status-0 path setting answered true: 'the
    //       daemon going away after a refusal. Actual (): unreadable'. Both reverted.
    function test_a_cause_does_not_outlast_itself() {
        const item = loadPlugin(suite.namedScreen);

        item.apply(400, '{"error":"output name is empty"}');
        compare(item.screenState, "refused", "the refusal was reached");
        item.apply(200, suite.goodReply);
        compare(item.screenState, "set", "a served wallpaper after a refusal");
        verify(item.describe().indexOf("refused") === -1,
               "the old refusal is gone from the desktop: [" + item.describe() + "]");

        item.apply(400, '{"error":"output name is empty"}');
        compare(item.screenState, "refused", "the refusal was reached again");
        item.apply(0, "");
        compare(item.screenState, "unreachable", "the daemon going away after a refusal");
        verify(item.describe().indexOf("refused") === -1,
               "a departed daemon is not still refusing: [" + item.describe() + "]");
    }

    // GOAL: the message reaches the desktop, on an opaque backdrop. False if describe() is
    //       right but nothing draws it -- which is the blank desktop the goal names, and is
    //       indistinguishable from the plugin not being installed.
    // WHY-THIS-SHAPE: the rendered Text is read for every case rather than trusting the binding
    //       once, because `text: root.describe()` is a binding on a function with no observable
    //       dependency on some paths; checking one state would not show that it re-evaluates.
    //       The backdrop's alpha is asserted because a transparent failure message over an
    //       unknown desktop is the same blank desktop by another route.
    // CONTROL: three mutations of main.qml, each reverted. Backdrop colour to "transparent" --
    //       'the backdrop is opaque. Actual (): 0. Expected (): 1'. The Text's binding replaced
    //       by "" -- 'the desktop shows the message for 1-unnamed. Actual (): '. `visible:
    //       false` on the Text -- 'the message item is visible for 1-unnamed returned FALSE'.
    // COVERAGE-ASSERT: the number of rendered comparisons is asserted at one per case. Seen red
    //       by shortening the loop: 'every case was checked as rendered. Actual (): 7'.
    function test_the_desktop_shows_the_message() {
        const cases = causeCases();
        let rendered = 0;
        for (let i = 0; i < cases.length; ++i) {
            const item = drivenPlugin(cases[i]);

            // By "radius", not "color": the message Text carries a color too.
            const backdrop = onlyDescendantWith(item, "radius");
            compare(backdrop.color.a, 1, "the backdrop is opaque");
            compare(backdrop.width, item.width, "the backdrop covers the screen");
            compare(backdrop.height, item.height, "the backdrop covers the screen");

            const text = onlyDescendantWith(item, "wrapMode");
            verify(text.visible, "the message item is visible for " + cases[i].tag);
            compare(text.color.a, 1, "the message is not drawn in transparent ink");
            compare(text.text, item.describe(),
                    "the desktop shows the message for " + cases[i].tag);
            verify(text.paintedHeight > 0,
                   "the message takes up space on the desktop for " + cases[i].tag
                   + " [" + text.text + "]");
            rendered += 1;
        }
        compare(rendered, cases.length, "every case was checked as rendered");
    }

    // The one visual descendant carrying `property`, found by walking the tree rather than by
    // index: the indices depend on declaration order in a production file these tests must not
    // pin down. Asserting there is exactly one is what makes the result unambiguous -- with two
    // matches the checks above would silently grade whichever came first.
    function onlyDescendantWith(item, property) {
        const found = [];
        const queue = [item];
        while (queue.length > 0) {
            const node = queue.shift();
            for (let i = 0; i < node.children.length; ++i) {
                const child = node.children[i];
                if (child[property] !== undefined) {
                    found.push(child);
                }
                queue.push(child);
            }
        }
        compare(found.length, 1, "exactly one descendant carries [" + property + "]");
        return found[0];
    }
}
