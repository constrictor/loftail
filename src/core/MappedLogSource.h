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

namespace loftail {

// POSIX read strategy: mmap the currently-indexed extent, re-mapping as the file
// grows (ARCHITECTURE.md §6). Chosen on POSIX because a mapping survives a
// rename/unlink (it holds the inode), giving rotation safety for free, and gives
// zero-copy random access on the paint path.
//
// Append-safe (invariant #5): the file is opened read-only and NON-BLOCKING for
// the writer — a shared mmap never prevents the logging process from appending,
// renaming, or unlinking. Truncation (copytruncate) is caught by refreshSize()'s
// shrink check so a read past the new EOF cannot SIGBUS, and a rewrite in place that
// does NOT shrink is caught by the head witness beside it (HeadWitness.h) — the one
// change that moves neither the inode nor the size below what we indexed.
class MappedLogSource final : public LogSource
{
public:
    ~MappedLogSource() override;

    // Open and map `path`. Returns nullptr on failure (missing file, mmap error).
    static std::unique_ptr<MappedLogSource> open(const QString &path);

    QByteArrayView bytes(qint64 offset, qint64 length, QByteArray &into) override;
    qint64 size() const override { return m_mappedSize; }
    qint64 refreshSize() override;
    bool isRandomAccess() const override { return true; }
    quint64 identity() const override { return m_identity; }
    bool wasTruncated() const override { return m_truncated; }
    bool wasReplaced() const override;
    bool originVanished() const override;

private:
    MappedLogSource() = default;
    void unmap();
    bool remap(qint64 newSize);

    int      m_fd = -1;
    void    *m_map = nullptr;   // MAP_FAILED-checked pointer to the mapping
    qint64   m_mappedSize = 0;  // bytes currently mapped (== indexed extent)
    QString  m_path;            // re-stat'd by wasReplaced(); the fd follows the inode
    quint64  m_identity = 0;    // dev<<32 ^ inode, for rotation detection
    bool     m_truncated = false;
    HeadWitness m_head;         // the file's first bytes, to catch a rewrite in place
};

} // namespace loftail
