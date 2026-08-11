#pragma once

#include "Highlight.h"

#include <QJsonObject>
#include <QVector>
#include <QWidget>

#include <optional>

QT_BEGIN_NAMESPACE
class QComboBox;
class QPushButton;
class QTableWidget;
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
    // reason: a column is identified by what it is, never by the header it shows, which
    // is an icon or a translated word (ARCHITECTURE.md §9.1).
    enum Column {
        kColEnabled = 0,    // the rule's own on/off tick
        kColRule,           // what it matches, as one line of prose
        kColForeground,     // text colour   — an icon-only swatch picker
        kColBackground,     // background    — the same
        kColDigest,         // HighlightAction::Digest
        kColNotify,         // HighlightAction::Notify
        kColTab,            // HighlightAction::Tab
        kColumnCount
    };

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
    // A theme switch changes every swatch icon in the table and every glyph in its
    // header, all of which are pixmaps painted once. Rebuilding the table is the
    // cheapest correct answer, and it keeps the selected rule.
    void changeEvent(QEvent *event) override;

private:
    void buildUi();
    void reloadRuleTable();      // rebuild the rule table from m_rules
    void buildRow(int row);      // fill one table row from m_rules[row]
    void loadEditorFor(int row); // fill the axis editor from m_rules[row]
    void commit();               // push m_rules into the document + emit change
    void syncToDocument();
    void updateActivity();       // emit activityChanged() when hasRules() flips

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
    // One row's swatch picker: icon-only, so the pair costs two table columns rather
    // than a row of the editor.
    QComboBox *makeSwatchCombo(int row, Column column);
    QComboBox *swatchCombo(int row, Column column) const;
    void setSwatchCombo(QComboBox *combo, int paletteIndex);
    int swatchValue(const QComboBox *combo) const;
    bool isDark() const;

    // HighlightAction::Color follows the two swatches and is not a control of its own
    // (ARCHITECTURE.md §7.5): a rule colours exactly when one of its two roles names a
    // palette entry. Applied on every ingest as well as on every edit, so the flag and
    // the swatches can never say different things.
    static void applyColourAction(HighlightRule &rule);
    // Take m_rules in from a document or a preset: normalise the colour action, and
    // report whether anything actually moved.
    bool normaliseRules();

    // The action a check column carries, for the itemChanged handler.
    static std::optional<HighlightAction> actionForColumn(int column);
    // Whether this desktop offers a notification service at all. False on a stock
    // GNOME/Wayland session, so the Notify column is not user-checkable and says why
    // rather than accepting a tick that would do nothing.
    static bool notificationsSupported();

    Document *m_document = nullptr;
    QVector<HighlightRule> m_rules;
    bool m_updating = false; // guards signal storms during (re)load
    std::optional<bool> m_activeState; // last hasRules() reported by activityChanged()

    // The rule list, which is now a TABLE: a tick, what the rule matches, its two
    // colours and its three remaining actions, one rule per row.
    QTableWidget *m_ruleTable = nullptr;
    int m_swatchColumnWidth = 0; // a swatch picker's width; the colour columns are fixed

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
