// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SerialSettingsDialog.h"

#include "Fonts.h"
#include "SectionBox.h"
#include "SerialProfileStore.h"
#include "UiColors.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace loftail {
namespace {

// Built from a TABLE rather than spelled out per control, which is the timestamp submenu's own
// precedent: a value added to the enum is a row here and nothing else.
void fillBaud(QComboBox *c)
{
    for (const int baud : {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800,
                           921600})
        c->addItem(QString::number(baud), baud);
}

void fillDataBits(QComboBox *c)
{
    for (const int bits : {5, 6, 7, 8})
        c->addItem(QString::number(bits), bits);
}

void fillParity(QComboBox *c)
{
    c->addItem(QObject::tr("None"), int(SerialProfile::Parity::None));
    c->addItem(QObject::tr("Even"), int(SerialProfile::Parity::Even));
    c->addItem(QObject::tr("Odd"), int(SerialProfile::Parity::Odd));
    c->addItem(QObject::tr("Mark"), int(SerialProfile::Parity::Mark));
    c->addItem(QObject::tr("Space"), int(SerialProfile::Parity::Space));
}

void fillStopBits(QComboBox *c)
{
    c->addItem(QStringLiteral("1"), int(SerialProfile::StopBits::One));
    c->addItem(QStringLiteral("1.5"), int(SerialProfile::StopBits::OneAndHalf));
    c->addItem(QStringLiteral("2"), int(SerialProfile::StopBits::Two));
}

void fillFlow(QComboBox *c)
{
    c->addItem(QObject::tr("None"), int(SerialProfile::Flow::None));
    c->addItem(QObject::tr("Hardware (RTS/CTS)"), int(SerialProfile::Flow::Hardware));
    c->addItem(QObject::tr("Software (XON/XOFF)"), int(SerialProfile::Flow::Software));
}

void selectData(QComboBox *c, int value)
{
    const int row = c->findData(value);
    c->setCurrentIndex(row >= 0 ? row : 0);
}

} // namespace

SerialSettingsDialog::SerialSettingsDialog(SerialProfileStore *store, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
{
    setObjectName(QStringLiteral("serialSettingsDialog")); // findChild, for tests
    setWindowTitle(tr("Serial Settings"));
    buildUi();
    reload();
}

void SerialSettingsDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    auto *columns = new QHBoxLayout;

    // --- the presets, on the left ------------------------------------------
    auto *listBox = new SectionBox(tr("Presets"), this);
    listBox->setObjectName(QStringLiteral("serialPresetsGroup"));
    auto *listLayout = new QVBoxLayout(listBox);
    m_list = new QListWidget(listBox);
    m_list->setObjectName(QStringLiteral("serialPresetList")); // findChild, for tests
    listLayout->addWidget(m_list);

    auto *listButtons = new QHBoxLayout;
    m_add = new QPushButton(tr("&New"), listBox);
    m_add->setObjectName(QStringLiteral("serialPresetNew"));
    m_duplicate = new QPushButton(tr("D&uplicate"), listBox);
    m_duplicate->setObjectName(QStringLiteral("serialPresetDuplicate"));
    m_remove = new QPushButton(tr("&Delete"), listBox);
    m_remove->setObjectName(QStringLiteral("serialPresetDelete"));
    listButtons->addWidget(m_add);
    listButtons->addWidget(m_duplicate);
    listButtons->addWidget(m_remove);
    listButtons->addStretch(1);
    listLayout->addLayout(listButtons);
    columns->addWidget(listBox, 1);

    // --- the settings, on the right ----------------------------------------
    auto *right = new QVBoxLayout;

    auto *lineBox = new SectionBox(tr("Line"), this);
    lineBox->setObjectName(QStringLiteral("serialLineGroup"));
    auto *lineForm = new QFormLayout(lineBox);
    m_name = new QLineEdit(lineBox);
    m_name->setObjectName(QStringLiteral("serialPresetName")); // findChild, for tests
    m_name->setPlaceholderText(tr("What to call this preset"));
    lineForm->addRow(tr("Na&me:"), m_name);
    m_baud = new QComboBox(lineBox);
    m_baud->setObjectName(QStringLiteral("serialBaud"));
    fillBaud(m_baud);
    lineForm->addRow(tr("&Baud:"), m_baud);
    m_dataBits = new QComboBox(lineBox);
    m_dataBits->setObjectName(QStringLiteral("serialDataBits"));
    fillDataBits(m_dataBits);
    lineForm->addRow(tr("&Data bits:"), m_dataBits);
    m_parity = new QComboBox(lineBox);
    m_parity->setObjectName(QStringLiteral("serialParity"));
    fillParity(m_parity);
    lineForm->addRow(tr("Pa&rity:"), m_parity);
    m_stopBits = new QComboBox(lineBox);
    m_stopBits->setObjectName(QStringLiteral("serialStopBits"));
    fillStopBits(m_stopBits);
    lineForm->addRow(tr("Stop bi&ts:"), m_stopBits);
    m_flow = new QComboBox(lineBox);
    m_flow->setObjectName(QStringLiteral("serialFlow"));
    fillFlow(m_flow);
    lineForm->addRow(tr("&Flow control:"), m_flow);
    m_pollMs = new QSpinBox(lineBox);
    m_pollMs->setObjectName(QStringLiteral("serialPollMs"));
    m_pollMs->setRange(200, 60000);
    m_pollMs->setSingleStep(250);
    m_pollMs->setSuffix(tr(" ms"));
    // Why it is slower than the network default, said where somebody is about to change it.
    m_pollMs->setToolTip(tr("How often to ask the device whether the log has grown. Slower than "
                            "a network log by default: a round trip here is a command and a "
                            "framed answer over a line that moves about 11 KB/s at 115200, so "
                            "polling quickly spends the line on asking rather than on reading."));
    lineForm->addRow(tr("Check &every:"), m_pollMs);
    right->addWidget(lineBox);

    auto *loginBox = new SectionBox(tr("Signing in"), this);
    loginBox->setObjectName(QStringLiteral("serialLoginGroup"));
    auto *loginForm = new QFormLayout(loginBox);
    m_loginPrompt = new QLineEdit(loginBox);
    m_loginPrompt->setObjectName(QStringLiteral("serialLoginPrompt"));
    m_loginPrompt->setToolTip(tr("A regular expression matched against what the device prints. "
                                 "Leave it empty for a device that never asks."));
    loginForm->addRow(tr("&Login prompt:"), m_loginPrompt);
    m_passwordPrompt = new QLineEdit(loginBox);
    m_passwordPrompt->setObjectName(QStringLiteral("serialPasswordPrompt"));
    loginForm->addRow(tr("Pass&word prompt:"), m_passwordPrompt);
    m_promptTimeout = new QSpinBox(loginBox);
    m_promptTimeout->setObjectName(QStringLiteral("serialPromptTimeout"));
    m_promptTimeout->setRange(500, 60000);
    m_promptTimeout->setSingleStep(500);
    m_promptTimeout->setSuffix(tr(" ms"));
    loginForm->addRow(tr("Wait &up to:"), m_promptTimeout);
    m_attempts = new QSpinBox(loginBox);
    m_attempts->setObjectName(QStringLiteral("serialAttempts"));
    m_attempts->setRange(1, 20);
    loginForm->addRow(tr("&Attempts:"), m_attempts);

    // ONE NOTE FOR BOTH PATTERNS, and it is a warning only when there is something to warn
    // about — the Open Remote form's consent-row rule, including its reason: a row that appears
    // and disappears resizes the dialog under the pointer. An invalid pattern MATCHES NOTHING,
    // so the setting silently does nothing, which is exactly what has to be said out loud.
    m_patternNote = new QLabel(loginBox);
    m_patternNote->setObjectName(QStringLiteral("serialPatternNote")); // findChild, for tests
    m_patternNote->setWordWrap(true);
    m_patternNote->setMinimumHeight(2 * m_patternNote->fontMetrics().lineSpacing());
    loginForm->addRow(QString(), m_patternNote);
    right->addWidget(loginBox);

    auto *afterBox = new SectionBox(tr("After signing in"), this);
    afterBox->setObjectName(QStringLiteral("serialAfterGroup"));
    auto *afterLayout = new QVBoxLayout(afterBox);
    auto *startupHint = new QLabel(tr("One command per line, run in order. This is where a "
                                      "chatty console is quietened — `dmesg -n 1` and the like — "
                                      "because everything the device prints unasked arrives on "
                                      "the same line loftail reads the log over."),
                                   afterBox);
    startupHint->setWordWrap(true);
    afterLayout->addWidget(startupHint);
    m_startup = new QPlainTextEdit(afterBox);
    m_startup->setObjectName(QStringLiteral("serialStartupCommands")); // findChild, for tests
    m_startup->setFont(monospaceFont()); // they are commands
    m_startup->setTabChangesFocus(true);
    m_startup->setFixedHeight(4 * m_startup->fontMetrics().lineSpacing()
                              + 2 * m_startup->frameWidth() + 8);
    afterLayout->addWidget(m_startup);
    auto *rebootForm = new QFormLayout;
    m_rebootPattern = new QLineEdit(afterBox);
    m_rebootPattern->setObjectName(QStringLiteral("serialRebootPattern")); // findChild, for tests
    m_rebootPattern->setPlaceholderText(QStringLiteral("U-Boot|Linux version"));
    m_rebootPattern->setToolTip(tr("What a restart looks like in what the device prints. When it "
                                   "matches, loftail signs in again and carries on — the log's "
                                   "own state then decides whether it is re-read. Leave it empty "
                                   "to notice nothing."));
    rebootForm->addRow(tr("&Reboot notice:"), m_rebootPattern);
    afterLayout->addLayout(rebootForm);
    right->addWidget(afterBox);

    right->addStretch(1);
    columns->addLayout(right, 2);
    root->addLayout(columns);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        commitCurrent();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_loading)
            return;
        commitCurrent();
        showPreset(row);
    });
    connect(m_add, &QPushButton::clicked, this, &SerialSettingsDialog::addPreset);
    connect(m_duplicate, &QPushButton::clicked, this, &SerialSettingsDialog::duplicatePreset);
    connect(m_remove, &QPushButton::clicked, this, &SerialSettingsDialog::removePreset);
    connect(m_loginPrompt, &QLineEdit::textChanged, this,
            &SerialSettingsDialog::updatePatternNotes);
    connect(m_passwordPrompt, &QLineEdit::textChanged, this,
            &SerialSettingsDialog::updatePatternNotes);
    connect(m_rebootPattern, &QLineEdit::textChanged, this,
            &SerialSettingsDialog::updatePatternNotes);
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString &text) {
        // The list follows the name as it is typed, so the entry a reader is editing is the entry
        // they can see — and the rename is not deferred to an OK they may never press.
        if (m_loading || m_current < 0 || m_current >= m_list->count())
            return;
        m_list->item(m_current)->setText(text.trimmed().isEmpty() ? tr("(unnamed)")
                                                                  : text.trimmed());
        updateActions();
    });
}

void SerialSettingsDialog::reload()
{
    m_presets = m_store ? m_store->presets() : QVector<SerialProfile>{};
    const QSignalBlocker block(m_list);
    m_list->clear();
    for (const SerialProfile &p : m_presets)
        m_list->addItem(p.name.trimmed().isEmpty() ? tr("(unnamed)") : p.name);
    m_current = -1;
    if (!m_presets.isEmpty()) {
        m_list->setCurrentRow(0);
        showPreset(0);
    } else {
        showPreset(-1);
    }
}

void SerialSettingsDialog::showPreset(int row)
{
    // GUARDED, and saved-and-restored is not needed here because nothing re-enters — but the
    // guard itself is, because every setter below fires a signal that would otherwise read as a
    // user edit and write the outgoing preset's values over the incoming one. HighlighterPane's
    // m_updating records the same trap, and it cost that class every rule from the second on.
    m_loading = true;
    m_current = row;
    const bool have = row >= 0 && row < m_presets.size();
    const SerialProfile p = have ? m_presets.at(row) : SerialProfile::builtIn();

    m_name->setText(have ? p.name : QString());
    selectData(m_baud, p.baud);
    selectData(m_dataBits, p.dataBits);
    selectData(m_parity, int(p.parity));
    selectData(m_stopBits, int(p.stopBits));
    selectData(m_flow, int(p.flow));
    m_pollMs->setValue(p.pollMs);
    m_loginPrompt->setText(p.loginPromptRe);
    m_passwordPrompt->setText(p.passwordPromptRe);
    m_promptTimeout->setValue(p.promptTimeoutMs);
    m_attempts->setValue(p.attempts);
    m_startup->setPlainText(p.startupCommands.join(u'\n'));
    m_rebootPattern->setText(p.rebootRe);
    m_loading = false;

    updateActions();
    updatePatternNotes();
}

SerialProfile SerialSettingsDialog::currentFields() const
{
    SerialProfile p;
    p.name = m_name->text().trimmed();
    p.baud = m_baud->currentData().toInt();
    p.dataBits = m_dataBits->currentData().toInt();
    p.parity = static_cast<SerialProfile::Parity>(m_parity->currentData().toInt());
    p.stopBits = static_cast<SerialProfile::StopBits>(m_stopBits->currentData().toInt());
    p.flow = static_cast<SerialProfile::Flow>(m_flow->currentData().toInt());
    p.pollMs = m_pollMs->value();
    p.loginPromptRe = m_loginPrompt->text();
    p.passwordPromptRe = m_passwordPrompt->text();
    p.promptTimeoutMs = m_promptTimeout->value();
    p.attempts = m_attempts->value();
    // TRIMMED AT THE ENDS ONLY, and empty lines dropped: a stray blank line would otherwise be a
    // command, and a command that is nothing is a step the login waits for a marker from.
    for (const QString &line : m_startup->toPlainText().split(u'\n')) {
        const QString command = line.trimmed();
        if (!command.isEmpty())
            p.startupCommands.append(command);
    }
    p.rebootRe = m_rebootPattern->text().trimmed();
    return p;
}

void SerialSettingsDialog::commitCurrent()
{
    if (m_loading || m_current < 0 || m_current >= m_presets.size())
        return;
    m_presets[m_current] = currentFields();
}

void SerialSettingsDialog::addPreset()
{
    commitCurrent();
    SerialProfile fresh = SerialProfile::builtIn();
    // A name that is unique among what is here, so the list never shows two entries a reader
    // cannot tell apart — SerialProfileStore's identity rule, enforced where the entry is made
    // rather than left for the save to resolve silently.
    int n = m_presets.size() + 1;
    forever {
        const QString candidate = tr("Device %1").arg(n);
        if (SerialProfileStore::indexOfName(m_presets, candidate) < 0) {
            fresh.name = candidate;
            break;
        }
        ++n;
    }
    m_presets.append(fresh);
    const QSignalBlocker block(m_list);
    m_list->addItem(fresh.name);
    m_list->setCurrentRow(m_presets.size() - 1);
    showPreset(m_presets.size() - 1);
    m_name->setFocus();
    m_name->selectAll();
}

void SerialSettingsDialog::duplicatePreset()
{
    commitCurrent();
    if (m_current < 0 || m_current >= m_presets.size())
        return;
    SerialProfile copy = m_presets.at(m_current);
    int n = 2;
    forever {
        const QString candidate = tr("%1 (%2)").arg(copy.name).arg(n);
        if (SerialProfileStore::indexOfName(m_presets, candidate) < 0) {
            copy.name = candidate;
            break;
        }
        ++n;
    }
    m_presets.append(copy);
    const QSignalBlocker block(m_list);
    m_list->addItem(copy.name);
    m_list->setCurrentRow(m_presets.size() - 1);
    showPreset(m_presets.size() - 1);
}

void SerialSettingsDialog::removePreset()
{
    if (m_current < 0 || m_current >= m_presets.size())
        return;
    // DOES NOT ASK, unlike removing a saved host — and the difference is what is being discarded.
    // A saved host may take a remembered password and every remembered path with it, which is why
    // that one confirms; a preset holds settings alone, the list is right there, and Cancel
    // discards the whole visit.
    m_presets.remove(m_current);
    const QSignalBlocker block(m_list);
    delete m_list->takeItem(m_current);
    const int next = qMin(m_current, m_presets.size() - 1);
    m_list->setCurrentRow(next);
    showPreset(next);
}

void SerialSettingsDialog::updateActions()
{
    const bool have = m_current >= 0 && m_current < m_presets.size();
    m_duplicate->setEnabled(have);
    m_remove->setEnabled(have);
    m_name->setEnabled(have);
    m_baud->setEnabled(have);
    m_dataBits->setEnabled(have);
    m_parity->setEnabled(have);
    m_stopBits->setEnabled(have);
    m_flow->setEnabled(have);
    m_pollMs->setEnabled(have);
    m_loginPrompt->setEnabled(have);
    m_passwordPrompt->setEnabled(have);
    m_promptTimeout->setEnabled(have);
    m_attempts->setEnabled(have);
    m_startup->setEnabled(have);
    m_rebootPattern->setEnabled(have);
}

void SerialSettingsDialog::updatePatternNotes()
{
    // AN INVALID PATTERN MATCHES NOTHING, so the setting silently does nothing — which is the one
    // thing about these three fields that has to be said out loud rather than discovered when a
    // login does not take. Named individually, because "one of your patterns is wrong" sends the
    // reader to check all three.
    QStringList broken;
    const auto check = [&broken](const QLineEdit *field, const QString &what) {
        const QString pattern = field->text().trimmed();
        if (!pattern.isEmpty() && !QRegularExpression(pattern).isValid())
            broken.append(what);
    };
    check(m_loginPrompt, tr("login prompt"));
    check(m_passwordPrompt, tr("password prompt"));
    check(m_rebootPattern, tr("reboot notice"));

    if (broken.isEmpty()) {
        // Muted and factual where there is nothing wrong, so the row is not a standing alarm —
        // a warning that is always on says nothing (ConfigView::BusyTone's own argument).
        m_patternNote->setText(tr("Patterns are regular expressions, matched anywhere in what "
                                  "the device prints."));
        m_patternNote->setStyleSheet(QStringLiteral("color: %1;").arg(mutedColor(palette()).name()));
        return;
    }
    m_patternNote->setText(tr("⚠ The %1 is not a valid regular expression, so it will match "
                              "nothing.")
                               .arg(broken.join(tr(" and the "))));
    m_patternNote->setStyleSheet(QStringLiteral("color: %1;").arg(errorColor(palette()).name()));
}

} // namespace loftail
