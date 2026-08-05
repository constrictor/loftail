#include "HighlighterPane.h"

#include "AxisEditor.h"
#include "Document.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"
#include "RecordIndex.h"

#include <QComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QVBoxLayout>

namespace loftail {

namespace {

QIcon swatchIcon(const QColor &c)
{
    QPixmap pm(14, 14);
    pm.fill(c.isValid() ? c : Qt::transparent);
    return QIcon(pm);
}

// One axis's contribution to a rule's one-line summary, or an empty string when the
// axis is off. Short by necessity: this is a row in a dock-width list.
QString axisSummary(const MatchCriteria &c)
{
    QStringList parts;
    if (c.priorityEnabled)
        parts << QStringLiteral("≥%1").arg(QString(priorityName(c.minPriority)));
    if (c.loggerEnabled) {
        parts << (c.loggerNames.size() == 1
                      ? c.loggerNames.first()
                      : QStringLiteral("%1 subsystems").arg(c.loggerNames.size()));
    }
    if (c.threadEnabled) {
        parts << (c.threadNames.size() == 1
                      ? QStringLiteral("thread %1").arg(c.threadNames.first())
                      : QStringLiteral("%1 threads").arg(c.threadNames.size()));
    }
    if (c.text.active()) {
        // Slashes for a regex, quotes for a substring — the same visual shorthand the
        // Find bar's two modes have.
        const QString pat = c.text.matcher.isRegex()
                                ? QStringLiteral("/%1/").arg(c.text.matcher.pattern())
                                : QStringLiteral("\"%1\"").arg(c.text.matcher.pattern());
        parts << (c.text.negate ? QStringLiteral("not %1").arg(pat) : pat);
    }
    if (c.timeEnabled)
        parts << QStringLiteral("in time range");
    return parts.join(QStringLiteral(", "));
}

} // namespace

HighlighterPane::HighlighterPane(QWidget *parent) : QWidget(parent)
{
    buildUi();
    setDocument(nullptr);
}

bool HighlighterPane::isDark() const
{
    // Dark when the base is darker than the text — the same cue the view uses.
    return palette().base().color().lightness() < palette().text().color().lightness();
}

QComboBox *HighlighterPane::makeSwatchCombo(QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    // Item 0 is the *default* sentinel: leave this role at the theme's normal color.
    combo->addItem(QStringLiteral("Default"), HighlightPalette::kDefault);
    const bool dark = isDark();
    for (int i = 0; i < HighlightPalette::count(); ++i) {
        const PaletteSlot &s = HighlightPalette::slot(i);
        combo->addItem(swatchIcon(HighlightPalette::color(i, dark)), QString(s.name), i);
    }
    return combo;
}

void HighlighterPane::setSwatchCombo(QComboBox *combo, int paletteIndex)
{
    const int at = combo->findData(paletteIndex);
    combo->setCurrentIndex(at >= 0 ? at : 0); // fall back to Default
}

int HighlighterPane::swatchValue(const QComboBox *combo) const
{
    const QVariant v = combo->currentData();
    return v.isValid() ? v.toInt() : HighlightPalette::kDefault;
}

void HighlighterPane::buildUi()
{
    auto *root = new QVBoxLayout(this);

    m_ruleList = new QListWidget(this);
    m_ruleList->setMinimumHeight(120);
    root->addWidget(m_ruleList);

    auto *btnRow = new QHBoxLayout;
    m_addBtn = new QPushButton(QStringLiteral("Add"), this);
    m_removeBtn = new QPushButton(QStringLiteral("Remove"), this);
    m_upBtn = new QPushButton(QStringLiteral("Up"), this);
    m_downBtn = new QPushButton(QStringLiteral("Down"), this);
    btnRow->addWidget(m_addBtn);
    btnRow->addWidget(m_removeBtn);
    btnRow->addWidget(m_upBtn);
    btnRow->addWidget(m_downBtn);
    root->addLayout(btnRow);

    // --- Editor for the selected rule --------------------------------------
    //
    // Five axes plus two color pickers do not fit a dock, so the whole editor lives
    // in a scroll area and the AxisEditor collapses each axis to its title row until
    // that axis is enabled. A rule with two axes set therefore shows two open groups
    // and three one-line stubs.
    m_editor = new QGroupBox(QStringLiteral("Selected rule"), this);
    auto *ev = new QVBoxLayout(m_editor);

    auto *scroll = new QScrollArea(m_editor);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    ev->addWidget(scroll);

    auto *content = new QWidget(scroll);
    scroll->setWidget(content);
    auto *cv = new QVBoxLayout(content);
    cv->setContentsMargins(0, 0, 0, 0);

    // Every axis is opt-in for a highlight rule: an unconfigured rule must stay inert
    // (SPEC.md §7), the opposite of the Filters pane's enabled-by-default metadata
    // axes, which exist so their controls act on the first click.
    m_axes = new AxisEditor(AxisEditor::Defaults{/*priorityOn=*/false, /*loggerOn=*/false},
                            content);
    m_axes->setCollapsible(true);
    cv->addWidget(m_axes);

    auto *bgRow = new QHBoxLayout;
    bgRow->addWidget(new QLabel(QStringLiteral("Background:"), content));
    m_bgCombo = makeSwatchCombo(content);
    bgRow->addWidget(m_bgCombo, 1);
    cv->addLayout(bgRow);

    auto *fgRow = new QHBoxLayout;
    fgRow->addWidget(new QLabel(QStringLiteral("Text:"), content));
    m_fgCombo = makeSwatchCombo(content);
    fgRow->addWidget(m_fgCombo, 1);
    cv->addLayout(fgRow);

    cv->addStretch(1);
    root->addWidget(m_editor, 1);

    // --- Wiring -------------------------------------------------------------
    connect(m_ruleList, &QListWidget::currentRowChanged, this, [this](int row) {
        loadEditorFor(row);
    });
    // A rule's checkbox toggles enable/disable in one click (SPEC.md §8).
    connect(m_ruleList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (m_updating)
            return;
        const int row = m_ruleList->row(item);
        if (row >= 0 && row < m_rules.size()) {
            m_rules[row].enabled = (item->checkState() == Qt::Checked);
            commit();
        }
    });

    connect(m_addBtn, &QPushButton::clicked, this, [this] {
        if (!m_document)
            return;
        HighlightRule r;                              // inert until an axis is configured
        r.match.priorityEnabled = true;               // a sensible starting axis
        r.match.minPriority = Priority::Error;
        r.background = 0;                             // Red
        m_rules.append(r);
        commit();
        reloadRuleList();
        m_ruleList->setCurrentRow(m_rules.size() - 1);
    });
    connect(m_removeBtn, &QPushButton::clicked, this, [this] {
        const int row = currentRow();
        if (row < 0)
            return;
        m_rules.remove(row);
        commit();
        reloadRuleList();
        m_ruleList->setCurrentRow(qMin(row, m_rules.size() - 1));
    });
    auto move = [this](int delta) {
        const int row = currentRow();
        const int to = row + delta;
        if (row < 0 || to < 0 || to >= m_rules.size())
            return;
        m_rules.swapItemsAt(row, to);
        commit();
        reloadRuleList();
        m_ruleList->setCurrentRow(to);
    };
    connect(m_upBtn, &QPushButton::clicked, this, [move] { move(-1); });
    connect(m_downBtn, &QPushButton::clicked, this, [move] { move(1); });

    auto editorChanged = [this] {
        if (m_updating)
            return;
        const int row = currentRow();
        if (row < 0 || row >= m_rules.size())
            return;
        HighlightRule &r = m_rules[row];
        r.match = m_axes->criteria();
        r.background = swatchValue(m_bgCombo);
        r.foreground = swatchValue(m_fgCombo);
        commit();
        // Refresh only this row's summary so the current selection is preserved.
        // Guard the setText so its itemChanged does not re-enter the enable handler.
        m_updating = true;
        if (QListWidgetItem *item = m_ruleList->item(row))
            item->setText(ruleSummary(r));
        m_updating = false;
    };
    connect(m_axes, &AxisEditor::changed, this, editorChanged);
    connect(m_bgCombo, &QComboBox::currentIndexChanged, this, [editorChanged](int) { editorChanged(); });
    connect(m_fgCombo, &QComboBox::currentIndexChanged, this, [editorChanged](int) { editorChanged(); });
}

int HighlighterPane::currentRow() const
{
    return m_ruleList ? m_ruleList->currentRow() : -1;
}

QString HighlighterPane::ruleSummary(const HighlightRule &r) const
{
    QString axes = axisSummary(r.match);
    if (axes.isEmpty())
        axes = QStringLiteral("(no match set)");

    auto slotName = [](int i) {
        return HighlightPalette::isSlot(i) ? QString(HighlightPalette::slot(i).name)
                                           : QStringLiteral("default");
    };
    return QStringLiteral("%1  →  bg:%2 / text:%3")
        .arg(axes, slotName(r.background), slotName(r.foreground));
}

void HighlighterPane::reloadRuleList()
{
    m_updating = true;
    m_ruleList->clear();
    for (const HighlightRule &r : m_rules) {
        auto *item = new QListWidgetItem(ruleSummary(r), m_ruleList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(r.enabled ? Qt::Checked : Qt::Unchecked);
    }
    m_updating = false;
    const bool has = !m_rules.isEmpty();
    m_editor->setEnabled(has);
    if (has)
        m_ruleList->setCurrentRow(0);
    else
        loadEditorFor(-1);
}

void HighlighterPane::loadEditorFor(int row)
{
    m_updating = true;
    const bool valid = row >= 0 && row < m_rules.size();
    m_editor->setEnabled(valid);
    if (valid) {
        const HighlightRule &r = m_rules.at(row);
        // setCriteria applies the rule's subsystem/thread selection EXACTLY, so moving
        // between rules shows each rule's own values rather than letting the discovery
        // rule ("a name never listed before arrives checked") leak the previous rule's
        // selection — or the whole file's — into this one.
        m_axes->setCriteria(r.match);
        setSwatchCombo(m_bgCombo, r.background);
        setSwatchCombo(m_fgCombo, r.foreground);
    }
    m_updating = false;
}

void HighlighterPane::syncToDocument()
{
    if (!m_document)
        return;
    m_document->highlighters().rules = m_rules;
    m_document->resolveHighlighters();
}

void HighlighterPane::commit()
{
    syncToDocument();
    emit highlightersChanged();
}

void HighlighterPane::setDocument(Document *document)
{
    m_document = document;
    setEnabled(document != nullptr);
    m_axes->setDocument(document);
    m_rules = document ? document->highlighters().rules : QVector<HighlightRule>();
    reloadRuleList();
}

void HighlighterPane::refreshDiscoveredLists()
{
    // Re-resolve the rules against the grown intern tables (SPEC.md §6 discovery
    // timing) and re-render the editor for the current rule, which repopulates its
    // subsystem/thread lists from the new tables.
    //
    // Deliberately NOT AxisEditor::refreshDiscoveredLists(): that applies the
    // discovery rule, which would tick every newly found subsystem into whichever
    // rule happens to be selected. A filter is a statement about the whole file and
    // should widen with it; a rule naming `net.io` must not silently grow to name
    // `db.pool` as well.
    if (m_document)
        m_document->resolveHighlighters();
    loadEditorFor(currentRow());
}

void HighlighterPane::refreshTimeBounds()
{
    m_axes->refreshTimeBounds();
    // The shown instant is unchanged, but the rule's stored wall clock was written in
    // the old zone; take the re-rendered values back so the two cannot disagree.
    const int row = currentRow();
    if (row >= 0 && row < m_rules.size()) {
        m_rules[row].match = m_axes->criteria();
        commit();
    }
}

void HighlighterPane::addRule(const MatchCriteria &criteria)
{
    if (!m_document)
        return;

    // The first palette slot no existing rule paints with, so a second "highlight
    // this thread" is distinguishable from the first at a glance. Once every slot is
    // spoken for, cycle rather than refuse — a repeated color is a small annoyance,
    // a menu item that silently does nothing is not.
    QSet<int> used;
    for (const HighlightRule &r : m_rules)
        used.insert(r.background);
    int slot = m_rules.size() % HighlightPalette::count();
    for (int i = 0; i < HighlightPalette::count(); ++i) {
        if (!used.contains(i)) {
            slot = i;
            break;
        }
    }

    HighlightRule r;
    r.match = criteria;
    r.background = slot;
    m_rules.append(r);
    commit();
    reloadRuleList();
    m_ruleList->setCurrentRow(m_rules.size() - 1);
}

QJsonObject HighlighterPane::saveState() const
{
    HighlighterSet set;
    set.rules = m_rules;
    QJsonObject o;
    o.insert(QStringLiteral("rules"), set.toJson());
    return o;
}

void HighlighterPane::restoreState(const QJsonObject &o)
{
    m_rules = HighlighterSet::fromJson(o.value(QStringLiteral("rules")).toArray()).rules;
    syncToDocument();
    reloadRuleList();
    emit highlightersChanged();
}

} // namespace loftail
