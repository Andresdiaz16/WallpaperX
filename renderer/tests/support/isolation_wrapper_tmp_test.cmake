# test_isolation_wrapper_tmp: isolated_run.sh with a private bus, in a mount namespace whose /tmp is
# an empty tmpfs of its own -- what such runs leave in /tmp, and the paths they accept.
#
#   cmake -DWRAPPER=<isolated_run.sh> -DCONF=<isolated_session.conf> -DBUS=<dbus-run-session> -P this
#
# Registered only where dbus-run-session was found (isolated_test.cmake), so a build without one
# has no such entry rather than one that passes without running.
#
# The wrapper, private_bus.sh beside it and CONF are copied into the namespace's /tmp before anything
# runs, under /tmp/.wpx-stage, which the leftover listing leaves out by that exact name: a checkout or
# build tree under /tmp would otherwise be hidden by the tmpfs, and the case would fail for that.
# The copies are made from the caller's files, so a helper the wrapper reads beside itself must be
# copied here too.

foreach(required WRAPPER CONF BUS)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "" OR NOT EXISTS "${${required}}")
        message(FATAL_ERROR "FAIL (harness): test_isolation_wrapper_tmp was run without an existing -D${required}")
    endif()
endforeach()
get_filename_component(support_dir "${WRAPPER}" DIRECTORY)
if(NOT EXISTS "${support_dir}/private_bus.sh")
    message(FATAL_ERROR "FAIL (harness): no private_bus.sh beside ${WRAPPER} to copy")
endif()

# GOAL: X.25 -- an entry run on a private bus leaves nothing in the shared /tmp, whether it ends
#       normally, its bus daemon is killed outright (a bus killed outright cannot remove its own
#       socket), or its runtime directory was moved to /tmp because TMPDIR was too long. A wrapper
#       killed outright, as a ctest timeout does, leaves its scratch directory for the next run's
#       sweep (test_isolation_wrapper's SIGKILL case); with such a long TMPDIR, its short runtime
#       directory in /tmp too, which the next such run sweeps along with its TMPDIR (the fifth run).
# GOAL: X.30 -- a run on a private bus works from a checkout path, and with a TMPDIR, holding ' ',
#       '+', '~', '@', '%', '&' and '<', with the bus listening inside the entry's runtime
#       directory; and with a TMPDIR too long for a socket inside it, its runtime directory then
#       under /tmp/wallpaperx-test.*.
# WHY-THIS-SHAPE: the tmpfs makes /tmp this case's alone, so no parallel entry can add or remove a
#       file there; TMPDIR is unset for the first two runs, so their scratch directories go to that
#       /tmp and must be gone as well. The bus gets 5 s after the last run to remove its socket.
#       Each nested command prints a marker, so a wrapper that refused to start cannot pass by
#       leaving nothing. The odd-path run uses the copies under the odd directory, so the
#       configuration path written into the bus's XML holds those characters, and its TMPDIR
#       holds them too, so the runtime directory in the bus address does.
# CONTROL: control_batch, 2026-10-04, each in renderer/tests/support/, reverted. X.25: private_bus.sh
#       listening on unix:tmpdir=/tmp inside this namespace -> "FAIL: a run on a private bus leaves
#       nothing in the caller's /tmp, ... (left: dbus-XvwD9ofN7E)"; isolated_run.sh not sweeping /tmp,
#       or not locking its short runtime directory -> the same line, "(left: wallpaperx-test.jGl9Xk62)".
#       X.30: private_bus.sh refusing characters outside [A-Za-z0-9_/.-] again, and isolated_run.sh
#       not moving a long runtime directory -> "FAIL: a run on a private bus works from a checkout
#       path ..." and "... works with a TMPDIR too long ..."; address_escape(), or xml_escape(),
#       returning its input -> the checkout-path line, dbus-daemon "not well-formed (invalid token)".
# COVERAGE-ASSERT: all five runs must print their markers, and the case count is checked; the
#       killed wrapper must be seen to leave exactly one directory in /tmp, or the sweep run proves
#       nothing and the case is a harness fault.
execute_process(COMMAND id -u OUTPUT_VARIABLE uid OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND id -g OUTPUT_VARIABLE gid OUTPUT_STRIP_TRAILING_WHITESPACE)
set(odd_name "my code+~@%&<x>")
set(odd_tmpdir "t +~@%&<")
string(REPEAT "x" 60 long_name)

# As root in a user and mount namespace of its own: read the files, put the tmpfs on /tmp, copy
# them in, then become the caller's uid again for the runs.
set(outer [=[
u=$1 g=$2 inner=$3 wrapper=$4 conf=$5 bus=$6 odd=$7 oddtmp=$8 long=$9
case "$bus" in /tmp/*) echo "NO-STAGE: $bus is under /tmp, which the tmpfs hides"; exit 0 ;; esac
w=$(cat "$wrapper") && p=$(cat "$(dirname "$wrapper")/private_bus.sh") && c=$(cat "$conf") ||
    { echo "NO-STAGE: the files under test could not be read"; exit 0; }
mount -t tmpfs wallpaperx-tmp /tmp || { echo "NO-TMPFS"; exit 0; }
stage=/tmp/.wpx-stage
for d in "$stage/plain" "$stage/$odd"; do
    mkdir -p "$d" && printf '%s\n' "$w" > "$d/isolated_run.sh" && printf '%s\n' "$p" > "$d/private_bus.sh" &&
        printf '%s\n' "$c" > "$d/isolated_session.conf" || { echo "NO-STAGE: the copies could not be written to $d"; exit 0; }
done
mkdir -p "$stage/$oddtmp" "$stage/$long" || { echo "NO-STAGE: the TMPDIRs could not be made"; exit 0; }
exec unshare --user --map-user="$u" --map-group="$g" -- /bin/sh -c "$inner" sh "$stage" "$odd" "$oddtmp" "$long" "$bus"
]=])
set(inner [=[
stage=$1 odd=$2 oddtmp=$3 long=$4 bus=$5
plain="$stage/plain"
# run DIR TMPDIR COMMAND: the copies in DIR, TMPDIR unset when empty.
run() {
    if [ -n "$2" ]; then
        TMPDIR=$2 /bin/sh "$1/isolated_run.sh" --config "$1/isolated_session.conf" --bus "$bus" -- /bin/sh -c "$3"
    else
        /bin/sh "$1/isolated_run.sh" --config "$1/isolated_session.conf" --bus "$bus" -- /bin/sh -c "$3"
    fi
}
unset TMPDIR TMP TEMP
run "$plain" "" 'echo RAN-NORMAL'
run "$plain" "" 'kill -KILL $(pgrep -P $PPID -x dbus-daemon) && echo RAN-KILLED'
run "$stage/$odd" "$stage/$oddtmp" 'echo "ODD-ADDRESS:[$DBUS_SESSION_BUS_ADDRESS]" && echo RAN-ODD'
run "$plain" "$stage/$long" 'echo "LONG-RUNTIME:[$XDG_RUNTIME_DIR]" && echo RAN-LONG'
# A wrapper with the long TMPDIR killed outright while its test runs, then the test ended: its short
# runtime directory stays in /tmp until the next such run's sweep. Waited on until no process (the
# bus inherits it) holds its lock, so a kept directory is the sweep's doing, not a late bus's.
TMPDIR="$stage/$long" /bin/sh "$plain/isolated_run.sh" --config "$plain/isolated_session.conf" --bus "$bus" -- \
    /bin/sh -c "echo \$\$ > '$stage/child.pid'; exec sleep 30" > /dev/null 2>&1 &
w=$! i=0
while [ ! -s "$stage/child.pid" ] && [ $i -lt 200 ]; do sleep 0.05; i=$((i + 1)); done
kill -KILL $w; wait $w 2> /dev/null
[ -s "$stage/child.pid" ] && kill -KILL "$(cat "$stage/child.pid")"
i=0
while [ $i -lt 100 ]; do
    free=1
    for d in /tmp/wallpaperx-test.*; do [ -e "$d/owner" ] && ! flock -n "$d/owner" true && free=0; done
    [ $free = 1 ] && break
    sleep 0.05; i=$((i + 1))
done
echo "KILLED-LEFT:[$(ls -A /tmp | grep -vx '\.wpx-stage' | tr '\n' ' ')]"
run "$plain" "$stage/$long" 'echo RAN-SWEEP'
left() { ls -A /tmp | grep -vx '\.wpx-stage'; ls -A "$stage/$oddtmp"; ls -A "$stage/$long"; }
i=0
while [ -n "$(left)" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
echo "LEFT:[$(left | tr '\n' ' ')]"
]=])
execute_process(COMMAND env -u TMPDIR unshare --user --map-root-user --mount -- /bin/sh -c "${outer}" sh
                        ${uid} ${gid} "${inner}" "${WRAPPER}" "${CONF}" "${BUS}" "${odd_name}" "${odd_tmpdir}" "${long_name}"
                OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc TIMEOUT 100)

set(failures 0)
set(cases 0)
set(harness FALSE)
macro(fail)
    string(JOIN "" _fail_text ${ARGV})
    message("FAIL: ${_fail_text}")
    math(EXPR failures "${failures} + 1")
endmacro()
# Read on their own: CMake leaves CMAKE_MATCH_1 unset, not empty, when the group matched nothing.
foreach(field LEFT ODD-ADDRESS LONG-RUNTIME KILLED-LEFT)
    set(value_${field} "")
    set(seen_${field} FALSE)
    if(o MATCHES "(^|\n)${field}:\\[([^\n]*)\\]")
        set(seen_${field} TRUE)
        set(value_${field} "${CMAKE_MATCH_2}")
    endif()
endforeach()
string(STRIP "${value_LEFT}" left)

if(o MATCHES "NO-TMPFS")
    message("FAIL (harness): no mount namespace with a tmpfs on /tmp could be made, so what a run leaves in /tmp is not known: ${e}")
    math(EXPR failures "${failures} + 1")
    set(harness TRUE)
elseif(o MATCHES "NO-STAGE")
    message("FAIL (harness): the files under test could not be copied into the namespace's /tmp: ${o} ${e}")
    math(EXPR failures "${failures} + 1")
    set(harness TRUE)
else()
    foreach(marker NORMAL KILLED ODD LONG SWEEP)
        if(o MATCHES "RAN-${marker}")
            math(EXPR cases "${cases} + 1")
        endif()
    endforeach()
    string(STRIP "${value_KILLED-LEFT}" killed_left)
    if(NOT killed_left MATCHES "^wallpaperx-test\\.[^ ]+$")
        message("FAIL (harness): a wrapper with the long TMPDIR, killed outright, left '${killed_left}' in /tmp, not the one "
                "short runtime directory the sweep case needs, so the sweep after it proves nothing")
        math(EXPR failures "${failures} + 1")
    endif()
    if(NOT o MATCHES "RAN-NORMAL" OR NOT o MATCHES "RAN-KILLED")
        fail("the wrapper never ran both plain commands on a private bus (exit '${rc}', said: ${o} ${e})")
    endif()
    # The bus's own address, as dbus-daemon escapes it: inside the odd TMPDIR's scratch runtime directory.
    if(NOT o MATCHES "RAN-ODD" OR NOT value_ODD-ADDRESS MATCHES "^unix:path=/tmp/\\.wpx-stage/t%20%2b%7e%40%25%26%3c/wallpaperx-test\\.[^/]+/runtime/dbus-")
        fail("a run on a private bus works from a checkout path, and with a TMPDIR, holding ' ', '+', '~', '@', '%', '&' and '<', "
             "listening in its runtime directory (address: '${value_ODD-ADDRESS}'; said: ${o} ${e})")
    endif()
    if(NOT o MATCHES "RAN-LONG" OR NOT value_LONG-RUNTIME MATCHES "^/tmp/wallpaperx-test\\.[^/]+/runtime$")
        fail("a run on a private bus works with a TMPDIR too long for a socket inside it, its runtime directory moved to "
             "/tmp/wallpaperx-test.* (runtime: '${value_LONG-RUNTIME}'; said: ${o} ${e})")
    endif()
    if(NOT seen_LEFT)
        fail("the runs never got as far as listing /tmp (exit '${rc}', said: ${o} ${e})")
    elseif(NOT left STREQUAL "")
        fail("a run on a private bus leaves nothing in the caller's /tmp, whether it ends normally, its bus is killed outright, "
             "or its runtime directory was moved to /tmp (left: ${left})")
    endif()
endif()

message("test_isolation_wrapper_tmp: ${cases} of 5 runs ran their command")
if(NOT harness AND NOT cases EQUAL 5)
    fail("5 runs were expected to run their command, ${cases} did")
endif()
if(failures GREATER 0)
    message(FATAL_ERROR "test_isolation_wrapper_tmp: ${failures} failure(s)")
endif()
