#pragma once

#include "Filter.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"
#include "Record.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimeZone>
#include <QVector>

#include <optional>
#include <utility>

namespace loftail {

struct LogFormat;
class RecordIndex;

// M5 — Highlighting (SPEC.md §7, ARCHITECTURE.md §8, invariant #4). A highlight
// rule colors matching records in place — it never removes them. Rules are an
// ORDERED list evaluated first-match-wins; the matched rule supplies a background
// AND a foreground role, each either a palette INDEX (0..11) or `kDefault` meaning
// "leave that role at the theme's normal color".
//
// A rule matches on the SAME five axes as a filter — subsystem, thread, priority,
// time range and message text — by embedding a `MatchCriteria` (SPEC.md §6, §7).
// The criteria carry NAMES, levels and wall clock, so a rule survives export/import,
// a re-index and a time-zone change; HighlighterSet::resolve() turns each rule's copy
// into a `FilterSet` whose integer axes the paint path compares directly.
//
// Two things differ from filtering, both expressed as arguments to that resolve
// rather than as a second predicate chain:
//   - AbsentField::DoesNotMatch — a record lacking the field a rule keys on is not
//     colored, the deliberate inverse of §6's promise that it is not hidden.
//   - NoOpAxes::Keep — "≥TRACE" or "every subsystem" is a legitimate *color
//     everything parsed* rule, so no axis is collapsed away.
//
// Priority match semantics: MINIMUM level (`>=`), consistent with the filter axis
// (§7.2). A rule matching "at least WARN" colors WARN, ERROR and FATAL; order the
// list high-severity-first for a per-level look.
struct HighlightRule
{
    bool enabled = true;

    // The five match axes. A rule with no active axis is inert (see match()), so a
    // freshly added, unconfigured rule colors nothing until an axis is set.
    MatchCriteria match;

    // The two roles. Each is a palette index (0..11) or HighlightPalette::kDefault.
    int background = HighlightPalette::kDefault;
    int foreground = HighlightPalette::kDefault;

    // Portable JSON: names and palette INDICES only — never ids, never RGB — so an
    // exported rule imports on any theme and after a re-index (ARCHITECTURE.md §8).
    // The axes go under a nested "match" object; fromJson() still reads the original
    // flat two-axis keys when that object is absent, so highlighter presets, exported
    // files and sessions written before the axis set grew keep loading. That
    // backward read is what makes a PresetStore/SessionStore schema bump unnecessary
    // — and both stores gate on exact version equality with no migration, so a bump
    // would silently discard every preset a user already has.
    QJsonObject toJson() const;
    static HighlightRule fromJson(const QJsonObject &o);
};

// The complete highlight state for one Document (per-file, invariant #7). Holds the
// ordered rule list plus, per rule, its criteria resolved to the integer-comparing
// `FilterSet` the paint path evaluates (invariant #4).
class HighlighterSet
{
public:
    QVector<HighlightRule> rules;

    // Resolve every rule's criteria against `idx` (which owns the intern tables),
    // `format` (which gates the thread and time axes) and `displayZone` (which
    // interprets the typed time bounds). Must be re-run whenever the intern tables
    // grow, the rule list changes, or the display zone moves. Cheap — a hash lookup
    // per name and one regex compile per text axis, all of it kept OFF the paint
    // path. After this, match() compares integers and runs an already-compiled regex.
    void resolve(const RecordIndex &idx, const LogFormat &format,
                 const QTimeZone &displayZone);

    // First-match-wins over the ENABLED rules. `msg` is any callable returning the
    // record's decoded message text; it is invoked ONLY when a rule's integer axes
    // have already passed AND that rule has an active text axis, and its result is
    // memoized across rules — so N text rules still cost at most ONE decode, and a
    // record no rule's integer axes admit costs none (invariant #4, §7.2, exactly the
    // ordering FilterSet::accepts uses).
    //
    // Returns the index of the matching rule, or -1 when none matches (the record
    // keeps its un-highlighted appearance). A rule whose criteria have no active axis
    // never matches, so an unconfigured rule is inert.
    template <class MessageFn>
    int match(const Record &r, MessageFn &&msg) const
    {
        static const FilterSet kInert; // no active axis => never matches

        std::optional<QString> message;
        for (int i = 0; i < rules.size(); ++i) {
            if (!rules.at(i).enabled)
                continue;
            const FilterSet &fs = (i < m_resolved.size()) ? m_resolved.at(i) : kInert;
            if (!fs.anyActive() || !fs.acceptsIntegerAxes(r))
                continue;
            if (fs.text.active()) {
                if (!message)
                    message = msg();
                if (!fs.acceptsText(*message))
                    continue;
            }
            return i;
        }
        return -1;
    }

    // Convenience for callers with no decode available (the integer axes are all
    // they can evaluate). A rule with an active text axis sees an EMPTY message, so
    // it matches nothing — use the MessageFn overload wherever the text is reachable.
    int match(const Record &r) const
    {
        return match(r, [] { return QString(); });
    }

    // True when some enabled rule is actually configured to match something. The
    // paint path's early-out: with no such rule there is nothing to evaluate and no
    // decode to risk.
    bool anyEnabled() const;

    // Portable JSON round-trip for presets, export/import, and per-file session
    // persistence (all name/index based — theme- and reindex-portable, §8).
    QJsonArray toJson() const;
    static HighlighterSet fromJson(const QJsonArray &a);

private:
    // Parallel to `rules`: rules[i]'s criteria resolved to the integer predicate.
    // Sized to rules.size() by resolve(); match() tolerates a shorter/absent vector
    // by treating the rule as inert until resolve() runs.
    QVector<FilterSet> m_resolved;
};

} // namespace loftail
