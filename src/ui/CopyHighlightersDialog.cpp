#include "CopyHighlightersDialog.h"

#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <utility>

namespace loftail {

int CopyHighlightersDialog::preselect(const QVector<Source> &sources)
{
    for (int i = 0; i < sources.size(); ++i) {
        if (!sources.at(i).seeded)
            return i;
    }
    return sources.isEmpty() ? -1 : 0;
}

CopyHighlightersDialog::CopyHighlightersDialog(QVector<Source> sources, const QString &targetLabel,
                                               int targetRuleCount, QWidget *parent)
    : QDialog(parent), m_sources(std::move(sources))
{
    setObjectName(QStringLiteral("copyHighlightersDialog"));
    setWindowTitle(tr("Copy Highlighters"));

    auto *root = new QVBoxLayout(this);

    // What is being LOST, named, because there is no undo. The target's own count is in
    // the sentence for the same reason: replacing an empty list and replacing a list of
    // fifteen rules are different decisions.
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("copySummary"));
    m_summary->setWordWrap(true);
    m_summary->setText(targetRuleCount == 0
                           ? tr("%1 has no highlight rules. Choose the log to copy from.")
                                 .arg(targetLabel)
                           : tr("Replace %n highlight rule(s) on %1 with an exact copy of "
                                "another log's.",
                                nullptr, targetRuleCount)
                                 .arg(targetLabel));
    root->addWidget(m_summary);

    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("copySources"));
    m_list->setColumnCount(2);
    m_list->setHeaderLabels({tr("Log"), tr("Rules")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setAllColumnsShowFocus(true);
    for (const Source &s : std::as_const(m_sources)) {
        // What taking this one MEANS, not merely how many rules it holds: a log nobody
        // has set rules on is the one worth naming, because copying it is very nearly a
        // way of clearing the target rather than of setting it. Counted rather than
        // spelled out, so this stays true if the seeded list ever grows or shrinks —
        // HighlighterSet::defaults() is the only place that decides how many it is.
        const QString what = s.seeded
            ? tr("the %n it arrived with", nullptr, s.ruleCount)
            : tr("%n rule(s)", nullptr, s.ruleCount);
        auto *item = new QTreeWidgetItem(m_list, {s.label, what});
        // The pane is narrow and a label elides, so the whole address is one hover away
        // — and it is a DISPLAY path, which is what keeps a remote log's password off
        // the screen (RemoteLocation::withoutPassword).
        item->setToolTip(0, s.address);
        item->setToolTip(1, s.address);
    }
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    root->addWidget(m_list, 1);

    // Double-click is the gesture anyone tries on a one-of-N list; without it the
    // dialog answers only to a click and then a second click somewhere else.
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] { accept(); });

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->setObjectName(QStringLiteral("copyButtons"));
    // "Replace", not "OK": the button is the last thing read before a list is discarded,
    // and it is the one place a word costs nothing and says what happens.
    if (QPushButton *ok = m_buttons->button(QDialogButtonBox::Ok))
        ok->setText(tr("Replace"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &CopyHighlightersDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(m_buttons);

    // A starting selection, so Replace means something without a click first.
    const int start = preselect(m_sources);
    if (start >= 0) {
        m_list->setCurrentItem(m_list->topLevelItem(start));
        m_list->topLevelItem(start)->setSelected(true);
    }

    resize(480, 320);
}

void CopyHighlightersDialog::accept()
{
    const int row = m_list ? m_list->indexOfTopLevelItem(m_list->currentItem()) : -1;
    if (row < 0 || row >= m_sources.size())
        return; // nothing chosen: stay up rather than answer with a log nobody picked
    m_chosen = row;
    QDialog::accept();
}

int CopyHighlightersDialog::chooseSource(const QVector<Source> &sources,
                                         const QString &targetLabel, int targetRuleCount,
                                         QWidget *parent)
{
    if (sources.isEmpty())
        return -1; // nothing to choose: do not ask. The command is dead in this state.

    CopyHighlightersDialog dialog(sources, targetLabel, targetRuleCount, parent);
    if (dialog.exec() != QDialog::Accepted)
        return -1;
    return dialog.chosenIndex();
}

} // namespace loftail
