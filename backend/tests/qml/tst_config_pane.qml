// SPDX-License-Identifier: GPL-2.0-or-later

// S11.1, S11.2 and S11.4: the pane Plasma shows under Wallpaper settings for a WallpaperX
// screen. Driven through the real config.qml, loaded headless the same way tst_wallpaper_states
// loads main.qml -- not a copy of its contents, which would drift from the file that ships.
//
// What this file does NOT cover, so nobody reads it as covering more than it does: S11.3 (no
// control writes wallpaper state) and S11.5 (no KConfigXT schema) are claims about the package
// as shipped, not about a loaded instance, and are graded in kpackage_metadata_test.cpp.

import QtQuick
import QtTest
import org.kde.kirigami as Kirigami

TestCase {
    id: suite

    name: "WallpaperXConfigPane"

    // Qt Quick Test leaves its TestCase item hidden, and `visible` is the effective value: with
    // it left alone every item below is invisible whatever the pane declares, and the check
    // that the heading is visible would pass on the harness rather than on config.qml.
    visible: true

    // Through a Loader for the reason the states harness uses one: the pane imports Kirigami and
    // Plasma5Support, and a failed import inside a Component surfaces as a Loader status rather
    // than as a thrown QML error nothing here would catch.
    Component {
        id: pane
        Loader {
            width: 600
            height: 400
            source: "../../kde/kpackage/contents/ui/config.qml"
        }
    }

    function loadPane() {
        const host = createTemporaryObject(pane, suite);
        verify(host !== null, "the pane component was created: " + pane.errorString());
        compare(host.status, Loader.Ready,
                "config.qml loaded (Loader status " + host.status + ")");
        const item = host.item;
        verify(item !== null, "config.qml produced an item");
        return item;
    }

    function heading(item) {
        for (let i = 0; i < item.children.length; ++i) {
            if (item.children[i].objectName === "wallpaperxConfigHeading") {
                return item.children[i];
            }
        }
        return null;
    }

    // GOAL: S11.1 -- the pane says where wallpapers are configured. False if it is empty: a user
    //       who opens Plasma's wallpaper settings and finds a blank pane has been told the
    //       plugin is broken, when the emptiness is deliberate.
    // WHY-THIS-SHAPE: the heading is found by objectName rather than by index into children,
    //       so adding a row above it is not a failure; and the text is asserted to name the app
    //       rather than merely to be non-empty, because a pane that said "..." would pass the
    //       weaker form while telling the user nothing.
    // CONTROL: `text:` deleted from the Kirigami.Heading in
    //       backend/kde/kpackage/contents/ui/config.qml -> "FAIL: 'the heading says something'
    //       returned FALSE. ()", one red in the run, no skips. Observed 2026-08-20. Reverted.
    function test_the_pane_says_where_wallpapers_are_configured() {
        const item = loadPane();
        const title = heading(item);
        verify(title !== null, "the pane has a heading to read");
        verify(title.text.length > 0, "the heading says something");
        verify(title.text.indexOf("WallpaperX") !== -1,
               "the heading names the app: [" + title.text + "]");
        verify(title.visible, "the heading is visible");
    }

    // GOAL: S11.1's other half -- the heading is in TRANSLATED text, not a hardcoded string.
    //       False if the string is written into the file bare: the pane then renders English on
    //       every desktop, and the check above cannot tell, because it reads identical
    //       characters either way.
    // WHY-THIS-SHAPE: nothing about the rendered text distinguishes the two -- qsTr() with no
    //       translation loaded returns its own source string. The obvious alternative, grepping
    //       config.qml for "qsTr", asserts the spelling of the fix and passes for a qsTr sitting
    //       on some other string entirely. So a translator is installed for the length of three
    //       statements and the binding is asked to re-evaluate: only a qsTr binding moves.
    //       No assertion runs between installing and removing it, deliberately -- a failure
    //       there would leave every later case in this process reading rewritten strings.
    // CONTROL: `qsTr(...)` on the Kirigami.Heading in config.qml replaced by the bare string ->
    //       "FAIL: the heading is translated text, not a string written into the file: it did
    //       not change when a translation was loaded", that check alone. Observed 2026-08-21.
    //       Reverted.
    function test_the_heading_is_translated_text() {
        const item = loadPane();
        const title = heading(item);
        verify(title !== null, "the pane has a heading to read");

        const before = title.text;
        verify(translationProbe.translateEverything(),
               "the harness could install a translation at all");
        const translated = title.text;
        translationProbe.stopTranslating();
        const restored = title.text;

        verify(translated !== before,
               "the heading is translated text, not a string written into the file: it did not "
               + "change when a translation was loaded [" + before + "]");
        verify(translated.indexOf(translationProbe.translationMarker()) === 0,
               "the heading went through the loaded translation: [" + translated + "]");
        compare(restored, before,
                "the harness put the untranslated text back for the cases that follow");
    }

    // GOAL: S11.2 -- that text is legible against the settings background, because its colour
    //       comes from the theme rather than being written into the file. False if a colour is
    //       hardcoded: the pane shipped with color: "#ffffff", which is white on the light
    //       Breeze dialog -- a message nobody can read, which is the same defect as no message.
    // WHY-THIS-SHAPE: compared against Kirigami.Theme.textColor rather than asserted to be
    //       "not white". A not-white check passes for any other hardcoded colour, including one
    //       that happens to match this theme and no other; equality with the theme's own value
    //       is the property that actually holds across themes.
    // CONTROL: `color: "#ffffff"` added to the Kirigami.Heading in config.qml -- which is
    //       the exact defect the pane shipped with, so this check would have caught it -> "FAIL:
    //       the heading's colour is the theme's text colour, not one written into the file", one
    //       red in the run, no skips. Observed 2026-08-20. Reverted.
    function test_the_heading_takes_its_colour_from_the_theme() {
        const item = loadPane();
        const title = heading(item);
        verify(title !== null, "the pane has a heading to read");
        compare(title.color.toString(), Kirigami.Theme.textColor.toString(),
                "the heading's colour is the theme's text colour, not one written into the file");
    }

    // GOAL: S11.4 -- a launch that fails says so. False if it is silent: a button that appears
    //       to do nothing leaves the user believing the app is missing, which is the one thing
    //       this pane exists to rule out.
    // WHY-THIS-SHAPE: the mapping is driven as a function rather than by running a process,
    //       because the process is the part this harness cannot stage. The failing codes are
    //       asserted to differ from EACH OTHER, not just to be non-empty: 127 is the shell's
    //       "not found" for kstart itself, so reporting it as "the app is not installed" sends
    //       the user to reinstall the wrong thing.
    // WHY-124-IS-HERE: measured against the real command, not assumed. `kstart --application`
    //       on an id it cannot resolve waits for a window that never appears -- still running
    //       at 60 s -- so the launch is wrapped in timeout(1), and 124 is that wrapper reporting
    //       it ended a command that had not finished. Unbounded, the likeliest failure of all
    //       would produce no exit code at all and the pane would stay silent forever.
    // CONTROL: `if (code === 127)` changed to `if (false)` in config.qml's
    //       launchFailureMessage() -> a missing launcher reported as a missing app; one red, no
    //       skips. Observed 2026-08-20 against the then-two-arm form. Reverted.
    // CONTROL: `if (code === 124)` changed to `if (false)` -> "FAIL: 'an app that never appeared
    //       and a failed start are told apart: [The WallpaperX app could not be started.] vs
    //       [the same again]' returned FALSE", one red in the run, no skips. Observed
    //       2026-08-20. Reverted.
    function test_a_failed_launch_is_reported() {
        const item = loadPane();

        compare(item.launchFailureMessage(0), "", "a launch that worked says nothing");

        const missingLauncher = item.launchFailureMessage(127);
        const neverAppeared = item.launchFailureMessage(124);
        const failedStart = item.launchFailureMessage(1);
        verify(missingLauncher.length > 0, "a missing launcher is reported");
        verify(neverAppeared.length > 0, "a launch that never produced a window is reported");
        verify(failedStart.length > 0, "a failed start is reported");

        // Pairwise, so a build that collapsed any two of them is red rather than only a build
        // that collapsed all three.
        verify(missingLauncher !== neverAppeared,
               "a missing launcher and an app that never appeared are told apart: ["
               + missingLauncher + "] vs [" + neverAppeared + "]");
        verify(missingLauncher !== failedStart,
               "a missing launcher and a failed start are told apart: ["
               + missingLauncher + "] vs [" + failedStart + "]");
        verify(neverAppeared !== failedStart,
               "an app that never appeared and a failed start are told apart: ["
               + neverAppeared + "] vs [" + failedStart + "]");

        // COVERAGE-ASSERT: the timeout the launch is wrapped in is real and non-zero. It says
        // nothing about the command using it -- that is the check below, which reads the command
        // the pane actually composes.
        verify(item.launchTimeoutSeconds > 0,
               "the launch is bounded, so 124 is a code this pane can actually see");
    }

    // The pane's launcher, found by the pair of members only it has. Non-visual, so it lives in
    // data[] rather than children[], and it is found by identity rather than by index because
    // the index depends on declaration order in a production file.
    function launcherOf(item) {
        for (let i = 0; i < item.data.length; ++i) {
            const child = item.data[i];
            if (child.connectedSources !== undefined && typeof child.launch === "function") {
                return child;
            }
        }
        return null;
    }

    // GOAL: S11.4 -- the command the pane runs is actually bounded, so the failure it reports as
    //       124 is one it can reach. False if the bound is dropped from the command: kstart on an
    //       id it cannot resolve waits forever -- measured, still running at 60 s -- so the most
    //       likely failure of all produces no exit code, the pane stays silent, and the button
    //       appears to do nothing. Which is the whole defect S11.4 names.
    // WHY-THIS-SHAPE: the composed command is read, not the property it is built from. The
    //       simpler form next door -- assert launchTimeoutSeconds > 0 -- passes for a build that
    //       keeps the property and stops using it, which is exactly the unbounded launch. The
    //       DataSource is pointed at an engine that does not exist first, so the command is
    //       composed and recorded WITHOUT being run: launching for real would either start the
    //       app on the developer's desktop or block this case for the full timeout.
    // CONTROL: the "timeout N " prefix removed from connectSource() in config.qml -> "FAIL: the
    //       command the pane runs is bounded by a timeout: [kstart --application
    //       com.wallpaperx.app]", that check alone. Observed 2026-08-21. Reverted.
    // COVERAGE-ASSERT: exactly one command is asserted to have been requested, so a launcher
    //       that was neutered into connecting nothing fails here rather than reading an empty
    //       list and finding nothing wrong with it.
    function test_the_launch_command_is_bounded() {
        const item = loadPane();
        const launcher = launcherOf(item);
        verify(launcher !== null, "the pane has a launcher whose command can be read");

        launcher.engine = "wallpaperx-no-such-engine";
        launcher.launch();

        compare(launcher.connectedSources.length, 1,
                "the pane asked to run exactly one command");
        const command = launcher.connectedSources[0];
        verify(command.indexOf("timeout " + item.launchTimeoutSeconds + " ") === 0,
               "the command the pane runs is bounded by a timeout: [" + command + "]");
        verify(command.indexOf("kstart") !== -1,
               "the command launches through the desktop launcher: [" + command + "]");
    }

    // GOAL: the pane starts with nothing to apologise for -- the error is shown only once a
    //       launch has actually failed. False if it is visible from the start: an error sitting
    //       there before the user has clicked anything reports a failure that has not happened.
    // CONTROL: `visible: root.launchError !== ""` on the InlineMessage replaced by
    //       `visible: true` in config.qml -> "FAIL: 'the error message is hidden until a launch
    //       fails' returned FALSE. ()", one red in the run, no skips. Observed 2026-08-20.
    //       Reverted.
    function test_no_error_is_shown_before_a_launch() {
        const item = loadPane();
        compare(item.launchError, "", "the pane opens with no launch error");

        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i];
            if (child instanceof Kirigami.InlineMessage) {
                verify(!child.visible, "the error message is hidden until a launch fails");
            }
        }
    }
}
