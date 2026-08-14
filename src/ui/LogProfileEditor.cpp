#include "LogProfileEditor.h"

#include "FormatEditor.h"
#include "SectionBox.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QVBoxLayout>

namespace loftail {

namespace {
// Each combo row carries its enum value in item data rather than relying on the row
// index, so a value added to either enum cannot silently re-point every stored node
// one row along — the trap the priority combo records.
void addTimeDisplayItems(QComboBox *c)
{
    c->addItem(QObject::tr("As written in the file"), int(TimeDisplay::AsWritten));
    c->addItem(QObject::tr("Local time"), int(TimeDisplay::LocalTime));
    c->addItem(QObject::tr("UTC"), int(TimeDisplay::Utc));
    c->addItem(QObject::tr("Seconds since the epoch"), int(TimeDisplay::EpochSeconds));
    c->addItem(QObject::tr("Seconds from run start"), int(TimeDisplay::RunSeconds));
}

void addWrapItems(QComboBox *c)
{
    c->addItem(QObject::tr("Off"), int(WrapMode::Off));
    c->addItem(QObject::tr("Selected record only"), int(WrapMode::SelectedRecordOnly));
    c->addItem(QObject::tr("Always on"), int(WrapMode::AlwaysOn));
}

void selectData(QComboBox *c, int value)
{
    const int row = c->findData(value);
    c->setCurrentIndex(row >= 0 ? row : 0);
}
} // namespace

LogProfileEditor::LogProfileEditor(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_format = new FormatEditor(this);
    m_format->setObjectName(QStringLiteral("formatEditor")); // findChild, for tests
    root->addWidget(m_format, 1);

    auto *runBox = new SectionBox(tr("Run splitting"), this);
    runBox->setObjectName(QStringLiteral("profileRunGroup")); // findChild, for tests
    runBox->setFlat(true);
    runBox->setTitleDivider(true);
    auto *runForm = new QFormLayout(runBox);

    m_runStart = new QLineEdit(runBox);
    m_runStart->setObjectName(QStringLiteral("profileRunStartPattern")); // findChild, for tests
    m_runStart->setPlaceholderText(tr("Leave empty for no run splitting"));
    // Not "starts at" (which reads as an offset or a time, with no object) and not
    // "starts with": the pattern is matched against the WHOLE line, anywhere in it, so
    // the line that begins a run typically starts with a timestamp instead. The line is
    // the thing that matches, and saying so is what the Runs pane says at length.
    runForm->addRow(tr("New run at lines matching:"), m_runStart);

    auto *flags = new QHBoxLayout;
    m_runRegex = new QCheckBox(tr("Regular expression"), runBox);
    m_runRegex->setObjectName(QStringLiteral("profileRunStartRegex")); // findChild, for tests
    m_runCase = new QCheckBox(tr("Case sensitive"), runBox);
    m_runCase->setObjectName(QStringLiteral("profileRunStartCase")); // findChild, for tests
    flags->addWidget(m_runRegex);
    flags->addWidget(m_runCase);
    flags->addStretch();
    runForm->addRow(QString(), flags);
    root->addWidget(runBox);

    auto *viewBox = new SectionBox(tr("Display"), this);
    viewBox->setObjectName(QStringLiteral("profileDisplayGroup")); // findChild, for tests
    viewBox->setFlat(true);
    viewBox->setTitleDivider(true);
    auto *viewForm = new QFormLayout(viewBox);

    m_timeDisplay = new QComboBox(viewBox);
    m_timeDisplay->setObjectName(QStringLiteral("profileTimeDisplay")); // findChild, for tests
    addTimeDisplayItems(m_timeDisplay);
    viewForm->addRow(tr("Timestamps:"), m_timeDisplay);

    m_wrap = new QComboBox(viewBox);
    m_wrap->setObjectName(QStringLiteral("profileWrapMode")); // findChild, for tests
    addWrapItems(m_wrap);
    // Say what it IS, because it is the one setting here that a view can then override:
    // the node seeds a new view and the view owns its mode from then on (invariant #7).
    m_wrap->setToolTip(tr("The line wrapping a newly opened view of this log starts in. "
                          "View ▸ Line Wrap still changes the view in front of you."));
    viewForm->addRow(tr("Line wrap:"), m_wrap);
    root->addWidget(viewBox);
}

void LogProfileEditor::setSample(const QByteArray &sample)
{
    m_format->setSample(sample);
}

void LogProfileEditor::setPreviewCaption(const QString &text)
{
    m_format->setPreviewCaption(text);
}

void LogProfileEditor::setSampleBelongsHere(bool own)
{
    m_format->setSampleBelongsHere(own);
}

void LogProfileEditor::setProfile(const LogProfile &p)
{
    // FormatEditor stashes the fields it does not own and hands them back; seeding it
    // with the whole struct is what keeps that stash correct even though the controls
    // for those fields are below rather than inside it.
    m_format->setSettings(p.format);
    m_runStart->setText(p.format.runStartPattern);
    m_runRegex->setChecked(p.format.runStartIsRegex);
    m_runCase->setChecked(p.format.runStartCaseSensitive);
    selectData(m_timeDisplay, int(p.format.timeDisplay));
    selectData(m_wrap, int(p.wrapMode));
}

LogProfile LogProfileEditor::profile() const
{
    LogProfile p;
    // FIRST — settings() builds a fresh struct, so everything this editor owns is
    // written over the top of it and never before.
    p.format = m_format->settings();
    p.format.runStartPattern = m_runStart->text();
    p.format.runStartIsRegex = m_runRegex->isChecked();
    p.format.runStartCaseSensitive = m_runCase->isChecked();
    p.format.timeDisplay =
        static_cast<TimeDisplay>(m_timeDisplay->currentData().toInt());
    p.wrapMode = static_cast<WrapMode>(m_wrap->currentData().toInt());
    return p;
}

} // namespace loftail
