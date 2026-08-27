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

#include "Priority.h"
#include "Record.h"

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QVector>
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

    // Where inside `text` the query matched, as character spans — the SAME decision
    // matches() makes, reported positionally so a view can mark on screen what the
    // search found (SPEC.md §5, ARCHITECTURE.md §7.1.4). Not a second matcher: regex
    // vs substring and the case option are read from this one object, which is what
    // stops a mark and a hit ever disagreeing.
    //
    // Spans never overlap and are in ascending order. A zero-width regex match yields
    // no span — there is nothing on screen to mark. `limit` caps how many are
    // returned (non-positive means no bound), because the caller is a paint path and
    // a one-character query over a hundred-thousand-character message would otherwise
    // hand back one span per character.
    struct Span
    {
        int start = 0;
        int length = 0;
    };
    QVector<Span> spans(const QString &text, int limit = -1) const;

    // Value equality over WHAT WAS TYPED — the pattern, the regex flag and the case
    // option — and nothing else. m_re and m_valid are derived from those three by
    // set(), so a comparison that included them would either be redundant or, if one
    // day the compile became lazy, wrong. Exists for the Highlighters tab's marker,
    // which asks whether a log's rules still are the ones loftail seeded
    // (HighlighterPane::hasCustomRules), so every field a rule carries has to be
    // reachable by ==.
    bool operator==(const TextMatcher &o) const
    {
        return m_pattern == o.m_pattern && m_regex == o.m_regex && m_cs == o.m_cs;
    }
    bool operator!=(const TextMatcher &o) const { return !(*this == o); }

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

    // Every field, including a pattern sitting behind a switched-off axis: it is
    // still something the user typed, and the marker this serves reports what they
    // set rather than what is currently in force.
    bool operator==(const TextFilter &o) const
    {
        return enabled == o.enabled && negate == o.negate && matcher == o.matcher;
    }
    bool operator!=(const TextFilter &o) const { return !(*this == o); }
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

    // Whether a record MISSING the field an axis tests passes that axis. The one
    // place filtering and highlighting genuinely disagree (SPEC.md §6 vs §7):
    //   true  — FILTERING. An unparsed plain-text line has no subsystem, thread,
    //           priority or timestamp; a filter on a field it never carried must not
    //           hide it, or enabling the subsystem axis would swallow every such line
    //           (SPEC.md §4 promises they stay visible).
    //   false — HIGHLIGHTING. A rule keyed on a field a record lacks must not color
    //           it, or a subsystem rule would paint every plain-text line.
    // Same axes, same predicate, opposite exemption — hence a flag rather than a
    // second copy of the chain (ARCHITECTURE.md §7.2).
    bool absentFieldMatches = true;

    // Any axis actually narrowing the view. Drives whether the FilteredIndex
    // materializes a visible subset at all (an all-inactive set stays identity), and
    // supplies highlighting's inertness rule: a rule with no active axis matches
    // nothing (SPEC.md §7).
    bool anyActive() const
    {
        return priorityEnabled || loggerEnabled || threadEnabled || timeEnabled || text.active();
    }

    // Integer-only axes, in cheapest-first order (invariant #4). No decoding.
    //
    // Each axis first asks whether the record even CARRIES the field it tests, and
    // defers to absentFieldMatches when it does not: id 0 is InternTable's "field
    // absent" sentinel (an unparsed plain-text record, or one whose pattern has no
    // %c/%t), Priority::Unknown is the unparsed priority, and kNoTimestamp the
    // unparsed time. See absentFieldMatches for why the answer differs between
    // filtering and highlighting.
    bool acceptsIntegerAxes(const Record &r) const
    {
        if (priorityEnabled) {
            const Priority p = r.priorityEnum();
            if (p == Priority::Unknown) {
                if (!absentFieldMatches)
                    return false;
            } else if (p < minPriority) {
                return false;
            }
        }
        if (loggerEnabled) {
            if (r.loggerId == 0) {
                if (!absentFieldMatches)
                    return false;
            } else if (!loggerIds.contains(r.loggerId)) {
                return false;
            }
        }
        if (threadEnabled) {
            if (r.threadId == 0) {
                if (!absentFieldMatches)
                    return false;
            } else if (!threadIds.contains(r.threadId)) {
                return false;
            }
        }
        if (timeEnabled) {
            if (r.timestamp == Record::kNoTimestamp) {
                if (!absentFieldMatches)
                    return false;
            } else if (r.timestamp < startMs || r.timestamp > endMs) {
                return false;
            }
        }
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

// Where the match that was landed on sits among the others, so the bar can say
// "3 of 47" (SPEC.md §5). Counting them is a different question from finding one:
// search() stops at the first hit, whereas a total means asking `match` about EVERY
// visible row, and `match` decodes a record's text (invariant #1). Over a
// multi-million-record log that is not something to do on a keystroke, so the count
// is BOUNDED and says whether it finished (ARCHITECTURE.md §7.1.3).
struct Tally
{
    int  total = 0;        // matches seen within the bound
    int  index = 0;        // 1-based position of `hit` among them; 0 == never reached
    bool complete = false; // the whole view was counted, so `total` is the real total
};

// Count matching rows over [0, count), giving up after `rowLimit` rows or `msLimit`
// milliseconds, whichever comes first — either bound non-positive means "no bound".
// `hit` is the row search() landed on; when the count stops short of it, `index`
// stays 0 and there is no position to report. Mutates nothing.
Tally tally(int count, int hit, int rowLimit, int msLimit,
            const std::function<bool(int)> &match);

} // namespace Find

} // namespace loftail
