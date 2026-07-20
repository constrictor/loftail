#pragma once

#include "Priority.h"

#include <QtGlobal>

#include <limits>

namespace loftail {

// One indexed log record. This is the single most size-sensitive type in the
// project — there is one per record and a large log has millions — so it stores
// only what filtering needs eagerly plus byte coordinates into the source. The
// message text and most fields are parsed LAZILY in LogModel::data() from the
// mapped bytes (invariant #1, ARCHITECTURE.md §5). No parsed strings live here.
//
// A record is NOT a line: `length`/`lineCount` span continuation lines
// (invariant #2, §4). `offset`/`length` stay in BYTE terms in every encoding —
// only the Decoder converts to text (invariant #8, §6.1).
//
// Layout is exactly 32 bytes with no padding waste; see the static_assert below.
struct Record
{
    qint64  offset;     // byte offset of the record's first line in the source
    qint64  timestamp;  // UTC epoch ms (invariant #10); kNoTimestamp when absent
    quint32 length;     // byte span, including continuation lines
    quint32 loggerId;   // interned id into the logger table (invariant #4)
    quint32 threadId;   // interned id into the thread table (invariant #4)
    quint16 lineCount;  // physical lines; drives row height (§7.1). Clamped below.
    quint8  priority;   // Priority enum, in severity order (§7.2)
    quint8  reserved;   // pad to a clean 32 bytes; keeps the struct trivially copyable

    // Sentinel timestamp for records whose pattern has no date field, or whose
    // date text failed to parse. Sorts before any real time (§5, §5.1).
    static constexpr qint64 kNoTimestamp = std::numeric_limits<qint64>::min();

    // lineCount is a quint16, so a pathologically tall record is clamped here at
    // index time (§5). Display caps further at 100 lines (§7.1); that clamp lives
    // in the prefix-sum builder, not the record.
    static constexpr quint16 kMaxLineCount = 65535;

    Priority priorityEnum() const { return static_cast<Priority>(priority); }
};

static_assert(sizeof(Record) == 32, "Record must be exactly 32 bytes (invariant #1)");
static_assert(alignof(Record) == 8, "Record must stay 8-byte aligned with no padding waste");

} // namespace loftail
