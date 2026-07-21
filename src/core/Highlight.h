#pragma once

#include "Palette.h"
#include "Priority.h"
#include "Record.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace loftail {

class RecordIndex;

// M5 — Highlighting (SPEC.md §7, ARCHITECTURE.md §8, invariant #4). A highlight
// rule colors matching records in place — it never removes them. Rules are an
// ORDERED list evaluated first-match-wins; the matched rule supplies a background
// AND a foreground role, each either a palette INDEX (0..11) or `kDefault` meaning
// "leave that role at the theme's normal color".
//
// The match axes are the same integer axes as filtering (invariant #4): a set of
// interned subsystem ids and/or a minimum priority level. A rule carries subsystem
// NAMES (portable across files and across the light/dark theme, so a rule survives
// export/import and a re-index), which HighlighterSet::resolve() turns into the
// interned id sets the hot path in LogModel::data() compares — never strings.
//
// Priority match semantics: MINIMUM level (`>=`), consistent with the filter axis
// (§7.2). A rule matching "at least WARN" colors WARN, ERROR and FATAL; order the
// list high-severity-first for a per-level look. Unparsed records (Unknown) are
// exempt, exactly as the priority filter exempts them.
struct HighlightRule
{
    bool enabled = true;

    // Subsystem axis: OR over these interned-by-name subsystems. Active only when
    // matchLogger is set (an axis toggles without losing its configuration, §7).
    bool        matchLogger = false;
    QStringList loggerNames;

    // Priority axis: a single minimum level, one `>=` test (invariant #4, §7.2).
    bool     matchPriority = false;
    Priority minPriority = Priority::Warn;

    // The two roles. Each is a palette index (0..11) or HighlightPalette::kDefault.
    int background = HighlightPalette::kDefault;
    int foreground = HighlightPalette::kDefault;

    // Portable JSON: names and palette INDICES only — never ids, never RGB — so an
    // exported rule imports on any theme and after a re-index (ARCHITECTURE.md §8).
    QJsonObject toJson() const;
    static HighlightRule fromJson(const QJsonObject &o);
};

// The complete highlight state for one Document (per-file, invariant #7). Holds the
// ordered rule list plus, per rule, the resolved interned-id set for its subsystem
// axis so match() does integer comparisons only (invariant #4).
class HighlighterSet
{
public:
    QVector<HighlightRule> rules;

    // Resolve every rule's subsystem NAMES to interned ids against `idx` (which owns
    // the logger intern table). Must be re-run whenever the intern table grows (the
    // scan discovers more subsystems) or the rule list changes; cheap — a hash
    // lookup per name. After this, match() compares ids, not strings.
    void resolve(const RecordIndex &idx);

    // First-match-wins over the ENABLED rules using the resolved id sets and the
    // integer priority test. Returns the index of the matching rule, or -1 when no
    // rule matches (the record keeps its un-highlighted appearance). A rule with no
    // active axis never matches, so a freshly-added, unconfigured rule is inert.
    int match(const Record &r) const;

    bool anyEnabled() const;

    // Portable JSON round-trip for presets, export/import, and per-file session
    // persistence (all name/index based — theme- and reindex-portable, §8).
    QJsonArray toJson() const;
    static HighlighterSet fromJson(const QJsonArray &a);

private:
    // Parallel to `rules`: rules[i]'s subsystem axis resolved to interned ids. Sized
    // to rules.size() by resolve(); match() tolerates a shorter/absent vector (an
    // unresolved logger axis simply matches no record until resolve() runs).
    QVector<QSet<quint32>> m_resolvedLoggerIds;
};

} // namespace loftail
