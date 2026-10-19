#!/usr/bin/env bash
# loftail — a desktop viewer for log4cplus logs.
# Copyright (C) 2026 Valentyn Pavliuchenko
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

# Runs tst_avahilive against a REAL Avahi daemon — the only thing that executes loftail's Avahi
# backend (src/core/AvahiBrowser.cpp) and its AvahiPoll adapter at all. Identical locally and in
# CI, run-keychain-tests.sh's shape for run-keychain-tests.sh's reason: a scripted browser
# satisfies the DiscoveryBrowser seam whether or not the platform half works.
#
#   packaging/test-mdns/run-mdns-tests.sh --build build
#
# A PRIVATE SYSTEM BUS, which is what makes it safe: avahi-client reaches the daemon over the
# SYSTEM bus, so the harness starts a dbus-daemon of its own and points DBUS_SYSTEM_BUS_ADDRESS
# at it, and an avahi-daemon on that bus. Nothing touches the machine's own bus or responder.
# Where a system avahi-daemon is ALREADY running, it is used instead — two responders on one
# host fight over UDP 5353 and the pid file, and the system one answers the same questions.
#
# avahi-daemon refuses to run as anybody but root, so the daemon (and only the daemon) is
# started through sudo when the harness is not root.
#
# EACH RUN NAMES THE FUNCTIONS THAT MUST HAVE PASSED, the rule run-ssh-tests.sh and
# run-keychain-tests.sh keep: every case QSKIPs without LOFTAIL_TEST_MDNS, and a skip is a zero
# exit status.

set -euo pipefail

build_dir=build
log_dir=
while [ $# -gt 0 ]; do
    case $1 in
        --build) build_dir=$2; shift 2 ;;
        --logs) log_dir=$2; shift 2 ;;
        -h|--help)
            echo "Usage: run-mdns-tests.sh [--build DIR] [--logs DIR]"
            echo "Needs: dbus-daemon (dbus), avahi-daemon and avahi-publish (avahi-daemon, avahi-utils)"
            exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

binary="$build_dir/tests/tst_avahilive"
if [ ! -x "$binary" ]; then
    echo "error: $binary not found — install libavahi-client-dev, reconfigure and build" >&2
    echo "       the tst_avahilive target" >&2
    exit 1
fi
binary=$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary")

for tool in dbus-daemon avahi-daemon avahi-publish; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: $tool is required (packages: dbus, avahi-daemon, avahi-utils)" >&2
        exit 1
    }
done

sudo_=
[ "$(id -u)" -ne 0 ] && sudo_=sudo

scratch=$(mktemp -d /tmp/loftail-mdns-XXXXXX)
pids=()
own_daemon=0
cleanup()
{
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
    if [ "$own_daemon" -eq 1 ]; then
        $sudo_ avahi-daemon --kill 2>/dev/null || true
    fi
    [ -n "${bus_pid:-}" ] && kill "$bus_pid" 2>/dev/null || true
    rm -rf "$scratch"
}
trap cleanup EXIT

if avahi-daemon --check 2>/dev/null; then
    echo "==> using the avahi-daemon already running on the system bus"
else
    cat > "$scratch/bus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=$scratch/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow own="*"/>
    <allow send_destination="*"/>
    <allow receive_sender="*"/>
  </policy>
</busconfig>
CONF
    cat > "$scratch/avahi.conf" <<CONF
[server]
use-ipv4=yes
use-ipv6=no
enable-dbus=yes
[publish]
publish-hinfo=no
publish-workstation=no
CONF
    dbus-daemon --config-file="$scratch/bus.conf" --fork --print-address=1 \
        --print-pid=3 >"$scratch/addr" 3>"$scratch/buspid"
    bus_pid=$(cat "$scratch/buspid")
    DBUS_SYSTEM_BUS_ADDRESS=$(head -n1 "$scratch/addr")
    export DBUS_SYSTEM_BUS_ADDRESS
    # The bus socket must be reachable by the daemon, which runs as root.
    chmod 755 "$scratch"
    $sudo_ env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
        avahi-daemon --daemonize --no-drop-root --no-chroot --no-rlimits \
        -f "$scratch/avahi.conf" 2>"$scratch/avahi.log" || {
            echo "error: avahi-daemon did not start" >&2
            cat "$scratch/avahi.log" >&2 || true
            exit 1
        }
    own_daemon=1
    echo "==> avahi-daemon is up on a private system bus"
fi

avahi-publish -s loftail-cam-1 _loftailtest._tcp 2222 model=X100 >"$scratch/p1.log" 2>&1 &
pids+=($!)
avahi-publish -s loftail-printer _loftailtest._tcp 631 model=LaserJet >"$scratch/p2.log" 2>&1 &
pids+=($!)
for i in $(seq 1 50); do
    grep -q Established "$scratch/p1.log" 2>/dev/null \
        && grep -q Established "$scratch/p2.log" 2>/dev/null && break
    sleep 0.2
done

log=$scratch/tst_avahilive.log
set +e
LOFTAIL_TEST_MDNS=1 QT_QPA_PLATFORM=offscreen "$binary" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

if [ -n "$log_dir" ]; then
    mkdir -p "$log_dir"
    cp "$log" "$scratch"/*.log "$log_dir/" 2>/dev/null || true
fi

failed=0
for fn in \
    theBrowserFindsAPublishedServiceAndItsTxtRecords \
    theConnectTimeLookupWorksOnAWorkerThread \
    anInstanceNobodyPublishedIsNotFound
do
    if ! grep -Eq "^PASS[[:space:]]*:[[:space:]]*TestAvahiLive::${fn}\(\)" "$log"; then
        echo "::error::${fn}() did not pass — it was skipped or never ran"
        failed=1
    fi
done

echo
if [ "$status" -ne 0 ] || [ "$failed" -ne 0 ]; then
    echo "FAILED — see the output above."
    exit 1
fi
echo "The real Avahi daemon answered and every case passed."
