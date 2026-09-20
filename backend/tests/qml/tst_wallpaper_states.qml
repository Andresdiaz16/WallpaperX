// SPDX-License-Identifier: GPL-2.0-or-later

// S10.10: whatever has gone wrong, the plugin says something visible and says which cause it
// is, in characters rather than in markup. The seven cases main.qml derives are driven here
// through its real file -- not a copy of its logic, which would drift -- and both halves are
// asserted for each: the state the binding produces and the message describe() hands to the
// Text that lands on the desktop.
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
        item.connectorOverride = connector;
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
    // The two decisions main.qml makes inside refresh()'s onreadystatechange handler, ahead
    // of the apply() call every case above drives. Reaching them needs a real HTTP reply, so
    // these run against the stub daemon tests/qml_states_test.cpp puts on 127.0.0.1:18462 --
    // see that file for why it is that port and not one of the harness's choosing.
    // ---------------------------------------------------------------------------------- //

    // Loads a plugin, waits out any request its own start fired, and points the stub at the
    // shape this case wants. The wait is not decoration: main.qml's refresh() counts a poll
    // and returns when one is already pending, so a case that called it while a startup fetch
    // was in flight would send nothing and then grade the startup fetch's answer.
    function pluginReadyToFetch() {
        const item = loadPlugin(suite.namedScreen);
        tryVerify(function () { return item.pending === null; }, 5000,
                  "the request the plugin's own start may have fired has settled");
        stubDaemon.reset();
        return item;
    }

    // GOAL (a property of this harness): the stub really does serve a reply the plugin
    //       accepts over real HTTP. False if the transport is broken -- in which case the two
    //       cases below would come out "unreadable" or "unreachable" whatever main.qml did,
    //       and would establish nothing about the origin check or the size bound.
    // WHY-THIS-SHAPE: the same body the oversized case pads and the same body the foreign
    //       origin serves, so those two differ from this one in exactly one respect each.
    //       The request count is asserted, not just the state: a plugin that never sent
    //       anything and a stub that never answered both leave "unreachable", and only the
    //       counter tells them apart.
    // CONTROL: `res.status = 500` added to the stub's Good arm in tests/qml_states_test.cpp
    //       -> "FAIL: a reply served over HTTP reaches the set state. Actual (): refused.
    //       Expected (): set", 23 other cases green. Observed 2026-08-19. Reverted.
    function test_the_stub_serves_a_reply_the_plugin_accepts() {
        if (!stubDaemon.available()) {
            skip(stubDaemon.unavailableReason());
        }
        const item = pluginReadyToFetch();
        stubDaemon.serveGood();
        item.refresh();

        tryCompare(item, "screenState", "set", 5000,
                   "a reply served over HTTP reaches the set state");
        compare(stubDaemon.requests(), 1, "the plugin made exactly one request to the daemon's origin");
        compare(item.wallpaperPath, "/home/u/Pictures/beach.png", "the served path reached the binding");
    }

    // GOAL: a reply that did not come from root.daemonOrigin is treated as unreadable, whatever
    //       it holds. False if the plugin parses it: a redirect is enough to make every screen's
    //       wallpaper plugin fetch from wherever a process on loopback names, once a second,
    //       from inside plasmashell -- and then act on what it gets back.
    // WHY-THIS-SHAPE: the foreign origin serves the SAME body the accepted case above serves,
    //       and a 200 at that. So the two runs differ in the origin and in nothing else, and
    //       "unreadable" here cannot be caused by the content. The redirect target is a second
    //       real listener on an ephemeral port rather than the same socket under another host
    //       name, which would rest on how Qt resolves "localhost". The alt server's own request
    //       count is asserted, because a redirect Qt declined to follow would leave the plugin
    //       reading the 302's empty body -- also "unreadable", for a reason this goal is not
    //       about, and it would pass a state-only check.
    // CONTROL: `elsewhere` dropped from the `usable` expression in main.qml's
    //       onreadystatechange -> "FAIL: a reply redirected away from the daemon's origin is
    //       unreadable. Actual (): set. Expected (): unreadable" -- the plugin having acted on
    //       a wallpaper served by a foreign origin. That single mutation is also what shows the
    //       check is not vacuous on this Qt: if responseURL were empty, `elsewhere` would be
    //       false either way and both builds would read "set". 23 other cases green. Observed
    //       2026-08-19 on Qt 6.11.2. Reverted.
    function test_a_reply_from_another_origin_is_unreadable() {
        if (!stubDaemon.available()) {
            skip(stubDaemon.unavailableReason());
        }
        const item = pluginReadyToFetch();
        stubDaemon.serveFromAnotherOrigin();
        item.refresh();

        tryCompare(item, "screenState", "unreadable", 5000,
                   "a reply redirected away from the daemon's origin is unreadable");
        compare(stubDaemon.requests(), 1, "the plugin asked the daemon's origin first");
        compare(stubDaemon.altRequests(), 1,
                "Qt followed the redirect, so what the plugin refused is a served reply and not an empty 302 body");
        verify(item.describe().indexOf("cannot read") !== -1,
               "the desktop message names the unreadable cause: [" + item.describe() + "]");
    }

    // GOAL: a body larger than maxBodyBytes is treated as unreadable rather than parsed. False
    //       if the plugin buffers and parses whatever arrives: the reply comes from whatever
    //       holds the port, nothing else bounds what the shell takes in, and the parse runs on
    //       plasmashell's GUI thread.
    // WHY-THIS-SHAPE: the oversized body is VALID JSON describing the same wallpaper the
    //       accepted case above serves, so its SIZE is the only thing that can make it
    //       unreadable. A check driven with garbage would be satisfied by the JSON parser and
    //       would say nothing about the bound.
    //       Served CHUNKED, deliberately, and that is not a detail: main.qml has two
    //       independent bounds, and chunked is the only shape that reaches the one graded here.
    //       With a Content-Length present the plugin refuses at HEADERS_RECEIVED instead, and
    //       that path CANNOT BE DRIVEN -- it calls request.abort(), which segfaults Qt's
    //       XMLHttpRequest. See the block below; the coverage that costs is stated there rather
    //       than papered over with a row that crashes the binary.
    // CONTROL: `body.length <= root.maxBodyBytes` in main.qml's onreadystatechange replaced by
    //       `true` -> "FAIL: an oversized reply with no declared length is unreadable. Actual
    //       (): set. Expected (): unreadable" -- 70 KB parsed and acted on. 23 other cases
    //       green. Observed 2026-08-19. Reverted.
    // COVERAGE-ASSERT: the served body is asserted to be larger than the plugin's own
    //       maxBodyBytes, read off the plugin rather than written as a literal. A fixture that
    //       stopped being oversized -- or a ceiling raised past 70000 -- fails here instead of
    //       going green against a body the bound never applied to.
    //
    // WAS NOT COVERABLE, and the defect it named is now fixed: driving a reply with an
    // oversized `Content-Length` reached `request.abort()` inside the HEADERS_RECEIVED handler,
    // and the process died with SIGSEGV in QIODevice::readAll() under QQmlXMLHttpRequest on the
    // next network event -- so the defence let anything holding 127.0.0.1:18462 kill
    // plasmashell. main.qml no longer refuses at HEADERS_RECEIVED; a declared length is left to
    // the same bound below, which catches it a moment later. A row serving 200 with a valid
    // body and `Content-Length: 70050` should now pass here, and adding one would cover the
    // second arm -- it is not shipped because nobody has run it.
    function test_an_oversized_reply_is_unreadable() {
        if (!stubDaemon.available()) {
            skip(stubDaemon.unavailableReason());
        }
        const item = pluginReadyToFetch();
        verify(stubDaemon.oversizedBytes() > item.maxBodyBytes,
               "the stub's oversized body (" + stubDaemon.oversizedBytes() + " bytes) is past the plugin's "
               + "ceiling of " + item.maxBodyBytes);

        stubDaemon.serveOversizedChunked();
        item.refresh();

        tryCompare(item, "screenState", "unreadable", 5000,
                   "an oversized reply with no declared length is unreadable");
        compare(stubDaemon.requests(), 1, "the plugin made exactly one request");
        verify(item.describe().indexOf("cannot read") !== -1,
               "the desktop message names the unreadable cause: [" + item.describe() + "]");
    }

    // GOAL: the same bound holds for a reply that DECLARES its size, and reaching that verdict
    //       does not kill the process. Both halves matter: the plugin used to refuse this shape
    //       at HEADERS_RECEIVED by calling abort() from inside the handler, which segfaults
    //       Qt's XMLHttpRequest -- so the defence against a hostile reply was itself a way for
    //       whatever holds the port to take plasmashell down.
    // WHY-THIS-SHAPE: a separate function from the chunked row above, not a data row beside it.
    //       Under the defect this does not fail, it ABORTS -- and a crash in a shared function
    //       destroys the other row's diagnostics too. Kept apart so a regression prints as one
    //       named case dying rather than as the file going silent.
    // CONTROL: (a) `body.length <= root.maxBodyBytes` in main.qml's DONE handler replaced by
    //       `true` -> "FAIL: an oversized reply that declares its length is unreadable. Actual
    //       (): set. Expected (): unreadable", the chunked row red beside it. (b) the
    //       HEADERS_RECEIVED arm put back -- `if (!isNaN(declared) && declared >
    //       root.maxBodyBytes) { ... request.abort(); root.apply(status, ""); }` -> ctest
    //       reports `qml_states (SEGFAULT)` with a stack trace in place of results, and no
    //       case in this file reports at all. Both observed 2026-08-20, both reverted.
    function test_an_oversized_reply_that_declares_its_length_is_unreadable() {
        if (!stubDaemon.available()) {
            skip(stubDaemon.unavailableReason());
        }
        const item = pluginReadyToFetch();
        verify(stubDaemon.oversizedBytes() > item.maxBodyBytes,
               "the stub's oversized body (" + stubDaemon.oversizedBytes() + " bytes) is past the plugin's "
               + "ceiling of " + item.maxBodyBytes);

        stubDaemon.serveOversizedWithLength();
        item.refresh();

        tryCompare(item, "screenState", "unreadable", 5000,
                   "an oversized reply that declares its length is unreadable");
        compare(stubDaemon.requests(), 1, "the plugin made exactly one request");
        verify(item.describe().indexOf("cannot read") !== -1,
               "the desktop message names the unreadable cause: [" + item.describe() + "]");
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
    //       shows its message. The renderer is opaque and fills the item, so a build that
    //       activates it in a non-"set" state covers the only thing the user had to read.
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
        compare(surface.item.monitorId, 0, "the renderer is told which screen to ask about");
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
    //       Whether com.wallpaperx loads is MADE FALSE by this harness rather than assumed absent
    //       (qml_states_test.cpp rewrites its qmldir URL to one that does not exist). It used to be
    //       absent by accident, because a test binary's import path does not include the module it
    //       is built beside -- and the moment the plugin was installed system-wide for a live
    //       desktop test the import succeeded and this case failed against a correct build. A check
    //       that reads differently depending on what is installed grades the machine.
    // CONTROL: the `if (surface.status === Loader.Error)` block deleted from describe() in
    //       backend/kde/kpackage/contents/ui/main.qml, so it falls through to the "will draw" line
    //       -> "FAIL!: the plugin does not claim it will draw when the module could not be loaded"
    //       and "FAIL!: the message names the module as the cause", that case alone. Observed
    //       2026-09-20. Reverted.
    // COVERAGE-ASSERT: the interceptor is asserted to have rewritten at least one candidate qmldir
    //       URL, so a run in which it silently stopped matching cannot report Loader.Error as
    //       evidence of anything -- on a machine with no module installed the state would look
    //       identical.
    function test_a_missing_renderer_module_leaves_the_message_working() {
        const item = drivenPlugin({
            connector: suite.namedScreen,
            drive: [200, '{"output":"DP-3","known":true,"path":"/w.png","monitor_id":0}']
        });
        const surface = childNamed(item, "wallpaperxSurface");
        verify(surface !== null, "the plugin has a surface");
        verify(surface.active, "the Loader actually tried to load the renderer module");
        verify(moduleBlock.blocked() > 0,
               "this harness made com.wallpaperx unloadable, rather than hoping it was absent");
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
}
