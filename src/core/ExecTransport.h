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

#include "ExecSizeProbe.h"
#include "PathTrouble.h"
#include "SshExecCommands.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <functional>

namespace loftail {

// Reading a log through ORDINARY SHELL COMMANDS, and every judgement that takes
// (ARCHITECTURE.md §6.3.1, §6.11).
//
// This is what `tail -c +N | head -c L`, the `stat`/`ls`/`wc` size ladder and
// `PathTrouble`'s `test` ladder add up to once they are lifted off whatever carries them.
// TWO TRANSPORTS CARRY THEM: libssh2's exec channel, for a server that signs you in and
// will not do SFTP (M16), and a device's serial console, where there is no other kind of
// channel at all (M27).
//
// EXTRACTED RATHER THAN COPIED, which is the tree's standing rule about a decision: a
// second hand-written copy of this ladder is a second chance to get the three-way answer
// to `SizeSource::None` wrong, and getting it wrong once already spent a tab for ever on
// "it is missing, or the account cannot read it" about a file that was present, readable
// and growing (M16). The M23 precedent is `logAnchorOf()` and `SshWorkerPool`, with the
// existing suite passing UNALTERED as the evidence the extraction was inert.
//
// WHAT IS HERE AND WHAT IS NOT, because the cut is the design. Here: the ladder, the
// three-way None discrimination, the sentences those produce, and the rung query the poll
// makes. Not here: how a command is actually run, how bytes actually arrive, and how a
// long read is kept open across calls — those are channel MECHANICS, they differ
// completely between a libssh2 channel and a framed tty, and neither transport learns
// anything by sharing them. `SshSession` keeps `runCommand()`, `execRead()`'s streaming
// reuse and `runScriptStreaming()`; the serial session writes its own and shares none of
// it.
class ExecTransport
{
public:
    // Run `command` on the far end, collecting its standard output.
    //
    // TWO-WAY, AND THE TWO MUST NOT BE FOLDED. False means the command could not be run at
    // all, which is a fact about the LINK; true with empty output means it ran and printed
    // nothing, which is a fact about the FILE. Collapsing them is what makes a dropped link
    // read as a missing log for ever — `ExecSizeProbe`'s own contract, restated because
    // this is now the interface that carries it.
    using RunCommand = std::function<bool(const QString &command, QByteArray *stdOut)>;

    // Bytes of the open file at `offset`. HOW is the channel's business: libssh2 keeps one
    // `tail -c +N` streaming and serves out of it while the offsets line up, a tty runs a
    // bounded `readCommand()` per chunk because it has one channel and cannot do both.
    using ReadForward = std::function<qint64(qint64 offset, qint64 length)>;

    // `path` is the log on the far end; `peerName` is what the sentences call the far end
    // — a host name, or a device.
    ExecTransport(QString path, QString peerName, RunCommand run, ReadForward read);

    // What the far end can run, from one command (`probeCommand()`). Settled once per
    // connect, because it is a fact about the machine rather than about the file.
    bool probe(ExecTools *tools);
    void setTools(const ExecTools &tools) { m_tools = tools; }
    ExecTools tools() const { return m_tools; }

    // Why an open did not work, in the vocabulary the caller already reports.
    enum class Trouble {
        None,
        LinkGone,       // the channel died: fixed by reconnecting
        NoSuchFile,     // absent, or under a folder that is absent, or unreadable
        Refused,        // a folder, or too big to measure at any rate worth polling at
    };

    // Settle a way to MEASURE this file and confirm it answers, which is what "opening"
    // means where every read runs its own command.
    //
    // THE LADDER RUNS FIRST AND ITS SUCCESS IS THE EXISTENCE PROOF, rather than the other
    // way round: there is nothing to dispatch a stat on until a rung is settled, and "no
    // rung answered" and "the file is not there" are the same observation from out here.
    //
    // SETTLED ON EVERY CALL, which means on every rotation too. Settling once per session
    // would let one rotated-to file the chosen rung cannot parse make statPath() invalid
    // for good, and the log would then be reported as waiting on every poll —
    // indistinguishable from one that was deleted.
    bool openFile(Trouble *trouble, QString *error);
    void closeFile() { m_fileOpen = false; }
    bool hasFile() const { return m_fileOpen; }
    SizeSource sizeSource() const { return m_size; }

    // Whichever rung openFile() settled on. No re-validation: that question was answered
    // once, and asking it again would double the cost of every poll.
    ExecAttrs statPath() const;

    // WHICH of the five is at the path, asked of the far end rather than guessed at. One
    // `test` command, on the failure path only. `known` stays false for a channel that
    // would not answer, which is what makes the sentence quote the far end rather than
    // invent one of the five.
    RemotePathReport classifyPath() const;

    // The sentence for a Trouble, as the caller reports it. Here rather than at the two
    // call sites because four surfaces used to word this for themselves, which is exactly
    // how they came to disagree (§6.5).
    QString troubleText(const RemotePathReport &report) const;

private:
    // THE PROBE BINDS THIS AND NEVER A PUBLIC READ, and after the extraction that is a
    // fact about scope rather than a comment somebody has to obey. `ExecSizeProbe` settles
    // BEFORE `m_fileOpen` is set, so a seam routed through anything that checks that flag
    // would read zero bytes, reject every rung and disable the fallback entirely —
    // silently, and only on the machines it exists for.
    ExecSizeProbe sizeProbe() const;

    QString     m_path;
    QString     m_peerName;
    RunCommand  m_run;
    ReadForward m_read;

    ExecTools  m_tools;
    SizeSource m_size = SizeSource::None;
    bool       m_fileOpen = false;
};

} // namespace loftail
