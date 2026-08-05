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

// Size and mtime of `path`, one line, as "<bytes> <epoch-seconds>".
//
// GNU and BSD `stat` take incompatible flags and neither accepts the other's, so the
// command tries both and keeps whichever answers — one round trip instead of a probe
// plus a call, and no state to remember about which kind of server this is.
QString statCommand(const QString &path);

// `length` bytes of `path` starting at `offset` (0-based), on stdout.
//
// `tail -c +N` counts from ONE, not zero, which is the off-by-one this function exists
// to get right in a single place. `head -c` bounds the transfer so a poll near the start
// of a large log does not stream the whole thing.
QString readCommand(const QString &path, qint64 offset, qint64 length);

// Whether this server can run what the exec transport needs at all. Printed marker
// rather than exit status alone: a restricted shell can exit 0 while running nothing.
QString probeCommand();
QByteArray probeMarker();

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
    qint64 mtime = 0;
};
ExecAttrs parseStatOutput(const QByteArray &output);

} // namespace loftail
