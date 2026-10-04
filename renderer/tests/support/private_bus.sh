#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Starts COMMAND on a private session bus whose socket is made inside the entry's private runtime
# directory ($XDG_RUNTIME_DIR, which isolated_run.sh creates and removes), never in the shared /tmp:
# a bus killed outright -- ctest's SIGKILL on a timeout -- leaves its socket behind.
#
#   private_bus.sh [--conf-out FILE] DBUS_RUN_SESSION CONF -- COMMAND...
#
# CONF is a bus configuration with no <listen> of its own (isolated_session.conf, or one including
# it); the configuration run is CONF included under a <listen> here, written to FILE (default: the
# runtime directory). Used bare, a CONF with no <listen> makes dbus-daemon refuse to start.
set -u

fail() {
    echo "private_bus.sh: FAIL (harness): $*" >&2
    exit 2
}

# Text content of an XML element: '&' and '<' must be escaped, and '>' too where it follows "]]".
xml_escape() {
    printf '%s' "$1" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g'
}

# A D-Bus address value: every byte outside [-0-9A-Za-z_/.*] as %hh. The result holds no XML
# special character, so it goes into the configuration as it is.
address_escape() {
    printf '%s' "$1" | LC_ALL=C od -An -v -tx1 | tr -s ' ' '\n' | while read -r h; do
        [ -n "$h" ] || continue
        case $h in
            2a | 2d | 2e | 2f | 3[0-9] | 4[1-9a-f] | 5[0-9a] | 5f | 6[1-9a-f] | 7[0-9a])
                # shellcheck disable=SC2059 # the octal escape is the format on purpose
                printf "\\$(printf %03o "0x$h")" ;;
            *) printf '%%%s' "$h" ;;
        esac
    done
}

out=""
if [ "${1-}" = --conf-out ]; then
    [ $# -ge 2 ] || fail "--conf-out needs a file"
    out=$2
    shift 2
fi
if [ $# -lt 4 ] || [ "$3" != -- ]; then
    fail "usage: private_bus.sh [--conf-out FILE] DBUS_RUN_SESSION CONF -- COMMAND..."
fi
drs=$1 conf=$2
shift 3

dir=${XDG_RUNTIME_DIR-}
[ -n "$dir" ] && [ -d "$dir" ] || fail "XDG_RUNTIME_DIR names no directory, so the bus has nowhere private to listen"
case "$conf" in /*) ;; *) fail "CONF '$conf' is not an absolute path" ;; esac
# A socket path holds 107 bytes, of which dbus-daemon's own name for the socket takes 16.
# isolated_run.sh keeps the runtime directory short enough, whatever TMPDIR is.
[ ${#dir} -le 90 ] || fail "the runtime directory '$dir' is too long for a socket inside it (isolated_run.sh keeps it short)"
[ -n "$out" ] || out="$dir/bus-$$.conf"

if ! cat > "$out" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <listen>unix:dir=$(address_escape "$dir")</listen>
  <include>$(xml_escape "$conf")</include>
</busconfig>
EOF
then
    fail "the bus configuration could not be written to $out"
fi
exec "$drs" --config-file="$out" -- "$@"
