#include "RunPane.h"

#include "Document.h"
#include "Record.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

    auto *box = new QGroupBox(QStringLiteral("Run start"), this);
    auto *v = new QVBoxLayout(box);

    v->addWidget(new QLabel(
        QStringLiteral("Regexp marking where each run begins (matched against the\n"
                       "whole log line). Leave empty to view the entire file."),
        box));

    m_patternEdit = new QLineEdit(box);
    m_patternEdit->setPlaceholderText(QStringLiteral("e.g. Application starting"));
    m_patternEdit->setClearButtonEnabled(true);
    v->addWidget(m_patternEdit);

    auto *opts = new QHBoxLayout;
    m_regex = new QCheckBox(QStringLiteral("Regex"), box);
    m_case = new QCheckBox(QStringLiteral("Case sensitive"), box);
    opts->addWidget(m_regex);
    opts->addWidget(m_case);
    opts->addStretch(1);
    m_apply = new QPushButton(QStringLiteral("Apply"), box);
    opts->addWidget(m_apply);
    v->addLayout(opts);

    m_info = new QLabel(box);
    m_info->setWordWrap(true);
    v->addWidget(m_info);

    root->addWidget(box);

    auto *runBox = new QGroupBox(QStringLiteral("Run"), this);
    auto *rv = new QVBoxLayout(runBox);
    m_runCombo = new QComboBox(runBox);
    rv->addWidget(m_runCombo);
    root->addWidget(runBox);

    root->addStretch(1);

    connect(m_apply, &QPushButton::clicked, this, &RunPane::emitPattern);
    connect(m_patternEdit, &QLineEdit::returnPressed, this, &RunPane::emitPattern);
    connect(m_regex, &QCheckBox::toggled, this, &RunPane::emitPattern);
    connect(m_case, &QCheckBox::toggled, this, &RunPane::emitPattern);
    // activated (not currentIndexChanged) fires only on user choice, so setting the
    // index programmatically in rebuildRunList() never re-emits a selection.
    connect(m_runCombo, &QComboBox::activated, this, [this](int comboIndex) {
        if (m_populating)
            return;
        emit runSelected(comboIndex - 1); // combo 0 == "All runs" == run index -1
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
    m_runCombo->setEnabled(enabled);

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
    m_runCombo->clear();
    m_runCombo->addItem(QStringLiteral("All runs"));

    if (m_document) {
        const QVector<Document::Run> &runs = m_document->runs();
        for (int i = 0; i < runs.size(); ++i) {
            const Document::Run &r = runs.at(i);
            QString when;
            if (r.startTimestamp != Record::kNoTimestamp) {
                when = QDateTime::fromMSecsSinceEpoch(r.startTimestamp, m_document->displayZone())
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            } else {
                when = QStringLiteral("(no time)");
            }
            QString snippet = r.firstLine.simplified();
            if (snippet.size() > 60)
                snippet = snippet.left(59) + QChar(0x2026); // ellipsis
            const QString label = r.isPreamble
                ? QStringLiteral("#%1  (before first run)  ·  %2 rec")
                      .arg(i).arg(m_document->runRecordCount(i))
                : QStringLiteral("#%1  %2  ·  %3  ·  %4 rec")
                      .arg(i).arg(when, snippet).arg(m_document->runRecordCount(i));
            m_runCombo->addItem(label);
        }

        const int sel = m_document->selectedRun();
        m_runCombo->setCurrentIndex(sel >= 0 ? sel + 1 : 0);

        // Status line under the pattern field.
        const TextMatcher &m = m_document->runStartMatcher();
        if (m.pattern().isEmpty())
            m_info->setText(QStringLiteral("No run-start pattern — viewing the whole file."));
        else if (!m.isValid())
            m_info->setText(QStringLiteral("Invalid regex — nothing matched."));
        else if (runs.isEmpty())
            m_info->setText(QStringLiteral("Pattern matched no run starts."));
        else
            m_info->setText(QStringLiteral("%1 run(s) detected.").arg(runs.size()));
    } else {
        m_runCombo->setCurrentIndex(0);
        m_info->clear();
    }
    m_populating = false;
}

} // namespace loftail
