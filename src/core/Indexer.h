#pragma once

#include "Decoder.h"
#include "LogFormat.h"
#include "RecordIndex.h"

#include <QTimeZone>

#include <functional>

namespace loftail {

class LogSource;

// Builds a RecordIndex from a LogSource in a SINGLE FORWARD PASS (invariant #9,
// §6.2): scan start to finish, no backward passes, no seek-and-re-read, so that
// future non-seekable sources (gz, SSH) drop in unchanged. Random access is only
// ever used later on the paint path, over already-indexed records.
//
// The scan applies the record-start rule (invariant #2, §4): a line matching
// recordStartRe opens a record; non-matching lines are continuations appended to
// the record's byte span and lineCount. Leading non-matching lines (or a wrong
// pattern) become Unparsed records so the view is never empty (§4). Logger AND
// thread names are interned (invariant #4); the timestamp is normalized to UTC
// epoch ms (invariant #10). No message text is stored — that is lazy in data().
class Indexer
{
public:
    // Progress callback: invoked periodically with (bytesProcessed, totalBytes).
    // Returns false to request cancellation. May be empty.
    using Progress = std::function<bool(qint64, qint64)>;

    Indexer(const LogFormat &format, const Decoder &decoder, const QTimeZone &sourceZone);

    // Index the whole source. `progress` may cancel; a cancelled run returns what
    // was indexed so far with `cancelled` set.
    RecordIndex index(LogSource &source, const Progress &progress = {}, bool *cancelled = nullptr) const;

    // Chunk size for reading from the source. Public for the perf harness.
    static constexpr qint64 kChunkBytes = 4 * 1024 * 1024;

private:
    const LogFormat &m_format;
    Decoder          m_decoder;
    QTimeZone        m_sourceZone;
};

} // namespace loftail
