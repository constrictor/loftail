#pragma once

#include "MatchCriteria.h"

#include <QSet>
#include <QString>
#include <QTimeZone>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAbstractButton;
class QComboBox;
class QDateTimeEdit;
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

    // Leave out the axes this log's format cannot fill — the thread axis with no %t,
    // the time axis with no %d — instead of showing them greyed with the reason in
    // their title.
    //
    // The two panes want opposite things here, and both are right. The Filters pane
    // describes the whole log, so a missing axis is worth SAYING: a restored session or
    // a preset can leave it ticked-but-dropped, and that needs somewhere to show. The
    // Highlighters pane shows the axes of ONE rule, under a rule list and above four
    // actions, and repeats that block for every rule the user clicks — an axis that can
    // never match anything is dead weight in the tightest pane there is, and the answer
    // to "why can I not match on thread" is the one the Filters pane already gives.
    //
    // NOT a collapse: every axis the format DOES carry stays expanded whether it is
    // ticked or not, in both panes. An axis that reveals its controls only once it is
    // ticked cannot be read, only explored — the user has to switch it on to find out
    // whether it was the one they wanted. Qt greys an unticked group box's body, which
    // says "not in force" without moving anything.
    void setHidesUnsupportedAxes(bool hide);

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
    // Everything an axis's own state decides about its controls: whether the priority
    // combo is live (it has no group box to grey it), and whether an axis the format
    // cannot fill is on screen at all.
    void updateAxisState();
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
    // The file's observed timestamp span, or false when it has no parsed timestamps.
    bool observedSpan(qint64 &lo, qint64 &hi) const;
    // A UTC instant as the zone-less display-zone wall clock the editors hold.
    QDateTime wallClockOf(qint64 utcMs) const;

    Document *m_document = nullptr;
    Defaults  m_defaults;           // what clearAll() returns the axes to
    bool      m_populating = false; // guards itemChanged storms during (re)population
    bool      m_hideUnsupported = false;

    // The zone the time editors were last rendered in. refreshTimeBounds() needs it to
    // recover the instant the shown wall clock currently denotes before re-rendering
    // it in the new zone.
    QTimeZone m_renderZone = QTimeZone::utc();

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
    QSet<QString> m_loggerSeen;        // every subsystem name ever listed
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
    QSet<QString> m_threadSeen;
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

    // Time range
    QGroupBox     *m_timeGroup = nullptr;
    QDateTimeEdit *m_timeStart = nullptr;
    QDateTimeEdit *m_timeEnd = nullptr;
    // Whether a bound came from the user rather than from the observed span. The seed
    // has to keep tracking a growing file, but it must never overwrite a deliberate
    // bound — the same distinction the "Others" row draws between a value the user
    // chose and one the scan turned up. Set by the editors' own signals (outside
    // m_populating) and by the record-menu setters; cleared on a rebind and on loading
    // criteria with the axis off.
    bool           m_timeUserEdited = false;
};

} // namespace loftail
