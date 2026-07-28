#pragma once

#include "MatchCriteria.h"

#include <QSet>
#include <QString>
#include <QTimeZone>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLineEdit;
class QListWidget;
QT_END_NAMESPACE

namespace loftail {

class Document;

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

    // Collapse each axis to its title row and enable checkbox while that axis is off.
    // The Highlighters pane needs it — five axes plus a rule list do not fit a dock
    // otherwise — and the Filters pane, which has the pane to itself, leaves it off.
    void setCollapsible(bool collapsible);

    // Rebind to a document (or nullptr to clear). Repopulates the auto-discovered
    // subsystem/thread lists from its intern tables, gates the thread and time axes on
    // whether the format carries those fields, and seeds the time editors to the
    // file's observed span so the pickers open near useful values.
    void setDocument(Document *document);
    Document *document() const { return m_document; }

    // Refresh the auto-discovered lists from the document's intern tables — called as
    // indexing progresses so newly-seen subsystems and threads appear (SPEC.md §6).
    // A name never listed before arrives CHECKED; one the user unticked stays
    // unticked. Does not emit changed(): a plain repopulation is not a user edit.
    void refreshDiscoveredLists();

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

signals:
    // Emitted on any user edit. Never emitted by setCriteria(), setDocument() or
    // refreshDiscoveredLists(), so the owning pane can load state without recursing.
    void changed();

private:
    void buildUi(Defaults defaults);
    void emitChanged();
    void updateCollapse();
    void updateTextValidity();

    // Repopulate one checkable list. `exact` picks the check-state rule: false is
    // discovery (a name never listed before arrives checked, so an enabled-by-default
    // axis does not start dropping records mid-scan), true is loading a stored
    // selection (checked means exactly the given names).
    void populateList(QListWidget *list, const QStringList &names,
                      const QSet<QString> &checked, const QSet<QString> &manual,
                      QSet<QString> &seen, bool exact);
    static bool   allChecked(const QListWidget *list);
    QSet<QString> checkedNames(const QListWidget *list) const;
    void          setAllChecked(QListWidget *list, bool checked);
    void          invertChecked(QListWidget *list);
    void          narrowList(QListWidget *list, const QString &needle);
    void          repopulate(const QSet<QString> &loggerChecked,
                             const QSet<QString> &threadChecked, bool exact);

    Document *m_document = nullptr;
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
    QCheckBox    *m_loggerEnable = nullptr;
    QLineEdit    *m_loggerNarrow = nullptr;
    QListWidget  *m_loggerList = nullptr;
    QLineEdit    *m_loggerManual = nullptr;
    QWidget      *m_loggerBody = nullptr;
    QSet<QString> m_loggerManualNames; // manually-added subsystems (may be absent)
    QSet<QString> m_loggerSeen;        // every subsystem name ever listed

    // Thread
    QCheckBox    *m_threadEnable = nullptr;
    QLineEdit    *m_threadNarrow = nullptr;
    QListWidget  *m_threadList = nullptr;
    QLineEdit    *m_threadManual = nullptr;
    QWidget      *m_threadBody = nullptr;
    QSet<QString> m_threadManualNames;
    QSet<QString> m_threadSeen;

    // Message text
    QCheckBox *m_textEnable = nullptr;
    QLineEdit *m_textEdit = nullptr;
    QCheckBox *m_textRegex = nullptr;
    QCheckBox *m_textCase = nullptr;
    QCheckBox *m_textNegate = nullptr;
    QWidget   *m_textBody = nullptr;

    // Time range
    QCheckBox     *m_timeEnable = nullptr;
    QDateTimeEdit *m_timeStart = nullptr;
    QDateTimeEdit *m_timeEnd = nullptr;
    QWidget       *m_timeBody = nullptr;
};

} // namespace loftail
