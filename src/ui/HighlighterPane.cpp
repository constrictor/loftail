#include "HighlighterPane.h"

#include "Document.h"
#include "Palette.h"
#include "Priority.h"
#include "RecordIndex.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace loftail {

namespace {
// Combo index -> Priority, in severity order (§7.2), matching the filter pane so
// the two priority selectors read identically.
const Priority kPriorityByIndex[] = {
    Priority::Trace, Priority::Debug, Priority::Info,
    Priority::Warn,  Priority::Error, Priority::Fatal,
};
constexpr int kPriorityCount = int(sizeof(kPriorityByIndex) / sizeof(kPriorityByIndex[0]));

int priorityIndexOf(Priority p)
{
    for (int i = 0; i < kPriorityCount; ++i)
        if (kPriorityByIndex[i] == p)
            return i;
    return 3; // WARN default
}

QIcon swatchIcon(const QColor &c)
{
    QPixmap pm(14, 14);
    pm.fill(c.isValid() ? c : Qt::transparent);
    return QIcon(pm);
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

QComboBox *HighlighterPane::makeSwatchCombo()
{
    auto *combo = new QComboBox(this);
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
    root->addWidget(m_ruleList, 1);

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
    m_editor = new QGroupBox(QStringLiteral("Selected rule"), this);
    auto *ev = new QVBoxLayout(m_editor);

    m_matchPriority = new QCheckBox(QStringLiteral("Match minimum priority"), m_editor);
    ev->addWidget(m_matchPriority);
    auto *prow = new QHBoxLayout;
    prow->addWidget(new QLabel(QStringLiteral("Minimum:"), m_editor));
    m_priorityCombo = new QComboBox(m_editor);
    for (Priority p : kPriorityByIndex)
        m_priorityCombo->addItem(priorityName(p));
    prow->addWidget(m_priorityCombo, 1);
    ev->addLayout(prow);

    m_matchLogger = new QCheckBox(QStringLiteral("Match subsystem"), m_editor);
    ev->addWidget(m_matchLogger);
    m_loggerList = new QListWidget(m_editor);
    m_loggerList->setMinimumHeight(80);
    ev->addWidget(m_loggerList);

    auto *bgRow = new QHBoxLayout;
    bgRow->addWidget(new QLabel(QStringLiteral("Background:"), m_editor));
    m_bgCombo = makeSwatchCombo();
    bgRow->addWidget(m_bgCombo, 1);
    ev->addLayout(bgRow);

    auto *fgRow = new QHBoxLayout;
    fgRow->addWidget(new QLabel(QStringLiteral("Text:"), m_editor));
    m_fgCombo = makeSwatchCombo();
    fgRow->addWidget(m_fgCombo, 1);
    ev->addLayout(fgRow);

    root->addWidget(m_editor);

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
        HighlightRule r;              // inert until an axis is configured
        r.matchPriority = true;       // a sensible starting axis
        r.minPriority = Priority::Error;
        r.background = 0;             // Red
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
        r.matchPriority = m_matchPriority->isChecked();
        r.minPriority = kPriorityByIndex[qBound(0, m_priorityCombo->currentIndex(), kPriorityCount - 1)];
        r.matchLogger = m_matchLogger->isChecked();
        r.loggerNames.clear();
        for (int i = 0; i < m_loggerList->count(); ++i) {
            QListWidgetItem *it = m_loggerList->item(i);
            if (it->checkState() == Qt::Checked)
                r.loggerNames.append(it->text());
        }
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
    connect(m_matchPriority, &QCheckBox::toggled, this, editorChanged);
    connect(m_priorityCombo, &QComboBox::currentIndexChanged, this, [editorChanged](int) { editorChanged(); });
    connect(m_matchLogger, &QCheckBox::toggled, this, editorChanged);
    connect(m_loggerList, &QListWidget::itemChanged, this, [editorChanged](QListWidgetItem *) { editorChanged(); });
    connect(m_bgCombo, &QComboBox::currentIndexChanged, this, [editorChanged](int) { editorChanged(); });
    connect(m_fgCombo, &QComboBox::currentIndexChanged, this, [editorChanged](int) { editorChanged(); });
}

int HighlighterPane::currentRow() const
{
    return m_ruleList ? m_ruleList->currentRow() : -1;
}

QString HighlighterPane::ruleSummary(const HighlightRule &r) const
{
    QStringList parts;
    if (r.matchPriority)
        parts << QStringLiteral("≥%1").arg(QString(priorityName(r.minPriority)));
    if (r.matchLogger) {
        if (r.loggerNames.size() == 1)
            parts << r.loggerNames.first();
        else
            parts << QStringLiteral("%1 subsystems").arg(r.loggerNames.size());
    }
    if (parts.isEmpty())
        parts << QStringLiteral("(no match set)");

    auto slotName = [](int i) {
        return HighlightPalette::isSlot(i) ? QString(HighlightPalette::slot(i).name)
                                           : QStringLiteral("default");
    };
    return QStringLiteral("%1  →  bg:%2 / text:%3")
        .arg(parts.join(QStringLiteral(", ")), slotName(r.background), slotName(r.foreground));
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
        m_matchPriority->setChecked(r.matchPriority);
        m_priorityCombo->setCurrentIndex(priorityIndexOf(r.minPriority));
        m_matchLogger->setChecked(r.matchLogger);
        setSwatchCombo(m_bgCombo, r.background);
        setSwatchCombo(m_fgCombo, r.foreground);
        // The subsystem list: discovered names plus any the rule references that the
        // scan has not produced yet, checking those the rule selects.
        m_loggerList->clear();
        QStringList names;
        if (m_document)
            names = m_document->index().loggers.names();
        for (const QString &n : r.loggerNames)
            if (!names.contains(n))
                names.append(n);
        QStringList clean;
        for (const QString &n : names)
            if (!n.isEmpty() && !clean.contains(n))
                clean.append(n);
        clean.sort(Qt::CaseInsensitive);
        const QSet<QString> selected(r.loggerNames.begin(), r.loggerNames.end());
        for (const QString &n : clean) {
            auto *it = new QListWidgetItem(n, m_loggerList);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(selected.contains(n) ? Qt::Checked : Qt::Unchecked);
        }
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
    m_rules = document ? document->highlighters().rules : QVector<HighlightRule>();
    reloadRuleList();
}

void HighlighterPane::refreshDiscoveredLists()
{
    // Re-resolve the rules against the grown intern table and refresh the editor's
    // subsystem list for the current selection (SPEC.md §6 discovery timing).
    if (m_document)
        m_document->resolveHighlighters();
    loadEditorFor(currentRow());
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
