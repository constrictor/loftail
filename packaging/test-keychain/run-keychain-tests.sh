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

# Runs tst_keychainlive against a REAL freedesktop Secret Service — the only thing that
# executes loftail's QtKeychain backend at all. Identical locally and in CI, and the
# run-ssh-tests.sh shape for the same reason: the standing instruction is to run it by
# hand before trusting a change to KeychainSecretStore.cpp, and until now there was
# nothing to run it against.
#
#   packaging/test-keychain/run-keychain-tests.sh --build build
#
# A THROWAWAY SESSION, WHICH IS WHAT MAKES IT SAFE AND WHAT MAKES IT MEAN ANYTHING.
# dbus-run-session gives the run a session bus of its own, and gnome-keyring-daemon takes
# org.freedesktop.secrets on it; the scratch HOME below is where the keyring file lands,
# so the developer's own keyring is never opened, read, written or unlocked. Five details
# are load-bearing and four of them were found the hard way:
#
#   * The unlock password is a NEWLINE on stdin, never EOF. `echo -n "" | …` starts the
#     daemon and it takes the bus name, but creates no login keyring — so read() and
#     erase() answer NotFound perfectly while store() fails with Failed. That is a
#     partial green, which is worse than a red: two of the three cases pass.
#   * XDG_RUNTIME_DIR must be SHORT. The daemon's control socket goes under it and a
#     Unix socket path is capped at 108 bytes, which a scratch directory under a build
#     tree overruns — reported as "Address already in use" on a path nothing is using.
#   * HOME is overridden, and that is the isolation. It is also what confines a kwalletd
#     that gets activated on the throwaway bus, which is what happens on a KDE box.
#   * XDG_CURRENT_DESKTOP=GNOME. QtKeychain picks its backend from the desktop and from
#     what is activatable, so on a Plasma developer machine it chooses KWallet and this
#     harness tests something other than the daemon it just started. Declaring GNOME is
#     both true here and deterministic.
#   * XDG_DATA_DIRS is emptied, so no D-Bus service file makes kwalletd activatable. A
#     CI runner has none installed and needs neither line; a developer box has both.
#
# The test binary skips every case unless a keychain actually answers, and a QSKIP is a
# zero exit status — so the exit code alone would go green against a session where
# nothing came up, which is the state this replaced. The run therefore names the
# functions that must have PASSED.
set -euo pipefail

build_dir=build
keep=0
runtime_dir=${LOFTAIL_KEYCHAIN_RUNTIME_DIR:-/tmp/loftail-keyring-$$}
log_dir=${LOFTAIL_KEYCHAIN_LOG_DIR:-}

usage()
{
    cat <<'USAGE'
Usage: run-keychain-tests.sh [--build DIR] [--keep] [--logs DIR]

  --build DIR   build directory holding tests/tst_keychainlive (default: build)
  --keep        leave the scratch home behind for poking at
  --logs DIR    copy the run's output here (CI uploads this)

Needs: dbus-run-session (dbus / dbus-x11), gnome-keyring-daemon (gnome-keyring),
libsecret-1 (libsecret-1-0). Nothing it does touches your own keyring.
USAGE
}

while [ $# -gt 0 ]; do
    case $1 in
        --build) build_dir=$2; shift 2 ;;
        --keep) keep=1; shift ;;
        --logs) log_dir=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

binary="$build_dir/tests/tst_keychainlive"
if [ ! -x "$binary" ]; then
    echo "error: $binary not found — configure with -DLOFTAIL_WITH_KEYCHAIN=ON and" >&2
    echo "       build the tst_keychainlive target" >&2
    exit 1
fi
binary=$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary")

for tool in dbus-run-session gnome-keyring-daemon; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: $tool is required (packages: dbus, gnome-keyring)" >&2
        exit 1
    }
done

# The scratch home. Short, because of the socket path cap above, and removed first so a
# previous run's login keyring cannot make this one pass without creating its own.
rm -rf "$runtime_dir"
mkdir -p "$runtime_dir/home/.local/share/keyrings" "$runtime_dir/home/.config" \
         "$runtime_dir/run" "$runtime_dir/empty"
chmod 700 "$runtime_dir/run"

cleanup()
{
    if [ "$keep" -eq 1 ]; then
        echo "==> keeping $runtime_dir"
    else
        rm -rf "$runtime_dir"
    fi
}
trap cleanup EXIT

# Everything below runs inside dbus-run-session, which is why it is a here-doc fed to sh
# rather than a function: the bus exists only for the life of that one command.
inner=$runtime_dir/inner.sh
cat > "$inner" <<'INNER'
#!/bin/sh
set -e
root=$1
binary=$2
export HOME="$root/home"
export XDG_DATA_HOME="$HOME/.local/share"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_RUNTIME_DIR="$root/run"

# A NEWLINE, not EOF — see the header. --components=secrets because the Secret Service is
# the whole of what QtKeychain talks to; the ssh-agent and pkcs11 components would only
# add things to go wrong.
printf '\n' | gnome-keyring-daemon --unlock --components=secrets --foreground \
    >"$root/keyring.log" 2>&1 &

# The daemon takes the bus name a moment after it starts. Poll rather than sleep, so a
# slow runner does not turn into a flake and a dead daemon fails in seconds. Through
# dbus-send and not busctl: dbus-send ships with dbus-run-session itself, so the check
# cannot be the thing that is missing.
has_secrets()
{
    dbus-send --session --print-reply --dest=org.freedesktop.DBus \
        /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner \
        string:org.freedesktop.secrets 2>/dev/null | grep -q 'boolean true'
}

i=0
while [ $i -lt 50 ]; do
    has_secrets && break
    i=$((i + 1))
    sleep 0.2
done
if ! has_secrets; then
    echo "error: gnome-keyring-daemon did not take org.freedesktop.secrets" >&2
    cat "$root/keyring.log" >&2 || true
    exit 1
fi
echo "==> org.freedesktop.secrets is up"

# set +e around it: the report below has to be read whether or not the binary passed,
# and a failing case is exactly when it is worth reading.
set +e
LOFTAIL_TEST_KEYCHAIN=1 QT_QPA_PLATFORM=offscreen "$binary"
status=$?
set -e

# The proof that the throwaway keyring is what answered, rather than something on the
# developer's own bus: the run created a login keyring here, under the scratch HOME.
if [ ! -f "$XDG_DATA_HOME/keyrings/login.keyring" ]; then
    echo "error: no keyring file under the scratch HOME — something else answered" >&2
    exit 1
fi
exit $status
INNER
chmod +x "$inner"

log=$runtime_dir/tst_keychainlive.log
echo "==> Running tst_keychainlive against a throwaway GNOME Keyring"
set +e
env -u KDE_SESSION_VERSION -u KDE_FULL_SESSION -u DESKTOP_SESSION \
    XDG_CURRENT_DESKTOP=GNOME XDG_DATA_DIRS="$runtime_dir/empty" \
    dbus-run-session -- "$inner" "$runtime_dir" "$binary" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

if [ -n "$log_dir" ]; then
    mkdir -p "$log_dir"
    cp "$log" "$log_dir/" 2>/dev/null || true
    cp "$runtime_dir/keyring.log" "$log_dir/" 2>/dev/null || true
fi

# Every case is gated on secretStore()->available(), and a QSKIP exits 0. Naming them is
# what tells "the keychain answered and the backend behaved" from "nothing came up".
failed=0
for fn in \
    roundTripsASecret \
    readingWhatIsNotThereIsNotFound \
    erasingWhatIsNotThereSucceeds \
    storingTwiceReplacesTheSecretRatherThanAddingOne \
    aSecretSurvivesBeingLongAndNotAscii \
    everyOperationRefusesToRunOffTheApplicationThread
do
    if ! grep -Eq "^PASS[[:space:]]*:[[:space:]]*TestKeychainLive::${fn}\(\)" "$log"; then
        echo "::error::${fn}() did not pass — it was skipped or never ran"
        grep -E "^[A-Z]+!?[[:space:]]*:[[:space:]]*TestKeychainLive::${fn}\(\)" "$log" || true
        failed=1
    fi
done

echo
if [ "$status" -ne 0 ] || [ "$failed" -ne 0 ]; then
    echo "FAILED — see the output above."
    exit 1
fi
echo "The real keychain answered and every case passed."
