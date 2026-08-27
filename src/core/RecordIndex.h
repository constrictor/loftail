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

#include "InternTable.h"
#include "Record.h"

#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace loftail {

// The indexed form of one file: the record array, the logger and thread intern
// tables, and the two-level block prefix sums that make the custom LogView scroll
// in line units without an O(n) height pass (invariant #6, ARCHITECTURE.md §7.1).
//
// Scrolling model: the vertical scrollbar's range is the total DISPLAY line
// count. Displayed height caps at 100 lines per record (§7.1), so the prefix sums
// use the clamped value while copy/byte-range operations use the true lineCount.
// A cumulative line count is stored every kBlockSize records; resolving a scroll
// line to a record is a binary search over the block array plus a short linear
// scan within one block — O(log n) with a cache-friendly inner loop.
class RecordIndex
{
public:
    static constexpr int     kBlockSize = 4096;       // records per block (§7.1)
    static constexpr quint16 kDisplayLineCap = 100;   // per-record display cap (§7.1)

    QVector<Record> records;
    InternTable     loggers;
    InternTable     threads;

    static quint16 displayLines(const Record &r)
    {
        return qMin<quint16>(r.lineCount, kDisplayLineCap);
    }

    int recordCount() const { return int(records.size()); }
    qint64 totalLines() const { return m_blockSums.isEmpty() ? 0 : qint64(m_blockSums.last()); }

    // Rebuild the block prefix sums over the current `records`. One linear pass,
    // no parsing — cheap enough to rerun on every filter change (§7.2, §11).
    void rebuildBlockSums()
    {
        m_blockSums.clear();
        const int n = int(records.size());
        m_blockSums.reserve(n / kBlockSize + 2);
        quint64 acc = 0;
        for (int i = 0; i < n; ++i) {
            if ((i % kBlockSize) == 0)
                m_blockSums.append(acc);
            acc += displayLines(records.at(i));
        }
        m_blockSums.append(acc); // final sentinel == total display lines
    }

    // Extend/repair the block prefix sums in place after records were appended (or
    // the trailing record changed height), WITHOUT the O(n) full rebuild (M6,
    // invariant #1). `validCount` is the number of leading records whose sums are
    // still correct — records at index < the block boundary at or before
    // `validCount` are assumed unchanged; everything from that boundary to the end
    // is recomputed from the live record data. Cost is O(kBlockSize + appended),
    // bounded and independent of the total record count. The common append path
    // passes validCount == oldRecordCount; the trailing-record-grew path passes
    // oldRecordCount - 1 so the reconsidered last record is recomputed too.
    void extendBlockSums(int validCount)
    {
        const int n = int(records.size());
        validCount = std::max(validCount, 0);
        validCount = std::min(validCount, n);
        if (m_blockSums.isEmpty()) {
            // No baseline to extend (sums were never built): fall back to a full
            // build. Cheap and keeps `resize(startBlock)` below from fabricating
            // zero-valued boundary entries.
            rebuildBlockSums();
            return;
        }
        const int startBlock = validCount / kBlockSize;
        const int startRec = startBlock * kBlockSize;
        // m_blockSums[startBlock] is the cumulative display lines before record
        // startRec; it was written by a previous build/extend and covers only
        // unchanged records, so it is safe to keep. Drop everything after it
        // (later boundary entries and the stale grand-total sentinel) and rebuild.
        quint64 acc = (startBlock < m_blockSums.size()) ? m_blockSums.at(startBlock) : 0;
        m_blockSums.resize(startBlock); // the boundary entry for startBlock is re-added below
        for (int i = startRec; i < n; ++i) {
            if ((i % kBlockSize) == 0)
                m_blockSums.append(acc);
            acc += displayLines(records.at(i));
        }
        m_blockSums.append(acc); // final sentinel == total display lines
    }

    // The display line at which record `r` begins (its first physical line).
    qint64 firstLineOfRecord(int r) const
    {
        if (r <= 0 || records.isEmpty())
            return 0;
        const int block = r / kBlockSize;
        auto line = qint64(m_blockSums.at(block));
        for (int i = block * kBlockSize; i < r; ++i)
            line += displayLines(records.at(i));
        return line;
    }

    // The record containing display line `line` (binary search the blocks, then
    // scan within one block). Returns the last record for an out-of-range line.
    int recordAtLine(qint64 line) const
    {
        const int n = int(records.size());
        if (n == 0)
            return -1;
        if (line < 0)
            return 0;

        // Largest block index b with m_blockSums[b] <= line.
        int lo = 0;
        int hi = int(m_blockSums.size()) - 1;
        while (lo < hi) {
            const int mid = (lo + hi + 1) / 2;
            if (std::cmp_less_equal(m_blockSums.at(mid), line))
                lo = mid;
            else
                hi = mid - 1;
        }
        int block = lo;
        if (block >= (n + kBlockSize - 1) / kBlockSize)
            return n - 1;

        auto acc = qint64(m_blockSums.at(block));
        int i = block * kBlockSize;
        const int end = qMin(i + kBlockSize, n);
        for (; i < end; ++i) {
            const qint64 next = acc + displayLines(records.at(i));
            if (line < next)
                return i;
            acc = next;
        }
        return n - 1;
    }

    int blockCount() const { return int(m_blockSums.size()); }

private:
    // m_blockSums[b] = cumulative display lines of all records before block b.
    // The final element is the grand total. One quint64 per 4096 records (§7.1).
    QVector<quint64> m_blockSums;
};

} // namespace loftail
