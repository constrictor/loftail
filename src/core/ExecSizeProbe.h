#pragma once

#include "SshExecCommands.h"

#include <QByteArray>
#include <QString>

#include <functional>

namespace loftail {

// Deciding HOW to measure a remote file on a server that will not do SFTP, and proving
// the decision before relying on it (ARCHITECTURE.md §6.3.1).
//
// ALWAYS COMPILED, for the same reason SshExecCommands is: this is the only part of the
// exec transport with a decision in it, and the transport that uses it is reachable in
// exactly one build configuration on exactly one kind of server. The two seams below let
// tst_execsizeprobe drive the whole ladder through a real /bin/sh against real files,
// with no libssh2 linked and no network — which is the only automated coverage the exec
// path has ever had.
//
// Everything here is one-thread-at-a-time, like the SshSession it is built from.
class ExecSizeProbe
{
public:
    // Run a command and collect its stdout. TWO-WAY on purpose, exactly like
    // SshSession::Impl::runCommand: "the command ran and printed nothing" and "no
    // channel could be opened at all" want opposite handling upstream — the first is a
    // file that is not there (wait for it), the second is a link that died (reconnect).
    // Collapsing them makes a dropped connection look like a missing log forever.
    using RunCommand = std::function<bool(const QString &command, QByteArray *out)>;

    // Bytes actually delivered for a read of `length` at `offset`, or -1 on error. The
    // bytes themselves are not wanted here, only how many arrived.
    using ReadAt = std::function<qint64(qint64 offset, qint64 length)>;

    ExecSizeProbe(QString path, ExecTools tools, RunCommand run, ReadAt read);

    // The `wc` rung reads the WHOLE file to answer, so it refuses to take on a file
    // bigger than this. It exists as an exactness backstop for an `ls` output shape
    // nobody anticipated, not as a way to poll a large log once a second.
    static constexpr qint64 kWcSettleCeiling = 8LL * 1024 * 1024;

    // Pick the cheapest rung this server can actually answer with, and fill `first` with
    // the attributes that settled it so the caller does not pay for a second round trip.
    // SizeSource::None when nothing answered.
    SizeSource settle(ExecAttrs *first = nullptr);

    // Ask one rung. No validation — that happens once, in settle(); repeating it would
    // double the cost of every poll for a question already answered.
    ExecAttrs query(SizeSource source);

    // After a settle() that returned None: nothing ran at all, so this is a dead channel
    // rather than a missing file. The distinction is the whole reason RunCommand is
    // two-way.
    bool channelDied() const { return m_channelDied; }

private:
    bool eligible(SizeSource source) const;

    // Whether a size of `size` is consistent with what the read path actually delivers.
    //
    // ONLY OVER-REPORTING IS DETECTABLE, and only over-reporting is harmful. The log
    // grows between the query and the read, so a read at `size - 1` may legitimately
    // return more than one byte, and a size that came back too SMALL is indistinguishable
    // from a file that grew — which costs one poll of latency and nothing else. A size
    // past the end returns nothing, and that is the one thing worth rejecting.
    //
    // It earns its round trip twice over: running readCommand() end to end is also the
    // only proof that `head -c` works on this box. `head -c` is NOT POSIX — POSIX head
    // has only -n — so `command -v head` succeeding says nothing about it, and a head
    // without -c prints a usage error to stderr and no bytes at all.
    bool provesReadPath(qint64 size);

    QString    m_path;
    ExecTools  m_tools;
    RunCommand m_run;
    ReadAt     m_read;
    bool       m_channelDied = false;
};

} // namespace loftail
