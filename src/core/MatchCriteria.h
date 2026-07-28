#pragma once

#include "Filter.h"
#include "Priority.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace loftail {

struct LogFormat;
class RecordIndex;

// The PORTABLE form of the five match axes (SPEC.md §6, §7). `FilterSet` is the
// *resolved* form — interned quint32 ids, UTC epoch ms, a compiled regex — and it is
// on the hot path, so it must stay that way. What the panes edit and what gets
// persisted is this: subsystem/thread NAMES, a priority level, display-zone wall
// clock, a pattern string. Names and wall clock survive a re-index, a file change and
// a time-zone change; ids and UTC ms do not.
//
// One type, two consumers (ARCHITECTURE.md §7.2, §8):
//   - the Filters pane resolves it into the Document's FilterSet;
//   - a HighlightRule *embeds* it, and HighlighterSet::resolve() turns each rule's
//     copy into a per-rule FilterSet evaluated in LogModel's paint path.
// That is what keeps filtering and highlighting matching on the same criteria
// instead of drifting into two half-overlapping axis sets.
//
// Combination semantics are FilterSet's (SPEC.md §6): OR within an axis, AND across
// axes, integers before the message-text decode.

// Whether a record MISSING the field an axis tests passes that axis. The one place
// filtering and highlighting genuinely disagree:
//   Matches       — filtering. SPEC.md §4 promises unparsed plain-text lines stay
//                   visible, so a filter on a field they never carried must not hide
//                   them.
//   DoesNotMatch  — highlighting. SPEC.md §7: a rule keyed on a field a record lacks
//                   must not color it, or a subsystem rule would paint every
//                   plain-text line.
enum class AbsentField { Matches, DoesNotMatch };

// Whether an axis whose selection excludes NOTHING is written inactive.
//   Collapse — filtering. The subsystem and priority axes ship enabled (SPEC.md §6),
//              and "enabled at TRACE" / "every subsystem ticked" narrows nothing;
//              collapsing keeps FilteredIndex on its allocation-free identity path
//              (ARCHITECTURE.md §7.2). Exact, not heuristic.
//   Keep     — highlighting. "≥TRACE" and "every subsystem" are legitimate *color
//              everything parsed* rules, and there is no compact index to protect.
enum class NoOpAxes { Collapse, Keep };

// The severity-ordered choice list behind every minimum-priority selector, shared so
// the Filters and Highlighters panes cannot drift. Index 0 is TRACE, the lowest
// selectable minimum (the one that narrows nothing).
namespace PriorityChoice {
int      count();
Priority at(int index);
int      indexOf(Priority p);
} // namespace PriorityChoice

struct MatchCriteria
{
    // By priority: a single MINIMUM level (SPEC.md §6, §7).
    bool     priorityEnabled = false;
    Priority minPriority = Priority::Trace;

    // By subsystem / by thread: OR-ed sets of NAMES, resolved to interned ids in
    // resolve(). A name the file has not emitted yet resolves to nothing and matches
    // no record until it appears.
    bool        loggerEnabled = false;
    QStringList loggerNames;
    bool        threadEnabled = false;
    QStringList threadNames;

    // Whether the selection above covers every value the user was OFFERED, i.e. the
    // axis excludes nothing. Set by the editor from its own list, never inferred from
    // the intern table: the table grows mid-scan and the list lags it, so asking the
    // table would make a subsystem discovered-but-not-yet-listed look excluded and
    // silently hide its records (ARCHITECTURE.md §7.2, "newly discovered values
    // arrive selected"). Read only under NoOpAxes::Collapse; not persisted, because
    // it is recomputed from the repopulated list on restore.
    bool loggerCoversAll = true;
    bool threadCoversAll = true;

    // By time range: display-zone WALL CLOCK. The single "in" conversion to UTC ms
    // (invariant #10, ARCHITECTURE.md §5.1) happens in resolve(), so moving the
    // display zone re-points these correctly instead of shifting the instant.
    bool      timeEnabled = false;
    QDateTime start;
    QDateTime end;

    // By message text — the only axis without an integer fast path, evaluated LAST.
    // Carries its own enable flag, negation and the compiled-once TextMatcher.
    TextFilter text;

    // True when some axis is configured to narrow anything. Highlighting uses this
    // (via FilterSet::anyActive() after resolve()) as its inertness rule: a freshly
    // added, unconfigured rule matches no record.
    bool anyActive() const;

    // Resolve names to interned ids and wall clock to UTC ms, producing the predicate
    // `FilterSet`. `format` gates the thread and time axes on the pattern actually
    // carrying those fields (SPEC.md §6); `displayZone` interprets the typed bounds.
    FilterSet resolve(const RecordIndex &idx, const LogFormat &format,
                      const QTimeZone &displayZone, AbsentField absent,
                      NoOpAxes noOps) const;

    // Portable JSON — names and levels, never ids or UTC ms. The key names are
    // FilterPane's original ones, unchanged, so existing filter presets, exported
    // preset files and stored sessions keep loading byte-identically and neither
    // PresetStore::kSchemaVersion nor SessionStore::kSchemaVersion needs a bump
    // (both check the version with exact equality and no migration path — a bump
    // would silently discard every saved preset).
    QJsonObject         toJson() const;
    static MatchCriteria fromJson(const QJsonObject &o);
};

} // namespace loftail
