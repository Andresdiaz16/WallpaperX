#!/usr/bin/env bash
# S15.15: the app, installed to a fresh prefix and run from outside its build tree, loads Main.qml
# and keeps running.
#
#   installed_app_test.sh CMAKE APP_BUILD_DIR DBUS_RUN_SESSION BUS_CONFIG
#
# Exit 0 pass, 1 fail or harness fault (the lines say which), 77 cannot be answered here.

set -u
cmake="$1" build_dir="$2" dbus_run_session="$3" bus_config="$4"
name=app_installed_starts

prefix="" drs_pid="" app_pid="" pidfile="" busfile="" launched=0
# dbus-run-session stops its bus daemon only when its child exits; signalled itself, it dies
# first and the daemon (--nofork) lives on. So only the app is ever signalled.
#
# GOAL (harness): no run leaves the private bus daemon running, whichever way it ends.
# WHY-THIS-SHAPE: the daemon's pid is recorded inside the session before the app starts, so a path
#   where the app exits by itself is checked too; dbus-run-session sends its daemon SIGTERM and does
#   not wait, so the check polls up to 5 s before calling it a leak, and asks whether the pid is
#   still this run's dbus-daemon (name and config file), not merely alive. An unrecorded pid is
#   reported, never taken as "no leak".
# CONTROL: by hand, test-side, each observed: cleanup signalling dbus-run-session before its wait
#   -> "FAIL (harness): the private bus daemon <pid> outlived its dbus-run-session", exit 1; the
#   inner shell killing dbus-run-session instead of starting the app -> "FAIL: S15.15: the app
#   exited with status 137 ..." then the same harness line; the app ignoring SIGTERM (the SIGKILL
#   path) -> passes, no leak reported; the pid never written -> "FAIL (harness): the private bus
#   daemon's pid was never recorded ...", exit 1. Restored.
bus_alive() {
    [ "$(cat "/proc/$1/comm" 2>/dev/null)" = dbus-daemon ] &&
        tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null | grep -qF -- "$bus_config"
}
cleanup() {
    local bus_pid=""
    [ -n "$busfile" ] && [ -s "$busfile" ] && bus_pid="$(cat "$busfile")"
    if [ -n "$drs_pid" ]; then
        for _ in $(seq 1 50); do
            [ -z "$app_pid" ] && [ -s "$pidfile" ] && app_pid="$(cat "$pidfile")"
            [ -n "$app_pid" ] && kill "$app_pid" 2>/dev/null
            kill -0 "$drs_pid" 2>/dev/null || break
            sleep 0.1
        done
        # An app that ignored SIGTERM, or a pid never recorded: kill every child but the bus,
        # which dbus-run-session then stops itself.
        if kill -0 "$drs_pid" 2>/dev/null; then
            [ -n "$app_pid" ] && kill -KILL "$app_pid" 2>/dev/null
            for child in $(pgrep -P "$drs_pid"); do
                [ "$child" = "$bus_pid" ] || kill -KILL "$child" 2>/dev/null
            done
        fi
        wait "$drs_pid" 2>/dev/null
    fi
    [ -n "$prefix" ] && rm -rf "$prefix"
    if [ "$launched" = 1 ] && [ -z "$bus_pid" ]; then
        echo "FAIL (harness): the private bus daemon's pid was never recorded, so no check that it stopped was made" >&2
        exit 1
    fi
    [ -n "$bus_pid" ] || return
    for _ in $(seq 1 50); do
        bus_alive "$bus_pid" || return
        sleep 0.1
    done
    echo "FAIL (harness): the private bus daemon $bus_pid outlived its dbus-run-session" >&2
    kill "$bus_pid" 2>/dev/null
    exit 1
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP
harness() { echo "FAIL (harness): $*" >&2; echo "$name: NOT VERIFIED -- the installed app was not run as intended" >&2; exit 1; }
fail() {
    echo "FAIL: S15.15: $*" >&2
    echo "--- app stderr (QML import trace removed) ---" >&2
    grep -v '^qt\.qml\.import' "$log" | head -40 >&2
    exit 1
}

prefix="$(mktemp -d "${TMPDIR:-/tmp}/wallpaperx-install.XXXXXX")" || harness "no temporary prefix"
case "$(realpath "$prefix")/" in
    "$(realpath "$build_dir")"/*) harness "the temporary prefix $prefix is inside the build tree" ;;
esac
if ! "$cmake" --install "$build_dir" --prefix "$prefix" > "$prefix/install.log" 2>&1; then
    cat "$prefix/install.log" >&2; harness "cmake --install $build_dir failed"
fi
app="$prefix/bin/wallpaperx"
[ -x "$app" ] && [ ! -L "$app" ] || harness "the install produced no executable $app"

# GOAL (harness precondition): the app is run with no qmldir on disk that it could find its module by.
# WHY-THIS-SHAPE: running app/build/wallpaperx (or anything beside a WallpaperX/qmldir, or with a
#   QML import path set) passes even when the module is only reachable through that qmldir, so any
#   qmldir under the prefix (which holds the working directory) is a harness fault, not a pass.
# CONTROL: by hand, test-side: `cp -r "$build_dir/WallpaperX" "$prefix/bin/"` added before this
#   guard gave "FAIL (harness): a qmldir is reachable from the installed app: …/bin/WallpaperX/qmldir",
#   exit 1; `--install "$build_dir/nonexistent"` gave "FAIL (harness): cmake --install … failed", exit 1.
work="$prefix/work"
mkdir -p "$work" "$prefix/xdg/config" "$prefix/xdg/cache" "$prefix/xdg/data" "$prefix/xdg/state"
stray="$(find "$prefix" -name qmldir -print -quit)"
[ -z "$stray" ] || harness "a qmldir is reachable from the installed app: $stray"

log="$prefix/app.log" pidfile="$prefix/app.pid" busfile="$prefix/bus.pid"
# CONTROL: by hand, test-side: `pgrep` renamed to a command that does not exist -> "FAIL (harness):
#   pgrep is needed to find the private bus daemon", exit 1; restored.
command -v pgrep > /dev/null || harness "pgrep is needed to find the private bus daemon"
# The inner shell records its bus daemon's pid (a child of dbus-run-session, started before this
# shell), proves the app is on dbus-run-session's bus and not the desktop's, records the app's own
# pid (dbus-run-session's pid is not the app's), then becomes the app. The bus guard's report was
# observed by inverting its `!=` by hand: "FAIL (harness): the app's bus was not a fresh
# dbus-run-session bus", exit 1.
cd "$work" || harness "cannot enter $work"
outer_bus="${DBUS_SESSION_BUS_ADDRESS:-}" \
"$dbus_run_session" --config-file="$bus_config" -- sh -c '
    pgrep -P "$PPID" -x dbus-daemon > "$4" || exit 98
    [ -n "$DBUS_SESSION_BUS_ADDRESS" ] && [ "$DBUS_SESSION_BUS_ADDRESS" != "$outer_bus" ] || exit 99
    echo $$ > "$1"
    exec env -u outer_bus -u WAYLAND_DISPLAY -u DISPLAY -u QML_IMPORT_PATH -u QML2_IMPORT_PATH \
        XDG_CONFIG_HOME="$2/config" XDG_CACHE_HOME="$2/cache" XDG_DATA_HOME="$2/data" \
        XDG_STATE_HOME="$2/state" QT_QPA_PLATFORM=offscreen QT_FORCE_STDERR_LOGGING=1 \
        QT_LOGGING_RULES="qt.qml.import.debug=true;qt.quick.window.debug=true" "$3"
' sh "$pidfile" "$prefix/xdg" "$app" "$busfile" > "$log" 2>&1 &
drs_pid=$! launched=1

# GOAL: the installed app resolves Main from the module compiled into it, shows Main.qml's
#   window, and is still running 2 s later.
# WHY-THIS-SHAPE: "still alive after N s" alone passes for an app stuck before loading anything,
#   and "Component completed" is logged even when creation then fails on an unset required
#   property, so the pass needs: Main resolved to the embedded qrc:/qt/qml path, the WallpaperX
#   window exposed, and the process alive after that. Every wait ends in a FAIL, never a pass.
# CONTROL: app/CMakeLists.txt `qt_standard_project_setup(REQUIRES 6.8)` -> `qt_standard_project_setup()`
#   (control_run.sh; cmake --build reconfigures): "FAIL: S15.15: the app exited with status 1
#   before showing Main.qml's window", stderr `Module "WallpaperX" contains no type named "Main"`.
#   app/main.cpp without `engine.setInitialProperties(...)`: the same FAIL line.
#   app/main.cpp with a queued `QCoreApplication::exit(0)` before exec(): see the check below.
resolved='resolveType: +"Main" +=> +"" +QUrl\("qrc:/qt/qml/WallpaperX/qml/Main\.qml"\)'
shown='Main_QMLTYPE_[0-9]+\(0x[0-9a-f]+ (active )?exposed, visibility=QWindow::Windowed.*title="WallpaperX"'
seen() { grep -qE "$resolved" "$log" && grep -qE "$shown" "$log"; }
loaded=0
for _ in $(seq 1 300); do
    seen && { loaded=1; break; }
    kill -0 "$drs_pid" 2>/dev/null || { seen && loaded=1; break; }
    sleep 0.1
done
if [ "$loaded" = 0 ]; then
    counts="resolved to the embedded module: $(grep -cE "$resolved" "$log"), window shown: $(grep -cE "$shown" "$log")"
    kill -0 "$drs_pid" 2>/dev/null && fail "Main.qml's window was not shown within 30 s ($counts)"
    wait "$drs_pid"; status=$?; drs_pid=""
    [ "$status" = 98 ] && harness "the private bus daemon's pid could not be recorded"
    [ "$status" = 99 ] && harness "the app's bus was not a fresh dbus-run-session bus"
    if grep -q 'Could not find the Qt platform plugin "offscreen"' "$log"; then
        echo "$name: SKIP -- the offscreen Qt platform plugin is not installed" >&2; exit 77
    fi
    fail "the app exited with status $status before showing Main.qml's window ($counts)"
fi
[ -s "$pidfile" ] || harness "the app's pid was not recorded"
app_pid="$(cat "$pidfile")"

# CONTROL: app/main.cpp with `QMetaObject::invokeMethod(&app, [] { QCoreApplication::exit(0); },
#   Qt::QueuedConnection)` before exec() (control_run.sh): "FAIL: S15.15: the app exited with
#   status 0 within 2 s of showing Main.qml's window". Restored: pass.
sleep 2
if ! kill -0 "$app_pid" 2>/dev/null; then
    wait "$drs_pid"; status=$?; drs_pid=""
    fail "the app exited with status $status within 2 s of showing Main.qml's window"
fi
echo "$name: all checks passed (Main resolved to qrc:/qt/qml/WallpaperX/qml/Main.qml, its window shown, running 2 s later)"
exit 0
