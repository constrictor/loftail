#include "PreferencesDialog.h"

#include "FormatEditor.h"

#include "FormatCache.h"

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace loftail {

PreferencesDialog::PreferencesDialog(const QByteArray &sample,
                                     const FormatSettings &initial,
                                     QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("preferencesDialog")); // findChild, for tests
    setWindowTitle(tr("Preferences"));
    resize(760, 620);

    auto *outer = new QVBoxLayout(this);

    // One group box rather than a tab widget: there is one section, and a tab bar with a
    // single tab on it is chrome that says nothing. The group is what makes adding the
    // second section a matter of adding a box.
    auto *formatGroup = new QGroupBox(tr("Default log format"), this);
    formatGroup->setObjectName(QStringLiteral("defaultFormatGroup")); // findChild, for tests
    auto *groupLayout = new QVBoxLayout(formatGroup);

    // State the two-level rule up front. It is the one thing about this setting that
    // surprises people: the default applies to files loftail has not seen, and a file
    // that has been configured outranks it.
    auto *explain = new QLabel(
        tr("Logs loftail has not seen before are opened with this format. A log you have "
           "already configured keeps its own format — use the button below to forget those."),
        formatGroup);
    explain->setObjectName(QStringLiteral("defaultFormatExplain")); // findChild, for tests
    explain->setWordWrap(true);
    groupLayout->addWidget(explain);

    m_editor = new FormatEditor(formatGroup);
    m_editor->setObjectName(QStringLiteral("formatEditor")); // findChild, for tests
    // The preview runs over whichever log is open, so a default can be checked against
    // real lines rather than typed blind. With nothing open the sample is empty and the
    // editor says so.
    m_editor->setPreviewCaption(sample.isEmpty()
        ? tr("Preview (open a log to see this format applied to it):")
        : tr("Preview (the current log, split into fields):"));
    m_editor->setSample(sample);
    m_editor->setSettings(initial);
    groupLayout->addWidget(m_editor, 1);

    auto *forgetRow = new QHBoxLayout;
    auto *forgetButton = new QPushButton(tr("&Forget Remembered Formats"), formatGroup);
    forgetButton->setObjectName(QStringLiteral("forgetFormatsButton")); // findChild, for tests
    forgetButton->setToolTip(
        tr("Clear the per-file formats, so every log falls back to the default"));
    connect(forgetButton, &QPushButton::clicked, this,
            &PreferencesDialog::forgetRememberedFormats);
    forgetRow->addWidget(forgetButton);
    forgetRow->addStretch();
    groupLayout->addLayout(forgetRow);

    outer->addWidget(formatGroup, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    // Explicit text for the same reason the other dialogs give it: the platform theme
    // labels standard buttons in the desktop's language, and loftail ships none.
    buttons->button(QDialogButtonBox::Ok)->setText(tr("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);
}

FormatSettings PreferencesDialog::defaultFormat() const
{
    return m_editor->settings();
}

void PreferencesDialog::forgetRememberedFormats()
{
    // Destructive and not undoable, so it asks. It also does NOT reach into open tabs:
    // they keep the format they are displaying and will write it back on their next
    // change, which is why the wording is about what opening a log does from now on
    // rather than about what is on screen.
    const auto answer = QMessageBox::question(
        this, tr("Forget Remembered Formats"),
        tr("Forget the format remembered for every log?\n\n"
           "From now on, opening a log will use the default format above. Logs already "
           "open are not affected."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    QSettings store;
    FormatCache::forgetAll(store);
    m_cacheCleared = true;
}

} // namespace loftail
