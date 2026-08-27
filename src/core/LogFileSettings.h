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

#include "LogProfile.h"
#include "Record.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace loftail {

// WHICH RUN was being read (SPEC.md §3a). The same three fields DocumentContext's
// RunRestore carried, moved into core because they are now a STORED value and not only a
// restore hint.
//
// "Follow the last" is a MODE and not a run, so it is spelled as the ABSENCE of an offset —
// exactly the spelling saveSession() already used, which is why carrying it needed no new
// vocabulary and no schema anywhere had to move. A saved session could not come back
// pinned to a run that has since finished, and neither can a saved record.
struct RunSelection
{
    bool   all = false;
    qint64 startOffset = -1;
    qint64 startTimestamp = Record::kNoTimestamp;

    // Nothing worth storing: follow whichever run is last, which is what a log opens in.
    bool saysNothing() const { return !all && startOffset < 0; }

    bool operator==(const RunSelection &o) const
    {
        return all == o.all && startOffset == o.startOffset
            && startTimestamp == o.startTimestamp;
    }
    bool operator!=(const RunSelection &o) const { return !(*this == o); }
};

// EVERYTHING ONE LOG SAYS ABOUT ITSELF — one JSON file per log under
// <AppConfigLocation>/fileSettings/ (SPEC.md §4, §10; ARCHITECTURE.md §8.2). This is the
// settings tree's FILE level, moved out of logsettings.json's `files[]`, plus the per-file
// half of what the QSettings session used to carry: the filters, the highlight rules and
// the run. The two levels above — the defaults and the ordered pattern list — stay in
// logsettings.json (LogSettings.h).
//
// THE WHOLE RECORD OBEYS ONE RULE: it exists only while it says something its parents do
// not. That rule was LogSettingsTree::setFileProfile()'s, applied to the profile alone;
// it is now applied SECTION BY SECTION by reduce(), and a section that has fallen back
// into line is dropped before the record is weighed. When nothing is left the file is
// deleted and its slot freed. Without it, opening a log and changing nothing would leave
// an entry behind — and in a pool of 500 slots that means evicting a log somebody
// configured to make room for one nobody did.
//
// What each section's parent is, and therefore what "says nothing" MEANS, differs:
//
//   profile       LogSettingsTree::inherited(address) — the matching pattern, or defaults
//   filters       a pristine Filters pane, i.e. no axis narrowing anything
//   highlighters  HighlighterSet::defaults(), the three level colours every log opens with
//   run           "follow the last run", which names no run
struct LogFileSettings
{
    // ALWAYS the logSettingsKey() form, and stored INSIDE the slot file as well as in the
    // map. That duplication is the whole of the stale-slot cure — LogFileStore.h.
    QString address;

    // nullopt IS the `inherited` mark: the log adds nothing to what its pattern or the
    // defaults say, and this record is here for its filters, its rules or its run.
    // Serialized as the STRING "inherited" rather than as an absent key, so a record that
    // states filters and nothing else still reads as a deliberate answer about the format
    // rather than as a file that was half written.
    //
    // Taken WHOLE when it is present. The levels are never merged field by field
    // (ARCHITECTURE.md §8) — a record differing from its pattern in one field carries a
    // complete copy of the rest, which is what Promote to Parent Pattern exists to undo.
    std::optional<LogProfile> profile;

    // The Filters pane's portable snapshot (FilterPane::saveState) — names and levels,
    // never interned ids, so it survives a re-index and moves between logs. An EMPTY
    // object means the pane's defaults, which is what a freshly-opened log gets.
    QJsonObject filters;

    // PRESENCE, NEVER EMPTINESS, and the reason this is an optional and not a bare array.
    // An empty rules array is the user having deleted every rule and must stay deleted;
    // absent is "nobody has ever said anything", which the caller seeds with
    // HighlighterSet::defaults(). Reading the emptiness instead re-seeds the three level
    // colours on every launch — the same trap HighlightRule::fromJson records for
    // "actions" and LogSettingsStore for "pattern", now four stores deep, and the one
    // tst_sessiongui::aDeletedDefaultRuleStaysDeletedAcrossARelaunch exists for.
    std::optional<QJsonArray> highlighters;

    RunSelection run;

    // Drop every section that has come back into line with its parent.
    // LogFileStore::save() calls this before deciding whether there is a file at all, so
    // the redundancy rule cannot be applied in one place and forgotten in another.
    //
    // The highlighters test compares the whole rule list IN ORDER against
    // HighlighterSet::defaults() — order is meaning, first-match-wins being per action.
    // It runs through HighlightRule::operator==, which tst_highlight::aRuleDiffersWhen-
    // AnyOneFieldOfItDoes keeps field-complete: A FIELD ADDED TO A RULE AND NOT TO THAT
    // COMPARISON makes every log's rules look like the seed and silently deletes their
    // records.
    void reduce(const LogProfile &inheritedProfile);

    // Whether anything survives reduce(). No I/O, no allocation worth naming.
    bool saysSomething() const;

    QJsonObject            toJson() const;
    static LogFileSettings fromJson(const QJsonObject &o);

    // Value equality over every field, for the change gate the writer needs. The record
    // is assembled on a tab switch, on a filter edit, on a run change and on every resume
    // of a remote log, and an atomic rewrite per poll of an identical record is exactly
    // the cost LogSettingsTree::setFileProfile()'s own gate existed to avoid.
    bool operator==(const LogFileSettings &o) const;
    bool operator!=(const LogFileSettings &o) const { return !(*this == o); }
};

// Whether a saved Filters pane state narrows anything.
//
// NOT a byte comparison against a pristine pane's JSON, and not
// `MatchCriteria::fromJson(state) == MatchCriteria{}` either. AxisEditor::criteria() is
// not the inverse of setCriteria(): a QDateTimeEdit ALWAYS holds a datetime, so an
// untouched time axis reads back as a valid 2000-01-01 bound and both of those spellings
// would call every log's filters customised — the same non-inverse that used to rewrite
// this log's seeded highlight rules on a bare run click (CLAUDE.md, "Nothing writes a
// highlight rule back that the reader did not edit"). The bounds are therefore read only
// while `timeEnabled`, and a value axis switched off is read as covering everything.
bool filterStateSaysNothing(const QJsonObject &state);

} // namespace loftail
