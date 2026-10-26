#!/bin/sh
# S15.966: the app configured without the library path still configures, builds, and holds only
# the D-Bus path -- twice: with KF6Screen disabled, and with KF6Screen found but wayland-client
# hidden from pkg-config.
# Usage: no_kscreen_library_build.sh <cmake> <app source dir> <scratch build dir> [cmake args...]
# The entry passes the outer tree's generator and an initial-cache script (prefix path, compiler,
# build type, KF6Screen_DIR).
#
# GOAL: S15.966 -- without KF6Screen, or with it but without wayland-client, the app configures with
#       the matching status line, builds, its binary neither links libKF6Screen nor contains the
#       library source while the D-Bus source is in it, and app_kscreen_library is not registered.
# WHY-THIS-SHAPE: read from the built binary (NEEDED entries and symbols), not from the configure
#       message alone, since a message can say "not found" while a target still links the library.
#       The D-Bus source's symbol must be present, so a stripped or empty binary cannot pass.
#       wayland-client is hidden with a PKG_CONFIG_LIBDIR holding every system .pc but its own, so
#       every other pkg-config package the app needs is still found.
# CONTROL: app/CMakeLists.txt, find_package(KF6Screen CONFIG QUIET) made REQUIRED -> red, "FAIL:
#       S15.966 (no KF6Screen): configuring failed on KF6Screen" (control_batch.sh 2026-10-26).
#       app/CMakeLists.txt, WALLPAPERX_KSCREEN_LIBRARY set ON whenever KF6Screen_FOUND -> red, "FAIL:
#       S15.966 (no wayland-client): configuring failed on KF6Screen" (control_run.sh 2026-10-26). Test-side, by hand: run without -DCMAKE_DISABLE_FIND_PACKAGE_
#       KF6Screen=ON -> red on the configure message, the NEEDED entry and the KScreenLibrarySource symbol.
set -u
cmake=$1 src=$2 base=$3
shift 3
fail=0

# A directory of links to every system .pc file except wayland-client's.
pcdir="$base.pkgconfig"
rm -rf "$pcdir"; mkdir -p "$pcdir"
for d in /usr/lib/pkgconfig /usr/share/pkgconfig; do
    [ -d "$d" ] || continue
    for pc in "$d"/*.pc; do
        case ${pc##*/} in wayland-client.pc) ;; *) [ -e "$pcdir/${pc##*/}" ] || ln -s "$pc" "$pcdir/" ;; esac
    done
done

# scenario <label> <status line expected> <env assignment or -> <extra cmake arg or -> <dir suffix> [cmake args...]
scenario() {
    label=$1 line=$2 envset=$3 extra=$4
    build="$base.$5"
    shift 5
    rm -rf "$build"
    if [ "$envset" = - ]; then envset=WALLPAPERX_UNUSED=1; fi
    if [ "$extra" = - ]; then extra=-DWALLPAPERX_UNUSED=1; fi
    if ! env "$envset" "$cmake" -S "$src" -B "$build" "$extra" -DWALLPAPERX_REQUIRE_ALL_TESTS=OFF "$@" \
            > "$build.configure.log" 2>&1; then
        # Only an error that names KF6Screen or wayland-client is the app's; anything else is this
        # run's setup. Graded on CMake's error blocks only: the status and not-registered lines
        # every such configure prints name KF6Screen too.
        # CONTROL: test-side, by hand 2026-10-26: -DCMAKE_PROJECT_INCLUDE of a file deferring a
        #       SEND_ERROR to the end of the configure (after the KF6Screen status line) -> "HARNESS:
        #       S15.966 (no KF6Screen): the configure failed on something else"; the old whole-log grep
        #       gave "FAIL: ... configuring failed on KF6Screen" for the same run.
        # A block runs from "CMake Error" to the next unindented line: its text has blank lines.
        if awk '/^CMake Error/ {b=1; print; next} b && /^[^ ]/ {b=0} b' "$build.configure.log" | grep -Eq "KF6Screen|wayland-client|WAYLAND_CLIENT"; then
            echo "FAIL: S15.966 ($label): configuring failed on KF6Screen:"; tail -20 "$build.configure.log"; fail=1; return
        fi
        echo "HARNESS: S15.966 ($label): the configure failed on something else, so this run says nothing about it:"
        tail -20 "$build.configure.log"; exit 1
    fi
    if [ "$envset" != WALLPAPERX_UNUSED=1 ] && grep -qF "KF6Screen: not found" "$build.configure.log"; then
        echo "HARNESS: S15.966 ($label): this tree's configuration does not find KF6Screen at all (KF6Screen_DIR unset?), so the wayland-client case cannot be made"
        exit 1
    fi
    if ! grep -qF "$line" "$build.configure.log"; then
        echo "FAIL: S15.966 ($label): configure did not print \"$line\":"; grep KF6Screen "$build.configure.log"; fail=1
    fi
    if ctest --test-dir "$build" -N 2>/dev/null | grep -Eq ' app_kscreen_library$'; then
        echo "FAIL: S15.966 ($label): app_kscreen_library is registered without the library path"; fail=1
    fi
    if ! "$cmake" --build "$build" --target wallpaperx -j "$(nproc)" > "$build.build.log" 2>&1; then
        echo "FAIL: S15.966 ($label): building wallpaperx failed:"; tail -20 "$build.build.log"; fail=1; return
    fi
    app=$(find "$build" -maxdepth 2 -type f -name wallpaperx -perm -u+x | head -1)
    if [ -z "$app" ]; then
        echo "HARNESS: S15.966 ($label): no wallpaperx binary in $build"; exit 1
    fi
    symbols=$(nm -C "$app")
    if ! printf '%s\n' "$symbols" | grep -q "KScreenDBusSource"; then
        echo "HARNESS: S15.966 ($label): $app has no KScreenDBusSource symbol, so the symbol check below proves nothing"; exit 1
    fi
    if readelf -d "$app" | grep NEEDED | grep -q libKF6Screen; then
        echo "FAIL: S15.966 ($label): $app links libKF6Screen:"; readelf -d "$app" | grep libKF6Screen; fail=1
    fi
    if printf '%s\n' "$symbols" | grep -q "KScreenLibrarySource"; then
        echo "FAIL: S15.966 ($label): $app contains the library source (KScreenLibrarySource)"; fail=1
    fi
}

scenario "no KF6Screen" "KF6Screen: not found" - -DCMAKE_DISABLE_FIND_PACKAGE_KF6Screen=ON nokf6 "$@"
scenario "no wayland-client" "found, but wayland-client not in the system's pkg-config path" PKG_CONFIG_LIBDIR="$pcdir" - nowl "$@"
[ "$fail" = 0 ] && echo "S15.966: wallpaperx without KF6Screen, and with it but without wayland-client, builds, holds the D-Bus source only, links no libKF6Screen, and registers no app_kscreen_library"
exit "$fail"
