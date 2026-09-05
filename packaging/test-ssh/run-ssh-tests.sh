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

# Runs tst_sshlive against real sshd servers in containers — the ONLY thing that
# executes loftail's libssh2 transport at all (CLAUDE.md: "a green pipeline means
# nothing about it"). Identical locally and in CI, deliberately: the standing
# instruction is to run this file by hand against a real host before trusting any change
# to the SSH layer, and until now there was no host to run it against.
#
#   packaging/test-ssh/run-ssh-tests.sh --build build
#
# THREE SERVERS, BECAUSE THE INTERESTING CODE IS IN THE FALLBACKS. A stock sshd only
# ever exercises the SFTP path; the exec transport is reached solely by a server with no
# working sftp-server, and the size ladder's lower rungs solely by one with no `stat`.
#
#   sftp     Ubuntu, stock sshd                the SFTP transport, config r/w, restart
#   nosftp   Ubuntu, no `Subsystem sftp`       Mode::Exec, its streaming read and write
#   busybox  Alpine, sftp but no `stat`        ExecSizeProbe's `ls -lnLd` and `wc -c`
#
# The test binary is run once per server, because which of its cases are reachable is
# decided by what the server offers — and a case that SKIPS looks exactly like a case
# that passed, so each run names the functions that must actually have run. That check
# is the point of the harness: without it a broken fixture is a green job.
set -euo pipefail

build_dir=build
keep=0
docker=${DOCKER:-docker}
port_base=${LOFTAIL_SSH_PORT_BASE:-2200}
log_dir=${LOFTAIL_SSH_LOG_DIR:-}

usage()
{
    cat <<'USAGE'
Usage: run-ssh-tests.sh [--build DIR] [--keep] [--port-base N]

  --build DIR     build directory holding tests/tst_sshlive (default: build)
  --keep          leave the containers and the scratch home behind for poking at
  --port-base N   first of the three loopback ports to publish on (default: 2200)
  --logs DIR      copy each run's output and each server's sshd log here before
                  tearing the containers down (CI uploads this)

  DOCKER=podman   use podman instead of docker
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
    --build)
        build_dir=${2:?--build needs a directory}
        shift 2
        ;;
    --keep)
        keep=1
        shift
        ;;
    --port-base)
        port_base=${2:?--port-base needs a number}
        shift 2
        ;;
    --logs)
        log_dir=${2:?--logs needs a directory}
        shift 2
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "unknown argument: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if ! command -v "$docker" >/dev/null 2>&1; then
    echo "error: $docker is not installed. Set DOCKER=podman to use podman." >&2
    exit 2
fi
if ! command -v ssh-keygen >/dev/null 2>&1 || ! command -v ssh >/dev/null 2>&1; then
    # tst_sshlive sets its fixtures up with the SYSTEM ssh client and reads the exec
    # transport's own commands back through it, so the client is as required as the
    # server. Without it every case fails on a fixture that was never written.
    echo "error: the OpenSSH client (ssh, ssh-keygen) is required" >&2
    exit 2
fi

binary=$build_dir/tests/tst_sshlive
if [ ! -x "$binary" ]; then
    echo "error: $binary not found or not executable." >&2
    echo "       Configure with SSH enabled and build it:" >&2
    echo "         cmake -S . -B $build_dir -G Ninja -DCMAKE_BUILD_TYPE=Debug" >&2
    echo "         cmake --build $build_dir --target tst_sshlive" >&2
    exit 2
fi
binary=$(cd -- "$(dirname -- "$binary")" && pwd)/$(basename -- "$binary")

sftp_port=$((port_base + 1))
nosftp_port=$((port_base + 2))
busybox_port=$((port_base + 3))

containers=(loftail-sshd-sftp loftail-sshd-nosftp loftail-sshd-busybox)
scratch=$(mktemp -d)
failed=0

cleanup()
{
    local status=$?
    # Before anything is removed: a server that refused every connection explains
    # itself in ITS log and nowhere else, and the containers are about to go.
    if [ -n "$log_dir" ]; then
        mkdir -p "$log_dir"
        cp "$scratch"/*.log "$log_dir/" 2>/dev/null || true
        for name in "${containers[@]}"; do
            "$docker" logs "$name" >"$log_dir/$name.sshd.log" 2>&1 || true
        done
    fi
    for name in "${containers[@]}"; do
        if [ "$keep" -eq 1 ]; then
            continue
        fi
        "$docker" rm -f "$name" >/dev/null 2>&1 || true
    done
    if [ "$keep" -eq 1 ]; then
        echo
        echo "Left behind: containers ${containers[*]}"
        echo "             scratch home $scratch/home (HOME= it to use the system ssh client)"
    else
        rm -rf "$scratch"
    fi
    exit "$status"
}
trap cleanup EXIT

# --- the client's identity ----------------------------------------------------------
#
# Generated per run, so nothing secret is committed and a stale key cannot linger. It
# serves BOTH clients: loftail's own auth ladder reads $HOME/.ssh/id_ed25519 (the same
# files OpenSSH would try), and the fixture helper shells out to `ssh`, which reads the
# same home. Pointing HOME at a scratch directory is also what keeps a run off the
# developer's real ~/.ssh — known_hosts included, which loftail appends to.
mkdir -p "$scratch/home/.ssh"
chmod 700 "$scratch/home" "$scratch/home/.ssh"
ssh-keygen -q -t ed25519 -N '' -C loftail-ssh-test -f "$scratch/home/.ssh/id_ed25519"
chmod 600 "$scratch/home/.ssh/id_ed25519"
pubkey=$(cat "$scratch/home/.ssh/id_ed25519.pub")
: >"$scratch/home/.ssh/known_hosts"
cat >"$scratch/home/.ssh/config" <<EOF
Host *
    IdentityFile ~/.ssh/id_ed25519
    IdentitiesOnly yes
    StrictHostKeyChecking yes
    BatchMode yes
EOF

# SSH_AUTH_SOCK is unset for every run below: an agent holding the developer's own keys
# would be offered first by both clients and burn MaxAuthTries against a server that
# knows one key.
run_env=(env -u SSH_AUTH_SOCK "HOME=$scratch/home" QT_QPA_PLATFORM=offscreen)

# --- images and servers -------------------------------------------------------------

echo "==> Building images"
"$docker" build -t loftail-sshd-ubuntu:test -f "$here/Dockerfile.ubuntu" "$here"
"$docker" build -t loftail-sshd-busybox:test -f "$here/Dockerfile.busybox" "$here"

start_server()
{
    local name=$1 image=$2 port=$3 with_sftp=$4 with_stat=$5

    "$docker" rm -f "$name" >/dev/null 2>&1 || true
    # No --rm: a server that dies on startup must leave its logs behind to be read.
    "$docker" run -d --name "$name" \
        -p "127.0.0.1:$port:22" \
        -e "LOFTAIL_CLIENT_PUBKEY=$pubkey" \
        -e "LOFTAIL_WITH_SFTP=$with_sftp" \
        -e "LOFTAIL_WITH_STAT=$with_stat" \
        "$image" >/dev/null

    # The host key is read out of the container rather than scanned off the port: it is
    # generated at startup, and ssh-keyscan would race it. loftail checks known_hosts
    # through libssh2_knownhost_checkp(), which understands the [host]:port form.
    local key=
    for _ in $(seq 60); do
        key=$("$docker" exec "$name" cat /etc/ssh/ssh_host_ed25519_key.pub 2>/dev/null || true)
        [ -n "$key" ] && break
        sleep 1
    done
    if [ -z "$key" ]; then
        echo "::error::$name never produced a host key"
        "$docker" logs "$name" || true
        exit 1
    fi
    printf '[127.0.0.1]:%s %s\n' "$port" "$(echo "$key" | cut -d' ' -f1-2)" \
        >>"$scratch/home/.ssh/known_hosts"

    # Readiness is a real non-interactive login, which is exactly what initTestCase()
    # asserts before it runs anything — so a fixture that cannot connect fails here,
    # with the server's log to hand, rather than as fifteen opaque test failures.
    for _ in $(seq 60); do
        if "${run_env[@]}" ssh -p "$port" -o ConnectTimeout=5 loftail@127.0.0.1 true \
            >/dev/null 2>&1; then
            echo "    $name ready on port $port (sftp=$with_sftp stat=$with_stat)"
            return 0
        fi
        sleep 1
    done
    echo "::error::could not log in to $name on port $port"
    "$docker" logs "$name" || true
    "${run_env[@]}" ssh -v -p "$port" loftail@127.0.0.1 true || true
    exit 1
}

echo "==> Starting servers"
start_server loftail-sshd-sftp loftail-sshd-ubuntu:test "$sftp_port" yes yes
start_server loftail-sshd-nosftp loftail-sshd-ubuntu:test "$nosftp_port" no yes
start_server loftail-sshd-busybox loftail-sshd-busybox:test "$busybox_port" yes no

# --- the runs -----------------------------------------------------------------------

# A QSKIP is a pass as far as an exit code is concerned, and every case in this file is
# gated on something about the server — so the exit code alone would go green against
# three servers that refuse every connection. Each run therefore names what must have
# actually run.
require_ran()
{
    local log=$1
    shift
    local fn missing=0
    for fn in "$@"; do
        if ! grep -Eq "^PASS[[:space:]]*:[[:space:]]*TestSshLive::${fn}\(\)" "$log"; then
            echo "::error::${fn}() did not pass — it was skipped or never ran"
            grep -E "^[A-Z]+!?[[:space:]]*:[[:space:]]*TestSshLive::${fn}\(\)" "$log" || true
            missing=1
        fi
    done
    return $missing
}

# $1 label, $2 main URL, $3 exec URL ("" for none), rest: functions to run.
# With no function names the whole file runs and the caller requires a subset; with
# them, QtTest runs exactly those, which is how a server that cannot reach a case is
# kept from failing it. Sets `last_log` rather than printing the path: the test's own
# output has to reach the terminal live, so this cannot run in a command substitution —
# which would also swallow every `failed=1` it sets, being a subshell.
last_log=
run_case()
{
    local label=$1 url=$2 exec_url=$3
    shift 3
    last_log="$scratch/$label.log"
    local -a env_extra=("LOFTAIL_TEST_SSH_URL=$url")
    if [ -n "$exec_url" ]; then
        env_extra+=("LOFTAIL_TEST_SSH_EXEC_URL=$exec_url")
    fi

    echo
    echo "==> $label: $url"
    local status=0
    set +e
    "${run_env[@]}" "${env_extra[@]}" "$binary" "$@" 2>&1 | tee "$last_log"
    status=${PIPESTATUS[0]}
    set -e
    if [ "$status" -ne 0 ]; then
        echo "::error::$label: tst_sshlive exited $status"
        failed=1
    fi
}

sftp_url="ssh://loftail@127.0.0.1:$sftp_port/tmp/loftail-test.log"
nosftp_url="ssh://loftail@127.0.0.1:$nosftp_port/tmp/loftail-test.log"
nosftp_exec_url="ssh://loftail@127.0.0.1:$nosftp_port/tmp/loftail-exec.log"
busybox_url="ssh://loftail@127.0.0.1:$busybox_port/tmp/loftail-test.log"

# Run A — everything, against the ordinary server, with the exec host named so that
# theExecStreamServesAForwardWalkFromOneChannel() (which needs BOTH: an SFTP main host
# for the rest of the file and an SFTP-less one of its own) is reachable.
run_case sftp "$sftp_url" "$nosftp_exec_url"
require_ran "$last_log" \
    connectsAndReadsTheRemoteFile \
    followsAppendsFromTheRealServer \
    detectsRealRotation \
    reportsAnUnreachableHostClearly \
    theExecFallbackReadsTheSameBytes \
    theExecFallbackSizesWithoutStat \
    theExecStreamServesAForwardWalkFromOneChannel \
    sequentialReadsLandWhereTheyAskedWithNoSeekBetweenThem \
    aConfigFileIsReadAndWrittenWholeOverSftp \
    writingAConfigKeepsItsPermissions \
    aRestartScriptRunsOnTheFarEndAndKeepsItsStderr \
    aRestartScriptOutlivesTheConnectTimeout \
    abortingARemoteScriptReturnsAtOnce \
    aRestartScriptRunsOnAnExecOnlyConnect \
    aDroppedLinkIsNoticedRatherThanPolledForEver \
    oneConnectionServesSeveralErrandsAndTheDrainLetsItGo || failed=1

# Run B — the exec transport as the LOG'S OWN transport, which is the shape a user on a
# stripped-down box gets: not SshSession driven directly, but Document, LiveController
# and the fetcher on top of it. The named cases are the ones that do not assert
# Mode::Sftp; oneConnectionServes... is left out because its drain latches the process's
# session cache shut, and it has already run above.
exec_cases=(
    connectsAndReadsTheRemoteFile
    followsAppendsFromTheRealServer
    detectsRealRotation
    theExecFallbackWritesTheSameBytes
    aRestartScriptRunsOnTheFarEndAndKeepsItsStderr
    aRestartScriptRunsOnAnExecOnlyConnect
)
run_case nosftp "$nosftp_url" "" "${exec_cases[@]}"
require_ran "$last_log" "${exec_cases[@]}" || failed=1

# Run C — a userland that is not the author's. theExecFallbackSizesWithoutStat() checks
# busybox's `ls -lnLd` and `wc -c` output against the size SFTP reports for the same
# file, which is the whole reason this server keeps its sftp-server while losing `stat`.
# theExecFallbackReadsTheSameBytes() is NOT run here: it requires `stat`.
busybox_cases=(
    connectsAndReadsTheRemoteFile
    followsAppendsFromTheRealServer
    detectsRealRotation
    theExecFallbackSizesWithoutStat
)
run_case busybox "$busybox_url" "" "${busybox_cases[@]}"
require_ran "$last_log" "${busybox_cases[@]}" || failed=1

echo
if [ "$failed" -ne 0 ]; then
    echo "FAILED — see the output above."
    exit 1
fi
echo "All three servers passed."
