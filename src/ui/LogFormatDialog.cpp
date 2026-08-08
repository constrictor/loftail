#include "LogFormatDialog.h"

#include "FormatEditor.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace loftail {

LogFormatDialog::LogFormatDialog(const QString &fileName,
                                 const QByteArray &sample,
                                 const FormatSettings &initial,
                                 QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("logFormatDialog")); // findChild, for tests
    setWindowTitle(fileName.isEmpty() ? tr("Log Format")
                                      : tr("Log Format — %1").arg(fileName));
    resize(760, 600);

    auto *outer = new QVBoxLayout(this);

    m_editor = new FormatEditor(this);
    m_editor->setObjectName(QStringLiteral("formatEditor")); // findChild, for tests
    m_editor->setSample(sample);
    m_editor->setSettings(initial);
    outer->addWidget(m_editor, 1);

    // M18: promote what is on screen to the default for never-seen files. Off by
    // default — this dialog is about one file, and a format confirmed for one log is
    // not a claim about the next one until the user says so.
    m_defaultCheck = new QCheckBox(tr("Also use this format for &new files"), this);
    m_defaultCheck->setObjectName(QStringLiteral("setAsDefaultCheck")); // findChild, for tests
    m_defaultCheck->setToolTip(
        tr("Files loftail has not seen before will be opened with this format"));
    outer->addWidget(m_defaultCheck);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    // The platform theme labels standard buttons in the DESKTOP's language, which
    // on a non-English desktop leaves a dialog that is English everywhere else
    // reading half-translated. loftail ships no translations, so state the text
    // explicitly and keep one language on screen.
    buttons->button(QDialogButtonBox::Ok)->setText(tr("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);
}

FormatSettings LogFormatDialog::settings() const
{
    return m_editor->settings();
}

bool LogFormatDialog::useAsDefault() const
{
    return m_defaultCheck->isChecked();
}

} // namespace loftail
