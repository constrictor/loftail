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

#include "ExecSizeProbe.h"

#include <utility>

namespace loftail {

ExecSizeProbe::ExecSizeProbe(QString path, ExecTools tools, RunCommand run, ReadAt read)
    : m_path(std::move(path))
    , m_tools(tools)
    , m_run(std::move(run))
    , m_read(std::move(read))
{
}

bool ExecSizeProbe::eligible(SizeSource source) const
{
    switch (source) {
    case SizeSource::Stat:
        return m_tools.hasStat;
    case SizeSource::Ls:
        // A newline in the path makes this rung unusable, because `ls` prints the name
        // back and the answer is taken from the LAST line: a filename can then carry a
        // complete, plausible `ls` line of its own and choose the size loftail believes.
        // The other two rungs are immune — `stat -c '%s %Y'` prints no name, and
        // `wc -c <` is fed by a redirect — so skipping this one loses nothing.
        return m_tools.hasLs && !m_path.contains(QLatin1Char('\n'));
    case SizeSource::Wc:
        return m_tools.hasWc;
    case SizeSource::None:
        break;
    }
    return false;
}

ExecAttrs ExecSizeProbe::query(SizeSource source)
{
    ExecAttrs out;
    QString command;
    switch (source) {
    case SizeSource::Stat:
        command = statCommand(m_path);
        break;
    case SizeSource::Ls:
        command = lsSizeCommand(m_path);
        break;
    case SizeSource::Wc:
        command = wcSizeCommand(m_path);
        break;
    case SizeSource::None:
        return out;
    }

    QByteArray printed;
    if (!m_run(command, &printed)) {
        m_channelDied = true;
        return out;
    }
    m_channelDied = false;

    switch (source) {
    case SizeSource::Stat:
        return parseStatOutput(printed);
    case SizeSource::Ls:
        return parseLsSizeOutput(printed);
    case SizeSource::Wc:
        return parseWcSizeOutput(printed);
    case SizeSource::None:
        break;
    }
    return out;
}

bool ExecSizeProbe::provesReadPath(qint64 size)
{
    // Nothing to read, so nothing to prove. A zero that should have been non-zero merely
    // under-reports, and the next poll picks the file up.
    if (size <= 0)
        return true;
    return m_read(size - 1, 1) >= 1;
}

SizeSource ExecSizeProbe::settle(ExecAttrs *first)
{
    static constexpr SizeSource kLadder[] = {SizeSource::Stat, SizeSource::Ls,
                                             SizeSource::Wc};
    bool attempted = false;
    bool anyRan = false;

    for (const SizeSource rung : kLadder) {
        if (!eligible(rung))
            continue;
        attempted = true;

        const ExecAttrs attrs = query(rung);
        if (!m_channelDied)
            anyRan = true;
        if (!attrs.ok)
            continue;

        // The exactness backstop must not become a way to re-read a large log forever.
        if (rung == SizeSource::Wc && attrs.size > kWcSettleCeiling)
            continue;

        if (!provesReadPath(attrs.size))
            continue;

        if (first)
            *first = attrs;
        m_channelDied = false;
        return rung;
    }

    // Nothing ran, as opposed to nothing answered: the caller must reconnect rather than
    // wait for a file to appear.
    m_channelDied = attempted && !anyRan;
    return SizeSource::None;
}

} // namespace loftail
