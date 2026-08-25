#include "LogProfileEditor.h"

#include "FormatEditor.h"
#include "Fonts.h"
#include "SectionBox.h"
#include "UiColors.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
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

    // Where the log's config file is (SPEC.md §4). Its own section rather than a row in
    // one of the three above, because it is not a property of the log's FORMAT, not part
    // of run splitting and not a display choice: it names a different file entirely.
    auto *configBox = new SectionBox(tr("Configuration file"), this);
    configBox->setObjectName(QStringLiteral("profileConfigGroup")); // findChild, for tests
    configBox->setFlat(true);
    configBox->setTitleDivider(true);
    auto *configForm = new QFormLayout(configBox);

    m_configPath = new QLineEdit(configBox);
    m_configPath->setObjectName(QStringLiteral("profileConfigPath")); // findChild, for tests
    m_configPath->setPlaceholderText(tr("Leave empty to be asked for one"));
    // Say what a RELATIVE path is relative to, because that is the whole reason this
    // setting is worth having above the per-log level and it is not guessable: one
    // pattern node saying `../conf/log4cplus.properties` resolves against each matching
    // log's own directory, so it names a different file for each of them.
    m_configPath->setToolTip(tr("The file that says which subsystems this application "
                                "logs at which priority. A relative path is resolved "
                                "against the log's own directory, so one entry here can "
                                "serve every log a pattern matches. The config file is "
                                "always on the same machine as the log."));
    configForm->addRow(tr("Path:"), m_configPath);
    root->addWidget(configBox);

    // The script that restarts the application writing this log (SPEC.md §4). Its own
    // section for the reason the config path has one, and more so: this is not a property
    // of the log's format, not run splitting and not a display choice — it names an
    // ACTION on another program.
    auto *restartBox = new SectionBox(tr("Restart script"), this);
    restartBox->setObjectName(QStringLiteral("profileRestartGroup")); // findChild, for tests
    restartBox->setFlat(true);
    restartBox->setTitleDivider(true);
    auto *restartLayout = new QVBoxLayout(restartBox);

    m_restartScript = new QPlainTextEdit(restartBox);
    m_restartScript->setObjectName(QStringLiteral("profileRestartScript")); // findChild
    // A QPlainTextEdit, not a QTextEdit: rich text would let a pasted script arrive with
    // formatting and, worse, with smart quotes — which a shell reads as ordinary
    // characters and then cannot find a command called `“systemctl”`.
    m_restartScript->setFont(monospaceFont()); // it is code
    // Tab must LEAVE the field. In a dialog the alternative is a control the keyboard
    // cannot get out of, and a tab is not something a shell script needs to contain.
    m_restartScript->setTabChangesFocus(true);
    m_restartScript->setPlaceholderText(tr("Leave empty for no restart script"));
    ensureReadablePlaceholder(m_restartScript);
    // FIXED, and modest. The dialog's right-hand panel has no scroll area of its own and
    // the format editor above already takes the stretch, so a field that grew with its
    // content would push the format preview off the bottom of the dialog.
    m_restartScript->setFixedHeight(6 * m_restartScript->fontMetrics().lineSpacing()
                                    + 2 * m_restartScript->frameWidth() + 8);
    m_restartScript->setToolTip(
        tr("A shell script that restarts the application writing this log. It is run as a "
           "whole by your default shell — on the remote machine for an ssh:// log, and on "
           "the machine holding the container for an archived one.\n\n"
           "It is given LOGFILE, this log's path on the machine the script runs on; and "
           "for an archived log ARCHIVE and MEMBER, the container's path and the log's "
           "path inside it. Write them $LOGFILE, and %LOGFILE% on Windows.\n\n"
           "A script that leaves a program running in the background should redirect its "
           "output — mycmd >/dev/null 2>&1 & — or loftail goes on waiting for it."));
    restartLayout->addWidget(m_restartScript);
    root->addWidget(restartBox);
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
    m_configPath->setText(p.configPath);
    m_restartScript->setPlainText(p.restartScript);
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
    p.configPath = m_configPath->text().trimmed();
    // TRIMMED AT THE ENDS ONLY, and the trim is load-bearing rather than tidy: a stray
    // trailing newline is a difference from what the log inherits, and a difference is
    // what gives a log a per-log record of its own — so without it, merely looking at a
    // log in Preferences would leave an entry behind saying nothing. The interior is
    // untouched, because a script's blank lines and indentation are the script's.
    p.restartScript = m_restartScript->toPlainText()
                          .replace(QLatin1String("\r\n"), QLatin1String("\n"))
                          .trimmed();
    return p;
}

} // namespace loftail
