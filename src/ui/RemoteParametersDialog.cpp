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

#include "RemoteParametersDialog.h"

#include "CollapsibleSection.h"
#include "DiscoveryBrowser.h"
#include "OpenRemoteDialog.h"
#include "SecretStore.h"
#include "SerialProfileStore.h"
#include "SshPrompter.h"
#include "UiColors.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace loftail {

namespace {

QLabel *mutedNote(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    QPalette p = label->palette();
    p.setColor(QPalette::WindowText, mutedColor(parent->palette()));
    label->setPalette(p);
    return label;
}

} // namespace

RemoteParametersDialog::RemoteParametersDialog(HostBookmarkStore *store, QWidget *parent)
    : QDialog(parent), m_store(store)
{
    setWindowTitle(tr("Remote Parameters"));
    setObjectName(QStringLiteral("remoteParametersDialog"));
    setModal(true);

    auto *outer = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("remoteParametersTabs"));
    outer->addWidget(m_tabs);

    m_tabs->addTab(buildTypesPage(), tr("Remote types"));

    // THE SAVED-HOST EDITOR IS THE OLD OPEN REMOTE FORM, embedded: one form for one file's
    // records, so its rules cannot drift (see the header). A QDialog becomes an ordinary child
    // widget with Qt::Widget, and manage mode takes its Open and Cancel away.
    m_hosts = new OpenRemoteDialog(store, this);
    m_hosts->setWindowFlags(Qt::Widget);
    m_hosts->setManageMode(true);
    m_tabs->addTab(m_hosts, tr("Saved hosts"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    outer->addWidget(buttons);

    reloadTypes();
    clearTypeForm();
}

RemoteParametersDialog::~RemoteParametersDialog() = default;

void RemoteParametersDialog::showHostsPage()
{
    m_tabs->setCurrentWidget(m_hosts);
}

QWidget *RemoteParametersDialog::buildTypesPage()
{
    auto *page = new QWidget(this);
    auto *columns = new QHBoxLayout(page);

    // --- the list ------------------------------------------------------------
    auto *listBox = new QGroupBox(tr("Remote types"), page);
    auto *listLayout = new QVBoxLayout(listBox);
    m_typeList = new QListWidget(listBox);
    m_typeList->setObjectName(QStringLiteral("remoteTypeList"));
    m_typeList->setMinimumWidth(170);
    listLayout->addWidget(m_typeList);
    // A list of names, so it is given a name's width and the form the rest: left to its own
    // size policy it took half the dialog to show a word.
    listBox->setMaximumWidth(260);
    listLayout->addWidget(mutedNote(
        tr("A remote type finds machines of one kind on the network, and says how to sign in "
           "to one. Logs opened through it are remembered by the type and the device, not by "
           "address."),
        listBox));
    auto *listButtons = new QHBoxLayout;
    m_typeNew = new QPushButton(tr("New"), listBox);
    m_typeNew->setObjectName(QStringLiteral("remoteTypeNew"));
    m_typeSave = new QPushButton(tr("Save"), listBox);
    m_typeSave->setObjectName(QStringLiteral("remoteTypeSave"));
    m_typeRemove = new QPushButton(tr("Remove"), listBox);
    m_typeRemove->setObjectName(QStringLiteral("remoteTypeRemove"));
    listButtons->addWidget(m_typeNew);
    listButtons->addWidget(m_typeSave);
    listButtons->addWidget(m_typeRemove);
    listLayout->addLayout(listButtons);
    columns->addWidget(listBox);

    auto *right = new QVBoxLayout;
    columns->addLayout(right, 1);

    // --- how to find one -------------------------------------------------------
    auto *findBox = new QGroupBox(tr("Finding devices"), page);
    auto *findForm = new QFormLayout(findBox);
    findForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_name = new QLineEdit(findBox);
    m_name->setObjectName(QStringLiteral("remoteTypeName"));
    m_name->setPlaceholderText(tr("Lab cameras"));
    findForm->addRow(tr("Name:"), m_name);
    m_discovery = new QComboBox(findBox);
    m_discovery->setObjectName(QStringLiteral("remoteTypeDiscovery"));
    m_discovery->addItem(tr("Bonjour (mDNS / DNS-SD)"), int(RemoteType::Discovery::Bonjour));
    m_discovery->addItem(tr("ONVIF (WS-Discovery)"), int(RemoteType::Discovery::Onvif));
    findForm->addRow(tr("Discovery:"), m_discovery);

    m_bonjourBox = new QGroupBox(tr("Bonjour"), findBox);
    auto *bonjourForm = new QFormLayout(m_bonjourBox);
    m_service = new QLineEdit(m_bonjourBox);
    m_service->setObjectName(QStringLiteral("remoteTypeService"));
    m_service->setPlaceholderText(QStringLiteral("_ssh._tcp"));
    bonjourForm->addRow(tr("Service type:"), m_service);
    m_domain = new QLineEdit(m_bonjourBox);
    m_domain->setObjectName(QStringLiteral("remoteTypeDomain"));
    m_domain->setPlaceholderText(QStringLiteral("local"));
    bonjourForm->addRow(tr("Domain:"), m_domain);
    m_instanceRe = new QLineEdit(m_bonjourBox);
    m_instanceRe->setObjectName(QStringLiteral("remoteTypeInstance"));
    m_instanceRe->setPlaceholderText(tr("regular expression; empty matches every name"));
    bonjourForm->addRow(tr("Name matches:"), m_instanceRe);
    m_txtRe = new QLineEdit(m_bonjourBox);
    m_txtRe->setObjectName(QStringLiteral("remoteTypeTxt"));
    m_txtRe->setPlaceholderText(tr("matched against each TXT record as key=value"));
    bonjourForm->addRow(tr("TXT record matches:"), m_txtRe);
    QString why;
    m_bonjourNote = mutedNote(bonjourDiscoveryAvailable(&why) ? QString() : why, m_bonjourBox);
    m_bonjourNote->setObjectName(QStringLiteral("remoteTypeBonjourNote"));
    m_bonjourNote->setVisible(!m_bonjourNote->text().isEmpty());
    bonjourForm->addRow(m_bonjourNote);
    findForm->addRow(m_bonjourBox);

    m_onvifBox = new QGroupBox(tr("ONVIF"), findBox);
    auto *onvifForm = new QFormLayout(m_onvifBox);
    m_scopeRe = new QLineEdit(m_onvifBox);
    m_scopeRe->setObjectName(QStringLiteral("remoteTypeScope"));
    m_scopeRe->setPlaceholderText(tr("matched against each scope, e.g. /hardware/ACME"));
    onvifForm->addRow(tr("Scope matches:"), m_scopeRe);
    findForm->addRow(m_onvifBox);
    right->addWidget(findBox);

    // --- how to sign in --------------------------------------------------------
    auto *signBox = new QGroupBox(tr("Signing in"), page);
    auto *signForm = new QFormLayout(signBox);
    signForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_connection = new QComboBox(signBox);
    m_connection->setObjectName(QStringLiteral("remoteTypeConnection"));
    m_connection->addItem(tr("SSH"), int(RemoteType::Connection::Ssh));
    m_connection->addItem(tr("Telnet"), int(RemoteType::Connection::Telnet));
    signForm->addRow(tr("Connection:"), m_connection);
    m_port = new QSpinBox(signBox);
    m_port->setObjectName(QStringLiteral("remoteTypePort"));
    m_port->setRange(0, 65535);
    // 0 IS "advertised", and it says so: a Bonjour record for the connection's own service
    // carries its port, and otherwise the connection's default applies (effectivePort()).
    m_port->setSpecialValueText(tr("advertised or default"));
    signForm->addRow(tr("Port:"), m_port);
    m_user = new QLineEdit(signBox);
    m_user->setObjectName(QStringLiteral("remoteTypeUser"));
    m_user->setPlaceholderText(tr("optional"));
    signForm->addRow(tr("User:"), m_user);
    m_password = new QLineEdit(signBox);
    m_password->setObjectName(QStringLiteral("remoteTypePassword"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(tr("optional — needs a user"));
    signForm->addRow(tr("Password:"), m_password);
    m_remember = new QCheckBox(tr("Remember the password"), signBox);
    m_remember->setObjectName(QStringLiteral("remoteTypeRemember"));
    signForm->addRow(QString(), m_remember);
    m_consent = new QLabel(signBox);
    m_consent->setObjectName(QStringLiteral("remoteTypeConsent"));
    m_consent->setTextFormat(Qt::RichText);
    m_consent->setWordWrap(true);
    m_consent->setMinimumHeight(2 * m_consent->fontMetrics().lineSpacing());
    signForm->addRow(m_consent);
    m_defaultLog = new QLineEdit(signBox);
    m_defaultLog->setObjectName(QStringLiteral("remoteTypeDefaultLog"));
    m_defaultLog->setPlaceholderText(QStringLiteral("/var/log/messages"));
    signForm->addRow(tr("Default log file:"), m_defaultLog);
    right->addWidget(signBox);

    // --- everything with a good default ----------------------------------------
    auto *advanced = new CollapsibleSection(tr("Advanced"), page);
    advanced->setObjectName(QStringLiteral("remoteTypeAdvanced"));
    auto *advancedContent = new QWidget(advanced);
    auto *advancedForm = new QFormLayout(advancedContent);
    advancedForm->setContentsMargins(16, 0, 0, 0);
    m_poll = new QSpinBox(advancedContent);
    m_poll->setObjectName(QStringLiteral("remoteTypePoll"));
    m_poll->setRange(200, 60000);
    m_poll->setSingleStep(250);
    m_poll->setSuffix(tr(" ms"));
    advancedForm->addRow(tr("Check for new lines every:"), m_poll);
    m_tailOnly = new QCheckBox(tr("Start from the end of the file only:"), advancedContent);
    m_tailOnly->setObjectName(QStringLiteral("remoteTypeTailOnly"));
    m_tailMb = new QSpinBox(advancedContent);
    m_tailMb->setObjectName(QStringLiteral("remoteTypeTailMb"));
    m_tailMb->setRange(1, 100000);
    m_tailMb->setSuffix(tr(" MB"));
    auto *tailRow = new QHBoxLayout;
    tailRow->addWidget(m_tailOnly);
    tailRow->addWidget(m_tailMb);
    tailRow->addStretch();
    advancedForm->addRow(tailRow);
    m_compress = new QCheckBox(tr("Compress the transfer (SSH, zlib)"), advancedContent);
    m_compress->setObjectName(QStringLiteral("remoteTypeCompress"));
    advancedForm->addRow(m_compress);
    m_consolePreset = new QComboBox(advancedContent);
    m_consolePreset->setObjectName(QStringLiteral("remoteTypeConsolePreset"));
    m_consolePreset->addItem(tr("(built-in defaults)"), QString());
    for (const SerialProfile &p : SerialProfileStore(SerialProfileStore::defaultDir()).presets())
        m_consolePreset->addItem(p.name, p.name);
    advancedForm->addRow(tr("Console login preset (telnet):"), m_consolePreset);
    advanced->setContentWidget(advancedContent);
    right->addWidget(advanced);

    // --- try it ------------------------------------------------------------------
    auto *testBox = new QGroupBox(tr("Try it"), page);
    auto *testLayout = new QVBoxLayout(testBox);
    auto *testRow = new QHBoxLayout;
    m_testButton = new QPushButton(tr("Test Discovery"), testBox);
    m_testButton->setObjectName(QStringLiteral("remoteTypeTest"));
    m_testButton->setToolTip(tr("Look for devices of this type now, with the settings as they "
                                "stand in the form — saved or not."));
    m_testStatus = mutedNote(QString(), testBox);
    m_testStatus->setObjectName(QStringLiteral("remoteTypeTestStatus"));
    testRow->addWidget(m_testButton);
    testRow->addWidget(m_testStatus, 1);
    testLayout->addLayout(testRow);
    m_testResults = new QListWidget(testBox);
    m_testResults->setObjectName(QStringLiteral("remoteTypeTestResults"));
    m_testResults->setMaximumHeight(6 * m_testResults->fontMetrics().lineSpacing());
    testLayout->addWidget(m_testResults);
    right->addWidget(testBox);
    right->addStretch();

    for (QLineEdit *e : {m_name, m_service, m_domain, m_instanceRe, m_txtRe, m_scopeRe, m_user,
                         m_password, m_defaultLog})
        ensureReadablePlaceholder(e);

    // --- wiring --------------------------------------------------------------------
    connect(m_typeList, &QListWidget::currentRowChanged, this, &RemoteParametersDialog::showType);
    connect(m_typeNew, &QPushButton::clicked, this, [this] {
        const QSignalBlocker block(m_typeList);
        m_typeList->clearSelection();
        m_typeList->setCurrentRow(-1);
        clearTypeForm();
        m_name->setFocus();
    });
    connect(m_typeSave, &QPushButton::clicked, this, &RemoteParametersDialog::saveType);
    connect(m_typeRemove, &QPushButton::clicked, this, &RemoteParametersDialog::removeType);
    connect(m_testButton, &QPushButton::clicked, this, &RemoteParametersDialog::testDiscovery);
    connect(m_discovery, &QComboBox::currentIndexChanged, this,
            &RemoteParametersDialog::updateDiscoveryFields);
    connect(m_connection, &QComboBox::currentIndexChanged, this,
            &RemoteParametersDialog::updateDiscoveryFields);
    connect(m_user, &QLineEdit::textChanged, this,
            &RemoteParametersDialog::updateTypePasswordField);
    connect(m_remember, &QCheckBox::toggled, this, &RemoteParametersDialog::updateTypeConsent);
    connect(m_tailOnly, &QCheckBox::toggled, m_tailMb, &QSpinBox::setEnabled);
    connect(m_name, &QLineEdit::textChanged, this, &RemoteParametersDialog::updateTypeActions);
    return page;
}

void RemoteParametersDialog::reloadTypes()
{
    m_types = m_store ? m_store->types() : QVector<RemoteType>();
    const QSignalBlocker block(m_typeList);
    m_typeList->clear();
    for (const RemoteType &t : std::as_const(m_types))
        m_typeList->addItem(t.name);
    updateTypeActions();
}

void RemoteParametersDialog::clearTypeForm()
{
    showType(-1);
}

void RemoteParametersDialog::showType(int row)
{
    const RemoteType t = row >= 0 && row < m_types.size() ? m_types.at(row) : RemoteType{};
    m_name->setText(t.name);
    m_discovery->setCurrentIndex(m_discovery->findData(int(t.discovery)));
    m_service->setText(t.serviceType);
    m_domain->setText(t.domain);
    m_instanceRe->setText(t.instanceRe);
    m_txtRe->setText(t.txtRe);
    m_scopeRe->setText(t.scopeRe);
    m_connection->setCurrentIndex(m_connection->findData(int(t.connection)));
    m_port->setValue(t.port);
    m_user->setText(t.user);
    m_password->setText(t.password);
    m_remember->setChecked(t.savePassword);
    m_defaultLog->setText(t.defaultLogPath);
    m_poll->setValue(t.pollMs > 0 ? t.pollMs : 1000);
    m_tailOnly->setChecked(t.tailStartBytes > 0);
    m_tailMb->setValue(t.tailStartBytes > 0 ? int(t.tailStartBytes / (1024LL * 1024)) : 64);
    m_tailMb->setEnabled(t.tailStartBytes > 0);
    m_compress->setChecked(t.compress);
    m_consolePreset->setCurrentIndex(qMax(0, m_consolePreset->findData(t.consolePreset)));
    m_testResults->clear();
    m_testStatus->clear();
    m_testBrowser.reset();
    updateDiscoveryFields();
    updateTypePasswordField();
    updateTypeActions();
}

RemoteType RemoteParametersDialog::currentType() const
{
    RemoteType t;
    t.name = m_name->text().trimmed();
    t.discovery = static_cast<RemoteType::Discovery>(m_discovery->currentData().toInt());
    t.serviceType = m_service->text().trimmed();
    t.domain = m_domain->text().trimmed();
    t.instanceRe = m_instanceRe->text();
    t.txtRe = m_txtRe->text();
    t.scopeRe = m_scopeRe->text();
    t.connection = static_cast<RemoteType::Connection>(m_connection->currentData().toInt());
    t.port = m_port->value();
    t.user = m_user->text().trimmed();
    // A PASSWORD ONLY WITH AN ACCOUNT, and only if it is to be remembered: the form is not a
    // place a password waits for a later save.
    if (!t.user.isEmpty()) {
        t.password = m_password->text();
        t.savePassword = m_remember->isChecked() && !t.password.isEmpty();
    }
    if (!t.savePassword)
        t.password.clear();
    t.defaultLogPath = m_defaultLog->text().trimmed();
    t.pollMs = m_poll->value();
    t.tailStartBytes = m_tailOnly->isChecked() ? qint64(m_tailMb->value()) * 1024 * 1024 : 0;
    t.compress = m_compress->isChecked();
    t.consolePreset = m_consolePreset->currentData().toString();
    return t;
}

void RemoteParametersDialog::updateTypeActions()
{
    const QString name = m_name->text().trimmed();
    m_typeSave->setEnabled(m_store && !name.isEmpty());
    m_typeSave->setText(HostBookmarkStore::indexOfType(m_types, name) >= 0 ? tr("Update")
                                                                           : tr("Save"));
    m_typeRemove->setEnabled(m_store && m_typeList->currentRow() >= 0);
}

void RemoteParametersDialog::updateDiscoveryFields()
{
    const bool bonjour =
        m_discovery->currentData().toInt() == int(RemoteType::Discovery::Bonjour);
    m_bonjourBox->setVisible(bonjour);
    m_onvifBox->setVisible(!bonjour);
    const bool telnet =
        m_connection->currentData().toInt() == int(RemoteType::Connection::Telnet);
    m_compress->setEnabled(!telnet);
    m_consolePreset->setEnabled(telnet);
}

void RemoteParametersDialog::updateTypePasswordField()
{
    // A password needs an account to go with it — the requirement, and the reason this is
    // greyed rather than merely ignored while the user field is empty.
    const bool hasUser = !m_user->text().trimmed().isEmpty();
    m_password->setEnabled(hasUser);
    m_remember->setEnabled(hasUser);
    if (!hasUser)
        m_remember->setChecked(false);
    updateTypeConsent();
}

void RemoteParametersDialog::updateTypeConsent()
{
    if (!m_remember->isChecked()) {
        m_consent->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                               .arg(mutedColor(palette()).name(),
                                    tr("Nothing is stored; a password typed above is used only "
                                       "until loftail closes.")
                                        .toHtmlEscaped()));
        return;
    }
    const QString backend = secretStore()->backendName();
    if (backend.isEmpty()) {
        m_consent->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                               .arg(warningColor(palette()).name(),
                                    tr("⚠ The password is stored as <b>plain text</b> in %1 — "
                                       "not encrypted.")
                                        .arg((m_store ? m_store->filePath() : QString())
                                                 .toHtmlEscaped())));
    } else {
        m_consent->setText(tr("The password goes to %1 — nothing is written to a file here.")
                               .arg(backend.toHtmlEscaped()));
    }
}

void RemoteParametersDialog::saveType()
{
    if (!m_store)
        return;
    RemoteType t = currentType();
    if (t.name.isEmpty())
        return;

    // WHERE THE PASSWORD GOES, the M14 rule: the keychain where one answers (and then not the
    // file), hosts.json where none does, and never the file after a keychain refused.
    const QString typed = m_password->text();
    if (!t.user.isEmpty() && m_remember->isChecked() && !typed.isEmpty()) {
        QString message;
        switch (rememberSshPassword(t.credentialTarget(), typed, &message)) {
        case RememberOutcome::StoredInKeychain:
            t.password.clear();
            t.savePassword = true;
            break;
        case RememberOutcome::UseFileFallback:
            t.password = typed;
            t.savePassword = true;
            break;
        case RememberOutcome::Failed:
            QMessageBox::warning(this, tr("Password not remembered"),
                                 tr("The keychain refused the password: %1").arg(message));
            t.password.clear();
            t.savePassword = false;
            break;
        }
    }
    // The password is also good for the rest of this session, remembered or not.
    if (!t.user.isEmpty() && !typed.isEmpty())
        SshCredentialCache::remember(t.credentialTarget(), typed);

    m_store->saveType(t);
    reloadTypes();
    const int row = HostBookmarkStore::indexOfType(m_types, t.name);
    {
        const QSignalBlocker block(m_typeList);
        m_typeList->setCurrentRow(row);
    }
    updateTypeActions();
}

void RemoteParametersDialog::removeType()
{
    const int row = m_typeList->currentRow();
    if (!m_store || row < 0 || row >= m_types.size())
        return;
    const QString name = m_types.at(row).name;
    // ASKED, and it says what removing costs: every log opened through the type names it, and
    // without it there is nothing that says how to reach those devices.
    const auto answer = QMessageBox::question(
        this, tr("Remove remote type"),
        tr("Remove the remote type “%1”? Logs opened through it will not open again until a "
           "type of that name exists.")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    m_store->removeType(name);
    reloadTypes();
    clearTypeForm();
}

void RemoteParametersDialog::testDiscovery()
{
    // With the form AS IT STANDS, saved or not: the point is to find out whether these
    // settings find anything before committing to them.
    RemoteType t = currentType();
    if (t.name.isEmpty())
        t.name = tr("(unsaved)");
    m_testResults->clear();
    m_testStatus->setText(tr("Looking…"));
    m_testBrowser = makeDiscoveryBrowser(t);
    connect(m_testBrowser.get(), &DiscoveryBrowser::found, this,
            &RemoteParametersDialog::onTestFound);
    connect(m_testBrowser.get(), &DiscoveryBrowser::failed, this,
            [this](const QString &reason) { m_testStatus->setText(reason); });
    m_testBrowser->start(t);
}

void RemoteParametersDialog::onTestFound(const DiscoveredDevice &device)
{
    const QString text = QStringLiteral("%1 — %2%3")
                             .arg(device.displayName, device.host,
                                  device.port > 0 ? QStringLiteral(":%1").arg(device.port)
                                                  : QString());
    for (int i = 0; i < m_testResults->count(); ++i) {
        if (m_testResults->item(i)->data(Qt::UserRole).toString() == device.id) {
            m_testResults->item(i)->setText(text);
            return;
        }
    }
    auto *item = new QListWidgetItem(text, m_testResults);
    item->setData(Qt::UserRole, device.id);
    item->setToolTip(device.details.join(u'\n'));
    m_testStatus->setText(tr("%n found", nullptr, m_testResults->count()));
}

} // namespace loftail
