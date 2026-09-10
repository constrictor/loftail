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

#include "SshExecCommands.h"

#include "PathTrouble.h"

#include <QList>

#include <ranges>

namespace loftail {

QString shellQuote(const QString &path)
{
    QString out;
    out.reserve(path.size() + 2);
    out += QLatin1Char('\'');
    for (const QChar c : path) {
        if (c == QLatin1Char('\'')) {
            // Close the quoted run, emit a backslash-escaped quote outside it, reopen.
            // The only way to get a literal ' into a single-quoted POSIX word.
            out += QLatin1String("'\\''");
        } else {
            out += c;
        }
    }
    out += QLatin1Char('\'');
    return out;
}

QString statCommand(const QString &path)
{
    const QString quoted = shellQuote(path);
    // GNU coreutils first because it is what Linux servers have; BSD/macOS second. The
    // redirect keeps a failed first attempt from mixing its complaint into the output
    // that the second one's answer has to be parsed out of.
    return QStringLiteral("stat -c '%s %Y' %1 2>/dev/null || stat -f '%z %m' %1")
        .arg(quoted);
}

QString lsSizeCommand(const QString &path)
{
    // No 2>/dev/null: the caller drains and discards stderr already, and statCommand()'s
    // redirect exists only to keep its first attempt's complaint out of its second's
    // answer, which this command has no equivalent of.
    return QStringLiteral("LC_ALL=C BLOCK_SIZE=1 LS_BLOCK_SIZE=1 QUOTING_STYLE=literal "
                          "ls -lnLd %1")
        .arg(shellQuote(path));
}

QString wcSizeCommand(const QString &path)
{
    return QStringLiteral("wc -c < %1").arg(shellQuote(path));
}

QString readCommand(const QString &path, qint64 offset, qint64 length)
{
    // +1 because `tail -c +N` is 1-BASED: +1 is the whole file, not "skip one byte".
    const qint64 from = qMax<qint64>(0, offset) + 1;
    // ONE arg() call with three arguments, and the numbers converted by hand. Chaining
    // arg() is wrong here and was: the second call rescans the WHOLE string, including
    // the path the first one just substituted in, so a path containing a literal %1 —
    // legal, and what a URL-decoded %251 arrives as — had that token replaced by the
    // length. And the tempting `.arg(from, quoted, length)` with the numbers left as
    // integers binds to arg(qlonglong, int fieldWidth, int base) instead, which means
    // something else entirely.
    return QStringLiteral("tail -c +%1 %2 | head -c %3")
        .arg(QString::number(from), shellQuote(path),
             QString::number(qMax<qint64>(0, length)));
}

QString streamReadCommand(const QString &path, qint64 offset)
{
    // The same 1-based `+N` as readCommand(), and deliberately the same expression rather
    // than a second derivation of it: the two commands have to agree about which byte
    // `offset` names, because the streaming one is what serves a sequential pass and the
    // bounded one is still what ExecSizeProbe proves a rung with, and a one-byte
    // disagreement between them would show up as a spool shifted by one byte rather than
    // as anything that fails.
    const qint64 from = qMax<qint64>(0, offset) + 1;
    // No `| head -c`, and see the header for why at length. In one line: the caller reads
    // as much as it wants out of the channel and closes it when it is done, so the bound
    // moved from the far end to this end.
    //
    // ONE arg() call with two arguments, for readCommand()'s reason — chaining rescans
    // the string it has already substituted the path into, so a path containing a literal
    // %1 has that token replaced by whatever the next call passes.
    return QStringLiteral("tail -c +%1 %2")
        .arg(QString::number(from), shellQuote(path));
}

namespace {
// The marker configExistsCommand() prints. Its own, not probeMarker()'s, so a stray
// probe reply arriving late on a reused channel cannot be read as an answer about a file.
QByteArray configMarker()
{
    return QByteArrayLiteral("loftail-cfg");
}
} // namespace

QString configReadCommand(const QString &path)
{
    return QStringLiteral("cat < %1").arg(shellQuote(path));
}

QString configExistsCommand(const QString &path)
{
    // `-e`, not `-f`: a config reached through a symlink is an ordinary arrangement, and
    // `-e` follows it. A directory answers 1 here and the write then fails with the
    // server's own words, which is a better message than anything invented here.
    return QStringLiteral("if test -e %1; then echo %2 1; else echo %2 0; fi")
        .arg(shellQuote(path), QString::fromLatin1(configMarker()));
}

QString configWriteCommand(const QString &path)
{
    return QStringLiteral("cat > %1").arg(shellQuote(path));
}

namespace {
// pathTroubleCommand()'s own marker, configMarker()'s rule: two questions about a path
// must not be able to answer each other.
QByteArray troubleMarker()
{
    return QByteArrayLiteral("loftail-why");
}
} // namespace

QString pathTroubleCommand(const QString &path)
{
    const QString p = shellQuote(path);
    // remoteParentFolderOf(), shared with the sentence that reports the answer and with
    // the SFTP transport's own stat of the parent (PathTrouble.h): three places have to
    // agree about which folder is being asked about, or loftail says one folder is
    // missing having looked at another. An empty answer means a relative path, whose
    // folder is wherever the far end's shell starts — `.` is the true thing to test.
    const QString folder = remoteParentFolderOf(path);
    const QString parent = shellQuote(folder.isEmpty() ? QStringLiteral(".") : folder);
    const QString m = QString::fromLatin1(troubleMarker());
    // ORDER IS THE WHOLE ANSWER, and every rung is reached only because the ones above it
    // said no:
    //   -d P   a folder AT the path. First, because a folder is readable and would
    //          otherwise answer `ok` on the next line and be reported as a file.
    //   -r P   there and readable — which means the trouble was somewhere else.
    //   -e P   there, and not readable: the account, not the file.
    //   -d D   not there, but the folder is, so it is the FILE that has not appeared.
    //   else   the folder is not there either, which is what a mistyped directory looks
    //          like and the case a plain "it is missing" sends the reader to the wrong
    //          place about.
    // `test`, not `[`, and no `-a`/`-o`: this transport exists for the userlands that
    // leave things out, and these five are the ones POSIX guarantees.
    return QStringLiteral("if test -d %1; then echo %3 notafile; "
                          "elif test -r %1; then echo %3 ok; "
                          "elif test -e %1; then echo %3 denied; "
                          "elif test -d %2; then echo %3 missing; "
                          "else echo %3 nodir; fi")
        .arg(p, parent, m);
}

QString restartScriptCommand(const QString &script,
                             const QList<QPair<QString, QString>> &variables)
{
    QString out;
    for (const auto &v : variables) {
        // The name is written bare and the value is quoted. See the header: the names are
        // loftail's own constants and the values are paths from an address the user typed.
        //
        // `NAME=value` and `export NAME` as two statements rather than `export NAME=value`,
        // which several of the older /bin/sh implementations this transport exists for do
        // not take; and never a `NAME=value cmd` prefix, which binds to one command where
        // a script is many.
        out += v.first + QLatin1Char('=') + shellQuote(v.second) + QLatin1Char('\n')
            + QLatin1String("export ") + v.first + QLatin1Char('\n');
    }

    // CRLF NORMALISED TO LF. A script typed on Windows, or hand-edited into
    // logsettings.json there, reaches a POSIX shell as `systemctl\r` — "command not
    // found", about a command that is plainly right on screen.
    QString body = script;
    body.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    body.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    // A BRACE GROUP WITH STDIN CLOSED. An exec channel's stdin is the channel itself, so
    // a script that runs `read` — or a `ssh`, or an `apt` that decides to ask something —
    // would otherwise wait forever on a link nobody is writing to. The group is what makes
    // the redirect cover the whole script rather than its first command, and a group
    // rather than a subshell so the exit status is still the script's last command's.
    //
    // The newlines matter as much as the braces: a script whose first line is `#!/bin/sh`
    // or any other comment would swallow the rest of its line, and one opening with a
    // here-document has to begin at a line start.
    return out + QLatin1String("{\n") + body + QLatin1String("\n} < /dev/null\n");
}

bool parseConfigExistsOutput(const QByteArray &output, bool *exists)
{
    const QByteArray line = lastNonEmptyLine(output);
    const QByteArray head = configMarker() + ' ';
    if (!line.startsWith(head))
        return false;
    const QByteArray tail = line.mid(head.size()).trimmed();
    if (tail != "0" && tail != "1")
        return false;
    if (exists)
        *exists = tail == "1";
    return true;
}

bool parsePathTroubleOutput(const QByteArray &output, LogPresence *presence)
{
    const QByteArray line = lastNonEmptyLine(output);
    const QByteArray head = troubleMarker() + ' ';
    if (!line.startsWith(head))
        return false;
    const QByteArray word = line.mid(head.size()).trimmed();
    LogPresence answer = LogPresence::Present;
    if (word == "ok")
        answer = LogPresence::Present;
    else if (word == "missing")
        answer = LogPresence::Absent;
    else if (word == "nodir")
        answer = LogPresence::NoDirectory;
    else if (word == "denied")
        answer = LogPresence::Unreadable;
    else if (word == "notafile")
        answer = LogPresence::NotAFile;
    else
        return false; // a word nobody wrote: not an answer, and must not become one
    if (presence)
        *presence = answer;
    return true;
}

QByteArray probeMarker()
{
    return QByteArrayLiteral("loftail-exec-ok");
}

QString probeCommand()
{
    // Asks the two questions the transport has, and keeps them apart.
    //
    // READING needs `tail` AND `head`, and both are checked because `readCommand()` uses
    // both: a box with `tail` and no `head` used to pass this probe, open the file, and
    // then return zero bytes from every read — which is indistinguishable from EOF, so
    // the log opened empty and never said why.
    //
    // MEASURING is a separate question with three acceptable answers, so the probe
    // reports which of them exist rather than insisting on one. It used to insist on
    // `stat`, which is exactly the utility a stripped-down embedded image leaves out.
    //
    // All on one line, and the marker first, so lastNonEmptyLine() still finds the answer
    // under a login banner and the parse has a fixed head to anchor on.
    return QStringLiteral(
               "command -v tail >/dev/null 2>&1 && command -v head >/dev/null 2>&1 "
               "&& { r=%1; for t in stat ls wc; do command -v \"$t\" >/dev/null 2>&1 "
               "&& r=\"$r $t\"; done; echo \"$r\"; }")
        .arg(QString::fromLatin1(probeMarker()));
}

ExecTools parseProbeOutput(const QByteArray &output)
{
    ExecTools out;
    const QList<QByteArray> fields = lastNonEmptyLine(output).simplified().split(' ');
    if (fields.isEmpty() || fields.constFirst() != probeMarker())
        return out;

    out.ok = true;
    for (qsizetype i = 1; i < fields.size(); ++i) {
        const QByteArray &tool = fields.at(i);
        // Unknown names are ignored rather than rejected: a later loftail that probes for
        // a fourth utility must not be defeated by an older one's answer, or the reverse.
        if (tool == "stat")
            out.hasStat = true;
        else if (tool == "ls")
            out.hasLs = true;
        else if (tool == "wc")
            out.hasWc = true;
    }
    return out;
}

QByteArray lastNonEmptyLine(const QByteArray &output)
{
    const QList<QByteArray> lines = output.split('\n');
    for (const auto &line : std::ranges::reverse_view(lines)) {
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            return trimmed;
    }
    return {};
}

ExecAttrs parseStatOutput(const QByteArray &output)
{
    ExecAttrs out;
    // Both stat flavours print one line, but a server may prepend a banner or a warning
    // on stdout. Take the LAST non-empty line: the answer is what the command printed
    // last, and anything before it is somebody else's noise.
    const QByteArray line = lastNonEmptyLine(output);
    if (line.isEmpty())
        return out;

    const QList<QByteArray> fields = line.simplified().split(' ');
    if (fields.size() != 2)
        return out;

    bool sizeOk = false;
    bool mtimeOk = false;
    const qint64 size = fields.at(0).toLongLong(&sizeOk);
    const qint64 mtime = fields.at(1).toLongLong(&mtimeOk);
    if (!sizeOk || !mtimeOk || size < 0 || mtime < 0)
        return out;

    out.ok = true;
    out.size = size;
    out.mtime = mtime;
    return out;
}

ExecAttrs parseLsSizeOutput(const QByteArray &output)
{
    ExecAttrs out;
    const QList<QByteArray> fields = lastNonEmptyLine(output).simplified().split(' ');
    // mode links uid gid size month day time-or-year name — nine on GNU, busybox, BSD and
    // toybox alike, once LC_ALL=C has pinned the date to its three fields. A floor rather
    // than a count, because a name containing spaces adds more. A line with fewer than
    // nine is not a short answer, it is a different answer: the columns are not where
    // this parser is about to look for them.
    if (fields.size() < 9)
        return out;

    // A regular file, AFTER -L has dereferenced. This one character is what rejects a
    // directory, a dangling symlink, a FIFO and a device — and a device is the case that
    // matters most, because its size column is "1, 3" and field 4 alone would parse the
    // major number as a byte count.
    if (!fields.constFirst().startsWith('-'))
        return out;

    // Link count, uid and gid, all numeric because of -n. Cheap, and it is the only
    // evidence available that the columns are where they are expected to be.
    for (int i = 1; i <= 3; ++i) {
        bool numeric = false;
        const qint64 value = fields.at(i).toLongLong(&numeric);
        if (!numeric || value < 0)
            return out;
    }

    bool sizeOk = false;
    const qint64 size = fields.at(4).toLongLong(&sizeOk);
    if (!sizeOk || size < 0)
        return out;

    out.ok = true;
    out.size = size;
    out.mtime = kUnknownMtime; // ls prints a human date, and an old file loses the clock
    return out;
}

ExecAttrs parseWcSizeOutput(const QByteArray &output)
{
    ExecAttrs out;
    const QList<QByteArray> fields = lastNonEmptyLine(output).simplified().split(' ');
    // Exactly one field. `wc -c < FILE` prints the count alone; anything else came from
    // something other than the command we asked for.
    if (fields.size() != 1)
        return out;

    bool sizeOk = false;
    const qint64 size = fields.constFirst().toLongLong(&sizeOk);
    if (!sizeOk || size < 0)
        return out;

    out.ok = true;
    out.size = size;
    out.mtime = kUnknownMtime;
    return out;
}

RotationVerdict rotationVerdict(const RemoteObservation &o)
{
    // Shrank below what we have already read. Nothing an append does can produce this, and
    // no content check is needed to be sure of it: truncation, or a rotate to a shorter
    // file. It is tested first because it is the one answer that is free.
    if (o.size < o.consumed)
        return RotationVerdict::Rotated;

    if (o.fstatTracksHandle) {
        // The inode substitute: our handle still refers to the file we opened, while stat
        // re-resolves the name. A disagreement means the name now points somewhere else —
        // including the same-size rotate a size check misses entirely.
        if (o.handleValid && o.handleSize != o.size)
            return RotationVerdict::Rotated;
        // It agrees. That rules out a RENAME and says nothing whatever about a rewrite in
        // place, where handle and name are still the same file. So growth still has to be
        // settled by content, on this rung exactly as on the weaker ones.
        return o.size > o.lastSize ? RotationVerdict::ComparePaced : RotationVerdict::Nothing;
    }

    // MUST COME BEFORE THE mtime COMPARISON BELOW: kUnknownMtime is -1 and -1 > -1 is
    // false, so a fall-through would leave this rung deciding Nothing forever.
    if (o.mtime == kUnknownMtime) {
        // No mtime at all, so "it changed without growing" cannot even be asked. Every
        // outcome here — stalled, grown, or the odd shrink that stays above what we read —
        // is equally uninformative, so all of them are paced and share one budget. A
        // stalled size is also exactly what an idle log looks like, and an idle log must
        // not cost a read per poll for ever.
        return RotationVerdict::ComparePaced;
    }

    // It changed WITHOUT growing. No append does that, so it is worth a read at once.
    if (o.mtime > o.lastMtime && o.size == o.lastSize)
        return RotationVerdict::CompareNow;

    // Grew, on a server whose handle cannot be trusted. Append or rewrite; only the
    // content knows, and it is not worth a read per poll to find out.
    if (o.size > o.lastSize)
        return RotationVerdict::ComparePaced;

    return RotationVerdict::Nothing;
}

} // namespace loftail
