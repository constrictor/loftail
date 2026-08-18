#pragma once

#include "Highlight.h"

#include <QColor>
#include <QJsonObject>
#include <QVector>
#include <QWidget>

#include <optional>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QToolButton;
QT_END_NAMESPACE

namespace loftail {

class AxisEditor;
class Document;

// M5 — the Highlighters side pane (SPEC.md §7, §8). A dockable widget bound to the
// ACTIVE document by signal (invariant #7): setDocument() rebinds it, never a
// global "current file". It edits an ordered list of highlight rules — add/remove,
// reorder, enable/disable per rule, and per rule the SAME five match axes a filter
// offers (subsystem, thread, priority, time range, message text) plus a background
// and foreground color picked from the 27-slot palette (or *default*).
//
// The axis controls are the shared `AxisEditor`, the very widget the Filters pane
// uses, sitting in a scroll area — five axes do not otherwise fit a dock under a rule
// list. Rules store criteria (names, levels, wall clock) and palette INDICES, never
// RGB or interned ids, so they stay portable across the theme, a re-index and a zone
// change (ARCHITECTURE.md §8).
//
// **What a rule DOES is edited in the rule list itself**, one column per action, and
// what is left below the list is the CONDITION alone (ARCHITECTURE.md §7.5). The four
// actions used to be a stack of checkable sections under an `Action` caption, which
// cost the pane's scarcest resource — height — to show one rule's settings while the
// list above already had a row per rule with nothing in it but a summary. A row is
// what an action belongs to: the answer is per rule, it is a tick, and the list is
// where every rule is on screen at once. That also deletes the `Condition` caption:
// with nothing to tell it apart from, the surviving half needs no name.
//
// The authoritative rule list lives in the Document's HighlighterSet (invariant #7);
// the pane keeps a synced working copy and, on every edit, writes it back and calls
// Document::resolveHighlighters() so LogModel matches on resolved criteria.
class HighlighterPane : public QWidget
{
    Q_OBJECT

public:
    explicit HighlighterPane(QWidget *parent = nullptr);

    // The rule table's columns, in order. Public because they are the test contract —
    // the same role object names play for the widgets in `AxisEditor`, and for the same
    // reason: a column is identified by what it is and not by a label, there being no
    // header row to carry one (ARCHITECTURE.md §7.5).
    enum Column {
        kColEnabled = 0,    // the rule's own on/off tick
        kColRule,           // what it matches, as one line of prose
        kColColours,        // BOTH swatch pickers, text before background
        kColDigest,         // HighlightAction::Digest
        kColNotify,         // HighlightAction::Notify
        kColTab,            // HighlightAction::Tab
        kColumnCount
    };

    // Which of a rule's two colour roles a swatch picker sets. There is one column for
    // the pair, so the column no longer names the role and this does.
    enum class ColourRole { Foreground, Background };

    void setDocument(Document *document);

    // Refresh the editor's discovered subsystem/thread lists as indexing progresses.
    void refreshDiscoveredLists();

    // Re-render the selected rule's time-range editors after the display zone moves
    // (SPEC.md §4). A time-range rule holds wall clock, so leaving the digits alone
    // would silently re-point it at a different instant — the same concern the
    // Filters pane has.
    void refreshTimeBounds();

    // Portable, name/index-based snapshot for highlighter presets and per-file
    // session restore (SPEC.md §9, §10): { "rules": [ ... ] }.
    QJsonObject saveState() const;
    void restoreState(const QJsonObject &state);

    // True when the pane holds any rule at all, enabled or not. What the dock's
    // marker reflects: unlike a filter axis, which can be switched on and still
    // exclude nothing, a rule in the list is something the user put there.
    bool hasRules() const { return !m_rules.isEmpty(); }

    // Add a rule built from the record under the cursor (the record menu, SPEC.md §5)
    // and select it, so the pane shows what was just added and it can be recolored or
    // removed without hunting for it. APPENDED, never inserted: rules are
    // first-match-wins (§7), and a rule the user placed deliberately must keep its
    // precedence over one added by a menu click. The background color is the first
    // palette slot no enabled rule is using, so two menu-made rules do not come out
    // the same color.
    void addRule(const MatchCriteria &criteria);

signals:
    // Emitted whenever the rules change. MainWindow re-resolves and repaints; no
    // model reset is needed since highlighting recolors rows without adding/removing.
    void highlightersChanged();

    // Emitted only when hasRules() CHANGES, so MainWindow can mark the dock while the
    // pane holds rules — it ships tabbed behind three others, so rules are usually in
    // force with the pane out of sight. Edge-triggered for the same reason the Filters
    // pane's is: the title rides a QTabBar entry, and re-setting it relays out the bar.
    void activityChanged(bool active);

protected:
    // A theme switch changes every swatch and every action glyph in the table, all of
    // which are pixmaps painted once. Rebuilding the table is the cheapest correct
    // answer, and it keeps the selected rule.
    void changeEvent(QEvent *event) override;

    // Only for the rule table's viewport, and only to keep the empty-table message
    // spanning it — the viewport is not a layout, so nothing else would resize a child
    // laid over it.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void reloadRuleTable();      // rebuild the rule table from m_rules
    void buildRow(int row);      // fill one table row from m_rules[row]
    void loadEditorFor(int row); // fill the axis editor from m_rules[row]
    void commit();               // push m_rules into the document + emit change
    void syncToDocument();
    void updateActivity();       // emit activityChanged() when hasRules() flips
    // Grey the four buttons that need something to act on. ONE writer for all of them,
    // called from the rebuild (where the rule count moves) and from the table's own
    // currentCellChanged (where the row does) — a setEnabled() at a third call site is
    // how the four drift out of step. Re-entrancy-safe by construction: it reads
    // m_rules and the current row and writes nothing but enabled states.
    void updateRuleButtons();
    // Show, word and place the empty table's message. TWO empty states, and they do not
    // say the same thing: no file open at all, versus a file whose rules the user has
    // just cleared. Driven from reloadRuleTable(), the one funnel the table is built by.
    void updatePlaceholder();
    // Take the message's colour from the palette, so it reads as an aside on a light
    // theme and on a dark one alike. Re-applied on a theme switch.
    void applyPlaceholderColour();

    int currentRow() const;
    void setCurrentRow(int row);
    QString ruleSummary(const HighlightRule &r) const;
    // Paint a rule's row in its own colours, so the table previews the rule rather
    // than only naming it.
    void paintRow(int row, const HighlightRule &r) const;
    // Re-read one row's summary and colours after an edit, leaving the selection and
    // every check state where they are.
    void refreshRow(int row);
    // The first palette slot no existing rule paints with, cycling once every slot is
    // spoken for. Shared by the New button and the record menu's one-click rule.
    int nextFreeBackground() const;
    // One row's swatch picker: icon-only, so the pair fits ONE table column beside
    // everything else a rule row carries.
    QComboBox *makeSwatchCombo(int row, ColourRole role, QWidget *parent);
    void setSwatchCombo(QComboBox *combo, int paletteIndex);
    int swatchValue(const QComboBox *combo) const;
    // Paint every swatch in one row's TWO pickers. A swatch previews the rule's PAIR —
    // the letter is its text colour, the tile behind it its background — so each picker's
    // items are drawn against the value the other one holds, and choosing in either
    // picker invalidates the other's whole list. The only place a swatch icon is made.
    void updateColourPreviews(int row);
    // The colour a role actually paints with: the palette entry, or the theme's own
    // colour where the rule names *default*. What a preview must be drawn against.
    QColor roleColour(int paletteIndex, ColourRole role) const;
    // One row's action toggle: an icon button, checked while the rule carries it.
    QToolButton *makeActionButton(int row, HighlightAction action, QWidget *parent);
    bool isDark() const;

    // HighlightAction::Color follows the two swatches and is not a control of its own
    // (ARCHITECTURE.md §7.5): a rule colours exactly when one of its two roles names a
    // palette entry. Applied on every ingest as well as on every edit, so the flag and
    // the swatches can never say different things.
    static void applyColourAction(HighlightRule &rule);
    // Take m_rules in from a document or a preset: normalise the colour action, and
    // report whether anything actually moved.
    bool normaliseRules();

    // Whether this desktop offers a notification service at all. False on a stock
    // GNOME/Wayland session, so the Notify button is disabled and says why rather
    // than accepting a press that would do nothing.
    static bool notificationsSupported();

    Document *m_document = nullptr;
    QVector<HighlightRule> m_rules;
    bool m_updating = false; // guards signal storms during (re)load
    std::optional<bool> m_activeState; // last hasRules() reported by activityChanged()

    // The rule list, which is now a TABLE: a tick, what the rule matches, its two
    // colours and its three remaining actions, one rule per row.
    QTableWidget *m_ruleTable = nullptr;
    // Every column but the summary holds a WIDGET or a bare tick, and a cell widget
    // contributes nothing to ResizeToContents, so their widths are measured once from a
    // prototype and fixed.
    int m_colourColumnWidth = 0;
    int m_actionColumnWidth = 0;
    int m_rowHeight = 0;
    // What the table says when it holds nothing. A LABEL over the viewport, never a row
    // in the table: a row would be a rule to everything that walks rows — the reorder
    // buttons, the per-row "ruleRow" property, saveState() — and the one thing it must
    // not be is countable.
    QLabel *m_tablePlaceholder = nullptr;

    QPushButton *m_newBtn = nullptr;
    QPushButton *m_removeBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QPushButton *m_upBtn = nullptr;
    QPushButton *m_downBtn = nullptr;

    // The CONDITION editor for the selected rule, and the whole of what is left below
    // the table. A bare container, not a captioned group box: setEnabled() on it is what
    // greys the axes while no rule is selected, and it carries no caption because the
    // table above says which rule is being edited and there is no second half left to
    // tell it apart from.
    QWidget    *m_editor = nullptr;
    AxisEditor *m_axes = nullptr;
};

} // namespace loftail
