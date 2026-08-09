#pragma once

#include "MatchCriteria.h"

#include <QSet>
#include <QString>
#include <QTimeZone>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAbstractButton;
class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QGroupBox;
class QLineEdit;
class QHBoxLayout;
class QLabel;
class QListWidget;
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
    // Which axes start enabled. The Filters pane ships priority and subsystem on so
    // their controls act on the first click (SPEC.md §6, and applyToDocument collapses
    // the resulting no-op state). A highlight rule starts with nothing on: every axis
    // is opt-in, and an unconfigured rule must stay inert (SPEC.md §7).
    struct Defaults
    {
        bool priorityOn = false;
        bool loggerOn = false;
    };

    explicit AxisEditor(Defaults defaults, QWidget *parent = nullptr);

    // Append a widget to the bottom of the MESSAGE TEXT axis's body, where it lives
    // and dies with that axis: shown and hidden by setCollapsible(), and greyed out
    // with the rest of the body while the axis is off.
    //
    // It exists for exactly one caller. Filter context (SPEC.md §6) widens the
    // message search and nothing else, so its two spinners belong beside that search
    // — but they are not a match criterion, are absent from MatchCriteria, and mean
    // nothing to a highlight rule, which is why they stay the FilterPane's own
    // widgets rather than becoming a sixth axis here. HighlighterPane calls this not
    // at all, and gets an editor with no context row.
    void addTextExtra(QWidget *w);

    // Collapse each axis to its title row — which is also its enable control, bar
    // priority's, whose row keeps the checkbox and drops the combo — while it is off.
    //
    // The Highlighters pane needs it: five axes plus a rule list plus a colour row do
    // not fit a dock otherwise. The Filters pane deliberately does NOT use it, because
    // an axis that reveals its controls only once it is ticked hides what the axis even
    // offers — the user has to switch a filter on to find out whether it is the one
    // they wanted. Off, its controls stay on screen and Qt greys them, which says the
    // same thing without moving anything.
    void setCollapsible(bool collapsible);

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

    // Re-render the time editors after the display zone moves (a timestamp
    // display-mode change, SPEC.md §4). The editors hold wall clock and criteria()
    // reinterprets it in the CURRENT zone, so leaving the digits alone would silently
    // re-point the bounds at a different instant. Preserves the instant, not the text.
    void refreshTimeBounds();

    // The axes as the user has them, in portable form.
    MatchCriteria criteria() const;

    // Load `c` into the controls. Applies its subsystem/thread selection EXACTLY —
    // unlike refreshDiscoveredLists(), which treats a never-listed name as checked —
    // so switching between two highlight rules shows each rule's own selection rather
    // than inheriting the other's. Does not emit changed().
    void setCriteria(const MatchCriteria &c);

    // False when the text axis holds a regex that failed to compile. The pattern edit
    // flags it inline; without this a malformed regex silently matches nothing.
    bool textPatternValid() const;

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
    // the result by its ordinary path and the user can see — and undo — what
    // happened by looking at the axis.
    //
    // showOnlyValue() replaces the axis's selection with the single named value and
    // marks it RESTRICTIVE (MatchCriteria::loggerRestrictive): a value discovered
    // later must not join a selection the user made by pointing at one record.
    // hideValue() only unticks, leaving both the rest of the selection and the
    // discovery rule alone — excluding one subsystem says nothing about the next one
    // to appear. Both enable their axis, since neither means anything with it off.
    void showOnlyValue(ValueAxis axis, const QString &name);
    void hideValue(ValueAxis axis, const QString &name);

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
    // Whether `name` is worth offering to add: non-empty and not already listed.
    bool canAddTyped(ValueAxis axis, const QString &name) const;
    // Keep the All / None / Invert tooltips telling the truth about how many entries
    // the current narrowing is hiding from them.
    void updateListButtonHints(ValueAxis axis);
    void emitChanged();
    void updateCollapse();
    void updateTextValidity();

    // Repopulate one checkable list. `exact` picks the check-state rule: false is
    // discovery (a name never listed before arrives checked, so an enabled-by-default
    // axis does not start dropping records mid-scan), true is loading a stored
    // selection (checked means exactly the given names). `restrictive` turns the
    // discovery rule off for this axis without turning it into a load — see
    // MatchCriteria::loggerRestrictive.
    void populateList(QListWidget *list, const QStringList &names,
                      const QSet<QString> &checked, const QSet<QString> &manual,
                      QSet<QString> &seen, bool exact, bool restrictive);
    static bool   allChecked(const QListWidget *list);
    QSet<QString> checkedNames(const QListWidget *list) const;
    void          setAllChecked(QListWidget *list, bool checked);
    void          invertChecked(QListWidget *list);
    void          narrowList(QListWidget *list, const QString &needle);
    void          repopulate(const QSet<QString> &loggerChecked,
                             const QSet<QString> &threadChecked, bool exact);

    // The widgets and per-axis state behind ValueAxis, so the record-menu edits are
    // written once rather than twice.
    QListWidget *listFor(ValueAxis axis) const;
    QGroupBox   *enableFor(ValueAxis axis) const;
    QSet<QString> &manualFor(ValueAxis axis);
    // The discovery rule, read off and written to the axis's "New" checkbox — which IS
    // the state, so there is no bool for the two to fall out of step over. Restrictive
    // is the box unticked: a value the file has not produced yet is not part of what
    // the user asked to see.
    bool          restrictiveFor(ValueAxis axis) const;
    void          setRestrictiveFor(ValueAxis axis, bool restrictive);
    QCheckBox    *newValuesBoxFor(ValueAxis axis) const;
    // Make sure `name` is in the axis's list — refreshing from the intern table
    // first, and only carrying it as a manual entry if the file has genuinely not
    // emitted it. Without this a menu edit could silently do nothing while the
    // pane's list lagged the scan behind it.
    void ensureListed(ValueAxis axis, const QString &name);
    // The file's observed timestamp span, or false when it has no parsed timestamps.
    bool observedSpan(qint64 &lo, qint64 &hi) const;
    // A UTC instant as the zone-less display-zone wall clock the editors hold.
    QDateTime wallClockOf(qint64 utcMs) const;

    Document *m_document = nullptr;
    Defaults  m_defaults;           // what clearAll() returns the axes to
    bool      m_populating = false; // guards itemChanged storms during (re)population
    bool      m_collapsible = false;

    // The zone the time editors were last rendered in. refreshTimeBounds() needs it to
    // recover the instant the shown wall clock currently denotes before re-rendering
    // it in the new zone.
    QTimeZone m_renderZone = QTimeZone::utc();

    // Priority
    QCheckBox *m_priorityEnable = nullptr;
    QComboBox *m_priorityCombo = nullptr;
    QWidget   *m_priorityBody = nullptr;

    // Subsystem
    QGroupBox    *m_loggerGroup = nullptr;
    QLineEdit    *m_loggerNarrow = nullptr; // narrows the list, and adds to it
    QListWidget  *m_loggerList = nullptr;
    QWidget      *m_loggerBody = nullptr;
    QAbstractButton *m_loggerListButtons[3] = {}; // All, None, Invert
    QSet<QString> m_loggerManualNames; // manually-added subsystems (may be absent)
    QSet<QString> m_loggerSeen;        // every subsystem name ever listed
    // "Tick subsystems that turn up later" — the discovery rule as a control the user
    // can see and set, sitting under All/None/Invert because those three set it too.
    // Unticked is MatchCriteria::loggerRestrictive: the list is a restriction, not a
    // snapshot. showOnlyValue() unticks it; nothing else moves it behind the user's
    // back, which is the difference between a flag and a control.
    QCheckBox    *m_loggerNewValues = nullptr;

    // Thread
    QGroupBox    *m_threadGroup = nullptr;
    QLineEdit    *m_threadNarrow = nullptr;
    QListWidget  *m_threadList = nullptr;
    QWidget      *m_threadBody = nullptr;
    QAbstractButton *m_threadListButtons[3] = {};
    QSet<QString> m_threadManualNames;
    QSet<QString> m_threadSeen;
    QCheckBox    *m_threadNewValues = nullptr;

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
    QWidget   *m_textBody = nullptr;

    // Time range
    QGroupBox     *m_timeGroup = nullptr;
    QDateTimeEdit *m_timeStart = nullptr;
    QDateTimeEdit *m_timeEnd = nullptr;
    QWidget       *m_timeBody = nullptr;
    // Whether a bound came from the user rather than from the observed span. The seed
    // has to keep tracking a growing file, but it must never overwrite a deliberate
    // bound — the same distinction the "New" checkbox draws between a value the user
    // chose and one the scan turned up. Set by the editors' own signals (outside
    // m_populating) and by the record-menu setters; cleared on a rebind and on loading
    // criteria with the axis off.
    bool           m_timeUserEdited = false;
};

} // namespace loftail
