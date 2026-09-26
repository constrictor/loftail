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

#include "ExecTransport.h"

#include <QCoreApplication>

#include <utility>

namespace loftail {
namespace {

// Nothing in core is a QObject, so the sentences go through a shim named for the type
// rather than a hand-rolled tr(), which lupdate would not understand (§9.1). THE SENTENCES
// MOVED HERE WITH THE LADDER and that is half the point of the extraction: left behind,
// the second transport would grow its own copies, and four surfaces wording this for
// themselves is exactly how they came to disagree (§6.5).
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::ExecTransport)
};

} // namespace

ExecTransport::ExecTransport(QString path, QString peerName, RunCommand run, ReadForward read)
    : m_path(std::move(path))
    , m_peerName(std::move(peerName))
    , m_run(std::move(run))
    , m_read(std::move(read))
{
}

ExecSizeProbe ExecTransport::sizeProbe() const
{
    // Built fresh per use: it holds no state worth keeping, and two std::functions cost
    // nothing beside a round trip.
    //
    // THE READ SEAM IS m_read, THE CHANNEL'S OWN FORWARD READ, and never anything that
    // consults m_fileOpen — which is false while this is settling. Before the extraction
    // that was a comment on a lambda; now it is a private member binding a private
    // callable, so the public read this must not use is not even in scope here.
    return {m_path, m_tools,
            [this](const QString &command, QByteArray *out) { return m_run(command, out); },
            [this](qint64 offset, qint64 length) { return m_read(offset, length); }};
}

bool ExecTransport::probe(ExecTools *tools)
{
    QByteArray output;
    if (!m_run(probeCommand(), &output))
        return false;
    m_tools = parseProbeOutput(output);
    if (tools)
        *tools = m_tools;
    return m_tools.ok;
}

bool ExecTransport::openFile(Trouble *trouble, QString *error)
{
    Trouble ignored = Trouble::None;
    Trouble &kind = trouble ? *trouble : ignored;
    kind = Trouble::None;

    closeFile();
    m_size = SizeSource::None;

    ExecSizeProbe probe = sizeProbe();
    const SizeSource source = probe.settle();
    if (source != SizeSource::None) {
        m_size = source;
        m_fileOpen = true;
        return true;
    }

    // A DEAD CHANNEL IS NOT A MISSING FILE. Both are worth retrying, but only one of them
    // is fixed by reconnecting, and the fetcher tells them apart by this code alone.
    if (probe.channelDied()) {
        kind = Trouble::LinkGone;
        if (error) {
            *error = Tr::tr("Lost the connection to %1 while opening %2.")
                         .arg(m_peerName, m_path);
        }
        return false;
    }

    // A log too big for the only measurement this far end can offer is the one outcome
    // here that does NOT mend itself, so it must not be waited for. The `stat` and `ls`
    // rungs are absent or unparseable and `wc` reads the whole file to answer, so
    // measuring it once a second is what invariant #5 forbids — and the file only ever
    // gets further past the ceiling as it grows. A refusal that keeps its tab and says why
    // (M17), naming the real cause: this used to spend a tab for ever on "it is missing,
    // or the account cannot read it" about a file that was present, readable and growing.
    if (probe.tooBigToMeasure()) {
        kind = Trouble::Refused;
        if (error) {
            *error = Tr::tr("%1 on %2 is %3 MB, and the far end offers no way to measure "
                            "it except by reading all of it. Installing `stat` or `ls` "
                            "there fixes this.")
                         .arg(m_path, m_peerName)
                         .arg((probe.sizeThatWasTooBig() + 524288) / (1024LL * 1024));
        }
        return false;
    }

    // WHICH OF THEM, asked of the far end rather than guessed at. This used to read "it is
    // missing, or the account cannot read it" — both answers at once about a file the far
    // end would have said which of, had anything asked — and that sentence is the whole of
    // what the reader has to act on, so it sent them to look in the wrong place half the
    // time. One `test` command, on the failure path only.
    const RemotePathReport report = classifyPath();
    // A folder does not become a log, so it is REFUSED rather than waited for: a tab that
    // keeps its place and says why (M17), where everything else here mends itself and is
    // worth another poll.
    kind = (report.known && report.presence == LogPresence::NotAFile) ? Trouble::Refused
                                                                     : Trouble::NoSuchFile;
    if (error)
        *error = troubleText(report);
    return false;
}

ExecAttrs ExecTransport::statPath() const
{
    if (m_size == SizeSource::None)
        return {};
    ExecSizeProbe probe = sizeProbe();
    return probe.query(m_size);
}

RemotePathReport ExecTransport::classifyPath() const
{
    RemotePathReport out;
    QByteArray output;
    // TWO-WAY, and the two must not be folded: a command that RAN and printed an answer is
    // a fact about the file, while a channel that would not open is a fact about the link
    // — and the caller is already reporting the second for itself. `known` stays false for
    // the second, which is what makes the sentence quote the far end rather than claim one
    // of the five.
    if (!m_run(pathTroubleCommand(m_path), &output))
        return out;
    LogPresence answer = LogPresence::Present;
    if (!parsePathTroubleOutput(output, &answer))
        return out;
    out.known = true;
    out.presence = answer;
    return out;
}

QString ExecTransport::troubleText(const RemotePathReport &report) const
{
    return remotePathTroubleText(report, m_path, m_peerName);
}

} // namespace loftail
