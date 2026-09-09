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
# SIX SERVERS, BECAUSE THE INTERESTING CODE IS IN THE FALLBACKS. A stock sshd only
# ever exercises the SFTP path; the exec transport is reached solely by a server with no
# working sftp-server, and the size ladder's lower rungs solely by one with no `stat`.
#
#   sftp     Ubuntu, stock sshd                the SFTP transport, config r/w, restart
#   nosftp   Ubuntu, no `Subsystem sftp`       Mode::Exec, its streaming read and write
#   busybox  Alpine, sftp but no `stat`        ExecSizeProbe's `ls -lnLd` and `wc -c`
#
# THREE MORE JOINED THEM, each for one fault that was invisible on all of the above —
# every one of them found by pointing loftail at a server shaped like somebody else's
# machine rather than like the author's:
#
#   blackhole Ubuntu, `Subsystem sftp /bin/cat`  the 20 s Need::ExecOnly saves. The server
#                                                CLAUDE.md recorded as needing a real
#                                                machine: it ACCEPTS the channel and then
#                                                says nothing, which is what makes the
#                                                fallback a probe and not an error code.
#   badkey    Ubuntu, key-only, wrong key        a rebooting host whose authorized_keys is
#                                                not readable yet. Classified Refused, so
#                                                ReconnectGrace never fired and the tab
#                                                died on the first attempt — the case that
#                                                policy was written for.
#   nosize    Ubuntu, no `stat`, no `ls`         a log past the `wc` ceiling, which used to
#                                                be reported as a missing file and waited
#                                                for for ever.
#
# The stock server also gets a 64 KB tmpfs at /tiny, which is where a config write is made
# to fail on the FILESYSTEM rather than on the link — the third of the three.
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
  --port-base N   first of the six loopback ports to publish on (default: 2200)
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
blackhole_port=$((port_base + 4))
badkey_port=$((port_base + 5))
nosize_port=$((port_base + 6))

containers=(loftail-sshd-sftp loftail-sshd-nosftp loftail-sshd-busybox
    loftail-sshd-blackhole loftail-sshd-badkey loftail-sshd-nosize)
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
        # loftail's own diagnostic log, which is on by default and lands under the
        # scratch HOME (QStandardPaths::AppLocalDataLocation). It is the only account of
        # what the FETCHER did — the test output says what the document ended up
        # showing, which is a different question from where the fetch stopped.
        find "$scratch/home/.local/share" -name loftail.log -exec cp {} "$log_dir/loftail-diag.log" \; \
            2>/dev/null || true
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
        echo "             scratch home $scratch/home; reach a server with its ssh shim:"
        echo "               PATH=$scratch/bin:\$PATH ssh -p $sftp_port loftail@127.0.0.1"
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
mkdir -p "$scratch/home/.ssh" "$scratch/bin"
chmod 700 "$scratch/home" "$scratch/home/.ssh"
ssh-keygen -q -t ed25519 -N '' -C loftail-ssh-test -f "$scratch/home/.ssh/id_ed25519"
chmod 600 "$scratch/home/.ssh/id_ed25519"
pubkey=$(cat "$scratch/home/.ssh/id_ed25519.pub")
: >"$scratch/home/.ssh/known_hosts"

# THE TWO CLIENTS ARE STEERED DIFFERENTLY, AND HOME ONLY REACHES ONE OF THEM.
# loftail finds its keys and known_hosts through QStandardPaths::HomeLocation, which
# reads $HOME. OPENSSH DOES NOT: ssh takes the home directory from the passwd database
# (getpwuid), so `HOME=... ssh` reads the real user's ~/.ssh whatever the environment
# says — `ssh -G` prints the resolved paths and shows it. Setting HOME alone therefore
# gave loftail the scratch identity and the fixture helper the developer's, and every
# server refused every fixture command ("Host key verification failed").
#
# There is no environment variable for ssh's config, and the helper inside tst_sshlive
# builds its own argv, so -F cannot be passed to it. A shim earlier on PATH is what
# reaches it: QProcess::start("ssh", ...) searches PATH, and the shim adds the -F. It
# affects fixtures only — loftail's own transport never runs the ssh binary.
#
# Absolute paths in that config, not `~`: the tilde would expand to the passwd home for
# exactly the same reason.
cat >"$scratch/home/.ssh/config" <<EOF
Host *
    IdentityFile $scratch/home/.ssh/id_ed25519
    IdentitiesOnly yes
    UserKnownHostsFile $scratch/home/.ssh/known_hosts
    StrictHostKeyChecking yes
    BatchMode yes
EOF

real_ssh=$(command -v ssh)
cat >"$scratch/bin/ssh" <<EOF
#!/bin/sh
exec "$real_ssh" -F "$scratch/home/.ssh/config" "\$@"
EOF
chmod +x "$scratch/bin/ssh"

# SSH_AUTH_SOCK is unset for every run below: an agent holding the developer's own keys
# would be offered first by both clients and burn MaxAuthTries against a server that
# knows one key.
run_env=(env -u SSH_AUTH_SOCK "HOME=$scratch/home" "PATH=$scratch/bin:$PATH"
    QT_QPA_PLATFORM=offscreen)

# So that `--build build-asan` simply works. Running this harness against a sanitizer
# build is worth being able to do — it is the only way the libssh2 transport is ever
# executed under ASan at all, neither CI job doing both — and without the suppression file
# it goes red on a leak inside libssh2's own SFTP init against the blackhole server, which
# tests/lsan.supp explains and which nothing here can free. Not forced: a caller who has
# set LSAN_OPTIONS meant it.
if [ -z "${LSAN_OPTIONS:-}" ] && [ -f "$here/../../tests/lsan.supp" ]; then
    run_env+=("LSAN_OPTIONS=suppressions=$(cd -- "$here/../.." && pwd)/tests/lsan.supp")
fi

# --- images and servers -------------------------------------------------------------

echo "==> Building images"
"$docker" build -t loftail-sshd-ubuntu:test -f "$here/Dockerfile.ubuntu" "$here"
"$docker" build -t loftail-sshd-busybox:test -f "$here/Dockerfile.busybox" "$here"

# $1 name, $2 image, $3 port, $4 sftp, $5 stat, $6 password. Beyond those, per-server
# shaping is passed in the environment through the four variables named below, which the
# entrypoint reads: keeping them out of the positional list is what stops six servers from
# needing a nine-argument call each.
#
#   server_ls          "no" removes `ls` as well as `stat`
#   server_blackhole   "yes" points Subsystem sftp at something that never answers
#   server_pubkey      the authorized key, defaulting to this run's client key
#   server_tmpfs       a mount spec for a tiny filesystem, or empty
start_server()
{
    local name=$1 image=$2 port=$3 with_sftp=$4 with_stat=$5 password=${6:-}
    local authkey=${server_pubkey:-$pubkey}
    local -a extra=()
    if [ -n "${server_tmpfs:-}" ]; then
        extra+=(--tmpfs "$server_tmpfs")
    fi

    "$docker" rm -f "$name" >/dev/null 2>&1 || true
    # No --rm: a server that dies on startup must leave its logs behind to be read.
    "$docker" run -d --name "$name" \
        -p "127.0.0.1:$port:22" \
        "${extra[@]+"${extra[@]}"}" \
        -e "LOFTAIL_CLIENT_PUBKEY=$authkey" \
        -e "LOFTAIL_WITH_SFTP=$with_sftp" \
        -e "LOFTAIL_WITH_STAT=$with_stat" \
        -e "LOFTAIL_WITH_LS=${server_ls:-yes}" \
        -e "LOFTAIL_SFTP_BLACKHOLE=${server_blackhole:-no}" \
        -e "LOFTAIL_PASSWORD=$password" \
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
            echo "    $name ready on port $port (sftp=$with_sftp stat=$with_stat ls=${server_ls:-yes} blackhole=${server_blackhole:-no})"
            return 0
        fi
        sleep 1
    done
    echo "::error::could not log in to $name on port $port"
    "$docker" logs "$name" || true
    "${run_env[@]}" ssh -v -p "$port" loftail@127.0.0.1 true || true
    exit 1
}

# Generated per run, like the client key and for the same reason. It reaches exactly one
# server and exactly one case: aFirstConnectAsksForThePasswordWhenNoKeyAnswers(), which
# is the only place the password rung of the auth ladder is ever executed — every other
# case here signs in with a key, and the unattended path bails before it can be reached.
account_password=$(head -c 18 /dev/urandom | base64 | tr -d '/+=')

# The key the `badkey` server authorizes: a real, well-formed key that is simply not this
# client's. Generated per run like the client's own, and never installed anywhere else —
# what that server stands for is a host whose authorized_keys cannot be read YET, and the
# only way to stage that from outside is a host that does not know the key.
ssh-keygen -q -t ed25519 -N '' -C loftail-not-our-key -f "$scratch/notours" >/dev/null

echo "==> Starting servers"
# A tiny filesystem on the stock server, for the config write that has to fail on the
# DISK rather than on the link. 64 KB: far smaller than anything the test writes, and far
# too small to fill by accident.
server_tmpfs=/tiny:size=64k,mode=1777 \
    start_server loftail-sshd-sftp loftail-sshd-ubuntu:test "$sftp_port" yes yes \
    "$account_password"
start_server loftail-sshd-nosftp loftail-sshd-ubuntu:test "$nosftp_port" no yes
start_server loftail-sshd-busybox loftail-sshd-busybox:test "$busybox_port" yes no

# Accepts the subsystem channel and never answers on it. `no` for with_sftp as well, so
# that a build of the entrypoint which ignored the blackhole knob would produce a server
# with no Subsystem line at all — which the case detects and fails on, rather than passing
# against a server that answers SFTP promptly.
server_blackhole=yes \
    start_server loftail-sshd-blackhole loftail-sshd-ubuntu:test "$blackhole_port" no yes

# Offers publickey, and does not have ours. Its readiness probe cannot be the ordinary one
# — no client this run has can log in to it, which is the point — so it is started with
# the client key authorized and then handed the wrong one, which is also a closer model of
# the thing it stands for: a host that WAS reachable and briefly is not.
start_server loftail-sshd-badkey loftail-sshd-ubuntu:test "$badkey_port" yes yes
"$docker" exec loftail-sshd-badkey sh -c \
    "printf '%s\n' '$(cat "$scratch/notours.pub")' > /home/loftail/.ssh/authorized_keys"
if "${run_env[@]}" ssh -p "$badkey_port" -o ConnectTimeout=5 loftail@127.0.0.1 true \
    >/dev/null 2>&1; then
    echo "::error::loftail-sshd-badkey still accepts this client's key"
    exit 1
fi
echo "    loftail-sshd-badkey now refuses this client's key, as it must"

# No `stat` and no `ls`, so `wc -c` is the only rung and its ceiling is reachable.
server_ls=no \
    start_server loftail-sshd-nosize loftail-sshd-ubuntu:test "$nosize_port" no no

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
    # The shaped servers, each named only for the run whose cases reach it. Set for a run
    # that does not name the case, and nothing happens; NOT set for the run that does, and
    # the case QSKIPs — which is why require_ran() below names every one of them.
    local shaped
    for shaped in BADKEY_URL NOSIZE_URL BLACKHOLE_URL FULL_DIR; do
        local value="case_${shaped,,}"
        if [ -n "${!value:-}" ]; then
            env_extra+=("LOFTAIL_TEST_SSH_$shaped=${!value}")
        fi
    done
    # Only the run whose server was given one, and the case is gated on it: a password
    # offered to a run with no prompter is somewhere for a wedged test to hang.
    if [ -n "${case_password:-}" ]; then
        env_extra+=("LOFTAIL_TEST_SSH_PASSWORD=$case_password")
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
badkey_url="ssh://loftail@127.0.0.1:$badkey_port/tmp/loftail-test.log"
nosize_url="ssh://loftail@127.0.0.1:$nosize_port/tmp/loftail-size.log"
blackhole_url="ssh://loftail@127.0.0.1:$blackhole_port/tmp/loftail-test.log"
nosftp_url="ssh://loftail@127.0.0.1:$nosftp_port/tmp/loftail-test.log"
nosftp_exec_url="ssh://loftail@127.0.0.1:$nosftp_port/tmp/loftail-exec.log"
busybox_url="ssh://loftail@127.0.0.1:$busybox_port/tmp/loftail-test.log"

# Run A — everything, against the ordinary server, with the exec host named so that
# theExecStreamServesAForwardWalkFromOneChannel() (which needs BOTH: an SFTP main host
# for the rest of the file and an SFTP-less one of its own) is reachable.
case_password=$account_password
# The four shaped servers are named for THIS run and no other: every case that reaches one
# of them talks to it directly rather than through m_url, so they need an ordinary SFTP
# host for initTestCase() and nothing else from the run they are in.
case_badkey_url=$badkey_url
case_nosize_url=$nosize_url
case_blackhole_url=$blackhole_url
case_full_dir=/tiny
run_case sftp "$sftp_url" "$nosftp_exec_url"
case_password=
case_badkey_url=
case_nosize_url=
case_blackhole_url=
case_full_dir=
require_ran "$last_log" \
    connectsAndReadsTheRemoteFile \
    followsAppendsFromTheRealServer \
    detectsRealRotation \
    reportsAnUnreachableHostClearly \
    aFirstConnectAsksAboutTheHostKeyAndRemembersIt \
    aRejectedHostKeySendsNoCredential \
    aFirstConnectAsksForThePasswordWhenNoKeyAnswers \
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
    aKeyOnlyHostThatRefusesTheKeyIsWorthRetryingRatherThanRefused \
    aLogTooBigForTheOnlySizeRungIsRefusedRatherThanCalledMissing \
    aConfigWriteThatCannotFitBlamesTheFilesystemAndNotTheLink \
    anExecOnlyConnectSkipsTheSftpWaitTheLogTransportPays \
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
echo "All six servers passed."
