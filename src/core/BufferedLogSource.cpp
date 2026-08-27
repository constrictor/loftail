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

#include "BufferedLogSource.h"

#include <QFileInfo>

namespace loftail {

std::unique_ptr<BufferedLogSource> BufferedLogSource::open(const QString &path)
{
    auto src = std::unique_ptr<BufferedLogSource>(new BufferedLogSource());
    src->m_path = path;
    // Binary read-only, and shared all three ways, so that the writer can go on
    // appending to this file, roll it, or delete it while loftail reads (SharedReadFile.h,
    // invariant #5).
    if (!src->m_file.open(path))
        return nullptr;
    src->m_size = src->m_file.size();
    src->m_identity = src->computeIdentity();
    src->m_pathIdentity = pathIdentity(path);
    src->m_head.take(src->readHead());
    return src;
}

// The first bytes of the file, read fresh. Deliberately NOT through bytes(), whose
// QByteArrayView is backed by m_buffer: this runs from refreshSize() on the watch tick,
// and clobbering the buffer a caller may still be reading from would be a use-after-free
// in everything but name.
QByteArray BufferedLogSource::readHead()
{
    return m_file.read(0, HeadWitness::kBytes);
}

bool BufferedLogSource::wasReplaced() const
{
    // Compare the path's identity NOW against the one captured at open — not against
    // identity(), which is the size+mtime stand-in below and moves on every append.
    // Real on both platforms since M6's Windows work was finished (SharedReadFile.cpp);
    // where it cannot answer it returns 0 on both sides of the comparison, and the
    // guard below reads that as "not replaced" rather than as a rotation.
    const quint64 current = pathIdentity(m_path);
    return current != 0 && m_pathIdentity != 0 && current != m_pathIdentity;
}

bool BufferedLogSource::originVanished() const
{
    // Deliberately NOT pathIdentity() == 0, which is how MappedLogSource answers this,
    // and it stayed that way after the Windows stub that first forced the split was
    // replaced by a real implementation. 0 there means "unknown", which is a file that
    // is gone AND a file that would not open AND a volume with no usable index (ReFS) —
    // and only the first of those is a log that is not there. Asking the filesystem
    // directly is the same question with one meaning, and it costs an attribute query
    // rather than an open, on a path walked every watch tick (§6.5).
    return !QFileInfo::exists(m_path);
}

quint64 BufferedLogSource::computeIdentity() const
{
    // Portable stand-in for the platform file-identity token. On POSIX this is a
    // best-effort proxy (size+mtime) since QFile does not expose the inode; the
    // real device+inode identity lives in MappedLogSource. Sufficient to wire the
    // rotation-detection seam here.
    const QFileInfo info(m_path);
    const quint64 mtime = static_cast<quint64>(info.lastModified().toMSecsSinceEpoch());
    return (mtime << 20) ^ static_cast<quint64>(info.size());
}

qint64 BufferedLogSource::refreshSize()
{
    const qint64 current = m_file.size();
    if (current < m_size)
        m_truncated = true;
    m_size = current;

    // A rewrite in place that did not shrink the file moves neither the size nor the
    // path identity, so the growth would otherwise read as an append and the pre-rewrite
    // records would stay on screen for ever (HeadWitness.h). AFTER m_size is updated,
    // because readHead() reads through the same handle the size bounds.
    if (!m_truncated) {
        const QByteArray head = readHead();
        if (m_head.contradicts(head))
            m_truncated = true;
        else if (m_head.wantsMore())
            m_head.take(head); // a log shorter than kBytes when it opened has grown
    }
    return m_size;
}

QByteArrayView BufferedLogSource::bytes(qint64 offset, qint64 length)
{
    if (offset < 0 || length <= 0 || offset >= m_size)
        return {};
    const qint64 clamped = qMin(length, m_size - offset);
    m_buffer = m_file.read(offset, clamped);
    return {m_buffer.constData(), m_buffer.size()};
}

} // namespace loftail
