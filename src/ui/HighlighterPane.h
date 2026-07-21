#pragma once

#include "Highlight.h"

#include <QJsonObject>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QListWidget;
class QListWidgetItem;
class QPushButton;
QT_END_NAMESPACE

namespace loftail {

class Document;

// M5 — the Highlighters side pane (SPEC.md §7, §8). A dockable widget bound to the
// ACTIVE document by signal (invariant #7): setDocument() rebinds it, never a
// global "current file". It edits an ordered list of highlight rules — add/remove,
// reorder, enable/disable per rule, and per rule a subsystem and/or minimum-priority
// match plus a background and foreground color picked from the 12-slot palette (or
// *default*). Rules store palette INDICES and subsystem NAMES, never RGB or interned
// ids, so they stay portable across the theme and a re-index (ARCHITECTURE.md §8).
//
// The authoritative rule list lives in the Document's HighlighterSet (invariant #7);
// the pane keeps a synced working copy and, on every edit, writes it back and calls
// Document::resolveHighlighters() so LogModel::data() matches on integers.
class HighlighterPane : public QWidget
{
    Q_OBJECT

public:
    explicit HighlighterPane(QWidget *parent = nullptr);

    void setDocument(Document *document);

    // Refresh the editor's discovered-subsystem list as indexing progresses.
    void refreshDiscoveredLists();

    // Portable, name/index-based snapshot for highlighter presets and per-file
    // session restore (SPEC.md §9, §10): { "rules": [ ... ] }.
    QJsonObject saveState() const;
    void restoreState(const QJsonObject &state);

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
    QComboBox *makeSwatchCombo();
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
    QWidget     *m_editor = nullptr;
    QCheckBox   *m_matchPriority = nullptr;
    QComboBox   *m_priorityCombo = nullptr;
    QCheckBox   *m_matchLogger = nullptr;
    QListWidget *m_loggerList = nullptr;
    QComboBox   *m_bgCombo = nullptr;
    QComboBox   *m_fgCombo = nullptr;
};

} // namespace loftail
