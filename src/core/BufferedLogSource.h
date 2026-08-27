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

#include "HeadWitness.h"
#include "LogSource.h"
#include "SharedReadFile.h"

#include <QByteArray>

namespace loftail {

// Buffered read strategy: the Windows source, and the portable fallback
// everywhere (ARCHITECTURE.md §6). On Windows it is PREFERRED over a mapping
// because a held file mapping can block the writer from rotating or truncating —
// exactly what a logging framework does — and under the always-watched model
// that risk would apply to every open file.
//
// The open itself uses full sharing (FILE_SHARE_READ | WRITE | DELETE) so that
// loftail never locks the writer out; that is SharedReadFile's whole job, and the
// share bits are the reason it exists rather than a plain QFile — see its header.
// This class held a plain QFile until M6's Windows work was finally settled, and a
// QFile's Windows open omits FILE_SHARE_DELETE — so for six milestones a log open in
// a tab could not be rolled or deleted by the process writing it.
class BufferedLogSource final : public LogSource
{
public:
    static std::unique_ptr<BufferedLogSource> open(const QString &path);

    QByteArrayView bytes(qint64 offset, qint64 length) override;
    qint64 size() const override { return m_size; }
    qint64 refreshSize() override;
    bool isRandomAccess() const override { return true; }
    quint64 identity() const override { return m_identity; }
    bool wasTruncated() const override { return m_truncated; }
    bool wasReplaced() const override;
    bool originVanished() const override;

private:
    BufferedLogSource() = default;
    quint64 computeIdentity() const;
    QByteArray readHead();

    SharedReadFile m_file;
    // The path, kept here rather than asked of m_file: a handle opened by descriptor
    // carries no name on either platform, and wasReplaced(), originVanished() and
    // computeIdentity() all need one. It outlives close() for the same reason —
    // "is it back yet?" is a question about a path, asked when nothing is open.
    QString    m_path;
    qint64     m_size = 0;
    quint64    m_identity = 0;
    // The platform identity OF THE PATH as it stood at open, which is what
    // wasReplaced() compares against. Distinct from m_identity: that one is the
    // portable size+mtime stand-in below, which changes on every append and so
    // cannot answer "was this file replaced".
    quint64    m_pathIdentity = 0;
    bool       m_truncated = false;
    // The file's first bytes, to catch a rewrite in place (HeadWitness.h) — the one
    // rotation shape no identity can see, since the file that was rewritten is the same
    // file. It used to carry more weight here than in the mapped source, because
    // pathIdentity() was stubbed on Windows and wasReplaced() was therefore always
    // false there; now that it is real, the two sources lean on this equally.
    HeadWitness m_head;
    QByteArray m_buffer;      // backs the QByteArrayView returned by bytes()
};

} // namespace loftail
