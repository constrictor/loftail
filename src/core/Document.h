#pragma once

#include "CompileError.h"
#include "Decoder.h"
#include "Encoding.h"
#include "Filter.h"
#include "FilteredIndex.h"
#include "Highlight.h"
#include "LogFormat.h"
#include "RecordIndex.h"

#include <QString>
#include <QTimeZone>
#include <QVector>

#include <limits>
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
    // Mutable access for the live-append path (M6): LiveController extends the
    // visible subset in place as records arrive, under the active filter, rather
    // than recomputing it (invariant #1). Not used by the normal filter apply.
    FilteredIndex &filtered() { return m_filtered; }

    // Recompute the visible subset from the current filters over the current index.
    // Runs the predicate chain with integer axes first and message-text last
    // (invariant #4): a message is decoded ONLY for records the integer axes let
    // through. An all-inactive filter set leaves the FilteredIndex as an identity
    // view (no subset materialized). A single linear pass; §11 repaint budget.
    // With a run selected (see below) the pass ALSO restricts to that run's
    // byte-offset interval, composing the run bound with the filter predicate.
    void applyFilters();

    // Run selection (SPEC.md §3a). A log file often concatenates several app runs;
    // a user-supplied "run-start" regexp splits it into runs and the user views one
    // at a time. A run is a contiguous record range expressed as a half-open
    // byte-offset interval [startOffset, endOffset) over Record::offset — stable
    // across appends, and fed to the SAME FilteredIndex view as filtering (a run is
    // just an extra integer bound, not a second view layer). Detection matches the
    // regexp against each record's WHOLE first decoded line (not just the message).
    struct Run
    {
        int     startRecord = 0;                        // ordinal of the run's first record
        qint64  startOffset = 0;                        // that record's byte offset (interval start)
        qint64  startTimestamp = Record::kNoTimestamp;  // for labelling + persist re-resolve
        QString firstLine;                              // the matched line, for the run label
        bool    isPreamble = false;                     // records before the first marker
    };

    // Configure the run-start matcher (whole-line match, invariant #8 via the
    // Decoder), re-detect runs over the current index, and select the newest run.
    // An empty pattern disables run splitting (the whole file is one view again).
    void setRunStart(const QString &pattern, bool regex, Qt::CaseSensitivity cs);

    // Rebuild the run list from the current index using the configured matcher.
    // Call after indexing finishes and after a rescan (the index just changed).
    // Preserves the current selection ordinal (clamped); the caller then picks a
    // selection policy (selectNewestRun / selectRunByStart).
    void detectRuns();

    // Incrementally fold newly-appended records [oldRecordCount, size) into the run
    // list during live tail. Appending a new run-start marker makes the previously
    // last run's end become finite automatically (the next marker's offset), so a
    // watched last run freezes at the boundary and the new run appears in the list
    // (the "stay on current run" behaviour). Returns true when the run list changed.
    bool updateRunsAfterAppend(int oldRecordCount);

    // Select which run restricts the view: an index into runs(), or -1 for "all
    // runs" (no restriction). Recomputes the cached view interval. The caller
    // re-applies via the model-reset path (applyFilters wrapped in a model reset).
    void selectRun(int index);
    void selectNewestRun() { selectRun(m_runs.isEmpty() ? -1 : int(m_runs.size()) - 1); }
    // Re-resolve a persisted selection to an ordinal by start offset, then timestamp,
    // else fall back to the newest run (offsets/ordinals shift across sessions).
    void selectRunByStart(qint64 startOffset, qint64 startTimestamp);

    const QVector<Run> &runs() const { return m_runs; }
    int  selectedRun() const { return m_selectedRun; }
    // Records in run i: derived so the last run's count tracks live appends.
    int  runRecordCount(int i) const;
    // True when a run currently narrows the view (drives subset materialization and
    // the live-append branch, exactly like FilterSet::anyActive()).
    bool viewRestricted() const { return m_viewRestricted; }
    const TextMatcher &runStartMatcher() const { return m_runStartMatcher; }

    // The full in-view predicate: the run byte-offset bound (cheapest, first) AND
    // the filter chain. Used by BOTH applyFilters() and the live-append path so run
    // restriction applies identically on initial load and on tail.
    bool acceptsInView(const Record &r) const;

    // Highlighting (M5, SPEC.md §7). The HighlighterSet is the per-file highlight
    // state (invariant #7): an ordered rule list, first-match-wins, each rule
    // supplying a background and a foreground palette reference. LogModel::data()
    // consults it for the Background/Foreground roles. Mutate highlighters() then
    // call resolveHighlighters() so the rules' subsystem NAMES are re-bound to the
    // current intern table's ids (the match runs on integers, invariant #4).
    HighlighterSet &highlighters() { return m_highlighters; }
    const HighlighterSet &highlighters() const { return m_highlighters; }

    // Re-resolve every highlight rule's subsystem names to interned ids against the
    // current index. Call after editing the rules and after indexing discovers more
    // subsystems. Cheap (a hash lookup per name); leaves the rule list unchanged.
    void resolveHighlighters() { m_highlighters.resolve(m_index); }

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

    // Re-open the file at the current path and rebuild the index from scratch with
    // the SAME compiled format, decoder, and source zone (M6 rotation/truncation:
    // the old content is gone or invalidated, so a silent full rescan replaces it —
    // SPEC.md §3, invariant #5). The pattern is NOT recompiled (invariant #3: the
    // format is already resolved). Clears the filtered subset; the caller re-applies
    // filters and re-resolves highlighters against the new intern tables. Returns
    // false only if the file cannot be reopened, leaving an empty, valid index.
    bool rescan();

    // The zone inferred from a compiled format's date specifier (§5.1): UTC for a
    // %d pattern, the system zone otherwise. A hint the user may override.
    static QTimeZone inferSourceZone(const LogFormat &format);

private:
    // Recompute the cached [m_viewStart, m_viewEnd) byte interval and m_viewRestricted
    // from the current run selection. Called after any run-list or selection change.
    void recomputeViewBounds();
    // The whole first decoded physical line of a record (the run-start match target).
    // Mirrors the first-line decode in messageText() but returns the full line, not
    // the message tail; byte-range decode only (invariant #8).
    QString recordFirstLine(const Record &rec) const;

    QString                    m_path;
    QString                    m_lastError;
    std::unique_ptr<LogSource> m_source;
    LogFormat                  m_format;
    Decoder                    m_decoder;
    RecordIndex                m_index;
    FilterSet                  m_filters;
    FilteredIndex              m_filtered;
    HighlighterSet             m_highlighters;
    QTimeZone                  m_sourceZone;
    QTimeZone                  m_displayZone;
    CompileError               m_formatError;
    Encoding                   m_requestedEncoding = Encoding::Auto;
    bool                       m_following = true;

    // Run selection state. m_runs holds start markers (ascending by offset); the
    // selected run's byte interval is cached in m_viewStart/m_viewEnd.
    QVector<Run> m_runs;
    int          m_selectedRun = -1;   // index into m_runs, or -1 == all runs
    TextMatcher  m_runStartMatcher;
    bool         m_runStartActive = false;
    bool         m_viewRestricted = false;
    qint64       m_viewStart = std::numeric_limits<qint64>::min();
    qint64       m_viewEnd   = std::numeric_limits<qint64>::max();
};

} // namespace loftail
