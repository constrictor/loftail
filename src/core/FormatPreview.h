#pragma once

#include "LogFormat.h"

#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QVector>

namespace loftail {

class Decoder;

// One record's worth of preview: whether the pattern matched it, the per-field
// breakdown (one entry per LogFormat::fields, in order), and the raw first line so
// an unmatched record can still be shown as plain text (SPEC.md §4).
struct PreviewRow
{
    bool        matched = false;
    QStringList fields;        // one per format.fields; empty strings when unmatched
    QString     rawFirstLine;  // the record's first physical line, verbatim
};

// The result of running a LogFormat over a file sample: the column headers, the
// per-record breakdown, and how many of the sample records the pattern matched.
// matchedCount == 0 is the "pattern does not match" signal MainWindow uses to
// offer Preferences on a first open.
struct PreviewResult
{
    QStringList        headers;
    QVector<PreviewRow> rows;
    int                matchedCount = 0;
    int                totalCount = 0;
};

// Builds the format editor's live preview (SPEC.md §4): it runs `format` over
// the leading `sample` bytes, splitting them into records with the SAME
// record-start rule the indexer uses (invariant #2) and reading through the
// Decoder rather than scanning raw bytes (invariant #8). Pure — no LogSource, no
// file, no QApplication — so the field-breakdown logic is unit-testable on its
// own. An empty/uncompiled `format` yields all-unmatched rows (plain text).
class FormatPreview
{
public:
    static PreviewResult build(const LogFormat &format, QByteArrayView sample,
                               const Decoder &decoder, int maxRecords = 20);

private:
    FormatPreview() = delete;
};

} // namespace loftail
