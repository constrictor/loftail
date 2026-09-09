#!/bin/sh
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

# Starts the sshd that tst_sshlive talks to. Shared by both images, so it is POSIX sh
# and reaches for nothing Ubuntu has and Alpine does not.
#
# THE SERVER IS CONFIGURED AT RUN TIME AND NOT AT BUILD TIME, which is what lets one
# image stand for two of the three servers: what the exec fallback and the size ladder
# exist for is a server missing a piece, and the pieces are removed here.
#
#   LOFTAIL_CLIENT_PUBKEY  the one authorized key. Required.
#   LOFTAIL_WITH_SFTP      "no" leaves out the Subsystem line, so libssh2_sftp_init()
#                          is refused and SshSession probes its way to Mode::Exec.
#   LOFTAIL_WITH_STAT      "no" deletes `stat` from every directory on PATH, which is
#                          the stripped-down image ExecSizeProbe's ls/wc rungs exist
#                          for (ARCHITECTURE.md §6.3.1).
#   LOFTAIL_WITH_LS        "no" deletes `ls` as well, which leaves `wc -c` as the only
#                          rung — the one with a size ceiling, and therefore the only
#                          way to reach ExecSizeProbe::tooBigToMeasure() at all.
#   LOFTAIL_SFTP_BLACKHOLE "yes" ACCEPTS the subsystem channel and puts nothing behind
#                          it, so libssh2_sftp_init() waits for a version packet that
#                          never comes. It is the server Need::ExecOnly exists to save
#                          twenty seconds against, and it overrides LOFTAIL_WITH_SFTP.
#   LOFTAIL_PASSWORD       when set, gives the account this password and turns
#                          PasswordAuthentication on. Empty (the default) leaves the
#                          server key-only, which is what every case with no prompter
#                          needs — see the PasswordAuthentication line below.
set -eu

: "${LOFTAIL_CLIENT_PUBKEY:?the client public key must be passed in}"
with_sftp=${LOFTAIL_WITH_SFTP:-yes}
with_stat=${LOFTAIL_WITH_STAT:-yes}
with_ls=${LOFTAIL_WITH_LS:-yes}
sftp_blackhole=${LOFTAIL_SFTP_BLACKHOLE:-no}
password=${LOFTAIL_PASSWORD:-}

home=/home/loftail
mkdir -p "$home/.ssh"
printf '%s\n' "$LOFTAIL_CLIENT_PUBKEY" >"$home/.ssh/authorized_keys"
chown -R loftail:loftail "$home/.ssh"
chmod 700 "$home/.ssh"
chmod 600 "$home/.ssh/authorized_keys"

# Generated per container start rather than baked in or mounted. The harness reads the
# public half back out with `docker exec` and writes known_hosts from it, so there is no
# private key in the repository and no ssh-keyscan race — and a bind-mounted host key
# would arrive owned by the host's uid, which sshd refuses to use.
ssh-keygen -A >/dev/null

# The account is created with `*` in /etc/shadow — no password will ever match — which
# is what a key-only account wants and what has to be undone to offer one.
if [ -n "$password" ]; then
    if ! command -v chpasswd >/dev/null 2>&1; then
        echo "loftail-sshd: LOFTAIL_PASSWORD was set but this image has no chpasswd" >&2
        exit 1
    fi
    printf 'loftail:%s\n' "$password" | chpasswd
fi

if [ "$with_stat" != yes ] || [ "$with_ls" != yes ]; then
    # Every directory on PATH, not a written-down /usr/bin/stat: it is a coreutils
    # binary on Ubuntu and a busybox applet link on Alpine, in different places.
    #
    # `ls` goes only where it is asked for, and only alongside a missing `stat`: with
    # both gone the size ladder has nothing left but `wc -c`, whose ceiling is the
    # subject. Removing `ls` on its own would prove nothing — the `stat` rung above it
    # answers first — and removing it from the busybox image would take the `ls -lnLd`
    # rung that image exists to exercise.
    old_ifs=$IFS
    IFS=:
    for dir in $PATH; do
        [ "$with_stat" != yes ] && rm -f "$dir/stat"
        [ "$with_ls" != yes ] && rm -f "$dir/ls"
    done
    IFS=$old_ifs
fi

conf=/etc/ssh/sshd_config.loftail
{
    echo "Port 22"
    echo "HostKey /etc/ssh/ssh_host_ed25519_key"
    echo "PermitRootLogin no"
    echo "PubkeyAuthentication yes"
    echo "AuthorizedKeysFile .ssh/authorized_keys"
    # Key auth by default: most of tst_sshlive installs no prompter, so a server that
    # offered a password would give a wedged run somewhere to hang rather than a clear
    # refusal. LOFTAIL_PASSWORD turns it on for the one server whose run drives the
    # password rung with a scripted prompter — the only execution there is of the branch
    # a person actually meets on a machine they have just been given access to.
    if [ -n "$password" ]; then
        echo "PasswordAuthentication yes"
    else
        echo "PasswordAuthentication no"
    fi
    echo "KbdInteractiveAuthentication no"
    echo "UsePAM no"
    # So that compressionIsNegotiatedWhenTheHostAsksForItAndNotOtherwise() has a server
    # willing to compress; without this it skips, and that case is the only execution of
    # LIBSSH2_FLAG_COMPRESS there is.
    echo "Compression yes"
    echo "LogLevel VERBOSE"
    echo "PidFile /run/sshd.pid"
} >"$conf"

if [ "$sftp_blackhole" = yes ]; then
    # `cat` reads its stdin for ever and writes nothing, so the channel opens and the
    # SFTP version packet never arrives. This is the shape a generic sshd config on a
    # stripped-down embedded image has — a Subsystem line pointing at a binary that is
    # not there or does not work — and the reason connectTo() probes for the exec
    # transport rather than reading libssh2's error code, which is a plain TIMEOUT here
    # after a SUCCESSFUL login.
    echo "Subsystem sftp /bin/cat" >>"$conf"
elif [ "$with_sftp" = yes ]; then
    for candidate in /usr/lib/openssh/sftp-server /usr/lib/ssh/sftp-server \
        /usr/libexec/openssh/sftp-server /usr/libexec/sftp-server; do
        if [ -x "$candidate" ]; then
            echo "Subsystem sftp $candidate" >>"$conf"
            break
        fi
    done
    if ! grep -q '^Subsystem sftp' "$conf"; then
        echo "loftail-sshd: no sftp-server binary in this image" >&2
        exit 1
    fi
fi

mkdir -p /run/sshd /var/empty
exec /usr/sbin/sshd -D -e -f "$conf"
