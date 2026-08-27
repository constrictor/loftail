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
    // arrive selected").
    //
    // Persisted — conditionally, see toJson — because it is the ONLY thing that tells
    // "the user selected nothing" from "nothing had been offered yet", and the name
    // list cannot: both are the empty list. A pane stashed before its log had been
    // indexed lists nothing and covers everything, and reading that back as the None
    // button's deliberate empty selection is what used to leave every tab but the last
    // showing none of its records. AxisEditor::setCriteria() reads it to choose the
    // rule the value lists are rebuilt under; resolve() reads it to collapse an axis
    // that narrows nothing, under NoOpAxes::Collapse only.
    bool loggerCoversAll = true;
    bool threadCoversAll = true;

    // Whether the selection above is a deliberate RESTRICTION rather than a snapshot
    // of everything the file had shown so far. It exists because the two are
    // indistinguishable from the name list alone and the editor must treat them
    // oppositely when the scan turns up a value nobody has seen yet: a hand-ticked
    // list is a statement about the whole file and widens with it (SPEC.md §6, "every
    // discovered subsystem starts selected, including ones that first appear later"),
    // while "show only net.http" — the record menu's edit, SPEC.md §5 — must not
    // quietly grow to name a subsystem discovered an hour into a tail.
    //
    // Persisted, unlike coversAll: it is part of what the selection MEANS, so a
    // restored session or an applied preset must not widen where the original did
    // not. Written only when true, so every state that predates it — and every state
    // that does not use it — serializes byte-identically and no schema bump is
    // needed (see toJson).
    bool loggerRestrictive = false;
    bool threadRestrictive = false;

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

    // Value equality over EVERY field above, in the order they are declared — the
    // list is the point, not the convenience. It answers the Highlighters tab's
    // marker, which is "are this log's rules still the ones loftail seeded"
    // (HighlighterPane::hasCustomRules, ARCHITECTURE.md §7.5): a field left out here
    // is a field the user can edit with nothing on screen changing, and it fails
    // silently, in the direction of saying nothing. So a new axis — or a new flag on
    // an existing one — belongs in this comparison in the same commit that adds it.
    //
    // coversAll is compared like the rest although it is derived: it only ever moves
    // as a consequence of an edit the user made in the axis editor, so treating it as
    // part of the value costs nothing and leaving it out would be a second rule to
    // remember. It is also why fromJson falls an absent key back on what the name list
    // implies rather than on some third value — the seeded highlight rules name no
    // subsystem and cover everything, and a default that read back differently would
    // light the Highlighters tab's marker on every log written by an older build.
    bool operator==(const MatchCriteria &o) const
    {
        return priorityEnabled == o.priorityEnabled && minPriority == o.minPriority
            && loggerEnabled == o.loggerEnabled && loggerNames == o.loggerNames
            && threadEnabled == o.threadEnabled && threadNames == o.threadNames
            && loggerCoversAll == o.loggerCoversAll && threadCoversAll == o.threadCoversAll
            && loggerRestrictive == o.loggerRestrictive
            && threadRestrictive == o.threadRestrictive
            && timeEnabled == o.timeEnabled && sameBound(start, o.start)
            && sameBound(end, o.end) && text == o.text;
    }
    bool operator!=(const MatchCriteria &o) const { return !(*this == o); }

    // Portable JSON — names and levels, never ids or UTC ms. The key names are
    // FilterPane's original ones, unchanged, so existing filter presets, exported
    // preset files and stored sessions keep loading byte-identically and neither
    // PresetStore::kSchemaVersion nor SessionStore::kSchemaVersion needs a bump
    // (both check the version with exact equality and no migration path — a bump
    // would silently discard every saved preset).
    QJsonObject         toJson() const;
    static MatchCriteria fromJson(const QJsonObject &o);

private:
    // Two time bounds are the same bound when they are the same INSTANT, or when
    // neither names one. Spelled out rather than left to QDateTime::operator==,
    // because an unset bound is an *invalid* QDateTime — which is what both a default
    // rule and a bound read back from an empty ISO string hold — and what Qt makes of
    // comparing two of those is a detail of the version in front of you, not a
    // promise (the floor is 6.4, ARCHITECTURE.md §1). Valid ones compare as instants,
    // which is right: the bound is the moment, never the digits (§5.1).
    static bool sameBound(const QDateTime &a, const QDateTime &b)
    {
        if (!a.isValid() || !b.isValid())
            return a.isValid() == b.isValid();
        return a == b;
    }
};

} // namespace loftail
