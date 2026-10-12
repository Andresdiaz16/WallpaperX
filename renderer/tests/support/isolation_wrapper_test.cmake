# test_isolation_wrapper: what isolated_run.sh does with a caller's session, driven from outside with
# a made-up caller -- its HOME, runtime directory, TMPDIR and search paths are directories this run
# creates -- so the cases a real ctest caller rarely has (TMPDIR inside its home or runtime
# directory, a home-rooted search path, a Vulkan layer in its home, an inherited "offscreen", a test
# killed by a signal, a wrapper killed outright) are exercised on every run. test_isolation grades
# the same wrapper from inside, with whatever caller ran ctest.
#
#   cmake -DWRAPPER=<isolated_run.sh> -DCONF=<isolated_session.conf> -DPROBE=<wallpaperx-wrapper-probe> -P this
#
# Runs inside the wrapper like every entry, so its own TMPDIR is a private scratch directory and its
# own network is private. Every nested wrapper runs with no --bus, so no bus is started; runs on a
# private bus are isolation_wrapper_tmp_test.cmake's.
#
# CONTROL: observed 2026-10-01, each in a scratch copy of the support directory (isolated_run.sh with
#       private_bus.sh beside it, which it needs since 2026-10-03) whose isolated_run.sh was passed as
#       -DWRAPPER, the entry itself under the real one; the re-raise control re-run that way
#       2026-10-04. Both unshare steps and the own-namespace check removed -> "FAIL: a
#       port bound inside one entry is not reachable ... (127.0.0.1:18462 accepted a connection from
#       outside)". `ip link set lo up` replaced by `true` -> "FAIL (harness): the listener inside the
#       nested entry was not reachable from that entry itself". The signal allowlist widened to every
#       signal -> "a test killed by SIGSEGV comes back as exit status 139 ... (got 'Segmentation
#       fault')" and "a test exiting 148 ... (got 'Process terminated due to timeout')"; the re-raise
#       deleted -> "...killed by SIGUSR1 (got '138')". The sweep's `flock -n ... || continue` made
#       `true` -> the held-lock line and "...still running after its wrapper was killed is kept";
#       the test run with `9>&-` -> that second line alone; the sweep's `rm -rf` made `:` -> the
#       unheld-lock, ten-minutes-old and "removed once its test has ended too" lines. The three
#       VK_ADD_* exports disabled -> "FAIL: the Vulkan manifest directories the entry loses are
#       passed to the loader". The compositor-failure `exit 2` deleted -> "FAIL: a display entry
#       whose compositor exits before opening its socket is not run". outside_session keeping every
#       entry -> "XDG_DATA_DIRS and XDG_CONFIG_DIRS keep the caller's entries outside its home ...".
#       TMPDIR left out of its export -> "the entry's tmpdir=... is unset or inside the caller's
#       home" and the TMP/TEMP line. The runtime-directory pattern deleted from in_session -> "the
#       entry's home=... is inside the caller's runtime directory" (and tmpdir, runtime).
#       `| offscreen*` added to the display arm -> "QT_QPA_PLATFORM: a display entry ... (got 'offscreen')".
# COVERAGE-ASSERT: every case's nested wrapper must have run its command (each prints a marker), so
#       a wrapper that refused to start cannot pass by printing nothing; the run ends by checking
#       how many cases ran.

foreach(required WRAPPER CONF PROBE)
    if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
        message(FATAL_ERROR "FAIL (harness): test_isolation_wrapper was run without an existing -D${required}")
    endif()
endforeach()
if("$ENV{TMPDIR}" STREQUAL "")
    message(FATAL_ERROR "FAIL (harness): no TMPDIR to work under -- this entry runs inside the wrapper, which sets one")
endif()

set(root "$ENV{TMPDIR}/wrapper-test")
file(REMOVE_RECURSE "${root}")
set(caller_home "${root}/caller-home")
set(caller_runtime "${root}/caller-runtime")
file(MAKE_DIRECTORY "${caller_home}/tmp" "${caller_runtime}/tmp" "${root}/sweep" "${root}/killed" "${root}/net")

# The made-up caller every case starts from. The Vulkan variables are dropped so this entry's own
# search paths cannot leak into what the cases compare.
set(caller_env env -u QT_QPA_PLATFORM -u XDG_DATA_DIRS -u XDG_CONFIG_DIRS -u XDG_DATA_HOME -u XDG_CONFIG_HOME
               -u VK_ADD_LAYER_PATH -u VK_ADD_IMPLICIT_LAYER_PATH -u VK_ADD_DRIVER_FILES
               "HOME=${caller_home}" "XDG_RUNTIME_DIR=${caller_runtime}")

set(failures 0)
set(cases 0)
macro(fail what)
    message("FAIL: ${what}")
    math(EXPR failures "${failures} + 1")
endmacro()

# Runs the wrapper as the made-up caller, with `extra_env` (a list of NAME=value) on top, and the
# command after it. Sets `out` to its stdout and `rc` to ctest's view of how it ended. No ';' in the
# command's words: ARGN would split them. The timeout turns a wrapper that hangs into a result no
# case accepts.
function(run_wrapper out rc extra_env mode)
    set(args --config "${CONF}")
    if(mode STREQUAL "display")
        list(APPEND args --display /nonexistent/wallpaperx-no-kwin)
    elseif(mode STREQUAL "dead-kwin")
        list(APPEND args --display /bin/false)
    endif()
    execute_process(COMMAND ${caller_env} ${extra_env} /bin/sh "${WRAPPER}" ${args} -- ${ARGN}
                    OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE r TIMEOUT 60)
    set(${out} "${o}" PARENT_SCOPE)
    set(${rc} "${r}" PARENT_SCOPE)
    if(NOT r EQUAL 0 AND NOT e STREQUAL "")
        message("  (the wrapper's stderr: ${e})")
    endif()
endfunction()

# The entry's environment, then a marker, so an empty answer is told from a run that never got as
# far as the command.
set(show /bin/sh -c "env && echo RAN")

function(value_of text name out)
    if(text MATCHES "(^|\n)${name}=([^\n]*)")
        set(${out} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

function(inside path dir out)
    string(LENGTH "${dir}/" n)
    string(SUBSTRING "${path}/" 0 ${n} head)
    if(head STREQUAL "${dir}/")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# ---- a port bound inside an entry is not reachable from the caller -----------------------------
# GOAL: an entry that binds 127.0.0.1:18462 -- as plasma_surface and every test daemon do -- cannot
#       be reached on that port from the namespace that started the wrapper, which for a real run is
#       the desktop session whose wallpaper plugin polls it.
# WHY-THIS-SHAPE: the listener must be shown reachable from inside its own entry first; "refused
#       from outside" alone is equally what a listener that never bound looks like. Exit 2 from the
#       outside client (no route, not refused) is the harness's fault, not the wrapper's.
set(net "${root}/net")
file(WRITE "${net}/inner.sh" [=[
probe=$1 dir=$2
"$probe" listen 18462 "$dir" & listener=$!
i=0
while [ ! -e "$dir/ready" ] && [ $i -lt 200 ] && kill -0 $listener 2>/dev/null; do sleep 0.05; i=$((i + 1)); done
"$probe" connect 18462
echo $? >"$dir/inside"
wait $listener
echo RAN
]=])
file(WRITE "${net}/outer.sh" [=[
wrapper=$1 conf=$2 probe=$3 dir=$4
/bin/sh "$wrapper" --config "$conf" -- /bin/sh "$dir/inner.sh" "$probe" "$dir" >"$dir/wrapper.out" 2>&1 & w=$!
i=0
while [ ! -e "$dir/inside" ] && [ $i -lt 200 ] && kill -0 $w 2>/dev/null; do sleep 0.05; i=$((i + 1)); done
if [ -e "$dir/inside" ]; then
    "$probe" connect 18462
    echo $? >"$dir/outside"
fi
touch "$dir/stop"
wait $w
]=])
execute_process(COMMAND ${caller_env} /bin/sh "${net}/outer.sh" "${WRAPPER}" "${CONF}" "${PROBE}" "${net}"
                ERROR_VARIABLE net_err RESULT_VARIABLE net_rc TIMEOUT 60)
math(EXPR cases "${cases} + 1")
file(READ "${net}/wrapper.out" net_out)
foreach(f inside outside)
    set(${f} "")
    if(EXISTS "${net}/${f}")
        file(STRINGS "${net}/${f}" ${f} LIMIT_COUNT 1)
    endif()
endforeach()
if(NOT net_out MATCHES "RAN" OR NOT EXISTS "${net}/ready" OR NOT inside STREQUAL "0")
    message("FAIL (harness): the listener inside the nested entry was not reachable from that entry itself "
            "(ready: ${net}/ready, inside connect exit '${inside}'), so the outside view proves nothing. "
            "The wrapper said: ${net_out}")
    math(EXPR failures "${failures} + 1")
elseif(outside STREQUAL "0")
    fail("a port bound inside one entry is not reachable from the caller's network (127.0.0.1:18462 accepted a connection from outside)")
elseif(NOT outside STREQUAL "1")
    message("FAIL (harness): the caller could not try 127.0.0.1:18462 at all (client exit '${outside}': ${net_err})")
    math(EXPR failures "${failures} + 1")
endif()

# ---- where the private directories go, whatever the caller's TMPDIR ----------------------------
foreach(caller_tmp "${caller_home}/tmp" "${caller_runtime}/tmp")
    run_wrapper(o rc "TMPDIR=${caller_tmp}" bus ${show})
    math(EXPR cases "${cases} + 1")
    if(NOT o MATCHES "RAN")
        fail("the wrapper never ran its command with TMPDIR=${caller_tmp} (exit ${rc})")
        continue()
    endif()
    value_of("${o}" HOME home)
    value_of("${o}" TMPDIR tmpdir)
    value_of("${o}" TMP tmp)
    value_of("${o}" TEMP temp)
    value_of("${o}" XDG_RUNTIME_DIR runtime)
    foreach(var home tmpdir runtime)
        inside("${${var}}" "${caller_home}" in_home)
        inside("${${var}}" "${caller_runtime}" in_runtime)
        if(in_home OR "${${var}}" STREQUAL "")
            fail("with the caller's TMPDIR=${caller_tmp}, the entry's ${var}='${${var}}' is unset or inside the caller's home")
        endif()
        if(in_runtime)
            fail("with the caller's TMPDIR=${caller_tmp}, the entry's ${var}='${${var}}' is inside the caller's runtime directory")
        endif()
    endforeach()
    get_filename_component(home_parent "${home}" DIRECTORY)
    get_filename_component(tmp_parent "${tmpdir}" DIRECTORY)
    if(NOT tmp_parent STREQUAL home_parent OR NOT tmp STREQUAL tmpdir OR NOT temp STREQUAL tmpdir)
        fail("TMPDIR, TMP and TEMP are one private directory beside the private HOME (TMPDIR=${tmpdir} TMP=${tmp} TEMP=${temp} HOME=${home})")
    endif()
endforeach()

# ---- the caller's search paths, less what lies in its home --------------------------------------
run_wrapper(o rc "XDG_DATA_DIRS=${caller_home}/.local/share:/opt/wpx-test/share:/usr/share;XDG_CONFIG_DIRS=${caller_home}/.config-extra:/etc/xdg:/opt/wpx-test/etc"
            bus ${show})
math(EXPR cases "${cases} + 1")
value_of("${o}" XDG_DATA_DIRS data_dirs)
value_of("${o}" XDG_CONFIG_DIRS config_dirs)
if(NOT o MATCHES "RAN" OR NOT data_dirs STREQUAL "/opt/wpx-test/share:/usr/share" OR
   NOT config_dirs STREQUAL "/etc/xdg:/opt/wpx-test/etc")
    fail("XDG_DATA_DIRS and XDG_CONFIG_DIRS keep the caller's entries outside its home and drop the rest (got '${data_dirs}' and '${config_dirs}')")
endif()
run_wrapper(o rc "" bus ${show})
math(EXPR cases "${cases} + 1")
value_of("${o}" XDG_DATA_DIRS data_dirs)
value_of("${o}" XDG_CONFIG_DIRS config_dirs)
if(NOT o MATCHES "RAN" OR NOT data_dirs STREQUAL "/usr/local/share:/usr/share" OR NOT config_dirs STREQUAL "/etc/xdg")
    fail("a caller with no search paths gets the system's (got '${data_dirs}' and '${config_dirs}')")
endif()

# ---- Vulkan layers and drivers in the caller's home stay visible to the loader ------------------
# GOAL: a layer or driver manifest directory that the private HOME and the filtered search paths
#       hide from the Vulkan loader is named to it instead, in the loader's own order, ahead of what
#       the caller already added; a directory that does not exist is not named.
# WHY-THIS-SHAPE: exact strings, so a forwarded directory in the wrong variable or order is red.
#       The made-up home's config directory has no vulkan/ at all, for the "does not exist" half.
set(vk_data "${caller_home}/.local/share/vulkan")
set(vk_extra "${caller_home}/extra-share/vulkan")
file(MAKE_DIRECTORY "${vk_data}/explicit_layer.d" "${vk_extra}/implicit_layer.d" "${vk_extra}/icd.d"
                    "${vk_extra}/explicit_layer.d")
run_wrapper(o rc "XDG_DATA_DIRS=${caller_home}/extra-share:/usr/share;VK_ADD_LAYER_PATH=/opt/wpx-test/layers"
            bus ${show})
math(EXPR cases "${cases} + 1")
value_of("${o}" VK_ADD_LAYER_PATH layers)
value_of("${o}" VK_ADD_IMPLICIT_LAYER_PATH implicit)
value_of("${o}" VK_ADD_DRIVER_FILES drivers)
set(want_layers "${vk_data}/explicit_layer.d:${vk_extra}/explicit_layer.d:/opt/wpx-test/layers")
if(NOT o MATCHES "RAN" OR NOT layers STREQUAL want_layers OR NOT implicit STREQUAL "${vk_extra}/implicit_layer.d" OR
   NOT drivers STREQUAL "${vk_extra}/icd.d")
    fail("the Vulkan manifest directories the entry loses are passed to the loader (VK_ADD_LAYER_PATH='${layers}', "
         "wanted '${want_layers}'; VK_ADD_IMPLICIT_LAYER_PATH='${implicit}'; VK_ADD_DRIVER_FILES='${drivers}')")
endif()

# ---- no way to the caller's sound server --------------------------------------------------------
# GOAL: a caller whose environment names its PipeWire or PulseAudio server -- a socket, a runtime
#       directory, a cookie, a config or module directory -- hands none of it to the entry, so a
#       daemon under test that plays sound can reach only a server inside its private runtime directory.
# WHY-THIS-SHAPE: every name set to a decoy under the made-up caller's runtime directory and looked
#       for by name AND by value, so a renamed copy (PIPEWIRE_REMOTE kept as another variable) is red too.
#       The end-to-end half -- the daemon's sound engine not connecting to a listening decoy -- is the
#       audio_isolation entry's.
# CONTROL: via control_run.sh 2026-10-11, reverted: the sound-server `unset` in
#       renderer/tests/support/isolated_run.sh deleted -> "FAIL: the caller's sound-server variables do not
#       reach the entry (PIPEWIRE_RUNTIME_DIR=...".
# The names are the wrapper's own `unset` statement under its sound-server comment, read from the script,
# so the list has one source; audio_playback_test's kSoundServerVariables must hold the same names.
# GOAL: S15.541 -- the sound-server variables this case decoys, the ones the wrapper unsets and the ones
#       audio_playback_test refuses to run with are one list. False if a name is added to one copy only.
# WHY-THIS-SHAPE: parsed, not written out a third time; a parse that finds nothing fails, so a moved
#       comment or reshaped statement cannot empty the loop below and leave this case vacuous.
# CONTROL: via control_run.sh 2026-10-11, reverted, in renderer/tests/support/isolated_run.sh: PIPEWIRE_LATENCY
#       added to the unset -> "FAIL: audio_playback_test's kSoundServerVariables is isolated_run.sh's
#       sound-server unset list"; the comment above the unset reworded -> "FAIL: the sound-server variables
#       read from ...'s unset statement include PIPEWIRE_RUNTIME_DIR (found '')", and each other required name.
file(READ "${WRAPPER}" wrapper_text)
string(REGEX MATCH "# What leads a sound client[^\n]*\n(#[^\n]*\n)*unset ([^\n]*\\\\\n)*[^\n]*" sound_unset "${wrapper_text}")
string(REGEX REPLACE "^.*\nunset " "" sound_unset "${sound_unset}")
string(REGEX REPLACE "[\\\\ \t\n]+" ";" sound_vars "${sound_unset}")
list(REMOVE_ITEM sound_vars "")
# GOAL: S15.541, second clause -- the names that lead a client straight to a server are always in that
#       list, so the script and the array cannot drop one together with the suite green.
# WHY-THIS-SHAPE: a short required set, not the whole list a third time; an empty parse misses every name.
# CONTROL: via control_run.sh 2026-10-11, reverted: PULSE_SERVER taken out of both isolated_run.sh's unset
#       and kSoundServerVariables -> "FAIL: the sound-server variables read from ...'s unset statement include
#       PULSE_SERVER".
foreach(required PIPEWIRE_RUNTIME_DIR PIPEWIRE_REMOTE PIPEWIRE_CORE PULSE_SERVER USERPROFILE)
    if(NOT required IN_LIST sound_vars)
        fail("the sound-server variables read from ${WRAPPER}'s unset statement include ${required} (found '${sound_vars}')")
    endif()
endforeach()
if(NOT DEFINED SOUND_TEST_SOURCE OR NOT EXISTS "${SOUND_TEST_SOURCE}")
    fail("audio_playback_test's source is given to this check (-DSOUND_TEST_SOURCE='${SOUND_TEST_SOURCE}')")
else()
    file(READ "${SOUND_TEST_SOURCE}" sound_test_text)
    string(REGEX MATCH "kSoundServerVariables\\[\\] = {[^}]*}" sound_array "${sound_test_text}")
    string(REGEX MATCHALL "\"[A-Z_]+\"" test_vars "${sound_array}")
    string(REPLACE "\"" "" test_vars "${test_vars}")
    set(script_sorted ${sound_vars})
    list(SORT script_sorted)
    list(SORT test_vars)
    if(NOT script_sorted STREQUAL test_vars)
        fail("audio_playback_test's kSoundServerVariables is isolated_run.sh's sound-server unset list (script: "
             "'${script_sorted}'; test: '${test_vars}')")
    endif()
endif()
set(decoy "${caller_runtime}/decoy-sound-server")
set(sound_env "")
foreach(var IN LISTS sound_vars)
    list(APPEND sound_env "${var}=${decoy}-${var}")
endforeach()
run_wrapper(o rc "${sound_env}" bus ${show})
math(EXPR cases "${cases} + 1")
set(leaked_sound "")
foreach(var IN LISTS sound_vars)
    value_of("${o}" ${var} got)
    if(NOT got STREQUAL "")
        list(APPEND leaked_sound "${var}=${got}")
    endif()
endforeach()
if(o MATCHES "decoy-sound-server")
    list(APPEND leaked_sound "a decoy path under another name")
endif()
if(NOT o MATCHES "RAN" OR NOT leaked_sound STREQUAL "")
    fail("the caller's sound-server variables do not reach the entry (${leaked_sound}; ran: ${rc})")
endif()

# ---- the Qt platform ----------------------------------------------------------------------------
run_wrapper(o rc "QT_QPA_PLATFORM=offscreen" display ${show})
math(EXPR cases "${cases} + 1")
value_of("${o}" QT_QPA_PLATFORM platform)
if(NOT o MATCHES "RAN" OR NOT platform STREQUAL "wayland;offscreen")
    fail("QT_QPA_PLATFORM: a display entry whose caller exported 'offscreen' is put on 'wayland;offscreen' (got '${platform}')")
endif()
run_wrapper(o rc "" bus ${show})
math(EXPR cases "${cases} + 1")
value_of("${o}" QT_QPA_PLATFORM platform)
if(NOT o MATCHES "RAN" OR NOT platform STREQUAL "offscreen")
    fail("QT_QPA_PLATFORM: a bus entry that chose nothing runs offscreen (got '${platform}')")
endif()
# GOAL: a display entry whose compositor was found at configure time but does not start fails,
#       rather than running on offscreen, where the GL entries skip and the real GPU goes unexercised.
run_wrapper(o rc "" dead-kwin ${show})
math(EXPR cases "${cases} + 1")
if(o MATCHES "RAN" OR NOT rc STREQUAL "2")
    fail("a display entry whose compositor exits before opening its socket is not run (exit '${rc}', ran: ${o})")
endif()

# ---- how a test ended reaches ctest -------------------------------------------------------------
# GOAL: a test killed by a signal whose default is only to terminate is reported to ctest as that
#       signal; one killed by a core-dumping signal, or one exiting with a stop signal's number,
#       comes back as the plain status, with the wrapper neither crashing nor stopping itself.
# WHY-THIS-SHAPE: the crashing test is the probe marked undumpable, so a run leaves no crash record;
#       the wrapper's own crash would show as rc "SIGSEGV" instead of "139". 148 is SIGTSTP's
#       number: a wrapper that re-raised it would stop and reach run_wrapper's timeout.
run_wrapper(o rc "" bus /bin/sh -c "echo RAN && kill -USR1 \$\$")
math(EXPR cases "${cases} + 1")
if(NOT o MATCHES "RAN" OR NOT rc STREQUAL "SIGUSR1")
    fail("a test killed by SIGUSR1 is reported as killed by SIGUSR1 (got '${rc}')")
endif()
run_wrapper(o rc "" bus /bin/sh -c "echo RAN && exec \"${PROBE}\" crash")
math(EXPR cases "${cases} + 1")
if(NOT o MATCHES "RAN" OR NOT rc STREQUAL "139")
    fail("a test killed by SIGSEGV comes back as exit status 139, the wrapper itself not crashing (got '${rc}')")
endif()
# GOAL: X.28 -- the probe this crash uses is non-dumpable as it raises, so the run adds nothing to
#       the host's crash collector.
# WHY-THIS-SHAPE: the probe reads the kernel's flag back after the call and prints it; 139 above is
#       the same whether or not a core was taken, and no shell or cmake call here exposes the core bit.
# CONTROL: via control_batch 2026-10-04, reverted: wrapper_probe.cpp's suppressCoreDump() replaced
#       by RLIMIT_CORE 1 (dumpable, but the kernel skips a piped collector at exactly 1, so the
#       control itself added no entry) -> "FAIL: the crashing probe was still dumpable as it raised SIGSEGV".
if(NOT o MATCHES "probe dumpable 0")
    fail("the crashing probe was still dumpable as it raised SIGSEGV, so the run reaches the crash collector (said: ${o})")
endif()
run_wrapper(o rc "" bus /bin/sh -c "echo RAN && exit 148")
math(EXPR cases "${cases} + 1")
if(NOT o MATCHES "RAN" OR NOT rc STREQUAL "148")
    fail("a test exiting 148, SIGTSTP's number, comes back as 148 without the wrapper stopping (got '${rc}')")
endif()
run_wrapper(o rc "" bus /bin/sh -c "echo RAN && exit 3")
math(EXPR cases "${cases} + 1")
if(NOT o MATCHES "RAN" OR NOT rc STREQUAL "3")
    fail("a test's own exit status reaches ctest unchanged (3 became '${rc}')")
endif()

# ---- what a run killed outright leaves behind ---------------------------------------------------
# GOAL: the next run removes a scratch directory whose owner lock nobody holds, and one with no
#       owner file that is old; it keeps one whose lock is held -- by a live wrapper or by a test
#       that outlived its killed wrapper -- and a new one with no owner file yet.
# WHY-THIS-SHAPE: the held lock is a separate process's, checked to be held before the sweep runs,
#       so "kept" cannot come from a lock that was never taken.
set(sweep "${root}/sweep")
foreach(d dead live fresh old)
    file(MAKE_DIRECTORY "${sweep}/wallpaperx-test.${d}")
endforeach()
file(WRITE "${sweep}/wallpaperx-test.dead/owner" "")
file(WRITE "${sweep}/wallpaperx-test.live/owner" "")
execute_process(COMMAND /bin/sh -c "exec 9>>'${sweep}/wallpaperx-test.live/owner'; flock -n 9 || exit 1
                                    exec sleep 60 </dev/null >/dev/null 2>&1 &
                                    echo \$!"
                OUTPUT_VARIABLE holder OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND flock -n "${sweep}/wallpaperx-test.live/owner" true RESULT_VARIABLE free_rc)
execute_process(COMMAND touch -d "10 minutes ago" "${sweep}/wallpaperx-test.old")
math(EXPR cases "${cases} + 1")
if(holder STREQUAL "" OR free_rc EQUAL 0)
    message("FAIL (harness): no process could be made to hold the 'live' directory's lock, so the sweep case proves nothing")
    math(EXPR failures "${failures} + 1")
else()
    run_wrapper(o rc "TMPDIR=${sweep}" bus /bin/sh -c "echo RAN")
    if(NOT o MATCHES "RAN")
        fail("the wrapper never ran its command in the sweep case (exit ${rc})")
    endif()
    if(EXISTS "${sweep}/wallpaperx-test.dead")
        fail("a scratch directory whose owner lock nobody holds is removed by the next run")
    endif()
    if(EXISTS "${sweep}/wallpaperx-test.old")
        fail("a scratch directory with no owner, ten minutes old, is removed by the next run")
    endif()
    if(NOT EXISTS "${sweep}/wallpaperx-test.live" OR NOT EXISTS "${sweep}/wallpaperx-test.fresh")
        fail("a scratch directory whose owner lock is held, or that has no owner yet and is new, is kept")
    endif()
    file(GLOB left "${sweep}/wallpaperx-test.*")
    foreach(d dead live fresh old)
        list(REMOVE_ITEM left "${sweep}/wallpaperx-test.${d}")
    endforeach()
    if(NOT left STREQUAL "")
        fail("a wrapper that ends normally removes its own scratch directory (left: ${left})")
    endif()
endif()
if(NOT holder STREQUAL "")
    execute_process(COMMAND kill ${holder} ERROR_QUIET)
endif()

# A wrapper killed with SIGKILL, whose test keeps running: kept while the test lives, removed after.
set(killed "${root}/killed")
execute_process(COMMAND ${caller_env} "TMPDIR=${killed}" /bin/sh -c
                        "/bin/sh '${WRAPPER}' --config '${CONF}' -- /bin/sh -c 'echo \$\$ > ${killed}/child.pid; exec sleep 30' >/dev/null 2>&1 &
                         w=\$!; i=0
                         while [ ! -s '${killed}/child.pid' ] && [ \$i -lt 200 ]; do sleep 0.05; i=\$((i + 1)); done
                         kill -KILL \$w; wait \$w 2>/dev/null; exit 0"
                TIMEOUT 30)
set(child "")
if(EXISTS "${killed}/child.pid")
    file(STRINGS "${killed}/child.pid" child LIMIT_COUNT 1)
endif()
file(GLOB leaked "${killed}/wallpaperx-test.*")
list(LENGTH leaked n_leaked)
math(EXPR cases "${cases} + 1")
if(NOT n_leaked EQUAL 1 OR NOT child MATCHES "^[0-9]+$")
    message("FAIL (harness): a wrapper killed with SIGKILL left ${n_leaked} scratch directories and test pid '${child}', "
            "not the one directory and live test this case needs, so the sweeps after it would prove nothing")
    math(EXPR failures "${failures} + 1")
else()
    run_wrapper(o rc "TMPDIR=${killed}" bus /bin/sh -c "echo RAN")
    if(NOT EXISTS "${leaked}")
        fail("the scratch directory of a test still running after its wrapper was killed is kept by the next run (${leaked})")
    endif()
    execute_process(COMMAND /bin/sh -c "kill -KILL ${child}; i=0
                                        while kill -0 ${child} 2>/dev/null && [ \$i -lt 100 ]; do sleep 0.05; i=\$((i + 1)); done")
    run_wrapper(o rc "TMPDIR=${killed}" bus /bin/sh -c "echo RAN")
    if(EXISTS "${leaked}")
        fail("the scratch directory left by a wrapper killed outright is removed once its test has ended too (${leaked})")
    endif()
endif()
if(child MATCHES "^[0-9]+$")
    execute_process(COMMAND kill -KILL ${child} ERROR_QUIET)
endif()

file(REMOVE_RECURSE "${root}")
message("test_isolation_wrapper: ${cases} case(s) run")
if(NOT cases EQUAL 16)
    fail("16 cases were expected to run, ${cases} did")
endif()
if(failures GREATER 0)
    message(FATAL_ERROR "test_isolation_wrapper: ${failures} failure(s)")
endif()
