#pragma once

#include "Highlight.h"

#include <QJsonObject>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QGroupBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;
QT_END_NAMESPACE

namespace loftail {

class AxisEditor;
class Document;

// M5 — the Highlighters side pane (SPEC.md §7, §8). A dockable widget bound to the
// ACTIVE document by signal (invariant #7): setDocument() rebinds it, never a
// global "current file". It edits an ordered list of highlight rules — add/remove,
// reorder, enable/disable per rule, and per rule the SAME five match axes a filter
// offers (subsystem, thread, priority, time range, message text) plus a background
// and foreground color picked from the 12-slot palette (or *default*).
//
// The axis controls are the shared `AxisEditor`, the very widget the Filters pane
// uses, sitting in a scroll area with each axis collapsed until it is enabled — five
// axes and a rule list do not otherwise fit a dock. Rules store criteria (names,
// levels, wall clock) and palette INDICES, never RGB or interned ids, so they stay
// portable across the theme, a re-index and a zone change (ARCHITECTURE.md §8).
//
// The authoritative rule list lives in the Document's HighlighterSet (invariant #7);
// the pane keeps a synced working copy and, on every edit, writes it back and calls
// Document::resolveHighlighters() so LogModel matches on resolved criteria.
class HighlighterPane : public QWidget
{
    Q_OBJECT

public:
    explicit HighlighterPane(QWidget *parent = nullptr);

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

private:
    void buildUi();
    void reloadRuleList();       // rebuild the rule list widget from m_rules
    void loadEditorFor(int row); // fill the editor from m_rules[row]
    void commit();               // push m_rules into the document + emit change
    void syncToDocument();

    int currentRow() const;
    QString ruleSummary(const HighlightRule &r) const;
    QComboBox *makeSwatchCombo(QWidget *parent);
    void setSwatchCombo(QComboBox *combo, int paletteIndex);
    int swatchValue(const QComboBox *combo) const;
    bool isDark() const;

    Document *m_document = nullptr;
    QVector<HighlightRule> m_rules;
    bool m_updating = false; // guards signal storms during (re)load

    QListWidget *m_ruleList = nullptr;
    QPushButton *m_addBtn = nullptr;
    QPushButton *m_removeBtn = nullptr;
    QPushButton *m_upBtn = nullptr;
    QPushButton *m_downBtn = nullptr;

    // Editor for the selected rule.
    QGroupBox  *m_editor = nullptr;
    AxisEditor *m_axes = nullptr;
    QComboBox  *m_bgCombo = nullptr;
    QComboBox  *m_fgCombo = nullptr;
};

} // namespace loftail
