#include "RunPane.h"

#include "MessageLabel.h"
#include "UiColors.h"

#include "Document.h"
#include "Record.h"

#include <QCheckBox>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

namespace loftail {

RunPane::RunPane(QWidget *parent) : QWidget(parent)
{
    buildUi();
    setDocument(nullptr);
}

void RunPane::buildUi()
{
    auto *root = new QVBoxLayout(this);

    auto *box = new QGroupBox(tr("Run start"), this);
    auto *v = new QVBoxLayout(box);

    v->addWidget(new QLabel(
        tr("Regexp marking where each run begins (matched against the\n"
                       "whole log line). Leave empty to view the entire file."),
        box));

    m_patternEdit = new QLineEdit(box);
    m_patternEdit->setObjectName(QStringLiteral("runStartPattern")); // test contract, never translated
    m_patternEdit->setPlaceholderText(tr("e.g. Application starting"));
    ensureReadablePlaceholder(m_patternEdit);
    m_patternEdit->setClearButtonEnabled(true);
    v->addWidget(m_patternEdit);

    auto *opts = new QHBoxLayout;
    m_regex = new QCheckBox(tr("Regex"), box);
    m_regex->setObjectName(QStringLiteral("runStartRegex")); // test contract, never translated
    m_case = new QCheckBox(tr("Case sensitive"), box);
    m_case->setObjectName(QStringLiteral("runStartCase")); // test contract, never translated
    opts->addWidget(m_regex);
    opts->addWidget(m_case);
    opts->addStretch(1);
    m_apply = new QPushButton(tr("Apply"), box);
    m_apply->setObjectName(QStringLiteral("runApply")); // test contract, never translated
    opts->addWidget(m_apply);
    v->addLayout(opts);

    // This pane is the only one with an Apply button — the Filters and Highlighters panes
    // act as the user types — so it says why, next to the button that is the difference.
    // Splitting a log into runs re-partitions and re-applies the whole view, which is not
    // something to do per keystroke; but by the time a reader reaches this pane they have
    // learned everywhere else that an edit takes effect as it is made, so an unpressed
    // Apply otherwise reads as a pane that has stopped responding.
    m_applyNote = new MessageLabel(box);
    m_applyNote->setObjectName(QStringLiteral("runApplyNote")); // test contract, never translated
    v->addWidget(m_applyNote);

    m_info = new QLabel(box);
    m_info->setObjectName(QStringLiteral("runInfo")); // test contract, never translated
    m_info->setWordWrap(true);
    v->addWidget(m_info);

    root->addWidget(box);

    auto *runBox = new QGroupBox(tr("Run"), this);
    auto *rv = new QVBoxLayout(runBox);
    m_runList = new QListWidget(runBox);
    m_runList->setObjectName(QStringLiteral("runList"));
    // A run label is a start time, a first line and a record count; at a dock's width
    // it will not fit. Eliding is the right answer rather than a horizontal scrollbar
    // — the head of the label is the part that identifies the run, and the whole of it
    // is one hover away — and switching the scrollbar off is what makes the view elide
    // instead of widening past the pane (the same width trap the Filters pane records).
    m_runList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_runList->setTextElideMode(Qt::ElideRight);
    m_runList->setUniformItemSizes(true);
    rv->addWidget(m_runList, 1);
    // The run box is the one thing here that takes the spare height, and there is
    // deliberately NO trailing addStretch: a stretch below would compete for the same
    // pixels and pin the list to its floor with an empty gap under it.
    root->addWidget(runBox, 1);

    // APPLY AND RETURN ARE THE ONLY ROUTE OUT OF THIS PANE. Both boxes were once wired
    // to emitPattern as well, which meant ticking Regex applied whatever half-typed
    // pattern was standing in the field — re-splitting and re-reading the whole log, and
    // persisting that pattern into the log's settings node — for a gesture the user had
    // not pressed Apply for, and which threw away a pinned run on the way (SPEC.md §3a).
    // Nothing here may emit runStartChanged on an edit; a new control belongs on the
    // note below, not on this list.
    connect(m_apply, &QPushButton::clicked, this, &RunPane::emitPattern);
    connect(m_patternEdit, &QLineEdit::returnPressed, this, &RunPane::emitPattern);
    // NOTHING in this pane is applied as it is edited, so every editable control has to
    // say so — which is the whole of what these three connections do.
    connect(m_patternEdit, &QLineEdit::textChanged, this, &RunPane::updateApplyNote);
    connect(m_regex, &QCheckBox::toggled, this, &RunPane::updateApplyNote);
    connect(m_case, &QCheckBox::toggled, this, &RunPane::updateApplyNote);
    // currentRowChanged, not itemClicked: a list is walked with the arrow keys as well
    // as clicked, and only the current-row signal covers both. It fires on programmatic
    // changes too — including the clear() in rebuildRunList(), which emits row -1 — so
    // the m_populating guard is what keeps a repopulation from looking like a choice.
    connect(m_runList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_populating || row < 0)
            return;
        if (row == kLastRunRow)
            emit runSelected(kLastRun);
        else if (row == kAllRunsRow)
            emit runSelected(kAllRuns);
        else
            emit runSelected(row - kFirstRunRow);
    });
}

void RunPane::setDocument(Document *document)
{
    m_document = document;

    m_populating = true;
    if (m_document) {
        const TextMatcher &m = m_document->runStartMatcher();
        m_patternEdit->setText(m.pattern());
        m_regex->setChecked(m.isRegex());
        m_case->setChecked(m.caseSensitivity() == Qt::CaseSensitive);
    } else {
        m_patternEdit->clear();
        m_regex->setChecked(false);
        m_case->setChecked(false);
    }
    m_populating = false;

    const bool enabled = m_document != nullptr;
    m_patternEdit->setEnabled(enabled);
    m_regex->setEnabled(enabled);
    m_case->setEnabled(enabled);
    m_apply->setEnabled(enabled);
    m_runList->setEnabled(enabled);

    rebuildRunList();
}

void RunPane::refresh()
{
    rebuildRunList();
}

void RunPane::emitPattern()
{
    if (m_populating || !m_document)
        return;
    emit runStartChanged(m_patternEdit->text(), m_regex->isChecked(), m_case->isChecked());
}

void RunPane::rebuildRunList()
{
    m_populating = true;
    // refresh() runs on every live append, so the whole list is rebuilt while the user
    // is looking at it; keeping the scroll offset is what stops it jumping back to the
    // top each time a run's record count ticks over.
    const int scroll = m_runList->verticalScrollBar()->value();
    m_runList->clear();

    // Neither of the first two rows is a run, so both are italic for the same reason
    // AxisEditor's "Others" is: they are the entries that say something ABOUT the list
    // rather than name a member of it, and a log may well start a run with a line that
    // reads like either of them.
    QFont fixedFont = m_runList->font();
    fixedFont.setItalic(true);

    // Row 0, and first because it is the one the pane opens on: a standing instruction
    // to show whichever run is last, not a run. It is what makes a live log show the
    // application's current run across a restart — the run below it is pinned, this
    // one moves (SPEC.md §3a).
    auto *lastRun = new QListWidgetItem(tr("Last run"), m_runList);
    lastRun->setFont(fixedFont);
    lastRun->setToolTip(tr("Always show the newest run, switching to each new one as it "
                           "starts. With no runs detected, the whole file."));

    auto *allRuns = new QListWidgetItem(tr("All runs"), m_runList);
    allRuns->setFont(fixedFont);
    allRuns->setToolTip(tr("Show the whole file, with no run restriction."));

    if (m_document) {
        const QVector<Document::Run> &runs = m_document->runs();
        for (int i = 0; i < runs.size(); ++i) {
            const Document::Run &r = runs.at(i);
            QString when;
            if (r.startTimestamp != Record::kNoTimestamp) {
                when = QDateTime::fromMSecsSinceEpoch(r.startTimestamp, m_document->displayZone())
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            } else {
                when = tr("(no time)");
            }
            QString snippet = r.firstLine.simplified();
            if (snippet.size() > 60)
                snippet = snippet.left(59) + QChar(0x2026); // ellipsis
            const QString label = r.isPreamble
                ? tr("#%1  (before first run)  ·  %2 rec")
                      .arg(i).arg(m_document->runRecordCount(i))
                : tr("#%1  %2  ·  %3  ·  %4 rec")
                      .arg(i).arg(when, snippet).arg(m_document->runRecordCount(i));
            auto *item = new QListWidgetItem(label, m_runList);
            // The label is elided at the pane's width, so the full one is the tooltip.
            item->setToolTip(label);
        }

        // Following the last run is read off the document, never inferred from the
        // selection matching runs().size() - 1: those two agree exactly when following
        // is doing its job, and telling them apart is the whole feature — a run the
        // user pinned while it was last must not silently start moving.
        const int sel = m_document->selectedRun();
        m_runList->setCurrentRow(m_document->followingLastRun() ? kLastRunRow
                                 : sel >= 0                     ? sel + kFirstRunRow
                                                                : kAllRunsRow);
        m_runList->verticalScrollBar()->setValue(scroll);

        // Status line under the pattern field.
        const TextMatcher &m = m_document->runStartMatcher();
        if (m.pattern().isEmpty())
            m_info->setText(tr("No run-start pattern — viewing the whole file."));
        else if (!m.isValid())
            m_info->setText(tr("Invalid regex — nothing matched."));
        else if (runs.isEmpty())
            m_info->setText(tr("Pattern matched no run starts."));
        else
            m_info->setText(tr("%1 run(s) detected.").arg(runs.size()));
    } else {
        m_runList->setCurrentRow(0);
        // NOT cleared. The list below still shows "Last run" and "All runs" — two rows
        // that read as something to click — so the one widget here built to explain the
        // list must not fall silent exactly when there is nothing behind it to explain.
        m_info->setText(tr("No log is open. Open one to split it into runs."));
    }
    m_populating = false;
    updateApplyNote();
}

void RunPane::updateApplyNote()
{
    if (m_populating)
        return;

    // "Unapplied" is measured against the matcher that is actually in force, never
    // against a remembered copy of the field: the pattern can change from under this
    // pane — a rebind to another log, a session restore, a settings node edited in
    // Preferences — and every one of those routes ends at rebuildRunList().
    //
    // All THREE clauses are load-bearing, and the two box clauses most of all: a tick is
    // reported by nothing but this note, so dropping either one leaves a control whose
    // change the pane never mentions and Apply then silently acts on.
    bool pending = false;
    if (m_document) {
        const TextMatcher &m = m_document->runStartMatcher();
        pending = m_patternEdit->text() != m.pattern()
            || m_regex->isChecked() != m.isRegex()
            || m_case->isChecked() != (m.caseSensitivity() == Qt::CaseSensitive);
    }

    const int state = pending ? 1 : 0;
    if (state == m_noteState)
        return;
    m_noteState = state;

    m_applyNote->setText(pending
        ? tr("Edited — press Apply to re-read the runs.")
        : tr("The run-start pattern takes effect when you press Apply."));
    // Both colours come from the palette rather than being picked, so the line lands on
    // a dark theme as well as a light one: muted while it is only an explanation, and
    // the caution amber once it is describing a difference the user has to resolve —
    // which is what a caution is, something that still works and is not what was asked
    // for. It is a colour change and not a bold, because the line does not move.
    m_applyNote->setStyleSheet(QStringLiteral("color: %1;")
        .arg((pending ? warningColor(palette()) : mutedColor(palette())).name()));
}

} // namespace loftail
