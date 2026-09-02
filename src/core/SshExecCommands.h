// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
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

// The rung's name for the diagnostic log, NOT for the user (DiagnosticLog.h) — which is
// why it is untranslated, exactly as the commands in this file are. Which rung a server
// settled on explains most of what is otherwise puzzling about a remote tail (no mtime,
// therefore weaker rotation detection; a clamped poll; a size ceiling), and by the time
// anybody asks, the connect that decided it is long past.
inline const char *sizeSourceName(SizeSource source)
{
    switch (source) {
    case SizeSource::None: return "none";
    case SizeSource::Stat: return "stat";
    case SizeSource::Ls:   return "ls";
    case SizeSource::Wc:   return "wc";
    }
    return "?";
}

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

// Everything from `offset` (0-based) to the end of the file, on stdout, AS ONE STREAM.
//
// THE MISSING `| head -c L` IS THE WHOLE POINT, and putting it back defeats the feature
// rather than merely slowing it down. readCommand() above is one command per window, and
// a window is one fetch chunk (kSshFetchChunkBytes) — so catching up on a 100 MB log
// meant hundreds of channel opens, hundreds of remote `tail` processes and hundreds of
// round-trip sequences, to read a file that one `cat` would have handed over in a single
// pass. `SshFetcher::fetchForward()` reads STRICTLY FORWARD, each offset being the
// previous one plus what the previous call returned, so a single `tail -c +N` started at
// the first offset already contains every byte the whole pass is going to ask for, in
// exactly the order it is going to ask for them. The exec transport streams from one
// channel and reads out of it per call (§6.3.1).
//
// WHAT BOUNDS THE READ, then, since the command no longer does. Two things, and neither
// is on the far end: the caller's own `length` per call, which is what SshSession reads
// out of the channel and hands back, and the tear-down when the caller stops asking —
// closing the channel makes the server close the pipe, `tail` takes an EPIPE on its next
// write and dies. Nothing streams that nobody is reading; what a stream costs while it
// is idle is one blocked remote process per open log, which is the price of not paying a
// process per chunk.
//
// `tail` with no `-f`, deliberately: it stops at the file's EOF as it stood when it got
// there, which is exactly the extent one catch-up pass wants. Following on the far end
// would be a second thing that decides when there are new bytes, competing with the poll
// that already does (§6.3) and answering with no size to clamp against.
QString streamReadCommand(const QString &path, qint64 offset);

// --- The config-file editor's whole-file operations (SPEC.md §4) -------------
//
// A log is read in windows with `tail`/`head` because it is large and grows. A CONFIG
// file is neither: it is read once, whole, into an editor, and written back whole. So
// these three are their own commands rather than a special case of readCommand().

// The whole of `path` on stdout.
//
// A REDIRECT, not `cat 'path'`, for the two reasons wcSizeCommand() gives: a missing
// file leaves stdout empty because the shell's complaint goes to stderr, and a path that
// begins with `-` cannot be mistaken for an option — which `cat --` would also fix, but
// only on the userlands that implement it, and this transport exists for the ones that
// leave things out.
QString configReadCommand(const QString &path);

// Whether `path` is there, as a marked line: "<marker> 1" or "<marker> 0".
//
// Its own round trip rather than inferring absence from an empty read, because an EMPTY
// FILE THAT EXISTS and a file that does not are the same empty stdout — and telling them
// apart is the whole of whether the editor says "new file" and whether saving is
// creating something. The marker is there for lastNonEmptyLine()'s reason: on these
// machines stdout is not private, and a login banner would otherwise be the answer.
QString configExistsCommand(const QString &path);

// Take stdin and put it in `path`, creating it if it is not there.
//
// IN PLACE, and that is the whole reason this is a redirect and not a temp-and-move. A
// shell `>` on an existing file TRUNCATES IT — it does not unlink and recreate — so the
// inode survives and with it the file's owner, its group and its mode. A config that was
// `0640 root:adm` is still `0640 root:adm` afterwards, which a rename could not promise:
// that would leave a file owned by whoever loftail connected as, on the file that
// decides what an application logs, silently.
//
// What it costs is atomicity: a write that dies halfway leaves a short file where a
// rename would have left the old one intact. That is stated in SPEC.md §4 rather than
// left to be discovered, and the caller verifies the size afterwards.
QString configWriteCommand(const QString &path);

// The user's restart script, preceded by the variables it is given (SPEC.md §4).
//
// TWO HALVES WITH OPPOSITE RULES, and confusing them is the whole risk in this function.
// The VALUES are data: every one goes through shellQuote(), because a log path arrives
// from a URL somebody typed, was handed or restored from a session — exactly
// readCommand()'s provenance, and exactly its consequence, since without the quoting
// `'; rm -rf ~; '` is not a strange filename but remote code execution. The SCRIPT is
// code: it is deliberately NOT quoted, because it is what the user wrote for this to run
// and quoting it would execute a string instead of a script. Its trust comes from where
// it lives — the user's own settings tree — and from nowhere else.
//
// The variable NAMES are constants of loftail's (RestartTarget.h) and never anything
// typed, which is what makes writing them bare on the left of an `=` safe. A variable
// that does not apply is ABSENT from the list and therefore never assigned, not assigned
// empty, so `${ARCHIVE-}` tells an archived log from a plain one — the same promise the
// local QProcessEnvironment makes, and the two are pinned against each other.
//
// STDIN IS CLOSED for the whole script, and CRLF is normalised to LF. See the .cpp.
QString restartScriptCommand(const QString &script,
                             const QList<QPair<QString, QString>> &variables);

// Read what configExistsCommand() printed. False for anything unparseable, which is what
// a shell error, a restricted account and a missing marker all look like from here.
bool parseConfigExistsOutput(const QByteArray &output, bool *exists);

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

// ---------------------------------------------------------------------------
// Has the remote log been replaced, and what will it cost to find out?
// ---------------------------------------------------------------------------

// What one poll's stat justifies doing about it.
//
// The three non-trivial answers exist because settling the question is not free: the only
// thing that can distinguish a REWRITE from an APPEND is the content, and reading content
// over the network to answer a question nobody asked is exactly what invariant #5 forbids
// doing to somebody else's machine.
enum class RotationVerdict {
    Nothing,      // an append, or no change at all. The overwhelmingly common answer.
    Rotated,      // certain from the stat alone: it shrank, or the name left our handle.
    CompareNow,   // it changed in a way NO append can produce. Read the head immediately.
    ComparePaced, // it might be either. Read the head only when the clock allows.
};

// One poll's worth of what the server said, plus what the previous poll saw.
struct RemoteObservation
{
    qint64 size = 0;                 // stat-by-name, this poll
    qint64 mtime = kUnknownMtime;    // stat-by-name, this poll; unknown on the ls/wc rungs
    qint64 lastSize = 0;             // stat-by-name, previous poll
    qint64 lastMtime = kUnknownMtime;
    qint64 consumed = 0;             // baseOffset + committedSize: what we have already read

    // Whether this server's FSTAT genuinely follows the handle we opened. Where it does,
    // the handle is an inode substitute and a handle/name disagreement is proof of a
    // rename. Where it does not — and on the whole exec transport, which has no handle at
    // all — that evidence is simply unavailable.
    bool   fstatTracksHandle = false;
    bool   handleValid = false;
    qint64 handleSize = 0;
};

// The decision, as a pure function. IT IS HERE, ungated and always compiled, for exactly
// the reason shellQuote() and the size ladder are: this is the only judgement the remote
// transport makes on its own, and a rule compiled in one configuration is a rule tested in
// one configuration. Nothing in it needs libssh2, a socket or a server.
//
// THE ORDER OF THE TESTS IS THE CONTRACT, and two of the steps are easy to undo:
//
//   * kUnknownMtime is -1, and -1 > -1 is false — so the no-mtime rung must be decided
//     BEFORE any comparison against lastMtime, or rotation detection silently switches
//     itself off on precisely the stripped-down servers that have no `stat` to begin with.
//
//   * A file that GREW is not evidence of anything. It is what an ordinary append looks
//     like, and it is also what `cp bigger.log app.log` looks like — no stat can separate
//     them, on any rung, including the SFTP one where FSTAT works perfectly (the handle
//     and the name are still the same file after a rewrite in place; only the content
//     moved). So growth yields ComparePaced everywhere rather than Nothing. Downgrade it
//     to Nothing and a rewritten log keeps its pre-rewrite records on screen for ever,
//     with the new bytes parsed from the middle of a record.
RotationVerdict rotationVerdict(const RemoteObservation &o);

} // namespace loftail
