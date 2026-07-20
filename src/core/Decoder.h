#pragma once

#include "Encoding.h"

#include <QByteArrayView>
#include <QString>
#include <QtGlobal>

namespace loftail {

// The layer between raw bytes (LogSource) and the indexer. It exists to enforce
// invariant #8: NOTHING scans raw bytes for '\n' directly, because in UTF-16 a
// newline is `0A 00` or `00 0A` and a naive byte search both misses real
// terminators and fires inside unrelated code units (ARCHITECTURE.md §6.1). All
// line-boundary and text work goes through here; only Record offset/length stay
// byte-valued.
//
// A Decoder is bound to a CONCRETE encoding (Auto is resolved by detect()). It
// is cheap to copy and holds no file handle.
class Decoder
{
public:
    Decoder() = default;

    // Build a Decoder for `requested`. When `requested` is Auto, sniff `sample`
    // (the first ~64 KB of the file, per §6.1): a BOM is decisive; otherwise a
    // high frequency of NUL bytes at alternating positions means UTF-16 and their
    // position gives the byte order; otherwise validate as UTF-8 and fall back to
    // the system codepage. A BOM present under a forced encoding is still skipped.
    static Decoder detect(QByteArrayView sample, Encoding requested = Encoding::Auto);

    Encoding requestedEncoding() const { return m_requested; }
    Encoding resolvedEncoding() const { return m_resolved; }

    // Bytes to skip at the very start of the file (the BOM), if any.
    qint64 bomLength() const { return m_bomLength; }

    // Size of one code unit: 1 for UTF-8/System, 2 for UTF-16. Line starts and
    // ends are always aligned to this from the content start.
    int unitSize() const { return m_unitSize; }

    // Find the end of the line that begins at byte index `from` within `buf`.
    // Returns the index JUST PAST the newline code unit. When no newline is found
    // before the end of `buf`, returns buf.size() and sets *hadNewline to false —
    // the caller decides whether that is EOF or a chunk boundary needing more
    // bytes. `from` must be code-unit aligned relative to the content start.
    qsizetype lineEnd(QByteArrayView buf, qsizetype from, bool *hadNewline) const;

    // Decode a line's content bytes (the range returned by lineEnd, MINUS the
    // trailing newline code unit) into text, stripping one trailing CR so a
    // Windows-authored CRLF log reads identically to an LF one (§6, invariant #8).
    QString decodeLine(QByteArrayView content) const;

    // Decode an arbitrary byte range. Does not strip line terminators.
    QString decode(QByteArrayView bytes) const;

private:
    Encoding m_requested = Encoding::Auto;
    Encoding m_resolved = Encoding::Utf8;
    qint64   m_bomLength = 0;
    int      m_unitSize = 1;
};

} // namespace loftail
