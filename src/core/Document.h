#pragma once

#include "CompileError.h"
#include "Decoder.h"
#include "Encoding.h"
#include "Filter.h"
#include "FilteredIndex.h"
#include "LogFormat.h"
#include "RecordIndex.h"

#include <QString>
#include <QTimeZone>

#include <memory>

namespace loftail {

class LogSource;
class IFormatProvider;

// All per-file state for one open log (invariant #7, ARCHITECTURE.md §12). The
// main window holds a std::vector<std::unique_ptr<Document>> plus an active
// pointer — a vector of length one today, multi-file later — and NOTHING outside
// a Document may hold per-file state (no singletons, no "current file" global).
//
// M2a scope: Document owns the source, format, decoder, zones, and index, and can
// build the index synchronously. Moving indexing to a worker thread with batched
// model updates is M2b; the split is deliberate (PLAN.md).
class Document
{
public:
    Document();
    ~Document();

    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;

    // Open `path`, obtain the LogFormat from `provider`, resolve the encoding, and
    // build the index synchronously. Returns false ONLY when the file cannot be
    // opened. A bad/empty/uncompilable pattern is NOT a failure: the file still
    // opens with unparsed lines as plain text (SPEC.md §4), and the compile error
    // is left in formatError() for the Log Format dialog to surface.
    //
    // Routing the pattern through an IFormatProvider (ARCHITECTURE.md §9) is what
    // keeps the pattern string out of Document and everything below it — Document
    // holds only the compiled LogFormat (invariant #3).
    //
    // `requestedEncoding` defaults to Auto (the persisted default is the user's
    // choice, including Auto — §6.1). An invalid `sourceZone`/`displayZone` means
    // "infer": the source zone comes from the pattern's date specifier (§5.1) and
    // the display zone defaults to the source zone ("as written").
    bool open(const QString &path,
              IFormatProvider &provider,
              Encoding requestedEncoding = Encoding::Auto,
              const QTimeZone &sourceZone = QTimeZone(),
              const QTimeZone &displayZone = QTimeZone());

    // Everything open() does EXCEPT running the indexer: open the source, obtain
    // the format, resolve the encoding, and settle the source/display zones,
    // leaving the index empty. This is the split M2b needs to drive indexing on a
    // worker thread — the controller calls prepare() on the GUI thread (fast), then
    // streams records into index() from a background scan. Returns false only on an
    // unopenable file (see open() for the bad-pattern semantics).
    bool prepare(const QString &path,
                 IFormatProvider &provider,
                 Encoding requestedEncoding = Encoding::Auto,
                 const QTimeZone &sourceZone = QTimeZone(),
                 const QTimeZone &displayZone = QTimeZone());

    // Convenience overloads that build a ManualFormatProvider from a pattern
    // string. They keep the pattern confined to the provider (invariant #3) while
    // giving pattern-string call sites (tests, simple opens) a one-liner.
    bool open(const QString &path,
              QStringView pattern,
              Encoding requestedEncoding = Encoding::Auto,
              const QTimeZone &sourceZone = QTimeZone(),
              const QTimeZone &displayZone = QTimeZone());
    bool prepare(const QString &path,
                 QStringView pattern,
                 Encoding requestedEncoding = Encoding::Auto,
                 const QTimeZone &sourceZone = QTimeZone(),
                 const QTimeZone &displayZone = QTimeZone());

    // Re-derive Record::timestamp for every record under a new SOURCE zone WITHOUT
    // a rescan (§5.1, invariant #10): record boundaries and byte offsets are
    // unaffected, so this is a pass over the existing index that re-reads only the
    // %d field of each record and re-parses it in `sourceZone`. A no-op when the
    // format has no date field. An invalid `sourceZone` re-infers from the pattern.
    void reparseTimestamps(const QTimeZone &sourceZone);

    // Change the DISPLAY zone. This is free (§5.1): the view reformats timestamps
    // from the stored UTC ms on its next repaint. Nothing is re-derived.
    void setDisplayZone(const QTimeZone &displayZone) { m_displayZone = displayZone; }

    // The compile outcome of the last prepare(). isError() when the pattern was
    // empty or uncompilable — the file opened as plain text (SPEC.md §4); the
    // dialog reads this to point a caret at the offending offset.
    const CompileError &formatError() const { return m_formatError; }

    const QString &path() const { return m_path; }
    const QString &lastError() const { return m_lastError; }

    LogSource *source() const { return m_source.get(); }
    const LogFormat &format() const { return m_format; }
    const Decoder &decoder() const { return m_decoder; }
    const RecordIndex &index() const { return m_index; }
    RecordIndex &index() { return m_index; }

    // Filtering (M4, SPEC.md §6). The FilterSet is the per-file filter state
    // (invariant #7); the FilteredIndex is the visible subset the view scrolls over
    // (invariant #6). Mutate filters() then call applyFilters() to recompute the
    // subset. The UI wraps that in a model reset so the view/selection refresh.
    FilterSet &filters() { return m_filters; }
    const FilterSet &filters() const { return m_filters; }
    const FilteredIndex &filtered() const { return m_filtered; }

    // Recompute the visible subset from the current filters over the current index.
    // Runs the predicate chain with integer axes first and message-text last
    // (invariant #4): a message is decoded ONLY for records the integer axes let
    // through. An all-inactive filter set leaves the FilteredIndex as an identity
    // view (no subset materialized). A single linear pass; §11 repaint budget.
    void applyFilters();

    // Decode one record's message text through the Decoder (invariant #8, no raw
    // byte scans) — the message field when the pattern defines one, else the whole
    // record so text filtering still works on plain-text/unparsed logs (SPEC.md §6).
    // This is the message-text axis's decode; it runs last in the chain.
    QString messageText(const Record &rec) const;

    const QTimeZone &sourceZone() const { return m_sourceZone; }
    const QTimeZone &displayZone() const { return m_displayZone; }
    Encoding requestedEncoding() const { return m_requestedEncoding; }
    Encoding resolvedEncoding() const { return m_decoder.resolvedEncoding(); }

    // Every file opens at its end, following (SPEC.md §3); watching is always on.
    // The watch/append loop itself is M6; the flag lives here from the start.
    bool following() const { return m_following; }
    void setFollowing(bool f) { m_following = f; }

    // The zone inferred from a compiled format's date specifier (§5.1): UTC for a
    // %D pattern, the system zone otherwise. A hint the user may override.
    static QTimeZone inferSourceZone(const LogFormat &format);

private:
    QString                    m_path;
    QString                    m_lastError;
    std::unique_ptr<LogSource> m_source;
    LogFormat                  m_format;
    Decoder                    m_decoder;
    RecordIndex                m_index;
    FilterSet                  m_filters;
    FilteredIndex              m_filtered;
    QTimeZone                  m_sourceZone;
    QTimeZone                  m_displayZone;
    CompileError               m_formatError;
    Encoding                   m_requestedEncoding = Encoding::Auto;
    bool                       m_following = true;
};

} // namespace loftail
