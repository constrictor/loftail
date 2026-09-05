# Containerised sshd servers for `tst_sshlive`

`tst_sshlive` is the only thing that executes loftail's libssh2 transport at all.
Everything above `RemoteFetcher` is covered without a network by `tst_spooledsource`,
`tst_remotetail` and `tst_remoteopen`; the handshake, host-key verification, the auth
ladder, whether a real `sftp-server`'s FSTAT follows the handle, the exec fallback, the
size ladder, the config write and the restart script are covered here or nowhere.

Until this directory existed there was no server to run it against, so it skipped —
in CI always, and on the dev machine unless somebody had a spare box. `CLAUDE.md` says
of the whole SSH layer that "a green pipeline means nothing about it". This is what
makes that untrue.

## Running it

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target tst_sshlive
packaging/test-ssh/run-ssh-tests.sh --build build
```

Needs Docker (or `DOCKER=podman`) and the OpenSSH **client** — the test sets its
fixtures up by shelling out to `ssh`, and reads the exec transport's own commands back
through it, so a missing client is fifteen failures about a fixture that was never
written.

`--keep` leaves the containers and the scratch home up afterwards, and prints the one
command that reaches a server with the run's own identity.

## Three servers, because the interesting code is in the fallbacks

| Container | Image | Reaches |
| --- | --- | --- |
| `loftail-sshd-sftp` | Ubuntu 24.04, stock sshd | the SFTP transport, the seek elision, config read/write, the restart script, the session cache, the dropped-link latch |
| `loftail-sshd-nosftp` | same image, no `Subsystem sftp` | `Mode::Exec` — the probe, the streaming read, the `cat >` write |
| `loftail-sshd-busybox` | Alpine, sftp but no `stat` | `ExecSizeProbe`'s `ls -lnLd` and `wc -c` rungs, against a busybox userland rather than the author's |

A stock sshd reaches none of the second or third column. The exec transport is entered
only when `libssh2_sftp_init()` fails, and nothing a *client* can do makes it fail
against a server that has a working `sftp-server` — which is why the second server
exists and why `theExecFallbackWritesTheSameBytes()` had a second gate on it.

## Three design decisions that are easy to undo

**The server is stripped at run time, not at build time.** One image is both the SFTP
and the no-SFTP host, because `LOFTAIL_WITH_SFTP` decides whether the entrypoint writes
the `Subsystem` line. Bake it in and the pair drift.

**`$HOME` steers loftail and NOT the ssh client.** loftail finds its keys and
`known_hosts` through `QStandardPaths::HomeLocation`, which reads `$HOME`. OpenSSH takes
the home directory from the passwd database instead, so `HOME=... ssh` reads the real
user's `~/.ssh` whatever the environment says (`ssh -G` prints the resolved paths). There
is no environment variable for ssh's config and the fixture helper inside `tst_sshlive`
builds its own argv, so a shim earlier on `PATH` is what reaches it — it adds `-F`, and
affects fixtures only, loftail's transport never running the ssh binary. Absolute paths
inside that config, never `~`, for the same reason.

**Host keys are generated in the container and read back out with `docker exec`.**
Not committed (no private key in the repository), not scanned off the port
(`ssh-keyscan` races the startup). The harness writes `[127.0.0.1]:PORT` into a scratch
`known_hosts`, which is the form `libssh2_knownhost_checkp()` understands. A bind-mounted
host key would arrive owned by the host's uid, which sshd refuses to use.

**Each run names the functions that must have PASSED.** Every case in `tst_sshlive` is
gated on something about the server, and a `QSKIP` is a zero exit status — so the exit
code alone would go green against three servers that refuse every connection. That check
is the harness's real content; without it a broken fixture is a green job.

## What it still does not cover

The **Windows** build's libssh2 is compiled from source (`-DLOFTAIL_SSH_FETCH=ON`) and
is not exercised by this: the Windows runner cannot host a Linux container. Nothing here
touches the **keychain** either — see `tst_keychainlive`, which needs a session bus.
