#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace loftail {

// The shell commands loftail runs on a server that will not do SFTP, and the parsing of
// what they print back (ARCHITECTURE.md §6.3.1).
//
// PURE STRING WORK — no libssh2, no sockets — and therefore ALWAYS COMPILED, like
// RemoteLocation and ArchiveLocation, while the transport that uses it is gated. That is
// not tidiness: the quoting below is a security boundary, and a security boundary that
// only exists in one build configuration is one that only gets tested in one build
// configuration.

// `path` as a single POSIX shell word, safe to paste into a command line.
//
// THIS IS THE SECURITY BOUNDARY OF THE WHOLE EXEC TRANSPORT. A remote path arrives from
// a URL the user typed, was handed, or restored from a session, and it ends up inside a
// command that a shell on someone else's machine will interpret. Without this, a path
// containing `'; rm -rf ~; '` is not a strange filename, it is remote code execution.
//
// Single quotes, because inside them a POSIX shell expands NOTHING — no $, no backtick,
// no glob, no escape. The single quote itself is the only character that cannot appear,
// and it is handled the standard way: end the quoted run, emit an escaped quote, start a
// new one ('\'').
QString shellQuote(const QString &path);

// No epoch mtime is available from this size source.
//
// Only `stat` prints one. `ls` prints a localized human date whose resolution drops to a
// year once the file is old enough, and `wc` prints nothing but a count. SshFetcher reads
// this value and switches to the stalled-size rotation rule (§6.3.1) — and it has to be
// asked for by NAME rather than compared with `>`, because -1 > -1 is false and a session
// with no mtime would otherwise stop detecting rotation entirely, in silence.
constexpr qint64 kUnknownMtime = -1;

// How the exec transport measures the remote file. Probed at connect and settled at
// open, cheapest first (§6.3.1); `Stat` is the only rung that also yields an mtime.
//
// `None` is not a rung, it is "nothing has been settled" — which is also what a
// Mode::Sftp session reports, since none of this applies there.
enum class SizeSource {
    None,
    Stat, // stat -c '%s %Y' / stat -f '%z %m'  — O(1), size and mtime
    Ls,   // ls -lnLd                           — O(1), size only
    Wc,   // wc -c <                            — exact, and O(FILE SIZE) every time
};

// What the probe found on the far end.
//
// `ok` is about the READ path alone — `tail` and `head`, without which there is nothing
// to fall back to. Measuring is a separate question with three possible answers, and a
// server that can read but not measure gets its own message rather than the generic one:
// it names something the user can go and install.
struct ExecTools
{
    bool ok = false;
    bool hasStat = false;
    bool hasLs = false;
    bool hasWc = false;

    bool anySizeTool() const { return hasStat || hasLs || hasWc; }
};

// Size and mtime of `path`, one line, as "<bytes> <epoch-seconds>".
//
// GNU and BSD `stat` take incompatible flags and neither accepts the other's, so the
// command tries both and keeps whichever answers — one round trip instead of a probe
// plus a call, and no state to remember about which kind of server this is.
QString statCommand(const QString &path);

// Size of `path` from `ls`, which every POSIX userland has even where `stat` is absent.
//
// Every flag and every variable earns its place, and dropping one is a silent bug:
//   -d  a directory operand otherwise lists its CONTENTS, and lastNonEmptyLine() would
//       then take a member for the answer.
//   -L  dereference, so a log reached through a symlink reports the target's size and
//       type. POSIX is explicit that -L beats -d when both are given.
//   -n  numeric user and group, so a user NAME containing a space cannot shift columns.
//   BLOCK_SIZE/LS_BLOCK_SIZE  a remote /etc/profile that sets either makes GNU ls print
//       "1.0M" where a byte count is expected. Unknown variables are ignored elsewhere.
//   LC_ALL=C, QUOTING_STYLE=literal  keep the rest of the line in the shape parsed below.
QString lsSizeCommand(const QString &path);

// Size of `path` by reading all of it. Exact, and the last rung for that reason alone —
// see ExecSizeProbe for the ceiling that keeps this from re-reading a large log forever.
//
// The redirect is deliberate: `wc -c FILE` prints the filename alongside the count, and
// `wc -c < FILE` prints the count and nothing else. It also means a missing file leaves
// stdout empty (the shell's complaint goes to stderr), which is how this rung says no.
QString wcSizeCommand(const QString &path);

// `length` bytes of `path` starting at `offset` (0-based), on stdout.
//
// `tail -c +N` counts from ONE, not zero, which is the off-by-one this function exists
// to get right in a single place. `head -c` bounds the transfer so a poll near the start
// of a large log does not stream the whole thing.
QString readCommand(const QString &path, qint64 offset, qint64 length);

// What this server can run, on one line: the marker, then the size tools it has.
//
// Printed marker rather than exit status alone, because a restricted shell can exit 0
// while running nothing. ONE line, because every answer the exec transport reads is taken
// from the last non-empty one and a multi-line reply would defeat that; and the marker
// FIRST, so the parse has a fixed head to anchor on whatever follows it.
QString probeCommand();
QByteArray probeMarker();
ExecTools parseProbeOutput(const QByteArray &output);

// The last non-empty line of `output`, trimmed; empty if there is none.
//
// Every answer the exec transport reads goes through this, because on the machines this
// transport exists for stdout is not private: a login banner, a `/etc/profile` notice or
// a warning from a shell that echoes what it was given all arrive BEFORE the answer. The
// answer is what the command printed last.
QByteArray lastNonEmptyLine(const QByteArray &output);

// Parse what statCommand() printed. `ok` is false for empty, malformed or negative
// output — which is what a missing file, a permission denial and a shell error message
// all look like from here, and all of which must read as "no attributes" rather than as
// a plausible size.
struct ExecAttrs
{
    bool   ok = false;
    qint64 size = 0;
    // Defaults to unknown rather than to the epoch, because two of the three rungs below
    // never fill it in and a plausible-looking 0 would read as "1970" to a caller
    // comparing mtimes.
    qint64 mtime = kUnknownMtime;
};
ExecAttrs parseStatOutput(const QByteArray &output);

// Parse what lsSizeCommand() printed. Defensive for the same reason parseStatOutput() is,
// with one extra job: `ls` prints a whole line ABOUT the file, so the shape of that line
// is the evidence. A leading `-` is required — after -L that means a regular file, and it
// rejects a directory (`d`), a dangling symlink (`l`), a FIFO (`p`) and a device (`c`/`b`)
// whose size column is not a byte count at all but a major/minor pair.
ExecAttrs parseLsSizeOutput(const QByteArray &output);

// Parse what wcSizeCommand() printed: exactly one non-negative integer, or nothing.
ExecAttrs parseWcSizeOutput(const QByteArray &output);

} // namespace loftail
