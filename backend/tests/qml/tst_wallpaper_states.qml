// SPDX-License-Identifier: GPL-2.0-or-later

// S10.10: whatever has gone wrong, the plugin says something visible and says which cause it
// is, in characters rather than in markup. The seven cases main.qml derives are driven here
// through its real file -- not a copy of its logic, which would drift -- and both halves are
// asserted for each: the state the binding produces and the message describe() hands to the
// Text that lands on the desktop.
//
// The plugin is driven, never polled: apply(status, body) is the entry point its own read's
// handler calls, so the cause cases need no HTTP server and wait on nothing.

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
    // name, which is why main.qml takes an override the connector binding prefers when set.
    readonly property string namedScreen: "DP-3"

    // Spelled once. The case table below and the recovery check both key on it.
    readonly property string unreachableFragment: "Start the wallpaperx-daemon service"

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

    // A yardstick Text the markup check lays the same string out in, once as literal characters
    // and once as markup. Every property that moves a glyph is copied off the plugin's own Text
    // at use, so this stays a comparison of formats and not of two different fonts.
    Component {
        id: yardstick
        Text {}
    }

    // A stand-in for the renderer module, which this harness has no import path for. It carries
    // the properties main.qml exchanges with the loaded item and nothing else -- every binding
    // that decides WHETHER to load it stays main.qml's own.
    Component {
        id: standInRenderer
        Item {
            property int monitorId: -1
            property string output: ""
            property string unavailableReason: ""
            property string pauseMode: "unset by main.qml"
            // The real item reports this only once a frame is on screen. The stand-in draws
            // nothing, so a check that wants the backdrop hidden must set it.
            property bool frameShown: false
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
                says: suite.unreachableFragment
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

    // A stand-in for the change watch main.qml loads from the same absent module: the one signal
    // main.qml listens to, which a case emits to stand for the daemon's ConfigChanged, and the
    // plugin's real read behind fetch() -- made by a real ConfigWatch (compiled into this harness)
    // owned by this stand-in, so the reads go with the plugin instance as they do in plasmashell.
    Component {
        id: standInWatch
        Item {
            signal configChanged()
            readonly property QtObject real: readProbe.watchFor(this)
            function fetch(output) {
                return readProbe.fetchFrom(real, output);
            }
            // main.qml asks this after every failed read before scheduling a retry.
            function daemonAbsent() {
                return real.daemonAbsent();
            }
        }
    }

    // Every HTTP case needs the stub; its absence may not read as a pass.
    function requireRealRead() {
        if (!stubDaemon.available()) {
            skip(stubDaemon.unavailableReason());
        }
    }

    // GOAL (a property of this harness): every case but the ones about the watch itself runs with a
    //       change watch that loaded, so describe() reaches the cause the case drove. False with the
    //       real watch left in Loader.Error: describe() then answers "cannot follow changes" for every
    //       named cause, and each cause check would grade that one message.
    // WHY-THIS-SHAPE: found by objectName, not by index, for the reason childNamed() gives; the real
    //       watch is required to have FAILED first, so the stand-in replaces a load that could not
    //       work rather than one that might have.
    // CONTROL: test-side, by hand 2026-10-07, `watch.sourceComponent = standInWatch;` deleted below ->
    //       'the stand-in change watch loaded. Actual (): 3 Expected (): 1' on 34 of 45 cases. Restored.
    function standInTheWatch(item) {
        const watch = childNamed(item, "wallpaperxConfigWatch");
        verify(watch !== null, "main.qml has a change watch");
        compare(watch.status, Loader.Error, "the real change watch cannot load on this harness");
        watch.sourceComponent = standInWatch;
        compare(watch.status, Loader.Ready, "the stand-in change watch loaded");
        return watch;
    }

    // The real main.qml, from a fresh Loader. createTemporaryObject so each case gets a plugin with no
    // history from the last one.
    function loadHost() {
        const host = createTemporaryObject(plugin, suite);
        verify(host !== null, "the plugin component was created: " + plugin.errorString());
        compare(host.status, Loader.Ready,
                "main.qml loaded (Loader status " + host.status + ")");
        verify(host.item !== null, "main.qml produced an item");
        return host;
    }

    // Loads the real main.qml with the stand-in watch and puts it on the named screen the case wants.
    // Naming the screen makes main.qml ask the stub once; that answer is waited out, so a case that
    // drives apply() is not overwritten by it afterwards.
    function loadPlugin(connector) {
        const item = loadHost().item;
        standInTheWatch(item);
        item.connectorOverride = connector;
        tryVerify(function () { return item.pending === null; }, 5000,
                  "the read naming the screen made has settled");
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
            shown.push(drivenPlugin(cases[i]).describe());
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

    // Put at the END of an overlong reply field, so a clamp that did not happen shows up as
    // these characters arriving on the desktop rather than as a length nobody can picture.
    readonly property string clampTail: "TAIL-PAST-THE-LIMIT"

    function overlongValue(length) {
        let filler = "";
        while (filler.length < length) {
            filler += "0123456789";
        }
        return filler.substring(0, length - suite.clampTail.length) + suite.clampTail;
    }

    // The two reply fields that reach a text layout. `refusalReason` is the third and is
    // covered by its own case above; these two were the ones with no bound.
    function clampCases() {
        return [
            { tag: "the wallpaper path", key: "path", limit: 4096,
              field: "wallpaperPath", state: "set" },
            { tag: "the screen name", key: "output", limit: 256,
              field: "screenName", state: "unknown" }
        ];
    }

    function test_an_overlong_reply_field_is_clamped_data() {
        return clampCases();
    }

    // GOAL: a reply field the desktop message interpolates is bounded before it reaches the
    //       text layout. False if the plugin lays out whatever arrived: the reply comes from
    //       whatever holds port 18462, the layout runs on plasmashell's GUI thread, and a
    //       megabyte-long "path" is then a way to stall every screen's shell from loopback.
    // WHY-THIS-SHAPE: the clamped VALUE and the rendered message are both asserted, and the
    //       overlong fixture ends in a marker so the second one can be stated as "this tail
    //       never reached the desktop". Asserting only `length <= limit` on the property is the
    //       obvious simpler version and is weaker twice over: it passes for a plugin that
    //       clamped the property but interpolated `reply.path` directly into describe(), and it
    //       passes for one that dropped the field entirely. Equality against the first `limit`
    //       characters is what rules the second out.
    //       The state is asserted first, because each field is only interpolated by one arm of
    //       describe() -- a drive that landed elsewhere would grade a message that never
    //       contained the field at all.
    // CONTROL: one per row, both in main.qml, both observed 2026-08-19 and both reverted.
    //       `.substring(0, 4096)` dropped from the `wallpaperPath` binding -> "FAIL: the plugin
    //       clamps the wallpaper path. Actual (): 8192. Expected (): 4096", that row only, the
    //       screen-name row and the other 20 tests green. `.substring(0, 256)` dropped from the
    //       `screenName` binding -> "FAIL: the plugin clamps the screen name. Actual (): 512.
    //       Expected (): 256", that row only. QTest stops a data row at its first failure, so
    //       the tail-on-the-desktop check below is not reached under either mutation; it is
    //       there for a describe() that stopped using the clamped property, which the length
    //       check cannot see.
    // COVERAGE-ASSERT: the fixture is asserted to be twice the limit and to carry the marker in
    //       its last characters, so an edit that shortened it below the limit would leave the
    //       clamp untested rather than leave this green.
    function test_an_overlong_reply_field_is_clamped(testCase) {
        const value = suite.overlongValue(testCase.limit * 2);
        compare(value.length, testCase.limit * 2, "the fixture is longer than the limit");
        compare(value.indexOf(suite.clampTail), value.length - suite.clampTail.length,
                "the fixture carries its marker past the limit");

        const reply = { output: suite.namedScreen, known: testCase.state === "set" };
        if (testCase.state === "set") {
            reply.path = "/photos/short.png";
        }
        reply[testCase.key] = value;
        const item = drivenPlugin({ connector: suite.namedScreen,
                                    drive: [200, JSON.stringify(reply)] });

        compare(item.screenState, testCase.state,
                "the drive reaches the arm that interpolates " + testCase.tag);
        compare(item[testCase.field].length, testCase.limit,
                "the plugin clamps " + testCase.tag);
        compare(item[testCase.field], value.substring(0, testCase.limit),
                "the clamp keeps the front of " + testCase.tag + " rather than dropping it");

        const shown = onlyDescendantWith(item, "wrapMode");
        compare(shown.text.indexOf(suite.clampTail), -1,
                "nothing past the limit reaches the desktop for " + testCase.tag);
    }

    // ---------------------------------------------------------------------------------- //
    // What the plugin's read decides before main.qml's apply(), which every case above drives.
    // Reaching it needs a real HTTP reply, so these run against the stub daemon tests/qml_states_test.cpp puts on 127.0.0.1:18462 --
    // see that file for why it is that port and not one of the harness's choosing.
    // ---------------------------------------------------------------------------------- //

    // Loads a plugin whose naming read has been answered (loadPlugin waits it out), and resets the
    // stub's counts and shape, so the case's own refresh() is the only request counted. Reset before
    // the load too: the naming read is served in whatever shape the previous case left, and a plugin
    // that starts out "unreadable" would satisfy a case's wait for that state with no request at all.
    function pluginReadyToFetch() {
        stubDaemon.reset();
        const item = loadPlugin(suite.namedScreen);
        compare(item.screenState, "set", "the naming read was answered with the stub's good reply");
        stubDaemon.reset();
        return item;
    }

    // GOAL (a property of this harness): this engine refuses every request QML itself makes, as the
    //       lock screen's (kscreenlocker_greet) does. False if the refusing network-manager factory
    //       is not installed -- then every read case below would pass for a read made through QML's
    //       own XMLHttpRequest too, and none would show the plugin reads past the refusal.
    // WHY-THIS-SHAPE: the stub's count is the verdict, not the status: a refused request and an
    //       unanswered one both end at status 0. The factory's own count shows it was asked.
    // CONTROL: test-side, control_batch.sh 2026-11-02, reverted: setNetworkAccessManagerFactory deleted from
    //       tests/qml_states_test.cpp -> "a request made through QML's engine never reached the stub" red.
    function test_this_engine_refuses_qml_requests_like_the_lock_screen() {
        if (!stubDaemon.available()) {
            skip(stubDaemon.unavailableReason());
        }
        stubDaemon.reset();
        const refusedBefore = refusingNetwork.refused();
        const request = new XMLHttpRequest();
        let done = false;
        request.onreadystatechange = function () {
            if (request.readyState === XMLHttpRequest.DONE) {
                done = true;
            }
        };
        request.open("GET", "http://127.0.0.1:18462/config?output=DP-3");
        request.send();
        tryVerify(function () { return done; }, 5000, "QML's own request finished");
        compare(stubDaemon.requests(), 0, "a request made through QML's engine never reached the stub");
        compare(request.status, 0, "and it ended as nothing answering");
        verify(refusingNetwork.refused() > refusedBefore, "the refusing network manager was asked");
    }

    // GOAL: S15.1207 -- a /config read succeeds in an engine that refuses every request QML makes (the
    //       lock screen's setup): the stub sees it, and the screen reaches "set". Also, as a property of
    //       this harness, the stub serves a reply the plugin accepts over real HTTP, so the cases below
    //       that end "unreadable" or "refused" do so for their own reason.
    // WHY-THIS-SHAPE: the same body the oversized case pads and the same body the redirect
    //       target serves, so those differ from this one in exactly one respect each.
    //       The request count is asserted, not just the state: a plugin that never sent
    //       anything and a stub that never answered both leave "unreachable", and only the
    //       counter tells them apart.
    //       The wait is for this request's answer (`pending` cleared), not for "set": the naming
    //       read already left "set", so a state wait returned before the request reached a stub
    //       slowed by load, and counted 0 requests.
    // CONTROL: control_batch.sh 2026-11-02, reverted, tests/qml_states_test.cpp, each only after
    //       serveGood(): serveGood() serving 500 -> "a reply served over HTTP reaches the set state.
    //       Actual (): refused. Expected (): set"; serveGood() stalling 12 s -> "'the refresh's
    //       request was answered' returned FALSE" (ConfigRead's 5 s timeout and this 5 s wait race;
    //       either line is this case going red). The lock-screen half: control_batch.sh 2026-11-02,
    //       reverted, read() in backend/kde/kpackage/contents/ui/main.qml made to read through
    //       XMLHttpRequest -> "the naming read was answered with the stub's good reply" red here and in
    //       every HTTP case.
    function test_the_stub_serves_a_reply_the_plugin_accepts() {
        requireRealRead();
        const item = pluginReadyToFetch();
        stubDaemon.serveGood();
        item.refresh();

        verify(item.pending !== null, "the refresh sent a request");
        tryVerify(function () { return item.pending === null; }, 5000, "the refresh's request was answered");
        compare(item.screenState, "set", "a reply served over HTTP reaches the set state");
        compare(stubDaemon.requests(), 1, "the plugin made exactly one request to the daemon's origin");
        compare(item.wallpaperPath, "/home/u/Pictures/beach.png", "the served path reached the binding");
    }

    // GOAL: S15.1209 -- a 3xx from the daemon's port is not followed: the redirect target sees no
    //       request and the screen shows a refusal naming the status, not the target's answer.
    // WHY-THIS-SHAPE: the target serves the SAME good body the accepted case above serves, so a
    //       followed redirect shows "set" and the target's count reads 1. The target is a second
    //       real listener on an ephemeral port, so "elsewhere" does not rest on name resolution.
    // CONTROL: control_batch.sh 2026-11-02, reverted: configNetwork()'s ManualRedirectPolicy made
    //       NoLessSafeRedirectPolicy in backend/kde/config_read.cpp -> "the redirect target was never
    //       asked" red; re-run on config_read.cpp, control_batch.sh 2026-11-02, same line red.
    function test_a_redirect_is_not_followed() {
        requireRealRead();
        const item = pluginReadyToFetch();
        stubDaemon.serveFromAnotherOrigin();
        item.refresh();

        verify(item.pending !== null, "the refresh sent a request");
        tryVerify(function () { return item.pending === null; }, 5000, "the refresh's request was answered");
        compare(stubDaemon.altRequests(), 0, "the redirect target was never asked");
        compare(item.screenState, "refused", "a redirect reads as a refusal, not as the target's answer");
        compare(stubDaemon.requests(), 1, "the plugin asked the daemon's port once");
        verify(item.describe().indexOf("302") !== -1,
               "the desktop message names the redirect's status: [" + item.describe() + "]");
    }

    // Waits, with events processed, until fewer than `count` reads are alive, or `ms` (default 5 s)
    // pass. The caller grades the count after: this returns either way, and never decides anything.
    function waitForLiveReadsBelow(count, ms) {
        const rounds = (ms === undefined ? 5000 : ms) / 100;
        for (let t = 0; t < rounds && readProbe.liveReads() >= count; ++t) {
            wait(100);
        }
    }

    // GOAL: S15.1212 -- after N reads, each answered and handled by main.qml's own handler, no read
    //       object is left alive while the plugin lives. False if a read is kept after it finished.
    // WHY-THIS-SHAPE: counted from C++ through destroyed(), not by forcing gc(): what is in question is
    //       whether anything frees the read, and a collector run would grade the collector. The plugin
    //       is kept alive to the end, so its watch -- the reads' parent -- cannot be what freed them.
    // CONTROL: control_batch.sh 2026-11-02, reverted: `deleteLater();` deleted from ConfigRead::finish()
    //       (backend/kde/config_read.cpp) -> "every answered read was freed while its plugin lives.
    //       Actual (): 11 Expected (): 0". A second break, fetch() making the read with no parent, no C++
    //       ownership and no deleteLater -> the same line. Both re-run on config_read.cpp,
    //       control_batch.sh 2026-11-02, both red on that line.
    // COVERAGE-ASSERT: each refresh is required to have made exactly one read, and all N to be answered
    //       and applied, so the count is graded over N handled reads and not over none.
    function test_every_handled_read_is_freed() {
        requireRealRead();
        const item = pluginReadyToFetch();
        const madeBefore = readProbe.madeReads();
        const rounds = 8;
        for (let i = 0; i < rounds; ++i) {
            item.refresh();
            verify(item.pending !== null, "refresh " + i + " sent a read");
            tryVerify(function () { return item.pending === null; }, 5000, "read " + i + " was answered");
            compare(item.screenState, "set", "read " + i + " was applied");
        }
        compare(readProbe.madeReads() - madeBefore, rounds, "each refresh made exactly one read");
        compare(stubDaemon.requests(), rounds, "each read reached the stub");
        waitForLiveReadsBelow(1);
        compare(readProbe.liveReads(), 0, "every answered read was freed while its plugin lives");
        verify(item.pending === null, "the plugin itself still exists");
    }

    // GOAL: S15.1213 -- every finished handler of a read sees its real status and body, and the read
    //       is freed only after all of them have run. False if the result is stored after emitting, or
    //       if the read is freed before or during the emission (later handlers then never run).
    // WHY-THIS-SHAPE: three handlers, not one: a read freed by the first would still pass a one-handler
    //       case. The wait is for the read's destruction, not for the handlers, so a read freed without
    //       its handlers running ends the wait and is graded rather than waited out.
    // CONTROL: control_batch.sh 2026-11-02, reverted, ConfigRead::finish() in
    //       backend/kde/config_read.cpp, both re-run there 2026-11-02:
    //       `m_status`/`m_body` assigned after `emit finished()` ->
    //       "handler 0 saw the real status. Actual (): 0 Expected (): 200"; `deleteLater(); emit
    //       finished();` made `delete this; return;` -> "every finished handler ran before the read was
    //       freed. Actual (): 0 Expected (): 3".
    function test_every_finished_handler_sees_the_answer_before_the_read_is_freed() {
        requireRealRead();
        stubDaemon.reset();
        const destroyedBefore = readProbe.destroyedReads();
        const read = readProbe.fetch("DP-3");
        verify(read !== null, "a read was made");
        const seen = [];
        for (let h = 0; h < 3; ++h) {
            read.finished.connect(function () {
                seen.push({ handler: h, status: read.status, body: read.responseText });
            });
        }
        for (let t = 0; t < 50 && readProbe.destroyedReads() === destroyedBefore; ++t) {
            wait(100);
        }
        verify(readProbe.destroyedReads() > destroyedBefore, "the read was freed once it finished");
        compare(seen.length, 3, "every finished handler ran before the read was freed");
        for (let i = 0; i < seen.length; ++i) {
            compare(seen[i].status, 200, "handler " + seen[i].handler + " saw the real status");
            compare(seen[i].body, suite.goodReply, "handler " + seen[i].handler + " saw the real body");
        }
    }

    // Loads a plugin, holds the stub's answers, names the screen so main.qml sends its read, and
    // returns once the stub holds that read. The caller releases the stub (reset or releaseHeld).
    function pluginWithAHeldRead() {
        stubDaemon.reset();
        const host = loadHost();
        standInTheWatch(host.item);
        stubDaemon.holdAnswers();
        const liveBefore = readProbe.liveReads();
        host.item.connectorOverride = suite.namedScreen;
        verify(host.item.pending !== null, "naming the screen sent a read");
        tryVerify(function () { return stubDaemon.requests() === 1; }, 5000, "the stub holds the read");
        compare(readProbe.liveReads() - liveBefore, 1, "exactly one read is in flight");
        return { host: host, liveBefore: liveBefore };
    }

    // GOAL: S15.1214 -- a plugin instance destroyed while its read is held unanswered frees that read
    //       before the answer comes. False if the read outlives its plugin (it would then finish later,
    //       and run main.qml's handler against a plugin that no longer exists).
    // WHY-THIS-SHAPE: the stub holds the answer until released, so "before the answer" is a fact of
    //       the run, asserted below, rather than a race the fast path usually wins. A separate case
    //       from the one about the late answer, so each half is graded alone under the same break.
    //       The wait is bounded well below the read's own timeout and the read must never have
    //       finished: a read that outlived its plugin is ended by that timeout and frees itself, so a
    //       5 s wait on the live count alone passes for it.
    // CONTROL: control_batch.sh 2026-11-03, reverted: ConfigWatch::fetch() in backend/kde/config_read.cpp making the read with no
    //       parent -> "a read held unanswered is freed with its plugin. Actual (): 1 Expected (): 0" red.
    //       Test-side, same batch: boundMs made 6000 -> "the wait (6000 ms) ends well before a read's own
    //       timeout (5000 ms)" red.
    // COVERAGE-ASSERT: the wait bound is asserted to be below ConfigRead::kTimeoutMs, read off the
    //       constant, so raising the bound or lowering the timeout fails here instead of going vacuous.
    function test_a_plugin_destroyed_mid_read_frees_the_read() {
        requireRealRead();
        const boundMs = 2000;
        verify(boundMs < readProbe.timeoutMs() - 1000,
               "the wait (" + boundMs + " ms) ends well before a read's own timeout (" + readProbe.timeoutMs() + " ms)");
        try {
            const held = pluginWithAHeldRead();
            const finishedBefore = readProbe.finishedReads();
            held.host.destroy();
            waitForLiveReadsBelow(held.liveBefore + 1, boundMs);
            compare(stubDaemon.heldAnswered(), 0, "the answer was still held throughout");
            compare(readProbe.finishedReads(), finishedBefore, "the held read never finished");
            compare(readProbe.liveReads(), held.liveBefore, "a read held unanswered is freed with its plugin");
            // GOAL (a property of this harness): the hold lasted the whole case; one that ran out was
            //       answered 503, which finishes and frees the read before the plugin goes and leaves
            //       every check above green for a reason that is not the plugin's.
            // CONTROL: test-side, control_batch.sh 2026-11-02, reverted: `wait(21000);` before
            //       `held.host.destroy();` -> "no held answer ran out during the case" red.
            compare(stubDaemon.holdsExpired(), 0, "no held answer ran out during the case");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: S15.1215 -- when the answer to a read whose plugin was destroyed arrives, no finished
    //       handler runs (so nothing is applied anywhere) and the process survives. False if the read
    //       outlived its plugin and finished.
    // WHY-THIS-SHAPE: absence needs a window. Its end is a later read's full round trip, made after the
    //       stub has written the held answer, not a fixed sleep: an answer written first would, in
    //       practice, be read first. That ordering is across two connections, so this is a strong
    //       window rather than a proof. The plugin's watch going away is the wait, not the read's
    //       freeing, so this case does not depend on the one above passing.
    // CONTROL: control_batch.sh 2026-11-02, reverted: ConfigWatch::fetch() (backend/kde/
    //       config_read.cpp) making the read with no parent -> "no read finished for the answer that
    //       reached a destroyed plugin. Actual (): 2 Expected (): 1" red.
    function test_an_answer_after_the_plugin_is_destroyed_runs_nothing() {
        requireRealRead();
        try {
            const held = pluginWithAHeldRead();
            const watchesBefore = readProbe.destroyedWatches();
            held.host.destroy();
            tryVerify(function () { return readProbe.destroyedWatches() > watchesBefore; }, 5000,
                      "the plugin's watch went with it");
            // GOAL (a property of this harness): the answer is still unwritten when the plugin has
            //       gone -- neither released early nor run out (an expired hold answers 503 and the read
            //       finishes while its plugin lives) -- so the release below is what sends it.
            // CONTROL: test-side, control_batch.sh 2026-11-02, each reverted: `wait(21000);` before
            //       `held.host.destroy();` -> "no held answer ran out before the plugin went" red;
            //       `stubDaemon.releaseHeld(); wait(300);` before it -> "the held answer was still
            //       unwritten when the plugin went" red.
            compare(stubDaemon.holdsExpired(), 0, "no held answer ran out before the plugin went");
            compare(stubDaemon.heldAnswered(), 0, "the held answer was still unwritten when the plugin went");
            const finishedBefore = readProbe.finishedReads();
            stubDaemon.releaseHeld();
            tryVerify(function () { return stubDaemon.heldAnswered() === 1; }, 5000,
                      "the stub wrote the held answer");
            const later = { done: false };
            const read = readProbe.fetch("DP-3");
            verify(read !== null, "a later read was made");
            read.finished.connect(function () { later.done = true; });
            tryVerify(function () { return later.done; }, 5000, "the later read finished");
            compare(readProbe.finishedReads() - finishedBefore, 1,
                    "no read finished for the answer that reached a destroyed plugin");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: a body larger than the read's bound (ConfigRead::kMaxBodyBytes) is treated as
    //       unreadable rather than parsed. False
    //       if the plugin buffers and parses whatever arrives: the reply comes from whatever
    //       holds the port, nothing else bounds what the shell takes in, and the parse runs on
    //       plasmashell's GUI thread.
    // WHY-THIS-SHAPE: the oversized body is VALID JSON describing the same wallpaper the
    //       accepted case above serves, so its SIZE is the only thing that can make it
    //       unreadable. A check driven with garbage would be satisfied by the JSON parser and
    //       would say nothing about the bound. Served chunked, so no declared length is involved;
    //       the case below serves the same body with one.
    // CONTROL: control_batch.sh 2026-11-03, reverted, ConfigRead in backend/kde/config_read.cpp:
    //       takeArrived() always taking the bytes and returning true, and ending()'s
    //       `received.size() > kMaxBodyBytes` test made `false` -> "an oversized reply with no declared
    //       length is unreadable" red. Not taken for a timeout (S15.1229): control_batch.sh 2026-11-03,
    //       reverted, ending()'s first return made `Outcome::TimedOut` (status kept) -> "the plugin
    //       made exactly one request" red in this, the declared-length and the endless case: the kept
    //       answer is retried. The state line stays green under that break, since the retry's unkept
    //       timeout applies status 200 with no body, which also reads "unreadable".
    // COVERAGE-ASSERT: the served body is asserted to be larger than the read's own bound, read off
    //       ConfigRead's constant rather than written as a literal. A fixture that
    //       stopped being oversized -- or a ceiling raised past 70000 -- fails here instead of
    //       going green against a body the bound never applied to.
    function test_an_oversized_reply_is_unreadable() {
        requireRealRead();
        const item = pluginReadyToFetch();
        verify(stubDaemon.oversizedBytes() > readProbe.maxBodyBytes(),
               "the stub's oversized body (" + stubDaemon.oversizedBytes() + " bytes) is past the read's "
               + "ceiling of " + readProbe.maxBodyBytes());

        stubDaemon.serveOversizedChunked();
        item.refresh();

        tryCompare(item, "screenState", "unreadable", 5000,
                   "an oversized reply with no declared length is unreadable");
        compare(stubDaemon.requests(), 1, "the plugin made exactly one request");
        verify(item.describe().indexOf("cannot read") !== -1,
               "the desktop message names the unreadable cause: [" + item.describe() + "]");
    }

    // GOAL: the same bound holds for a reply that DECLARES its size, and reaching that verdict
    //       does not kill the process. Both halves matter: the read stops such a transfer with
    //       abort() from inside its own readyRead handler, and abort() emits finished() from inside
    //       itself -- a defence against a hostile reply that re-entered the read would be a way for
    //       whatever holds the port to take plasmashell down.
    // WHY-THIS-SHAPE: a separate function from the chunked row above, not a data row beside it.
    //       Under the defect this does not fail, it ABORTS -- and a crash in a shared function
    //       destroys the other row's diagnostics too. Kept apart so a regression prints as one
    //       named case dying rather than as the file going silent.
    // CONTROL: control_batch.sh 2026-11-03, reverted: the break named on the chunked case above
    //       (backend/kde/config_read.cpp) -> "an oversized reply that declares its length is
    //       unreadable" red. The crash half is graded by the endless case below, where the read is
    //       certain to stop inside readyRead; a 70,000-byte body may have finished before it is read.
    function test_an_oversized_reply_that_declares_its_length_is_unreadable() {
        requireRealRead();
        const item = pluginReadyToFetch();
        verify(stubDaemon.oversizedBytes() > readProbe.maxBodyBytes(),
               "the stub's oversized body (" + stubDaemon.oversizedBytes() + " bytes) is past the read's "
               + "ceiling of " + readProbe.maxBodyBytes());

        stubDaemon.serveOversizedWithLength();
        item.refresh();

        tryCompare(item, "screenState", "unreadable", 5000,
                   "an oversized reply that declares its length is unreadable");
        compare(stubDaemon.requests(), 1, "the plugin made exactly one request");
        verify(item.describe().indexOf("cannot read") !== -1,
               "the desktop message names the unreadable cause: [" + item.describe() + "]");
    }

    // GOAL: a body that never ends finishes the read promptly as unreadable, and the plugin closes
    //       the transfer rather than keeps taking it in. False if the bound is checked only once the
    //       body has ended: that read never finishes and buffers for as long as the server sends.
    //       Also: ending such a transfer from inside its own readyRead does not crash the process.
    // WHY-THIS-SHAPE: the 70,000-byte cases above cannot tell the two apart -- a finite body ends,
    //       so an end-of-body check is satisfied too. The stub's own count of ended transfers is the
    //       evidence the plugin let go; memory is not measured, but a read that stopped taking bytes
    //       at the bound cannot hold more than the bound and one chunk.
    // CONTROL: control_batch.sh 2026-11-03, each reverted, in ConfigRead (backend/kde/config_read.cpp):
    //       takeArrived() always appending and returning true, with ending()'s size test kept (the
    //       bound checked only at the end) -> "an endless reply is unreadable" red, the 70,000-byte
    //       cases above staying green; `reply->disconnect(this);` deleted from complete(), so abort()
    //       re-enters it -> "Received signal 11 (SIGSEGV)" in this case.
    function test_an_endless_reply_is_stopped_as_unreadable() {
        requireRealRead();
        const item = pluginReadyToFetch();
        // Reset however this ends: the cases after it that do not reset would be served this shape.
        try {
            stubDaemon.serveEndless();
            item.refresh();

            tryCompare(item, "screenState", "unreadable", 5000, "an endless reply is unreadable");
            compare(stubDaemon.requests(), 1, "the plugin made exactly one request");
            tryVerify(function () { return stubDaemon.endlessEnded() === 1; }, 5000,
                      "the plugin closed the endless transfer");
            console.log("endless body: " + stubDaemon.endlessBytes() + " bytes sent before the plugin closed it");
        } finally {
            stubDaemon.reset();
        }
    }

    // A retry is only due while the daemon is not known to be stopped: with its bus name unowned
    // main.qml rightly retries nothing. qml_states_test.cpp owns the name on the private bus.
    function requireDaemonNotAbsent(item) {
        if (item.watchItem.daemonAbsent()) {
            skip("the daemon's bus name has no owner here, so main.qml rightly schedules no retry");
        }
    }

    // GOAL: a 200 whose connection closes before its declared length reads as unreadable, and a retry
    //       follows. False if it reads as nothing answering ("not running" for a daemon that answered),
    //       or if the truncated body is applied as a 200, which is never retried.
    // WHY-THIS-SHAPE: cut short once, then served whole, so the retry is seen arriving and applied;
    //       waiting for "unreadable" alone would pass for a plugin that never asks again.
    // CONTROL: control_batch.sh 2026-11-03, each reverted: ConfigRead::ending()'s `status != 0`
    //       branch made `false` (backend/kde/config_read.cpp) -> "a 200 cut short mid-body reads as
    //       unreadable" red; in retryIfFailed (backend/kde/kpackage/contents/ui/
    //       main.qml) `outcome === "answered"` made `true`, and `retry.start();` deleted -> each "'a retry
    //       follows a read cut short' returned FALSE".
    function test_a_reply_cut_short_reads_as_unreadable_and_is_retried() {
        requireRealRead();
        const item = pluginReadyToFetch();
        requireDaemonNotAbsent(item);
        stubDaemon.serveCutShortOnce();
        item.refresh();

        tryVerify(function () { return item.pending === null; }, 5000, "the cut-short read ended");
        compare(item.screenState, "unreadable", "a 200 cut short mid-body reads as unreadable");
        compare(stubDaemon.requests(), 1, "one request so far")
        tryVerify(function () { return stubDaemon.requests() === 2; }, 5000, "a retry follows a read cut short");
        tryCompare(item, "screenState", "set", 5000, "the retry's whole answer is applied");
    }

    // GOAL: a server that accepts and never answers ends the read in about 5 s, `pending` clears, the
    //       last answer stays on screen, and a later notice reads again. False if the read waits on
    //       (pending stays set and every notice only queues behind it), if something else ends it
    //       early, or if the timeout is applied as nothing answering, which tears the renderer down.
    // WHY-THIS-SHAPE: timed from the refresh and bounded on both sides: an upper bound alone would
    //       pass a read ended at once by any failure, which is not the timeout. The stub stalls for up
    //       to 12 s, past the wait, so its own 503 cannot be what ends the read.
    // CONTROL: control_batch.sh 2026-11-03, reverted, each, backend/kde/config_read.cpp/.h and main.qml:
    //       `request.setTransferTimeout(kTimeoutMs);` deleted -> "'a read nobody answers ends' returned
    //       FALSE"; kTimeoutMs made 1000 -> "a read nobody answers ends in about 5 s (took 1006 ms)" red;
    //       main.qml's `const keep = ...` made `false` -> "and the last answer stays on screen",
    //       'Actual (): unreachable'. The later-notice half has no control of its own.
    function test_a_server_that_never_answers_ends_the_read_in_about_5_s() {
        requireRealRead();
        const item = pluginReadyToFetch();
        const watch = childNamed(item, "wallpaperxConfigWatch");
        stubDaemon.serveStall();
        const started = Date.now();
        item.refresh();
        verify(item.pending !== null, "the refresh sent a read");
        tryVerify(function () { return stubDaemon.requests() === 1; }, 5000, "the stub holds the read unanswered");
        tryVerify(function () { return item.pending === null; }, 9000, "a read nobody answers ends");
        const took = Date.now() - started;
        verify(took >= 4000 && took < 8000, "a read nobody answers ends in about 5 s (took " + took + " ms)");
        compare(item.screenState, "set", "and the last answer stays on screen");

        stubDaemon.serveGood();
        watch.item.configChanged();
        verify(item.pending !== null, "a later notice reads again");
        tryVerify(function () { return item.pending === null; }, 5000, "that read is answered");
        compare(item.screenState, "set", "and applied");
    }

    // GOAL: an HTTP 400 or 403 keeps its status and the daemon's reason, so the screen says refused,
    //       with that reason. False if a refusal is reported as nothing answering ("not running"),
    //       which sends the user to start a service that is up.
    // WHY-THIS-SHAPE: both codes, since Qt reports them as different errors (ProtocolInvalidOperation
    //       and ContentAccessDenied, both in isStatusError()), and the reason compared exactly.
    // CONTROL: control_batch.sh 2026-11-03, each reverted, in ConfigRead::ending()
    //       (backend/kde/config_read.cpp): `|| wholeRefusal` in the answered branch made `|| false`, and
    //       `*declaredLength == received.size()` made `false` -> each "an HTTP 400 keeps its status" red.
    function test_a_refusal_keeps_its_status_and_reason() {
        requireRealRead();
        const codes = [400, 403];
        for (let i = 0; i < codes.length; ++i) {
            const item = pluginReadyToFetch();
            const reason = "refused with " + codes[i] + " by the stub";
            stubDaemon.serveStatus(codes[i], reason, false);
            item.refresh();
            tryVerify(function () { return item.pending === null; }, 5000, "the refused read ended");
            compare(item.screenState, "refused", "an HTTP " + codes[i] + " keeps its status");
            compare(item.refusalReason, reason, "an HTTP " + codes[i] + " keeps the daemon's reason");
        }
    }

    // GOAL: a 503 is retried. False if it is treated as a refusal that stands until something changes.
    // CONTROL: control_batch.sh 2026-11-03, reverted: retryIfFailed's `status < 500` made `status < 600`
    //       in backend/kde/kpackage/contents/ui/main.qml -> "'a 503 is retried' returned FALSE".
    function test_a_503_is_retried() {
        requireRealRead();
        const item = pluginReadyToFetch();
        requireDaemonNotAbsent(item);
        stubDaemon.serveStatus(503, "busy", true);
        item.refresh();
        tryVerify(function () { return item.pending === null; }, 5000, "the 503 read ended");
        compare(item.screenState, "refused", "a 503 is shown as the daemon's answer meanwhile");
        tryVerify(function () { return stubDaemon.requests() === 2; }, 5000, "a 503 is retried");
        tryCompare(item, "screenState", "set", 5000, "the retry's answer is applied");
    }

    // Waits up to 9 s for `item`'s read to end: past ConfigRead's 5 s timeout, short of the stall's 12 s.
    function waitOutATimedOutRead(item, what) {
        tryVerify(function () { return item.pending === null; }, 9000, what);
    }

    // GOAL: S15.1224 -- a read ended by its timeout changes nothing shown: `answered`, `reply` (the
    //       same object) and `refusalReason` stay as they were, and a loaded renderer stays loaded. False
    //       if the timeout is applied as nothing answering, which clears all three and unloads it.
    // WHY-THIS-SHAPE: two starting answers, a wallpaper with a monitor id (the renderer's Loader is
    //       active) and a refusal, since apply(0) clears a different field in each. The reply is
    //       compared by identity: an equal object re-parsed from the same body would be a new apply.
    // CONTROL: control_batch.sh 2026-11-03, reverted: main.qml's `const keep = ...` made `false`
    //       (backend/kde/kpackage/contents/ui/main.qml) -> "'a timed-out read keeps the same reply'
    //       returned FALSE". That ends the case, so the answered, renderer and refusal lines after it
    //       have no control of their own.
    function test_a_timed_out_read_keeps_the_last_answer() {
        requireRealRead();
        try {
            const item = pluginReadyToFetch();
            const surface = childNamed(item, "wallpaperxSurface");
            item.apply(200, '{"output":"DP-3","known":true,"path":"/home/u/Pictures/beach.png","monitor_id":0}');
            verify(surface.active, "a wallpaper with a monitor id makes the renderer's Loader active");
            const reply = item.reply;
            stubDaemon.serveStall();
            item.refresh();
            verify(item.pending !== null, "the refresh sent a read");
            waitOutATimedOutRead(item, "the stalled read ended");
            compare(stubDaemon.requests(), 1, "the stub held exactly that read");
            verify(item.reply === reply, "a timed-out read keeps the same reply");
            verify(item.answered, "a timed-out read keeps the screen answered");
            compare(item.screenState, "set", "a timed-out read keeps the wallpaper's state");
            verify(surface.active, "a timed-out read keeps the renderer loaded");

            stubDaemon.reset();
            item.refresh();
            waitOutATimedOutRead(item, "the good read ended");
            item.apply(400, '{"error":"kept reason"}');
            compare(item.screenState, "refused", "the refusal is shown");
            stubDaemon.serveStall();
            item.refresh();
            waitOutATimedOutRead(item, "the second stalled read ended");
            compare(item.refusalReason, "kept reason", "a timed-out read keeps the refusal's reason");
            compare(item.screenState, "refused", "a timed-out read keeps the refusal shown");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: S15.1225 -- a read ended by its timeout is retried within the first backoff (2 s). False if
    //       a timeout leaves the screen waiting for the next notice.
    // WHY-THIS-SHAPE: the stub keeps stalling, so the second request is the retry and not an answer
    //       to anything; 4 s is the first backoff doubled, short of the second read's own timeout.
    // CONTROL: control_batch.sh 2026-11-03, reverted: main.qml's `root.retryIfFailed(outcome,
    //       request.status)` moved inside the `if (!keep)` block (backend/kde/kpackage/contents/ui/
    //       main.qml) -> "'a timed-out read is retried' returned FALSE".
    function test_a_timed_out_read_is_retried() {
        requireRealRead();
        try {
            const item = pluginReadyToFetch();
            requireDaemonNotAbsent(item);
            stubDaemon.serveStall();
            item.refresh();
            waitOutATimedOutRead(item, "the stalled read ended");
            compare(stubDaemon.requests(), 1, "one request so far");
            tryVerify(function () { return stubDaemon.requests() === 2; }, 4000, "a timed-out read is retried");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: S15.1226 -- while nobody owns the daemon's bus name, a read ended by its timeout is not
    //       retried. False if a stopped daemon's port is polled every few seconds.
    // WHY-THIS-SHAPE: absence needs a window; 4.5 s is more than twice the 2 s backoff a retry would
    //       use. The name is given up by this process (it owns it on the private bus), and
    //       daemonAbsent() is required to say so before the read, so the case grades the timed-out
    //       path and not a watch that has not heard yet.
    // CONTROL: control_batch.sh 2026-11-03, reverted: the daemonAbsent() test in retryIfFailed
    //       (backend/kde/kpackage/contents/ui/main.qml) made `if (false)` -> "a timed-out read is not
    //       retried while the daemon's name has no owner. Actual (): 2 Expected (): 1".
    function test_a_timed_out_read_is_not_retried_while_the_daemon_name_has_no_owner() {
        requireRealRead();
        const item = pluginReadyToFetch();
        if (!daemonName.owned()) {
            skip("this process does not own the daemon's bus name here, so it cannot make it go");
        }
        try {
            verify(daemonName.release(), "the daemon's bus name was given up");
            tryVerify(function () { return item.watchItem.daemonAbsent(); }, 5000, "the watch heard the name go");
            stubDaemon.reset();
            stubDaemon.serveStall();
            item.refresh();
            waitOutATimedOutRead(item, "the stalled read ended");
            compare(item.failedReads, 1, "the timed-out read counted as failed");
            wait(4500);
            compare(stubDaemon.requests(), 1, "a timed-out read is not retried while the daemon's name has no owner");
        } finally {
            stubDaemon.reset();
            daemonName.take();
        }
        tryVerify(function () { return !item.watchItem.daemonAbsent(); }, 5000, "the name was taken back");
    }

    // GOAL: S15.1227 -- with nothing ever answered, a read ended by its timeout leaves the screen
    //       "unreachable". False if the timeout marks the screen answered.
    // CONTROL: control_batch.sh 2026-11-03, reverted: main.qml's `outcome === "unreadable" ? 200` made
    //       `outcome !== "answered" ? 200` (backend/kde/kpackage/contents/ui/main.qml) -> "'nothing
    //       answered' returned FALSE".
    function test_a_timeout_before_any_answer_leaves_the_screen_unreachable() {
        requireRealRead();
        try {
            stubDaemon.reset();
            stubDaemon.serveStall();
            const item = loadHost().item;
            standInTheWatch(item);
            item.connectorOverride = suite.namedScreen;
            verify(item.pending !== null, "naming the screen sent a read");
            waitOutATimedOutRead(item, "the stalled first read ended");
            compare(stubDaemon.requests(), 1, "the stub held the first read");
            verify(!item.answered, "nothing answered");
            compare(item.screenState, "unreachable", "a timeout before any answer leaves the screen unreachable");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: S15.1228 -- a daemon whose bus name goes while its port hangs is shown as "not running".
    //       False if the read the departure prompts times out, keeps the last answer, and (the name
    //       being unowned) is never retried, leaving a stopped daemon's wallpaper described as set.
    // WHY-THIS-SHAPE: the stand-in watch is wired to the real ConfigWatch's configChanged here only,
    //       so the departure reaches main.qml through the path plasmashell has; other cases keep a
    //       watch that hears nothing they did not send.
    // CONTROL: control_batch.sh 2026-11-03, reverted: the `!(... daemonAbsent())` clause dropped from
    //       `const keep` in backend/kde/kpackage/contents/ui/main.qml (the departure's read is then this
    //       screen's first timeout, and kept) -> "a daemon whose name went while its port hangs is shown
    //       as not running", 'Actual (): set'.
    function test_a_daemon_whose_name_goes_while_its_port_hangs_is_shown_not_running() {
        requireRealRead();
        const item = pluginReadyToFetch();
        if (!daemonName.owned()) {
            skip("this process does not own the daemon's bus name here, so it cannot make it go");
        }
        const watch = item.watchItem;
        watch.real.configChanged.connect(watch.configChanged);
        try {
            stubDaemon.serveStall();
            verify(daemonName.release(), "the daemon's bus name was given up");
            tryVerify(function () { return stubDaemon.requests() >= 1; }, 5000, "the name going prompted a read");
            tryCompare(item, "screenState", "unreachable", 9000,
                       "a daemon whose name went while its port hangs is shown as not running");
        } finally {
            watch.real.configChanged.disconnect(watch.configChanged);
            stubDaemon.reset();
            daemonName.take();
        }
        tryVerify(function () { return !item.watchItem.daemonAbsent(); }, 5000, "the name was taken back");
    }

    // GOAL: S15.1230 -- a 200 whose body cannot be decoded (claims gzip, is not) reads as unreadable
    //       and is retried. False if it reads as nothing answering ("not running" for a daemon that
    //       answered), or if, read as a 200 answer, it is never retried.
    // WHY-THIS-SHAPE: once, then the good reply, so the retry is seen arriving and applied.
    // CONTROL: control_batch.sh 2026-11-03, each reverted, in ConfigRead::ending()
    //       (backend/kde/config_read.cpp): `wholeRefusal` made `isStatusError(error)` alone, so Qt's
    //       content error reads as a whole answer -> "'a retry follows an undecodable 200' returned
    //       FALSE"; the `status != 0` branch made `false` -> "a 200 whose body cannot be decoded reads as
    //       unreadable" red. Deleting only `status >= 400 &&` stays GREEN here (Qt keeps no length for a
    //       body it tried to decompress, so the length rule rejects it alone); that clause is graded by
    //       config_read_ending's "200 undecodable, its declared length all present" row.
    function test_a_2xx_whose_body_cannot_be_decoded_reads_as_unreadable_and_is_retried() {
        requireRealRead();
        const item = pluginReadyToFetch();
        requireDaemonNotAbsent(item);
        stubDaemon.serveBadGzipOnce();
        item.refresh();
        tryVerify(function () { return item.pending === null; }, 5000, "the undecodable read ended");
        compare(item.screenState, "unreadable", "a 200 whose body cannot be decoded reads as unreadable");
        compare(stubDaemon.requests(), 1, "one request so far");
        tryVerify(function () { return stubDaemon.requests() === 2; }, 5000, "a retry follows an undecodable 200");
        tryCompare(item, "screenState", "set", 5000, "the retry's whole answer is applied");
    }

    // GOAL: S15.1231 -- after a rename, a first read for the new name that times out shows
    //       "unreachable", never the old monitor id or path. False if the old connector's answer is kept.
    // WHY-THIS-SHAPE: the old answer carries a monitor id, so a kept one would also keep the renderer
    //       drawing the old monitor; the rename itself sends the read, as plasmashell's would.
    // CONTROL: control_batch.sh 2026-11-03, reverted: `root.keepableFor === asked` made
    //       `root.keepableFor !== ""` in backend/kde/kpackage/contents/ui/main.qml -> "after a rename, a
    //       timed-out first read shows unreachable", 'Actual (): set'.
    function test_a_timeout_after_a_rename_shows_unreachable() {
        requireRealRead();
        try {
            const item = pluginReadyToFetch();
            item.apply(200, '{"output":"DP-3","known":true,"path":"/home/u/Pictures/beach.png","monitor_id":4}');
            compare(item.monitorId, 4, "the old screen's answer names its monitor");
            stubDaemon.serveStall();
            item.connectorOverride = "HDMI-A-1";
            verify(item.pending !== null, "the rename sent a read");
            waitOutATimedOutRead(item, "the new name's stalled read ended");
            compare(stubDaemon.requests(), 1, "the stub held exactly that read");
            compare(stubDaemon.lastOutput(), "HDMI-A-1", "the read asked about the new name");
            compare(item.screenState, "unreachable", "after a rename, a timed-out first read shows unreachable");
            verify(item.monitorId !== 4, "the old monitor id is gone");
            compare(item.wallpaperPath, "", "the old path is gone");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: S15.1232 -- a second timeout in a row shows "unreachable". False if the keep has no bound.
    // WHY-THIS-SHAPE: the second timeout is the plugin's own retry of the first, so the case follows
    //       the path a wedged daemon takes; the first must still be kept, or this would pass for a
    //       plugin that keeps nothing.
    // CONTROL: control_batch.sh 2026-11-03, reverted: main.qml's keepableFor line made
    //       `root.keepableFor = asked;` (backend/kde/kpackage/contents/ui/main.qml) -> "a second timeout
    //       in a row shows unreachable", 'Actual (): set'.
    function test_a_second_timeout_in_a_row_shows_unreachable() {
        requireRealRead();
        try {
            const item = pluginReadyToFetch();
            requireDaemonNotAbsent(item);
            stubDaemon.serveStall();
            item.refresh();
            waitOutATimedOutRead(item, "the first stalled read ended");
            compare(item.screenState, "set", "the first timeout keeps the answer");
            tryVerify(function () { return stubDaemon.requests() === 2; }, 4000, "the retry was sent");
            waitOutATimedOutRead(item, "the retry's stalled read ended");
            compare(item.screenState, "unreachable", "a second timeout in a row shows unreachable");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: S15.1233 -- a read that answers between two timeouts lets the next timeout keep the answer
    //       again. False if the bound counts timeouts since the plugin started, not in a row.
    // CONTROL: control_batch.sh 2026-11-03, reverted: in backend/kde/kpackage/contents/ui/main.qml a
    //       `timedOutOnce` property set by any timeout and required clear before keepableFor is set
    //       again -> "a timeout after an answer between two timeouts keeps the answer",
    //       'Actual (): unreachable'.
    function test_an_answer_between_two_timeouts_lets_the_next_one_keep() {
        requireRealRead();
        try {
            const item = pluginReadyToFetch();
            requireDaemonNotAbsent(item);
            stubDaemon.serveStall();
            item.refresh();
            waitOutATimedOutRead(item, "the first stalled read ended");
            compare(item.screenState, "set", "the first timeout keeps the answer");
            stubDaemon.reset();
            tryVerify(function () { return stubDaemon.requests() === 1; }, 4000, "the retry was sent");
            tryVerify(function () { return item.pending === null; }, 5000, "the retry was answered");
            compare(item.screenState, "set", "the retry's answer is applied");
            stubDaemon.serveStall();
            item.refresh();
            waitOutATimedOutRead(item, "the second stalled read ended");
            compare(stubDaemon.requests(), 2, "the stub held exactly that read");
            compare(item.screenState, "set", "a timeout after an answer between two timeouts keeps the answer");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: S15.1234 -- after an unreadable read, the first timeout keeps "unreadable" once. False if a
    //       timeout after an unreadable read is applied at once as "not running".
    // WHY-THIS-SHAPE: the timeout is the plugin's own retry of the unreadable read, the stub switched
    //       to stalling before that retry is due (2 s).
    // CONTROL: control_batch.sh 2026-11-03, reverted: main.qml's keepableFor line made to set the name
    //       after an answered read only (backend/kde/kpackage/contents/ui/main.qml) -> "the first timeout
    //       after an unreadable read keeps unreadable", 'Actual (): unreachable'.
    function test_the_first_timeout_after_an_unreadable_read_keeps_unreadable() {
        requireRealRead();
        try {
            const item = pluginReadyToFetch();
            requireDaemonNotAbsent(item);
            stubDaemon.serveBadGzipOnce();
            item.refresh();
            tryVerify(function () { return item.pending === null; }, 5000, "the undecodable read ended");
            compare(item.screenState, "unreadable", "the read was unreadable");
            stubDaemon.serveStall();
            tryVerify(function () { return stubDaemon.requests() === 2; }, 4000, "the retry was sent");
            waitOutATimedOutRead(item, "the retry's stalled read ended");
            compare(item.screenState, "unreadable", "the first timeout after an unreadable read keeps unreadable");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: S15.1235 -- a 407 with a JSON body shows as refused, with that reason, and is not retried.
    //       False if Qt's proxy-authentication error makes it "not running" or "unreadable", or if it is
    //       read again with nothing announced.
    // WHY-THIS-SHAPE: absence needs a window: 3 s is past the first backoff (2 s), and the stub keeps
    //       answering 407, so a retry would be counted.
    // CONTROL: control_batch.sh 2026-11-03, each reverted: `error ==
    //       QNetworkReply::ProxyAuthenticationRequiredError ||` deleted from isStatusError()
    //       (backend/kde/config_read.cpp) -> "an HTTP 407 is shown as refused", 'Actual (): unreadable';
    //       retryIfFailed's `status < 500` made `status < 400` (main.qml) -> "a 407 is not retried.
    //       Actual (): 2 Expected (): 1".
    function test_a_407_is_a_refusal_and_not_retried() {
        requireRealRead();
        try {
            const item = pluginReadyToFetch();
            requireDaemonNotAbsent(item);
            const reason = "refused with 407 by the stub";
            stubDaemon.serveStatus(407, reason, false);
            item.refresh();
            tryVerify(function () { return item.pending === null; }, 5000, "the 407 read ended");
            compare(item.screenState, "refused", "an HTTP 407 is shown as refused");
            compare(item.refusalReason, reason, "an HTTP 407 keeps the daemon's reason");
            wait(3000);
            compare(stubDaemon.requests(), 1, "a 407 is not retried");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: S15.1236 -- a 503 cut short reads as unreadable and is retried. False if it is taken as a
    //       whole refusal, or as nothing answering ("not running" for a daemon that answered).
    // WHY-THIS-SHAPE: the length rule (a cut 4xx/5xx Qt reports as the status's own error) is NOT
    //       graded here: Qt picks that report in about one loaded read in four, so a break on it goes red
    //       only by chance. config_read_ending's "cut short" rows grade it on ending() directly; this case
    //       grades the network path and the retry.
    // CONTROL: control_batch.sh 2026-11-03, each reverted: ConfigRead::ending()'s `status != 0` branch
    //       made `false` (backend/kde/config_read.cpp) -> "a 503 cut short reads as unreadable" red.
    //       Retry, 2026-11-03 batch, line untouched since: `retry.start();` deleted from retryIfFailed
    //       (main.qml) -> "'a retry follows a 503 cut short' returned FALSE".
    function test_a_503_cut_short_reads_as_unreadable_and_is_retried() {
        requireRealRead();
        try {
            const item = pluginReadyToFetch();
            requireDaemonNotAbsent(item);
            stubDaemon.serveCutShortWithStatusOnce(503);
            item.refresh();
            tryVerify(function () { return item.pending === null; }, 5000, "the cut-short 503 read ended");
            compare(item.screenState, "unreadable", "a 503 cut short reads as unreadable");
            compare(stubDaemon.requests(), 1, "one request so far");
            tryVerify(function () { return stubDaemon.requests() === 2; }, 5000, "a retry follows a 503 cut short");
            tryCompare(item, "screenState", "set", 5000, "the retry's whole answer is applied");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: S15.1237 -- a connection closed with no status line reads as "unreachable" and is retried.
    //       False if it is kept like a timeout, read as an answer, or left waiting for the next notice.
    // WHY-THIS-SHAPE: counted in reads made, not stub requests: Qt may resend a request whose
    //       connection closed before any reply, so one read can be several requests. The stub must have
    //       dropped a connection, or the case would grade some other failure.
    // CONTROL: control_batch.sh 2026-11-03, each reverted: ConfigRead::ending()'s last return made
    //       `Outcome::TimedOut` (backend/kde/config_read.cpp) -> "a connection closed with no status line
    //       reads as unreachable" red; retryIfFailed's `outcome === "answered"` made `true`
    //       (main.qml) -> "'a retry follows a connection closed with no status line' returned FALSE".
    //       Test-side: dropConnection() matching ports never present -> "the stub closed the connection
    //       before any reply" red.
    function test_a_connection_closed_with_no_status_line_is_unreachable_and_retried() {
        requireRealRead();
        try {
            const item = pluginReadyToFetch();
            requireDaemonNotAbsent(item);
            const madeBefore = readProbe.madeReads();
            stubDaemon.serveNoStatusLine();
            item.refresh();
            tryVerify(function () { return item.pending === null; }, 5000, "the dropped read ended");
            verify(stubDaemon.dropped() >= 1, "the stub closed the connection before any reply");
            compare(item.screenState, "unreachable", "a connection closed with no status line reads as unreachable");
            stubDaemon.serveGood();
            tryVerify(function () { return readProbe.madeReads() - madeBefore === 2; }, 5000,
                      "a retry follows a connection closed with no status line");
            tryCompare(item, "screenState", "set", 5000, "the retry's answer is applied");
        } finally {
            stubDaemon.reset();
        }
    }

    // GOAL: the output name reaches the daemon intact, with `& = + space / %` in it. False if any of
    //       them is sent raw: `&` and `=` split the query, `+` is read as a space, `%` starts an escape.
    // WHY-THIS-SHAPE: read off what the stub's HTTP library decoded, the same library the daemon uses,
    //       rather than off the URL Qt built, which would only grade the encoder against itself.
    // CONTROL: control_batch.sh 2026-11-02, reverted: in ConfigRead's constructor
    //       (backend/kde/config_read.cpp) `QUrl::toPercentEncoding(output)` made
    //       `QUrl::toPercentEncoding(output, "+")` -> "the output name reaches the daemon intact" red.
    function test_the_output_name_reaches_the_daemon_intact() {
        requireRealRead();
        stubDaemon.reset();
        const name = "A&b=c+d e/%";
        const read = readProbe.fetch(name);
        verify(read !== null, "a read was made");
        const state = { done: false };
        read.finished.connect(function () { state.done = true; });
        tryVerify(function () { return state.done; }, 5000, "the read finished");
        compare(stubDaemon.requests(), 1, "the read reached the stub");
        compare(stubDaemon.lastOutput(), name, "the output name reaches the daemon intact");
    }

    // GOAL: a plugin makes exactly one read at startup, sent once its change watch has loaded. False
    //       if it makes none (the first ask lost while the watch had no item), or two.
    // WHY-THIS-SHAPE: the screen is named before the watch loads, so the name's own refresh finds no
    //       watch and the load is the only trigger left. The wait for `pending` to clear also waits out
    //       a second read queued behind the first, since that is sent from inside the first's handler.
    // CONTROL: control_batch.sh 2026-11-02, each reverted, in the watch Loader of
    //       backend/kde/kpackage/contents/ui/main.qml: `onLoaded: root.refresh()` deleted -> "the watch
    //       loading sent the first read" red; made `{ root.refresh(); root.refresh(); }` -> "startup
    //       made exactly one read" red, 'Actual (): 2'.
    function test_startup_makes_exactly_one_read_once_the_watch_has_loaded() {
        requireRealRead();
        stubDaemon.reset();
        const item = loadHost().item;
        item.connectorOverride = suite.namedScreen;
        compare(item.pending, null, "no read before the watch has loaded");
        const watch = childNamed(item, "wallpaperxConfigWatch");
        compare(watch.status, Loader.Error, "the real change watch cannot load on this harness");
        watch.sourceComponent = standInWatch;
        verify(item.pending !== null, "the watch loading sent the first read");
        tryVerify(function () { return item.pending === null; }, 5000, "the first read was answered");
        compare(item.screenState, "set", "and applied");
        compare(stubDaemon.requests(), 1, "startup made exactly one read");
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

    // Markup a hostile answer on port 18462 could put in either of the two reply fields the
    // desktop message interpolates. The anchor is what `linkAt` can find; the image is the one
    // that matters most, because Qt's styled text fetches an <img src>. Its source is a
    // relative name, never a URL: a styled yardstick below lays this out for real, and a check
    // must not make the request it exists to prove the plugin does not make. That yardstick is
    // what logs "Cannot open: .../nope.png" in a passing run -- from the test's own directory,
    // which is how a reader tells it apart from the plugin having parsed the tag.
    readonly property string markup:
        '<a href="http://example.invalid/x">CLICKME</a>'
        + '<img src="nope.png" width="60" height="240">'

    function markupCases() {
        return [
            {
                // reply.output, which the "unknown" message interpolates ahead of its newline.
                tag: "a screen name from the reply",
                drive: [200, JSON.stringify({ output: suite.markup, known: false })]
            },
            {
                // refusalReason, the field the goal was written from. Measured fact, and the
                // reason the control cannot red this row: Qt's auto-detection gives up at the
                // first newline, and the refusal wording puts one ahead of its %1, so today
                // this row would hold even with no textFormat set. It is asserted anyway --
                // the protection is an accident of wording, and moving %1 ahead of the newline
                // would end it. Row 1 is what makes the check able to fail.
                tag: "a refusal reason from the reply",
                drive: [400, JSON.stringify({ error: suite.markup })]
            }
        ];
    }

    // GOAL: markup arriving in a reply from whatever holds port 18462 is drawn as the
    //       characters it is, never interpreted. False if the desktop message renders a live
    //       link, or lays out as anything other than the literal string -- Qt's styled text
    //       consumes tags and fetches <img src>, which turns a wallpaper into a client that
    //       makes requests on behalf of whoever answered on loopback.
    // WHY-THIS-SHAPE: graded on the layout the Text actually produced, not on `textFormat`,
    //       which asserts the spelling of the fix rather than the property. The yardstick pair
    //       is what makes the layout comparison mean something: matching the plain twin alone
    //       would also hold if Qt stopped distinguishing the formats, so the styled twin is
    //       asserted to differ first (seen red by laying both twins out plain: '116 vs 116').
    //       `linkAt` alone is not enough either -- an <img> is not a link, and the image is the
    //       case that reaches the network.
    // CONTROL: `textFormat: Text.PlainText` removed from main.qml's Text, i.e. the file as it
    //       stood before 66be011 -- row 1 red with 'no part of the message is a live link: 154
    //       of 10880 probes hit [http://example.invalid/x]', and with the link half stubbed out
    //       to reach the next one, 'the message is laid out as the characters it contains.
    //       Actual (): 636.98. Expected (): 573.70'. That run also logged main.qml itself
    //       trying to open the image. Reverted. Row 2 does not red there, and cannot: see it.
    // COVERAGE-ASSERT: the probe grid is asserted against the item's own size, so a Text that
    //       collapsed to nothing scans 0 points and fails rather than finding no links because
    //       it looked nowhere. Seen red by halving the scan: 'Actual (): 2400. Expected ():
    //       4640'. The case count is asserted too.
    function test_markup_in_a_reply_is_drawn_as_text() {
        const cases = markupCases();
        let checked = 0;
        for (let i = 0; i < cases.length; ++i) {
            const item = drivenPlugin({ connector: suite.namedScreen, drive: cases[i].drive });
            const shown = onlyDescendantWith(item, "wrapMode");

            // Without this the rest grades a Text that never received the markup at all.
            verify(shown.text.indexOf(suite.markup) !== -1,
                   "the reply's markup reached the desktop message for " + cases[i].tag
                   + ": [" + shown.text + "]");

            let probes = 0;
            let hits = 0;
            let firstLink = "";
            for (let y = 0; y < shown.height; y += 4) {
                for (let x = 0; x < shown.width; x += 4) {
                    probes += 1;
                    const link = shown.linkAt(x, y);
                    if (link !== "") {
                        hits += 1;
                        if (firstLink === "") {
                            firstLink = link;
                        }
                    }
                }
            }
            compare(probes, Math.ceil(shown.width / 4) * Math.ceil(shown.height / 4),
                    "the whole message area was probed for links, " + cases[i].tag);
            compare(hits, 0, "no part of the message is a live link: " + hits + " of " + probes
                    + " probes hit [" + firstLink + "], " + cases[i].tag);

            // Same string, same font, same wrapping: once as characters, once as markup.
            const literal = suite.layOut(shown, Text.PlainText);
            const consumed = suite.layOut(shown, Text.StyledText);
            // Height, not width: a wrapped message's painted width is its longest line, which
            // for the refusal wording is the fixed prefix and stays put whichever way the
            // markup goes. The image's reserved height is what parts the two layouts.
            verify(Math.abs(literal.paintedHeight - consumed.paintedHeight) > 100,
                   "the two layouts are distinguishable at all -- otherwise the comparison "
                   + "below proves nothing (" + literal.paintedHeight + " vs "
                   + consumed.paintedHeight + "), " + cases[i].tag);
            fuzzyCompare(shown.paintedWidth, literal.paintedWidth, 0.5,
                         "the message is laid out as the characters it contains, "
                         + cases[i].tag);
            fuzzyCompare(shown.paintedHeight, literal.paintedHeight, 0.5,
                         "the message is laid out as the characters it contains, "
                         + cases[i].tag);
            checked += 1;
        }
        compare(checked, cases.length, "every markup route was checked");
    }

    // The plugin's own Text laid out again in a chosen format. Copied rather than declared, so
    // a font change in main.qml cannot make the yardstick disagree for a reason that has
    // nothing to do with markup.
    function layOut(source, format) {
        return createTemporaryObject(yardstick, suite, {
            width: source.width,
            font: source.font,
            wrapMode: source.wrapMode,
            horizontalAlignment: source.horizontalAlignment,
            textFormat: format,
            text: source.text
        });
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

    // Finds a named child of the plugin, so a check does not depend on declaration order.
    function childNamed(item, name) {
        for (let i = 0; i < item.children.length; ++i) {
            if (item.children[i].objectName === name) return item.children[i];
        }
        return null;
    }

    // GOAL: S12.6 -- whatever the renderer is doing, a screen that cannot show a wallpaper still
    //       shows its message, and a screen with no monitor id loads no renderer at all. The
    //       rows here carry no monitor id; "unset" WITH one loads the renderer under its message
    //       by design (S13.196), graded in its own test below.
    // WHY-THIS-SHAPE: driven across ALL the causes rather than one, because each reaches the
    //       Loader's condition by a different route -- no name, no answer, a refusal, an
    //       unreadable reply -- and a condition testing only `reply !== null` would pass four of
    //       them and fail the fifth.
    // CONTROL: `active:` on the Loader in main.qml changed to `true` -> "FAIL: [1-unnamed]
    //       the renderer is not active while the screen has a message", and the no-id check
    //       below with it. Observed 2026-08-21. Reverted.
    // WHAT THIS ONE DOES NOT GRADE: the backdrop line here is weak on its own -- with no
    //       renderer module on this harness's import path `surface.status` is never Ready, so
    //       `visible` is pinned true whatever main.qml binds it to. The check that a wallpaper
    //       actually uncovers it stands a renderer in; see
    //       test_the_message_stops_covering_a_wallpaper_that_is_drawn below.
    function test_the_message_is_never_covered_by_the_renderer() {
        const cases = causeCases();
        for (let i = 0; i < cases.length; ++i) {
            if (cases[i].state === "set") continue;
            const item = drivenPlugin(cases[i]);
            const surface = childNamed(item, "wallpaperxSurface");
            const backdrop = childNamed(item, "wallpaperxBackdrop");
            verify(surface !== null && backdrop !== null, "the plugin has a surface and a backdrop");
            verify(!surface.active,
                   "[" + cases[i].tag + "] the renderer is not active while the screen has a message");
            verify(backdrop.visible, "[" + cases[i].tag + "] the message is visible");
        }
    }

    // GOAL: S12.6 -- a screen the daemon has not given a monitor id is not rendered for. False if
    //       the id is ignored: the renderer would ask for frames belonging to no monitor, and the
    //       desktop would show neither a wallpaper nor a reason.
    // WHY-THIS-SHAPE: the reply is otherwise a complete "set" answer -- known, with a path -- so
    //       only the missing id distinguishes it. The daemon sends null rather than 0 for exactly
    //       this case, and 0 is a real monitor.
    // CONTROL: `&& root.monitorId >= 0` deleted from the Loader's `active` in main.qml ->
    //       "FAIL: the renderer stays inactive without a monitor id", and that check alone,
    //       which is what separates it from the broader one above. Observed 2026-08-21.
    //       Reverted.
    function test_a_screen_with_no_monitor_id_is_not_rendered_for() {
        const item = loadPlugin(suite.namedScreen);
        item.apply(200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":null}');
        compare(item.screenState, "set", "the reply is otherwise a set state");
        compare(item.monitorId, -1, "a null monitor id reads as -1, not 0");

        const surface = childNamed(item, "wallpaperxSurface");
        verify(surface !== null, "the plugin has a surface");
        verify(!surface.active, "the renderer stays inactive without a monitor id");
    }

    // GOAL: S13.196, S12.6's "unset" case with a monitor id -- a screen the daemon knows, with no
    //       wallpaper chosen yet, already loads the renderer (which is what reports the screen's
    //       size), told its monitor id and connector name, so the first wallpaper is drawn at the
    //       screen's real size; and the desktop stays legible meanwhile: the "no wallpaper chosen"
    //       message stays visible over the renderer until a frame is shown. False for the rule this
    //       replaced (the renderer loaded only once a wallpaper was set), and for a renderer that
    //       covers the message while it has nothing to draw.
    // WHY-THIS-SHAPE: driven with the monitor id present -- the unset row in causeCases() carries
    //       none, and without an id nothing loads either way. The stand-in renderer is what lets
    //       "loaded" and "told its screen" be read at all on this harness; `active` is main.qml's own.
    // CONTROL: in backend/kde/kpackage/contents/ui/main.qml, the Loader's `active` put back to
    //       `root.screenState === "set" && root.monitorId >= 0` -> "FAIL!: 'a known screen with no
    //       wallpaper loads the renderer, so its size is reported before the first wallpaper'
    //       returned FALSE", that check alone. And the backdrop's `visible` binding replaced by
    //       `!surface.active` -> "'the message stays over the renderer until a frame is shown'
    //       returned FALSE", beside four older checks that read the same binding. Observed
    //       2026-09-21. Reverted.
    function test_a_known_screen_with_no_wallpaper_loads_the_renderer_under_its_message() {
        const item = loadPlugin(suite.namedScreen);
        const surface = childNamed(item, "wallpaperxSurface");
        const backdrop = childNamed(item, "wallpaperxBackdrop");
        verify(surface !== null && backdrop !== null, "the plugin has a surface and a backdrop");
        surface.sourceComponent = standInRenderer;

        item.apply(200, '{"output":"DP-3","known":true,"path":"","monitor_id":2}');
        compare(item.screenState, "unset", "the reply is a known screen with no wallpaper");
        compare(item.monitorId, 2, "and it carries the monitor id");
        verify(surface.active,
               "a known screen with no wallpaper loads the renderer, so its size is reported before the first wallpaper");
        compare(surface.status, Loader.Ready, "the stand-in renderer loaded, which is what lets this check fail");
        compare(surface.item.monitorId, 2, "the renderer is told which monitor it is");
        verify(backdrop.visible, "the message stays over the renderer until a frame is shown");
        verify(item.describe().indexOf("No wallpaper chosen") >= 0, "and it is the no-wallpaper message");
    }

    // GOAL: S12.1/S12.6 -- once a wallpaper is actually being drawn, the message stops covering
    //       it, and it comes back the moment it stops. False if the backdrop stays up: it is an
    //       opaque rectangle over the whole screen, declared AFTER the renderer, so a build that
    //       leaves it visible hides every imported frame behind a dark grey panel -- the exact
    //       inverse of the check above, and just as fatal.
    // WHY-THIS-SHAPE: the Loader is given a stand-in component. Nothing else in this file can
    //       reach the state being graded: the real renderer module is not on this harness's
    //       import path, so `surface.status` is never Ready and the backdrop is pinned visible
    //       whatever main.qml says. Measured, not argued -- with `visible: true` written into
    //       main.qml the entire suite stayed green, including both S12.6 checks. Only the module
    //       is stood in for; `active` and `visible` are still main.qml's own bindings.
    // CONTROL: `visible:` on the backdrop replaced by `true` in main.qml -> "FAIL: the message
    //       stops covering the wallpaper once one is being drawn", that check alone. Observed
    //       2026-08-21. Reverted.
    // COVERAGE-ASSERT: the stand-in is asserted to have actually loaded. Without that, a Loader
    //       that refused the component would leave the backdrop legitimately visible and the
    //       check would grade nothing -- which is precisely how it reads today without the
    //       stand-in.
    function test_the_message_stops_covering_a_wallpaper_that_is_drawn() {
        const item = loadPlugin(suite.namedScreen);
        const surface = childNamed(item, "wallpaperxSurface");
        const backdrop = childNamed(item, "wallpaperxBackdrop");
        verify(surface !== null && backdrop !== null, "the plugin has a surface and a backdrop");
        surface.sourceComponent = standInRenderer;

        item.apply(200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":0}');
        compare(item.screenState, "set", "the drive reaches the state that draws a wallpaper");
        verify(surface.active, "the renderer is asked for once there is a frame to draw");
        compare(surface.status, Loader.Ready,
                "the stand-in renderer loaded, which is what lets this check fail");
        verify(surface.item !== null, "the renderer occupies the surface");
        compare(surface.width, item.width, "the renderer covers the screen");
        compare(surface.height, item.height, "the renderer covers the screen");
        // "Being drawn" is the claim, so the stand-in has to make it: a loaded renderer that has
        // not put a frame up leaves the message in place, deliberately.
        surface.item.frameShown = true;
        verify(!backdrop.visible,
               "the message stops covering the wallpaper once one is being drawn");

        // And back again: a screen that loses its wallpaper must get its message returned, or the
        // desktop is left showing a frame that no longer describes anything.
        item.apply(0, "");
        compare(item.screenState, "unreachable", "the daemon going away is noticed");
        verify(!surface.active, "the renderer is dropped when there is nothing to draw");
        verify(backdrop.visible, "the message comes back when the wallpaper stops");
        // The message names THIS cause. Any branch of describe() returns a non-empty string, so
        // asserting only that it has one passes while the desktop shows the wrong explanation.
        // The fragment comes from the case table, which is the one place it is spelled.
        verify(item.describe().indexOf(suite.unreachableFragment) >= 0,
               "and the message names the daemon being gone, not the previous cause");
    }

    // GOAL: S13.107 -- the renderer main.qml loads is handed the connector name its monitor id was
    //       resolved from, and a screen renamed afterwards hands the loaded renderer the new name, so
    //       the size the renderer reports is filed under the screen it is drawn on.
    // WHY-THIS-SHAPE: read off the stand-in, the only renderer this harness can load. The rename is made
    //       after the renderer loaded, and "set" does not depend on the name, so the second half reaches
    //       the rename handler rather than a fresh onLoaded.
    // CONTROL: `item.output = root.connector;` deleted from the Loader's onLoaded in main.qml
    //       (backend/kde/kpackage/contents/ui/main.qml) -> "the renderer is told the connector name its
    //       monitor id belongs to" red, 'Actual (): Expected (): DP-3'. Reverted.
    // CONTROL: the `onConnectorChanged` handler deleted from main.qml -> only "a renamed screen's new
    //       connector name reaches the loaded renderer" red, 'Actual (): DP-3 Expected (): DP-9'. Reverted.
    function test_the_renderer_is_told_its_connector_name() {
        const item = loadPlugin(suite.namedScreen);
        const surface = childNamed(item, "wallpaperxSurface");
        verify(surface !== null, "the plugin has a surface");
        surface.sourceComponent = standInRenderer;

        item.apply(200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":0}');
        compare(surface.status, Loader.Ready, "the stand-in renderer loaded, which is what lets this check fail");
        compare(surface.item.output, suite.namedScreen, "the renderer is told the connector name its monitor id belongs to");

        item.connectorOverride = "DP-9";
        compare(item.screenState, "set", "renaming the screen leaves it drawing");
        verify(surface.item !== null, "the renderer is still loaded after the rename");
        compare(surface.item.output, "DP-9", "a renamed screen's new connector name reaches the loaded renderer");
    }

    // GOAL: S15.1036 -- main.qml hands the reply's `pause` to the renderer when it loads and
    //       whenever a later reply changes it, and an empty mode when the field is missing or not a string.
    // WHY-THIS-SHAPE: the stand-in starts with a sentinel, so a load that never assigned the mode cannot
    //       pass as an empty one; the change and the bad-type cases are driven on the one loaded item.
    // CONTROL: via control_batch.sh 2026-11-02, each alone, reverted, in
    //       backend/kde/kpackage/contents/ui/main.qml: onPauseModeChanged's assignment deleted -> "...when it
    //       loads" red; the `typeof ... === "string"` test made `!== undefined` -> "...not a string..." red
    //       (actual 7). onLoaded's `rendererItem.pauseMode = ...` deleted goes NOT red: the reply that
    //       loads the renderer also changes pauseMode, and onPauseModeChanged sets it in the same turn.
    function test_the_renderer_is_told_its_pause_mode() {
        const item = loadPlugin(suite.namedScreen);
        const surface = childNamed(item, "wallpaperxSurface");
        verify(surface !== null, "the plugin has a surface");
        surface.sourceComponent = standInRenderer;

        item.apply(200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":0,"pause":"paused_while_focused"}');
        compare(surface.status, Loader.Ready, "the stand-in renderer loaded, which is what lets this check fail");
        compare(surface.item.pauseMode, "paused_while_focused", "the renderer is told its pause mode when it loads");

        item.apply(200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":0,"pause":"play"}');
        compare(surface.item.pauseMode, "play", "a later reply's pause mode reaches the loaded renderer");

        item.apply(200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":0,"pause":7}');
        compare(surface.item.pauseMode, "", "a pause field that is not a string reaches the renderer as an empty mode");

        item.apply(200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":0,"pause":"play"}');
        item.apply(200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":0}');
        compare(surface.item.pauseMode, "", "a reply with no pause field reaches the renderer as an empty mode");
    }

    // The one configuration where main.qml hides the backdrop: a stand-in renderer loaded on a
    // "set" screen. Every precondition is asserted, because a stand-in that failed to load leaves
    // the backdrop legitimately visible and the checks that move it would grade nothing.
    function drawnScreenWithStandIn() {
        const item = loadPlugin(suite.namedScreen);
        const surface = childNamed(item, "wallpaperxSurface");
        const backdrop = childNamed(item, "wallpaperxBackdrop");
        verify(surface !== null && backdrop !== null, "the plugin has a surface and a backdrop");
        surface.sourceComponent = standInRenderer;

        item.apply(200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":0}');
        compare(item.screenState, "set", "the drive reaches the state that draws a wallpaper");
        compare(surface.status, Loader.Ready, "the stand-in renderer loaded");
        verify(surface.item !== null, "the renderer occupies the surface");
        compare(item.rendererReason, "", "a renderer that has said nothing reports no reason");
        // A loaded renderer that has drawn nothing does NOT hide the backdrop: the desktop would
        // show neither a wallpaper nor a message. The stand-in has to claim a frame to reach the
        // hidden state these checks then move.
        verify(backdrop.visible, "a renderer that has not drawn yet leaves the message up");
        surface.item.frameShown = true;
        verify(!backdrop.visible, "the backdrop starts hidden, which is what these checks move");
        return { item: item, surface: surface, backdrop: backdrop };
    }

    // GOAL: a renderer that LOADED and still cannot draw brings the message back, and stops
    //       covering the screen once it recovers. False either way round: a Loader.Ready that
    //       hides the backdrop on its own leaves a black desktop for an import the driver
    //       refused, and a reason that never clears leaves the desktop covered after the next
    //       import succeeds.
    // WHY-THIS-SHAPE: driven through the loaded item's own `unavailableReason`, not by writing
    //       root.rendererReason -- setting the plugin's property directly would grade nothing but
    //       the backdrop binding, and the defect was that nothing carried the renderer's property
    //       across at all. Both directions are driven on ONE instance, because a fresh plugin per
    //       direction cannot show a stale reason outliving itself.
    // CONTROL: `&& root.rendererReason === ""` deleted from the backdrop's `visible` in main.qml
    //       -> "FAIL!: 'a renderer that loaded and cannot draw is covered by the message again'
    //       returned FALSE". The clearing half seen red separately, with main.qml's
    //       onUnavailableReasonChanged made conditional on a non-empty reason -> "FAIL!: 'a
    //       renderer that recovers does not leave the desktop covered' returned FALSE". Observed
    //       2026-08-21. Both reverted.
    function test_a_loaded_renderer_that_cannot_draw_brings_the_message_back() {
        const drawn = drawnScreenWithStandIn();
        const reason = "the scene graph is not on OpenGL";

        // The backdrop is asserted before the property that feeds it in both halves: it is the
        // goal, and a compare() failure aborts the function, so the reverse order would leave the
        // check that matters unreached whenever the reason itself stopped arriving.
        drawn.surface.item.unavailableReason = reason;
        verify(drawn.backdrop.visible,
               "a renderer that loaded and cannot draw is covered by the message again");
        compare(drawn.item.rendererReason, reason, "the renderer's reason reaches the plugin");
        compare(drawn.surface.status, Loader.Ready,
                "the renderer is still loaded, so this is not the module-missing case");

        drawn.surface.item.unavailableReason = "";
        verify(!drawn.backdrop.visible,
               "a renderer that recovers does not leave the desktop covered");
        compare(drawn.item.rendererReason, "", "the cleared reason reaches the plugin");
    }

    // GOAL: S12.7's QML half -- a renderer that is still loaded and has STOPPED drawing gets the
    //       message back. That is what a Plasma session sees when the scene graph takes the
    //       renderer's resources away: the item stays, its `frameShown` goes false, and nothing
    //       else about the screen changes. False if the panel stays hidden, which is a black
    //       desktop with neither a wallpaper nor an explanation on it -- the defect that shipped.
    // WHY-THIS-SHAPE: the surface is left LOADED and the reason left EMPTY, so the drawing claim
    //       is the only thing that moved. The file's existing "the message comes back when the
    //       wallpaper stops" reaches a visible backdrop by deactivating the whole Loader, which
    //       main.qml answers through onStatusChanged -- a different line, and one that stays
    //       correct while this one is broken. Measured: with main.qml's onFrameShownChanged
    //       reduced to `if (surface.item.frameShown) root.rendererDrawing = true;` that check
    //       still passed.
    // CONTROL: that same one-way onFrameShownChanged written into main.qml -> "FAIL!: 'a renderer
    //       that stopped drawing is covered by the message again' returned FALSE", one row red out
    //       of this file's 40. Observed 2026-08-23. Reverted.
    function test_a_renderer_that_stops_drawing_brings_the_message_back() {
        const drawn = drawnScreenWithStandIn();

        drawn.surface.item.frameShown = false;
        verify(drawn.backdrop.visible,
               "a renderer that stopped drawing is covered by the message again");
        compare(drawn.item.rendererDrawing, false, "the renderer's withdrawn claim reaches the plugin");
        compare(drawn.surface.status, Loader.Ready,
                "the renderer is still loaded, so this is not the Loader-deactivated case");
        compare(drawn.item.rendererReason, "", "and it said no reason, so only the drawing claim moved");
        verify(drawn.item.describe().length > 0, "and the panel has something to say");

        // And away again on the next frame, or a screen that recovers stays covered for good.
        drawn.surface.item.frameShown = true;
        verify(!drawn.backdrop.visible, "the message goes away again once drawing resumes");
        compare(drawn.item.rendererDrawing, true, "the renewed claim reaches the plugin");
    }

    // GOAL: the reason the renderer gave is what the desktop shows. False if the message is
    //       generic -- the user is then told the screen cannot be drawn on with no way to learn
    //       whether it was the scene graph, the driver, or the daemon, and the renderer had
    //       already said.
    // WHY-THIS-SHAPE: an unmistakable reason string, so `indexOf` cannot pass on a coincidence
    //       with the plugin's own wording, and the rendered Text is read rather than describe()
    //       alone -- a message that is right on demand but never reaches the item on the desktop
    //       is the blank-desktop failure by another route, and the "set" state is where the two
    //       are easiest to confuse, since the wallpaper text and describe() agree there.
    // CONTROL: the rendererReason arm of describe() in main.qml reduced to the same text with no
    //       `.arg(...)` -> "FAIL!: 'the renderer's own reason reaches the desktop -- looked for
    //       [dmabuf import refused: ...] in [WallpaperX cannot draw on this screen:]' returned
    //       FALSE". The rendered half seen red separately, with the Text's binding made to return
    //       the wallpaper message directly whenever the state is "set" -> "FAIL!: the desktop
    //       shows the reason, not a message it was handed once", that check alone and no other in
    //       the file. Observed 2026-08-21. Both reverted.
    function test_the_message_carries_the_renderers_own_reason() {
        const drawn = drawnScreenWithStandIn();
        const reason = "dmabuf import refused: modifier 0x4000000 unsupported by this driver";

        drawn.surface.item.unavailableReason = reason;
        const shown = drawn.item.describe();
        verify(shown.indexOf(reason) !== -1,
               "the renderer's own reason reaches the desktop -- looked for [" + reason
               + "] in [" + shown + "]");

        const text = onlyDescendantWith(drawn.item, "wrapMode");
        compare(text.text, shown, "the desktop shows the reason, not a message it was handed once");
        verify(text.visible && text.paintedHeight > 0,
               "the reason takes up space on the desktop [" + text.text + "]");
    }

    // GOAL: main.qml survives a QML module it cannot load. That is the whole reason the renderer
    //       sits behind a Loader rather than an import: a failed import at the top of this file
    //       takes the plugin down, and a wallpaper plugin that fails to load is a black desktop
    //       with nothing logged.
    // WHY-THIS-SHAPE: the drive carries a monitor_id, which is what makes the Loader ACTIVE -- and
    //       activation is the whole point. suite.goodReply has no id, so a drive using it leaves
    //       `active` false, the module load never attempted, and every assertion below satisfied
    //       by a Loader that is simply switched off.
    //
    //       WallpaperXPlugin cannot load here because main.qml is loaded from the source tree's
    //       package, which holds no WallpaperXPlugin folder, and its directory import never searches
    //       the import path, so no copy on the host can satisfy it. The harness checks that folder is
    //       absent (rendererModule) rather than assuming it.
    //       The change watch is stood in (loadPlugin), so the watch's own failure message, which
    //       describe() puts first, does not answer for the renderer's.
    // CONTROL: the `if (surface.status === Loader.Error)` block deleted from describe() in
    //       backend/kde/kpackage/contents/ui/main.qml, so it falls through to the "will draw" line
    //       -> "FAIL!: the plugin does not claim it will draw when the module could not be loaded",
    //       that case alone. Re-observed 2026-11-05 via control_batch.sh.
    // COVERAGE-ASSERT: the absence is asserted, so a source tree that gained a WallpaperXPlugin
    //       folder beside main.qml stops this case instead of letting it grade a module that loaded.
    //       CONTROL: test-side, by hand 2026-11-05: absentBesideMainQml() in qml_states_test.cpp pointed at
    //       contents/ui itself, which exists -> "'main.qml is loaded from the source tree, which holds no
    //       renderer module' returned FALSE", in both tests. Restored, green.
    function test_a_missing_renderer_module_leaves_the_message_working() {
        const item = drivenPlugin({
            connector: suite.namedScreen,
            drive: [200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":0}']
        });
        const surface = childNamed(item, "wallpaperxSurface");
        verify(surface !== null, "the plugin has a surface");
        verify(surface.active, "the Loader actually tried to load the renderer module");
        verify(rendererModule.absentBesideMainQml(),
               "main.qml is loaded from the source tree, which holds no renderer module");
        compare(surface.status, Loader.Error,
                "the module cannot be loaded on this harness, which is the state being graded");
        // Content, not length: every possible string has a length, so the old check could not
        // fail -- and the state it was meant to catch is the plugin claiming it will draw.
        const shown = item.describe();
        verify(shown.indexOf("will draw") === -1,
               "the plugin does not claim it will draw when the module could not be loaded [" + shown + "]");
        verify(shown.indexOf("could not be loaded") !== -1,
               "the message names the module as the cause [" + shown + "]");
        const backdrop = childNamed(item, "wallpaperxBackdrop");
        verify(backdrop !== null && backdrop.visible,
               "the message is still shown when the renderer module cannot be loaded");
    }

    // GOAL: a hidden copy of the plugin has no renderer, and showing it again creates one before
    //       anything else runs. Plasma keeps a hidden copy per activity on each screen, under the same
    //       screen name; a renderer there reports for the screen the user is looking at. And on an
    //       activity switch the shown copy's renderer must exist inside the switch, not after a poll.
    // WHY-THIS-SHAPE: hidden through its parent, as Plasma hides a containment, on a screen whose
    //       Loader was active before the hide, so `active` going false can only be the hide's doing.
    //       The "shown again" half is read with no wait at all, so a renderer that arrives a turn
    //       later -- or only after the next poll -- fails it.
    // CONTROL: in backend/kde/kpackage/contents/ui/main.qml, `root.visible &&` dropped from the
    //       surface Loader's `active` -> "a hidden copy has no renderer" red. `onVisibleChanged: if
    //       (!visible) root.reply = null;` added to the root, so a shown copy waits for its next poll
    //       -> "showing it again creates the renderer at once" red. (`asynchronous: true` on that
    //       Loader reddens this case too, but at drawnScreenWithStandIn()'s precondition first.)
    //       Both observed 2026-10-01 through control_batch.sh, reverted.
    function test_a_hidden_copy_has_no_renderer_and_gets_one_back_at_once() {
        const drawn = drawnScreenWithStandIn();
        const host = drawn.item.parent;
        verify(drawn.surface.active, "the renderer is loaded before the hide, which is what lets this fail");
        host.visible = false;
        verify(!drawn.item.visible, "the plugin is hidden through its parent");
        verify(!drawn.surface.active, "a hidden copy has no renderer");
        compare(drawn.surface.item, null, "and its renderer item is gone");

        host.visible = true;
        compare(drawn.surface.status, Loader.Ready, "showing it again creates the renderer at once");
        verify(drawn.surface.item !== null, "the renderer exists without waiting for anything");
    }

    // GOAL: S15.203 -- a hidden copy reads /config at a change notice, so that when it is shown its
    //       answer is current and the renderer is created inside the switch rather than after a read.
    // WHY-THIS-SHAPE: a real request to the stub, counted while the copy is hidden, after the stand-in
    //       watch emits the one signal main.qml listens to; exactly one, so a hidden copy that asked on
    //       anything else would show.
    // CONTROL: `enabled: root.visible` given to the `Connections` on `notice.item` in
    //       backend/kde/kpackage/contents/ui/main.qml -> "a hidden copy reads /config at a notice" red.
    //       Re-run on the plugin's own read, control_batch.sh 2026-11-02, reverted: same line red.
    function test_a_hidden_copy_reads_at_a_notice() {
        requireRealRead();
        const host = loadHost();
        const item = host.item;
        const watch = standInTheWatch(item);
        item.connectorOverride = suite.namedScreen;
        host.visible = false;
        verify(!item.visible, "the plugin is hidden through its parent");
        tryVerify(function () { return item.pending === null; }, 5000, "any request in flight has settled");
        stubDaemon.reset();
        verify(watch.item !== null, "a hidden copy still has its change watch");
        watch.item.configChanged();
        tryVerify(function () { return stubDaemon.requests() >= 1; }, 5000, "a hidden copy reads /config at a notice");
        tryVerify(function () { return item.pending === null; }, 5000, "and the read is answered");
        compare(stubDaemon.requests(), 1, "one notice, one read");
    }

    // GOAL: S15.392 -- a change watch whose module cannot be loaded is named as the cause on a named
    //       screen, ahead of whatever the last answer said: with nothing to say when that answer
    //       changes, any other cause shown could be long out of date.
    // WHY-THIS-SHAPE: driven through two answers that would otherwise each name their own cause (an
    //       unknown screen and a daemon gone), so the message is graded against a cause it must
    //       outrank, not only for containing the right words. The watch cannot load because the
    //       source-tree package has no WallpaperXPlugin folder (asserted), and it is required to be in
    //       Loader.Error, so a tree where it loaded cannot pass.
    // CONTROL: the `if (notice.status === Loader.Error)` branch deleted from describe() in
    //       backend/kde/kpackage/contents/ui/main.qml -> "an unloadable change watch is named as the
    //       cause" red, showing "does not know this screen". Re-observed 2026-11-05.
    function test_an_unloadable_watch_is_named_as_the_cause() {
        const item = loadHost().item;
        const watch = childNamed(item, "wallpaperxConfigWatch");
        verify(watch !== null, "main.qml has a change watch");
        verify(rendererModule.absentBesideMainQml(), "main.qml is loaded from the source tree, which holds no renderer module");
        compare(watch.status, Loader.Error, "the change watch could not be loaded, which is the state graded");
        item.connectorOverride = suite.namedScreen;
        tryVerify(function () { return item.pending === null; }, 5000, "the naming read has settled");

        const drives = [[200, '{"output":"DP-3","known":false}'], [0, ""]];
        for (let i = 0; i < drives.length; ++i) {
            item.apply(drives[i][0], drives[i][1]);
            const shown = item.describe();
            verify(shown.indexOf("cannot follow changes") !== -1,
                   "an unloadable change watch is named as the cause [" + shown + "]");
            verify(shown.indexOf("does not know this screen") === -1 && shown.indexOf("is not running") === -1,
                   "and not a cause it could no longer follow [" + shown + "]");
        }
    }
}
