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

#include "MatchCriteria.h"
#include "TimeDisplay.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QTimeZone>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAbstractButton;
class QComboBox;
class QDateTimeEdit;
class QDoubleSpinBox;
class QGroupBox;
class QLineEdit;
class QHBoxLayout;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QVBoxLayout;
QT_END_NAMESPACE

namespace loftail {

class Document;

// The two axes that offer a pick-list of discovered values, named so a caller can
// say which one it means without reaching for the widgets (SPEC.md §6).
enum class ValueAxis { Subsystem, Thread };

// Which end of the time range an edit sets.
enum class TimeBound { Start, End };

// The editor for the five match axes — priority, subsystem, thread, message text,
// time range (SPEC.md §6, §7). ONE widget, two users:
//
//   - `FilterPane` embeds it directly: its criteria become the Document's FilterSet.
//   - `HighlighterPane` embeds it inside a scroll area as the editor for the
//     currently selected rule, whose HighlightRule embeds the same MatchCriteria.
//
// Sharing it is the point. Highlighting and filtering match on the same criteria, so
// a subsystem list that learns a name mid-scan, a regex that reports itself invalid,
// or a time bound that survives a zone change must behave identically in both places;
// two parallel implementations would drift on the first change to either.
//
// It owns no document state. It reads the bound Document only for its discovered
// intern tables, its format (which axes the pattern supports) and its display zone
// (how to render and interpret typed time bounds); everything the user chooses leaves
// through criteria() as names, levels and wall clock (never ids or UTC ms), so a
// snapshot stays portable across files, themes, re-indexing and zone changes.
class AxisEditor : public QWidget
{
    Q_OBJECT

public:
    // Which axes start enabled. The Filters pane ships subsystem on so its controls act
    // on the first click (SPEC.md §6, and applyToDocument collapses the resulting no-op
    // state). A highlight rule starts with nothing on: every axis is opt-in, and an
    // unconfigured rule must stay inert (SPEC.md §7).
    //
    // Priority ships OFF in both, and used to ship on in the Filters pane. The reason it
    // shipped on was that its lowest offered level was TRACE, which narrows nothing, so
    // the axis could be ticked without hiding a record and the combo would then act on
    // the first click instead of needing the box ticked first. TRACE is no longer
    // offered (kDefaultFloor), so "ticked" and "narrows nothing" can no longer both be
    // true, and shipping it on would hide every DEBUG record in every log on open. The
    // first-click argument goes with it rather than being lost: Qt greys a checkable
    // box's body, so the combo cannot be changed at all while the axis is off, and the
    // "I moved the level and nothing happened" confusion it existed to prevent is now
    // unreachable by construction rather than by choosing a no-op default.
    struct Defaults
    {
        bool priorityOn = false;
        bool loggerOn = false;
    };

    // The minimum level a fresh editor offers, and what clearAll() returns the combo
    // to. INFO because it is the level the axis is nearly always reached for and the
    // one log4cplus configurations most often emit at; the two below it are the ones a
    // user turns the axis ON to be rid of.
    static constexpr Priority kDefaultFloor = Priority::Info;

    explicit AxisEditor(Defaults defaults, QWidget *parent = nullptr);

    // The inset this widget leaves round its own group boxes, so a rounded frame does
    // not run into the edge of the frameless scroll area both users put it in. Public
    // because it is the number anything STACKED with the axes has to use to line up
    // with them — the Highlighters pane's action blocks sit directly under the last
    // axis, and six pixels of disagreement between two framed columns reads as a
    // rendering fault rather than as two sections.
    static constexpr int kSideMargin = 6;

    // Row 0 of a value list is NOT a value. "Others" stands for everything the list
    // does not hold — every subsystem or thread the scan has not reached yet — and its
    // tick IS the discovery rule (MatchCriteria::loggerRestrictive). It is marked with
    // this role rather than recognised by its label, because its label is translated
    // prose and nothing may be identified by the text it happens to show
    // (ARCHITECTURE.md §9.1). Public so a test can point at the row without reading it.
    static constexpr int kOthersRole = Qt::UserRole + 1;
    static bool          isOthersRow(const QListWidgetItem *item);
    // Every loop that walks VALUES starts here, because row 0 is the "Others" row and
    // is not one. Missing it is silent in both directions: "Others" would arrive in
    // criteria() as a subsystem nothing is ever logged under, and All / None / Invert
    // would set the discovery rule twice per click — Invert, which reads it back after
    // flipping the list, would then flip it straight back. Public for the same reason
    // as the role: a test counts values, and the count is off by this row.
    static constexpr int kFirstValueRow = 1;

    // Append a widget to the bottom of the MESSAGE TEXT axis's body, where it lives
    // and dies with that axis: greyed out with the rest of the body while the axis
    // is off.
    //
    // It exists for exactly one caller. Filter context (SPEC.md §6) widens the
    // message search and nothing else, so its two spinners belong beside that search
    // — but they are not a match criterion, are absent from MatchCriteria, and mean
    // nothing to a highlight rule, which is why they stay the FilterPane's own
    // widgets rather than becoming a sixth axis here. HighlighterPane calls this not
    // at all, and gets an editor with no context row.
    void addTextExtra(QWidget *w);

    // Rebind to a document (or nullptr to clear). Repopulates the auto-discovered
    // subsystem/thread lists from its intern tables, gates the thread and time axes on
    // whether the format carries those fields, and seeds the time editors to the
    // file's observed span so the pickers open near useful values.
    void setDocument(Document *document);
    Document *document() const { return m_document; }

    // Refresh everything the editor derives from the index — called as indexing
    // progresses (SPEC.md §6). Two things:
    //
    //   - the auto-discovered subsystem/thread lists, from the intern tables. A name
    //     never listed before arrives CHECKED; one the user unticked stays unticked.
    //   - the time editors' seed, via refreshObservedSpan().
    //
    // The time seed belongs here and not only in setDocument() because setDocument()
    // runs from activeDocumentChanged at OPEN time, before the scan has produced a
    // single record: observedSpan() fails, the editors keep QDateTimeEdit's year-2000
    // default, and ticking the time axis on a freshly opened log hides every record.
    // Does not emit changed(): a plain repopulation is not a user edit.
    void refreshDiscoveredLists();

    // Re-seed the time editors to the file's observed span, so the pickers open near
    // useful values instead of the year 2000. A no-op once the user has set a bound
    // by hand — theirs is the answer, however far outside the span it lands.
    void refreshObservedSpan();

    // Re-render the time editors after anything the timestamp column's rendering
    // depends on moves: the display MODE, the zone it derives, or — in "seconds from
    // run start" — the selected run, whose baseline the digits are counted from
    // (SPEC.md §4, §6). Preserves the instant, not the text: a bound means an instant,
    // and every one of those changes would otherwise leave the same digits naming a
    // different one. Also swaps which pair of editors is on screen, since the two
    // seconds modes ask for a number and the three wall-clock modes for a date.
    void refreshTimeBounds();

    // The axes as the user has them, in portable form.
    MatchCriteria criteria() const;

    // Load `c` into the controls. Applies a subsystem/thread selection that NARROWS
    // exactly — unlike refreshDiscoveredLists(), which treats a never-listed name as
    // checked — so switching between two highlight rules shows each rule's own
    // selection rather than inheriting the other's. An axis that narrows nothing
    // (MatchCriteria::loggerCoversAll, and not restrictive) is loaded as the statement
    // it is instead: everything listed ticked, and the discovery rule left armed for
    // whatever the scan has not reached. Does not emit changed().
    void setCriteria(const MatchCriteria &c);

    // False when the text axis holds a regex that failed to compile. The pattern edit
    // flags it inline; without this a malformed regex silently matches nothing.
    bool textPatternValid() const;

    // Whether the keyboard focus is in a control that produces a STREAM of edits as it
    // is used — the message query box, either time editor, either seconds editor. The
    // filter undo history merges consecutive changes from one of these into a single
    // entry (FilterUndoStack::record), so a typed word is one Esc rather than one per
    // letter. Everything else here — a tick, the priority combo, a group-box title, a
    // record-menu edit — is a discrete gesture and earns an entry of its own, which is
    // why this asks about the control and not merely about whether focus moved: two
    // ticks in one list never move focus at all.
    bool focusIsInAContinuousEditor() const;

    // Every axis back to the state a freshly-built editor is in — the Defaults it was
    // constructed with, every discovered value ticked again, and every text, manual
    // and restriction flag dropped. Emits changed() exactly once, so the owning pane
    // recomputes on the ordinary path.
    void clearAll();

    // --- Edits driven from a record, not typed (the record menu, SPEC.md §5) ----
    //
    // Each of these is ONE edit to ONE axis, made through the same controls a hand
    // edit uses and followed by exactly one changed(): the widgets stay the
    // authoritative state (ARCHITECTURE.md §12.3), so the pane resolves and applies
    // the result by its ordinary path, the user can see what happened by looking at the
    // axis, and the edit goes on the log's filter history as one entry like any other
    // (FilterUndoStack.h) — Escape takes it back.
    //
    // showOnlyValue() replaces the axis's selection with the single named value and
    // marks it RESTRICTIVE (MatchCriteria::loggerRestrictive): a value discovered
    // later must not join a selection the user made by pointing at one record.
    // hideValue() only unticks, leaving both the rest of the selection and the
    // discovery rule alone — excluding one subsystem says nothing about the next one
    // to appear. Both enable their axis, since neither means anything with it off.
    void showOnlyValue(ValueAxis axis, const QString &name);
    void hideValue(ValueAxis axis, const QString &name);

protected:
    // Ctrl+click on a row of either value list is "only this one" — the same edit the
    // record menu's Show Only makes, reached from the list itself (SPEC.md §6). It is
    // an event filter on the lists' viewports rather than a connection, because the
    // click has to be TAKEN: left alone, the same press would toggle one tick (on the
    // indicator) or move the selection (anywhere else), and either would land on top of
    // the exclusive edit.
    bool eventFilter(QObject *watched, QEvent *event) override;

public:

    // Set the minimum-level axis and enable it. Priority::Unknown has no selector
    // entry (an unparsed record carries no level) and is ignored.
    void setMinimumPriority(Priority p);

    // Set one end of the time range from a record's UTC ms, rendered in the display
    // zone (invariant #10 — this is the ordinary "out" conversion). The OTHER end is
    // pushed out to the file's observed span when it would otherwise make the range
    // empty, which is what turning the axis on from a single record would otherwise
    // do: the editors always hold some wall clock, and an unseeded end bound sits in
    // the year 2000.
    void setTimeBound(TimeBound which, qint64 utcMs);
    void setTimeRange(qint64 fromUtcMs, qint64 toUtcMs);

    // Whether the bound document's format carries the field an axis tests, so a
    // caller can leave out what it cannot offer rather than showing it dead
    // (SPEC.md §6).
    bool supportsThread() const;
    bool supportsTime() const;

signals:
    // Emitted on any user edit. Never emitted by setCriteria(), setDocument() or
    // refreshDiscoveredLists(), so the owning pane can load state without recursing.
    void changed();

private:
    void buildUi(Defaults defaults);
    // The subsystem and thread axes, which differ only in a title, an object-name
    // prefix, a list height and whether they ship enabled. Everything else — the
    // discovery rule, the narrowing, the manual add, the three list buttons — is one
    // body of code rather than two that have to be kept in step by hand.
    void buildValueAxis(QVBoxLayout *root, ValueAxis axis, const QString &title,
                        const QString &prefix, int listMinHeight, bool enabledByDefault);

    // The priority combo's current level, and how to select one. These exist because
    // the combo's row order is NO LONGER PriorityChoice's index order and must never be
    // assumed to be again: PriorityChoice is the PERSISTENCE table — MatchCriteria
    // writes `minPriorityIndex` as an index into it and reads it straight back — so
    // dropping TRACE from that table to drop it from this combo would silently reinterpret
    // every preset and session ever written, one level too severe (a saved INFO floor
    // reading back as WARN), with no version bump able to catch it: PresetStore gates on
    // exact equality and discards on any bump. So the table keeps all six levels, the
    // combo carries the Priority it means in each item's data, and the two are related
    // only through these two functions.
    Priority comboPriority() const;
    // Selects the item for `p`. Returns false when the combo does not offer it, which
    // is the case for TRACE alone and only from data written before it was dropped;
    // the combo is left on its default and it is the CALLER's job to decide what an
    // unofferable floor means, because the answer differs (see setCriteria and
    // setMinimumPriority — both mean "do not narrow", but reach it differently).
    bool setComboPriority(Priority p);
    // Whether `name` is worth offering to add: non-empty and not already listed.
    bool canAddTyped(ValueAxis axis, const QString &name) const;
    // Keep the All / None / Invert tooltips telling the truth about how many entries
    // the current narrowing is hiding from them.
    void updateListButtonHints(ValueAxis axis);
    void emitChanged();
    // Everything an axis's own state decides about its controls: whether the priority
    // combo is live (it has no group box to grey it), and whether an axis the format
    // cannot fill is on screen at all.
    void updateAxisState();
    void updateTextValidity();

    // How one value list decides what arrives ticked when it is rebuilt.
    enum class ListRule {
        // A name never listed before arrives checked, so an enabled-by-default axis
        // does not start dropping records mid-scan as the scan finds subsystems.
        // `restrictive` turns that half off without turning the rule into a load —
        // see MatchCriteria::loggerRestrictive.
        Discover,
        // Checked means exactly the given names: the caller is reproducing a stored
        // selection, not discovering values.
        Load,
        // Everything listed arrives checked, whether it was in the stored selection or
        // not, and Discover keeps ticking whatever turns up next. What an axis IN FORCE
        // that excluded NOTHING means (MatchCriteria::loggerCoversAll) — which is not
        // the same picture as its name list, because that list is only as long as the
        // scan had got when it was written.
        CoverAll,
        // Load, except that an unticked name is not recorded as SEEN. What a switched-OFF
        // axis that excluded nothing means: it has no selection to show, so nothing is
        // ticked — and it excluded nothing, so nothing may be recorded as excluded
        // either, which is what leaves the discovery rule armed for whenever the axis
        // is switched on. Nothing arrives ticked, so nothing materialises into
        // criteria() for a highlight rule that does not use this axis.
        Unstated,
    };

    // Repopulate one checkable list under `rule`. Answers whether the rows on screen
    // actually moved: this runs on every ingest tick of a growing log, where the names
    // and their ticks are usually identical to what is already listed, and a rebuild
    // that changes nothing still empties and refills the widget in front of the user.
    // A false answer means every row, tick, hidden flag and the scroll position were
    // left alone, so a caller may skip whatever it would have done to follow the
    // rebuild.
    bool populateList(QListWidget *list, const QStringList &names,
                      const QSet<QString> &checked, const QSet<QString> &manual,
                      QHash<QString, bool> &seen, ListRule rule, bool restrictive);
    static bool   allChecked(const QListWidget *list);
    // What criteria() writes into MatchCriteria::loggerCoversAll / threadCoversAll:
    // allChecked(), plus the one state the ticks cannot express. An axis that is
    // switched OFF has no selection on screen — a stored one loads under
    // ListRule::Unstated, which ticks nothing and records nothing as seen — so reading
    // its empty ticks as "covers nothing" invents a narrowing the user never asked for
    // and writes it into the rule, the session and every preset. Off with nothing
    // ticked is therefore the default, `true`, which is also what the axis was built
    // from and what fromJson() infers from an empty name list; the two answers are
    // indistinguishable while the axis is off, and only this one round-trips.
    bool          coversAllFor(ValueAxis axis) const;
    static QSet<QString> checkedNames(const QListWidget *list);
    static void   setAllChecked(QListWidget *list, bool checked);
    static void   invertChecked(QListWidget *list);
    static void   narrowList(QListWidget *list, const QString &needle);
    // Per axis, because the two are loaded under rules that need not agree: a stored
    // state can cover every subsystem and name three threads.
    void          repopulate(const QSet<QString> &loggerChecked,
                             const QSet<QString> &threadChecked, ListRule loggerRule,
                             ListRule threadRule);

    // The widgets and per-axis state behind ValueAxis, so the record-menu edits are
    // written once rather than twice.
    QListWidget *listFor(ValueAxis axis) const;
    QGroupBox   *enableFor(ValueAxis axis) const;
    QSet<QString> &manualFor(ValueAxis axis);
    QHash<QString, bool> &seenFor(ValueAxis axis);
    // The discovery rule, read off and written to the axis's "Others" row — which IS
    // the state, so there is no bool for the two to fall out of step over. Restrictive
    // is the row unticked: a value the file has not produced yet is not part of what
    // the user asked to see.
    bool             restrictiveFor(ValueAxis axis) const;
    void             setRestrictiveFor(ValueAxis axis, bool restrictive);
    QListWidgetItem *othersItemFor(ValueAxis axis) const;
    // Drop the value rows and keep the "Others" row. QListWidget::clear() must never
    // be called on a value list: the row it would take with it holds the axis's
    // discovery rule, and populateList() is handed that rule rather than re-deriving
    // it.
    static void clearValueRows(QListWidget *list);
    // Make sure `name` is in the axis's list — refreshing from the intern table
    // first, and only carrying it as a manual entry if the file has genuinely not
    // emitted it. Without this a menu edit could silently do nothing while the
    // pane's list lagged the scan behind it.
    void ensureListed(ValueAxis axis, const QString &name);
    // Tick `target` and untick every other VALUE row, and set the discovery rule to
    // whichever of the two the click was on: a value row means "only this one", so
    // "Others" goes off; the "Others" row itself means "only what the scan has not
    // found yet", so it goes on and every listed value goes off. Acts on hidden rows
    // too — see showOnlyValue(), which is this function plus a lookup by name.
    // Emits changed() exactly once.
    void checkOnly(ValueAxis axis, QListWidgetItem *target);
    // The axis a viewport belongs to, for eventFilter(). False when it is neither.
    bool axisOfViewport(const QObject *viewport, ValueAxis &axis) const;

    // The file's observed timestamp span, or false when it has no parsed timestamps.
    bool observedSpan(qint64 &lo, qint64 &hi) const;
    // A UTC instant as the zone-less display-zone wall clock the editors hold, and back.
    QDateTime wallClockOf(qint64 utcMs) const;
    qint64    instantOfWallClock(const QDateTime &wallClock) const;

    // --- The time bounds, in whichever terms the timestamp column is using --------
    //
    // A bound IS an instant; the editors are two ways of asking for one, and which is
    // on screen follows Document::timeDisplay() so the pane asks for what the log
    // shows (SPEC.md §6). Everything outside these five functions works in UTC ms and
    // does not care which pair is visible.
    bool    secondsMode() const;
    // What "0" means in the two seconds modes: the epoch, or the selected run's
    // baseline. Zero for the wall-clock modes, where it goes unused.
    qint64  secondsBaseMs() const;
    double  secondsOf(qint64 utcMs) const;
    qint64  instantOfSeconds(double seconds) const;
    qint64  boundInstant(TimeBound which) const;
    // Callers hold m_populating around this: it writes a widget whose signal is a
    // user edit everywhere else.
    void    setBoundInstant(TimeBound which, qint64 utcMs);
    // Show the pair the display mode calls for, at the column's own precision. Holds
    // m_populating itself (saved and restored): changing the precision re-rounds a
    // held value and emits, and none of that is ever a user edit — see the comment on
    // the definition, and bugs.md 26 for what one unguarded caller cost.
    void    syncTimeEditorKind();

    Document *m_document = nullptr;
    Defaults  m_defaults;           // what clearAll() returns the axes to
    bool      m_populating = false; // guards itemChanged storms during (re)population

    // What the digits in the time editors are currently written in, so
    // refreshTimeBounds() can recover the instant they denote before re-rendering it:
    // the zone for the wall-clock modes, the mode itself and its baseline for the two
    // seconds modes. All three move independently of each other.
    QTimeZone   m_renderZone = QTimeZone::utc();
    TimeDisplay m_renderMode = TimeDisplay::AsWritten;
    qint64      m_renderBase = 0;

    // Priority. A checkable group box like the other four axes: every enable control
    // in this editor is a QGroupBox *, with no exception left to remember.
    QGroupBox *m_priorityEnable = nullptr;
    QComboBox *m_priorityCombo = nullptr;

    // Subsystem
    QGroupBox    *m_loggerGroup = nullptr;
    QLineEdit    *m_loggerNarrow = nullptr; // narrows the list, and adds to it
    QListWidget  *m_loggerList = nullptr;
    QAbstractButton *m_loggerListButtons[3] = {}; // All, None, Invert
    QSet<QString> m_loggerManualNames; // manually-added subsystems (may be absent)
    // Every subsystem name ever listed, and the check state it was last listed WITH.
    // The state half is what survives a name LEAVING the list, which is a thing only
    // a rotation does: the index is replaced wholesale, so for one pass — and for as
    // long as a remote log takes to re-fetch — the file names nothing at all. Recording
    // mere membership made every name that came back read as "shown, and unticked".
    QHash<QString, bool> m_loggerSeen;
    // "Tick subsystems that turn up later" — the discovery rule as a control the user
    // can see and set, and it is the FIRST ROW OF THE LIST, ticked like any other,
    // because that is the question the rest of the list leaves open: these values, and
    // the others. Unticked is MatchCriteria::loggerRestrictive — the list is a
    // restriction, not a snapshot. It outlives every repopulation (clearValueRows),
    // so the row is the state exactly as a checkbox was. showOnlyValue() unticks it;
    // nothing else moves it behind the user's back, which is the difference between a
    // flag and a control.
    QListWidgetItem *m_loggerOthers = nullptr;

    // Thread
    QGroupBox    *m_threadGroup = nullptr;
    QLineEdit    *m_threadNarrow = nullptr;
    QListWidget  *m_threadList = nullptr;
    QAbstractButton *m_threadListButtons[3] = {};
    QSet<QString> m_threadManualNames;
    QHash<QString, bool> m_threadSeen;
    QListWidgetItem *m_threadOthers = nullptr;

    // Message text
    QGroupBox   *m_textGroup = nullptr;
    QHBoxLayout *m_textOptionsRow = nullptr; // the toggles, and where addTextExtra() appends
    QLineEdit *m_textEdit = nullptr;
    // Checkable QToolButtons, not QCheckBoxes: three words stacked down the pane
    // became three glyphs across one row (see buildUi). QAbstractButton is all any
    // caller needs — setChecked/isChecked/toggled are the same either way.
    QAbstractButton *m_textRegex = nullptr;
    QAbstractButton *m_textCase = nullptr;
    QAbstractButton *m_textNegate = nullptr;
    QLabel    *m_textError = nullptr; // "not a valid regular expression", when it is not

    // Time range. Two editors per bound, one visible at a time (syncTimeEditorKind):
    // a date for the three wall-clock display modes, a number of seconds for the two
    // that render the column as seconds. Both spellings are built up front and kept
    // rather than swapped in and out, so the row's geometry does not jump when the
    // display mode changes and nothing has to be re-wired mid-session.
    QGroupBox      *m_timeGroup = nullptr;
    QDateTimeEdit  *m_timeStart = nullptr;
    QDateTimeEdit  *m_timeEnd = nullptr;
    QDoubleSpinBox *m_secStart = nullptr;
    QDoubleSpinBox *m_secEnd = nullptr;
    // Whether a bound came from the user rather than from the observed span. The seed
    // has to keep tracking a growing file, but it must never overwrite a deliberate
    // bound — the same distinction the "Others" row draws between a value the user
    // chose and one the scan turned up. Set by the editors' own signals (outside
    // m_populating) and by the record-menu setters; cleared on a rebind and on loading
    // criteria with the axis off.
    bool           m_timeUserEdited = false;
};

} // namespace loftail
