#pragma once

#include "Priority.h"
#include "Record.h"

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <Qt>

#include <functional>
#include <limits>

namespace loftail {

// M4 — Filtering (SPEC.md §6, ARCHITECTURE.md §7.2, invariant #4). The filter
// configuration is pure data + integer/text predicates; it holds no file state and
// needs no QApplication, so the whole predicate chain is unit-testable in core.
//
// The chain runs the CHEAP INTEGER tests first — priority is one `>=` against the
// severity-ordered enum, logger/thread are QSet<quint32> membership over interned
// ids, time is two qint64 compares against Record::timestamp — and the message-text
// axis (the one with no integer fast path) LAST, so a decode only happens for the
// records the integer axes already let through (§7.2). See FilterSet::accepts().

// One text query, shared by the message-text filter (SPEC.md §6) and Find/Find Next
// (SPEC.md §5): substring or regex, with a case-sensitivity option. Kept separate
// from negation/enable so Find can reuse exactly the matching code without the
// filter's hide-semantics.
class TextMatcher
{
public:
    TextMatcher() = default;

    // Configure the query. A regex is compiled once here (not per record); an
    // invalid regex leaves the matcher in a state that matches nothing, surfaced
    // via isValid() so the UI can flag it.
    void set(const QString &pattern, bool regex, Qt::CaseSensitivity cs);

    bool isEmpty() const { return m_pattern.isEmpty(); }
    bool isRegex() const { return m_regex; }
    bool isValid() const { return m_valid; }
    Qt::CaseSensitivity caseSensitivity() const { return m_cs; }
    const QString &pattern() const { return m_pattern; }

    // True when `text` matches the query. An empty pattern matches everything (the
    // axis is treated as inactive upstream); an invalid regex matches nothing.
    bool matches(const QString &text) const;

private:
    QString             m_pattern;
    bool                m_regex = false;
    Qt::CaseSensitivity m_cs = Qt::CaseInsensitive;
    QRegularExpression  m_re;
    bool                m_valid = true;
};

// The message-text axis: a matcher plus negation (hide matching, SPEC.md §6) and an
// individual enable flag (SPEC.md §6: filters toggle without being deleted).
struct TextFilter
{
    bool        enabled = false;
    bool        negate = false; // when true, HIDE records that match (exclude noise)
    TextMatcher matcher;

    // Active only when enabled with a non-empty pattern — an empty pattern would
    // match everything (or, negated, nothing), which is not a filter.
    bool active() const { return enabled && !matcher.isEmpty(); }
};

// The complete filter state for one Document (per-file, ARCHITECTURE.md §12). Every
// axis carries its own enable flag so it can be toggled in one click without losing
// its configuration (SPEC.md §6). Combination semantics: OR within an axis (the id
// sets), AND across axes (SPEC.md §6) — expressed by accepts() returning false on
// the first axis that rejects.
struct FilterSet
{
    static constexpr qint64 kMinTime = std::numeric_limits<qint64>::min();
    static constexpr qint64 kMaxTime = std::numeric_limits<qint64>::max();

    // By priority: a single MINIMUM level (SPEC.md §6). One `>=` test against the
    // severity-ordered enum (§7.2). Unknown (unparsed) is never hidden by it.
    bool     priorityEnabled = false;
    Priority minPriority = Priority::Trace;

    // By subsystem / by thread: OR-ed sets of interned ids (invariant #4). The pane
    // resolves checked names to ids via InternTable::idOf once, not per record.
    bool          loggerEnabled = false;
    QSet<quint32> loggerIds;
    bool          threadEnabled = false;
    QSet<quint32> threadIds;

    // By time range: UTC epoch-ms bounds (invariant #10). The pane converts the
    // display-zone values the user types into UTC ms once, here (§5.1). Records with
    // no timestamp (kNoTimestamp) are never hidden by a time bound.
    bool   timeEnabled = false;
    qint64 startMs = kMinTime; // inclusive
    qint64 endMs = kMaxTime;   // inclusive

    // By message text — the only axis without an integer fast path, evaluated LAST.
    TextFilter text;

    // Any axis actually narrowing the view. Drives whether the FilteredIndex
    // materializes a visible subset at all (an all-inactive set stays identity).
    bool anyActive() const
    {
        return priorityEnabled || loggerEnabled || threadEnabled || timeEnabled || text.active();
    }

    // Integer-only axes, in cheapest-first order (invariant #4). No decoding.
    bool acceptsIntegerAxes(const Record &r) const
    {
        // Priority min-level, but Unknown (unparsed, priority 0) is exempt so a
        // minimum level never hides plain-text lines (§7.2).
        if (priorityEnabled && r.priorityEnum() != Priority::Unknown
            && r.priorityEnum() < minPriority)
            return false;
        // Id 0 is InternTable's "field absent" sentinel — an unparsed plain-text
        // record, or one whose pattern has no %c/%t. Exempt, for the same reason
        // Unknown priority is: a record that never had the field must not be hidden
        // by a filter ON that field, or enabling the subsystem axis would silently
        // swallow every plain-text line (SPEC.md §4 promises they stay visible).
        if (loggerEnabled && r.loggerId != 0 && !loggerIds.contains(r.loggerId))
            return false;
        if (threadEnabled && r.threadId != 0 && !threadIds.contains(r.threadId))
            return false;
        if (timeEnabled && r.timestamp != Record::kNoTimestamp
            && (r.timestamp < startMs || r.timestamp > endMs))
            return false;
        return true;
    }

    // Apply the message-text axis to an already-decoded message. Separated so the
    // decode can be deferred by the caller until the integer axes have passed.
    bool acceptsText(const QString &message) const
    {
        if (!text.active())
            return true;
        const bool matched = text.matcher.matches(message);
        return matched != text.negate; // negate => hide matching
    }

    // The full predicate with integers FIRST and text LAST (invariant #4): `msg`
    // (the decode) is invoked ONLY when every integer axis has passed and the text
    // axis is active, so a decode never happens for a record an integer test
    // already rejected. `msg` is any callable returning the record's message text.
    template <class MessageFn>
    bool accepts(const Record &r, MessageFn &&msg) const
    {
        if (!acceptsIntegerAxes(r))
            return false;
        if (!text.active())
            return true;
        return acceptsText(std::forward<MessageFn>(msg)());
    }
};

// Find / Find Next (SPEC.md §5). Shares the text-matching code with the message
// filter but NOT its mechanism: it walks the currently-visible rows from the cursor
// and returns a row, changing no filter state. Pure and testable — the UI supplies
// `match` as a closure over the visible model rows.
namespace Find {

// Search `count` visible rows for the next (or previous) match relative to `from`
// (the current row, or -1 to start from the first/last row). Steps one row at a
// time, calling match(row); wraps around once when `wrap` is set. Returns the
// matched row, or -1 when nothing matches. Never mutates any filter state.
int search(int count, int from, bool forward, bool wrap,
           const std::function<bool(int)> &match);

} // namespace Find

} // namespace loftail
