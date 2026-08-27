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
#include <QByteArrayView>
#include <QtGlobal>

#include <cstring>

namespace loftail {

// A copy of the first bytes of the file, as they were when this source last agreed with
// what is on disk. Its one job is to answer the question neither the size nor the inode
// can: *has the extent we already read been rewritten underneath us?*
//
// WHY IT IS NEEDED (invariant #5, §6). A local source detects a changed file three ways,
// and a rewrite in place slips between all of them: the inode does not move, so
// wasReplaced() is false; the path still resolves, so originVanished() is false; and if
// the new content happens to reach or exceed the old size before the next poll, the size
// does not shrink either. The tick then reads the growth as an APPEND and resumes
// indexing from the old tail offset — so the pre-rewrite records stay on screen for ever
// and the new bytes are parsed from the middle of a record. `cp new.log app.log`,
// `truncate`+rewrite, and an editor saving over the file all land here.
//
// A rewrite is only ever visible in the CONTENT, which is why this exists and why the
// remote transport already carries its own version of it (SshFetcher's head probe). The
// local one can afford to run on every tick where the remote one cannot: the bytes are
// already mapped, so the check is a memcmp of at most a kilobyte.
//
// APPEND-SAFE BY CONSTRUCTION: an append never touches bytes that are already in the
// file, so a witness taken over a prefix stays valid for the whole life of a log that is
// only being appended to. That is what makes "the prefix changed" mean "rewritten"
// rather than "written to".
class HeadWitness
{
public:
    // Long enough to span several timestamped records — a rewrite whose first kilobyte
    // is byte-identical to the old one reads as an append, and is the residual limit
    // here. Short enough that comparing it costs nothing on the watch tick.
    static constexpr qint64 kBytes = 1024;

    qint64 length() const { return m_head.size(); }

    // Record `head` (the file's first bytes, however many are available) as the truth.
    void take(QByteArrayView head)
    {
        const qsizetype n = qMin<qsizetype>(head.size(), kBytes);
        m_head = n > 0 ? QByteArray(head.constData(), n) : QByteArray();
    }

    // True when `head` — the same prefix, read again now — is not what was witnessed.
    // An UNTAKEN witness contradicts nothing: a log that was empty when it was opened
    // has no prefix to have changed, and its first bytes are an append like any other.
    bool contradicts(QByteArrayView head) const
    {
        if (m_head.isEmpty())
            return false;
        if (head.size() < m_head.size())
            return true; // the prefix we witnessed is not even there any more
        return std::memcmp(head.constData(), m_head.constData(),
                           static_cast<size_t>(m_head.size())) != 0;
    }

    // Whether the witness still has room to grow. It is extended — never re-taken — as a
    // short log passes kBytes, so the ordinary tick over a settled log copies nothing.
    bool wantsMore() const { return m_head.size() < kBytes; }

private:
    QByteArray m_head;
};

} // namespace loftail
