#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs one ctest entry cut off from the desktop session that ran ctest. Every entry in this project
# is registered through isolated_test.cmake, which puts this in front of its command; the
# test_isolation entries check the result from inside, test_isolation_wrapper from outside.
#
#   isolated_run.sh --config SESSION_CONF [--bus DBUS_RUN_SESSION] [--display KWIN_WAYLAND] -- COMMAND...
#
# All of it is set before anything starts, because a process started first inherits the session:
#   - a network of its own, holding a loopback that is up and a dummy interface: the daemon's port is a fixed
#     127.0.0.1:18462, which the live desktop's wallpaper plugin polls, so a test standing in for
#     the daemon must answer on a 127.0.0.1 the desktop cannot see;
#   - HOME, every XDG base directory and TMPDIR point into a fresh scratch directory, removed
#     afterwards, and never placed inside the caller's home or runtime directory;
#   - no live display: WAYLAND_DISPLAY and DISPLAY are unset, Qt is put on the offscreen platform;
#   - the bus is a private one whose configuration has no service directories (--config). A bus that
#     can activate services starts real desktop daemons as the user who ran the tests.
# --display is for entries that need a real platform (OpenGL): a headless nested KWin is started on
# the scratch runtime directory and WAYLAND_DISPLAY names it. Without one they fall back to
# offscreen, where they report their own skip.
set -u

# The network, first: everything below, the compositor and the bus included, starts inside it.
# Two user namespaces: the outer maps this user to root, which may bring loopback up; the inner maps
# it back, so the test runs as the caller's uid with no capabilities. Every step is an exec, so ctest
# still holds this script's pid. A host that cannot do this fails the entry: running it on the shared
# network is exactly what this exists to prevent.
if [ "${1-}" = --private-network ]; then
    caller_net=$2
    shift 2
else
    caller_net=$(readlink /proc/self/ns/net 2>/dev/null) || caller_net=""
    uid=$(id -u) gid=$(id -g)
    # Also one address that is not loopback, 192.0.2.1 (a documentation range) on a dummy interface,
    # so a test that a server binds loopback only has another address to try. Best effort: the
    # first use loads the dummy driver, and where that is refused the test reports not verified.
    enter='u=$1 g=$2; shift 2
ip link set lo up || exit 2
{ ip link add wpx0 type dummy && ip addr add 192.0.2.1/24 dev wpx0 && ip link set wpx0 up; } 2>/dev/null
exec unshare --user --map-user="$u" --map-group="$g" -- "$@"'
    # Tried once with a command that does nothing, so a refusal is reported here in plain words.
    if [ -z "$caller_net" ] || ! command -v unshare >/dev/null 2>&1 || ! command -v ip >/dev/null 2>&1 ||
       ! why=$(unshare --user --map-root-user --net -- /bin/sh -c "$enter" sh "$uid" "$gid" true 2>&1); then
        echo "isolated_run.sh: FAIL (harness): this entry cannot be given a network of its own, so it is not run" \
             "on the shared one, where the live desktop polls 127.0.0.1:18462. It needs unprivileged user" \
             "namespaces, util-linux unshare 2.38 or later, and ip. ${why-}" >&2
        exit 2
    fi
    export WALLPAPERX_CALLER_NET_NS="$caller_net" WALLPAPERX_CALLER_UID="$uid"
    exec unshare --user --map-root-user --net -- /bin/sh -c "$enter" sh "$uid" "$gid" \
        /bin/sh "$0" --private-network "$caller_net" "$@"
fi
if [ "$(readlink /proc/self/ns/net 2>/dev/null)" = "$caller_net" ]; then
    echo "isolated_run.sh: FAIL (harness): the entry is still on the caller's network ($caller_net)" >&2
    exit 2
fi

conf="" bus="" kwin="" display=0
while [ $# -gt 0 ]; do
    case "$1" in
        --config) conf=$2; shift 2 ;;
        --bus) bus=$2; shift 2 ;;
        --display) display=1; kwin=$2; shift 2 ;;
        --) shift; break ;;
        *) echo "isolated_run.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
if [ $# -eq 0 ] || [ -z "$conf" ]; then
    echo "isolated_run.sh: usage: --config SESSION_CONF [--bus DBUS_RUN_SESSION] [--display KWIN] -- COMMAND..." >&2
    exit 2
fi

# What the caller had, so test_isolation can tell whether any of it reached the entry.
export WALLPAPERX_CALLER_HOME="${HOME-}" WALLPAPERX_CALLER_XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR-}" \
       WALLPAPERX_CALLER_WAYLAND_DISPLAY="${WAYLAND_DISPLAY-}" WALLPAPERX_CALLER_DISPLAY="${DISPLAY-}" \
       WALLPAPERX_CALLER_DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS-}" \
       WALLPAPERX_CALLER_TMPDIR="${TMPDIR-}"

# The places test_isolation forbids: the caller's home and runtime directory, and the account's own.
# Read before HOME and XDG_RUNTIME_DIR are replaced below.
uid=$(id -u)
account_home=$(getent passwd "$uid" 2>/dev/null | cut -d: -f6)
in_session() {
    case "$1/" in
        "${HOME:-/nonexistent}"/* | "${XDG_RUNTIME_DIR:-/nonexistent}"/* | "${account_home:-/nonexistent}"/* | \
            /run/user/"$uid"/*) return 0 ;;
    esac
    return 1
}

# The Vulkan loader searches these same directories for layers and drivers. Those the entry loses
# are named to it directly, read-only, so a validation layer installed in the caller's home still
# grades the tests that need one.
vk_layers="" vk_implicit="" vk_drivers=""
forward_vulkan() {
    [ -d "$1/vulkan/explicit_layer.d" ] && vk_layers="${vk_layers:+$vk_layers:}$1/vulkan/explicit_layer.d"
    [ -d "$1/vulkan/implicit_layer.d" ] && vk_implicit="${vk_implicit:+$vk_implicit:}$1/vulkan/implicit_layer.d"
    [ -d "$1/vulkan/icd.d" ] && vk_drivers="${vk_drivers:+$vk_drivers:}$1/vulkan/icd.d"
    return 0
}

# The caller's search paths less every entry inside its session: a prefix elsewhere (a Vulkan SDK, a
# Nix profile) holds layer and driver manifests the tests need.
outside_session() {
    kept=""
    set -f
    saved_ifs=$IFS
    IFS=:
    for dir in $1; do
        case "$dir" in /*) ;; *) continue ;; esac
        if in_session "$dir"; then forward_vulkan "$dir"; else kept="${kept:+$kept:}$dir"; fi
    done
    IFS=$saved_ifs
    set +f
    kept_dirs="${kept:-$2}"
}
# The loader's own order: config home, config dirs, data home, data dirs.
forward_vulkan "${XDG_CONFIG_HOME:-${HOME:-/nonexistent}/.config}"
outside_session "${XDG_CONFIG_DIRS-}" /etc/xdg
config_dirs=$kept_dirs
forward_vulkan "${XDG_DATA_HOME:-${HOME:-/nonexistent}/.local/share}"
outside_session "${XDG_DATA_DIRS-}" /usr/local/share:/usr/share
data_dirs=$kept_dirs
[ -n "$vk_layers" ] && export VK_ADD_LAYER_PATH="$vk_layers${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
[ -n "$vk_implicit" ] && export VK_ADD_IMPLICIT_LAYER_PATH="$vk_implicit${VK_ADD_IMPLICIT_LAYER_PATH:+:$VK_ADD_IMPLICIT_LAYER_PATH}"
[ -n "$vk_drivers" ] && export VK_ADD_DRIVER_FILES="$vk_drivers${VK_ADD_DRIVER_FILES:+:$VK_ADD_DRIVER_FILES}"

base=${TMPDIR:-/tmp}
if in_session "$base"; then base=/tmp; fi

# ctest ends a timed-out entry with SIGKILL, which no trap sees, so the next run removes what such a
# run left. A directory is in use while anyone holds the lock on its owner file: the wrapper takes it
# and everything it starts inherits it, so a test that outlives a killed wrapper keeps its directory.
# One with no owner file yet is kept until it is old.
for old in "$base"/wallpaperx-test.*; do
    [ -d "$old" ] && [ -O "$old" ] || continue
    if [ -e "$old/owner" ]; then
        flock -n "$old/owner" true 2>/dev/null || continue
    else
        [ -n "$(find "$old" -maxdepth 0 -mmin +5 2>/dev/null)" ] || continue
    fi
    rm -rf "$old"
done

scratch=$(mktemp -d "$base/wallpaperx-test.XXXXXXXX") || {
    echo "isolated_run.sh: FAIL (harness): no scratch directory under $base" >&2; exit 2; }
# Locked before it takes the name the sweep looks for, so no sweep sees an owner file nobody holds.
exec 9>"$scratch/owner.new"
if ! flock -n 9 || ! mv "$scratch/owner.new" "$scratch/owner"; then
    echo "isolated_run.sh: FAIL (harness): the owner lock under $scratch could not be taken" >&2
    rm -rf "$scratch"
    exit 2
fi

kwin_pid=""
cleanup() {
    if [ -n "$kwin_pid" ]; then
        kill "$kwin_pid" 2>/dev/null
        wait "$kwin_pid" 2>/dev/null
    fi
    rm -rf "$scratch"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

export HOME="$scratch/home"
export XDG_CONFIG_HOME="$HOME/.config" XDG_DATA_HOME="$HOME/.local/share" XDG_STATE_HOME="$HOME/.local/state" \
       XDG_CACHE_HOME="$HOME/.cache" XDG_RUNTIME_DIR="$scratch/runtime"
export XDG_CONFIG_DIRS="$config_dirs" XDG_DATA_DIRS="$data_dirs"
# TMP and TEMP as well: some libraries read those instead.
export TMPDIR="$scratch/tmp" TMP="$scratch/tmp" TEMP="$scratch/tmp"
if ! mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR" "$TMPDIR" ||
   ! chmod 700 "$XDG_RUNTIME_DIR"; then
    echo "isolated_run.sh: FAIL (harness): the private directories could not be made under $scratch" >&2
    exit 2
fi

# The live display and session, and what leads a toolkit back to them: the desktop's identity is
# what makes Qt load the KDE platform theme, which talks to session services.
unset WAYLAND_DISPLAY WAYLAND_SOCKET DISPLAY XAUTHORITY SESSION_MANAGER DBUS_SESSION_BUS_ADDRESS \
      DBUS_STARTER_ADDRESS DBUS_STARTER_BUS_TYPE XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP DESKTOP_SESSION \
      XDG_SESSION_TYPE XDG_SESSION_ID XDG_SESSION_PATH XDG_SEAT XDG_SEAT_PATH XDG_VTNR KDE_FULL_SESSION \
      KDE_SESSION_VERSION KDE_SESSION_UID QT_QPA_PLATFORMTHEME SSH_AUTH_SOCK

if [ "$display" = 0 ]; then
    # Kept when the entry chose offscreen itself, with options such as a screen configuration. A
    # caller's offscreen value arrives by the same route and is kept too; it reaches no display.
    case "${QT_QPA_PLATFORM-}" in offscreen*) ;; *) export QT_QPA_PLATFORM=offscreen ;; esac
else
    # Only a wayland choice is kept: an inherited "offscreen" cannot be told from the entry's own.
    case "${QT_QPA_PLATFORM-}" in *wayland*) ;; *) export QT_QPA_PLATFORM='wayland;offscreen' ;; esac
    socket=wallpaperx-test
    if [ -x "$kwin" ]; then
        # No bus for the compositor: it needs none to serve clients, and an unreachable address
        # leaves it nothing to fall back to. Killed with this script however it ends.
        pdeath=""
        command -v setpriv >/dev/null 2>&1 && pdeath="setpriv --pdeathsig KILL"
        # WALLPAPERX_TEST_KWIN_OUTPUTS: an entry that needs more than one screen sets it.
        DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/no-bus" $pdeath "$kwin" --virtual --no-lockscreen \
            --no-global-shortcuts --no-kactivities --socket "$socket" --width 1024 --height 768 \
            --output-count "${WALLPAPERX_TEST_KWIN_OUTPUTS:-1}" >"$scratch/kwin.log" 2>&1 &
        kwin_pid=$!
        tries=0
        while [ ! -S "$XDG_RUNTIME_DIR/$socket" ] && [ "$tries" -lt 200 ] && kill -0 "$kwin_pid" 2>/dev/null; do
            sleep 0.05
            tries=$((tries + 1))
        done
        if [ -S "$XDG_RUNTIME_DIR/$socket" ]; then
            export WAYLAND_DISPLAY="$socket"
        else
            # kwin_wayland was found when this build was configured, so a compositor that does not
            # start is a fault of this host to fix, not a reason for the GL entries to skip quietly.
            echo "isolated_run.sh: FAIL (harness): NO NESTED COMPOSITOR -- $kwin exited, or did not open its" \
                 "socket within 10 s. Its log:" >&2
            tail -20 "$scratch/kwin.log" >&2
            exit 2
        fi
    else
        echo "isolated_run.sh: NO NESTED COMPOSITOR -- kwin_wayland was not found when this build was configured;" \
             "this entry runs with no display and falls back to offscreen" >&2
    fi
fi

status=0
if [ -n "$bus" ]; then
    "$bus" --config-file="$conf" -- "$@" || status=$?
else
    # No dbus-run-session on this host: an address nothing listens on, so a client cannot fall back
    # to a bus it finds by itself.
    DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/no-bus" "$@" || status=$?
fi
# A test killed by a signal comes back as 128+N, and dying of the same signal lets ctest name it --
# but only for signals whose default action ends the process and nothing more. A core-dumping one
# would dump this shell's core as a second crash, and a stopping one would stop this shell until
# ctest's timeout; those leave as the plain status. A test that exits 129-159 itself is read the same way.
if [ "$status" -gt 128 ] && [ "$status" -lt 160 ]; then
    case "$(kill -l "$((status - 128))" 2>/dev/null)" in
        HUP | INT | KILL | PIPE | ALRM | TERM | USR1 | USR2)
            trap - EXIT HUP INT TERM
            cleanup
            kill -s "$(kill -l "$((status - 128))")" $$ ;;
    esac
fi
exit "$status"
