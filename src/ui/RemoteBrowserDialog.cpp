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

#include "RemoteBrowserDialog.h"

#include "DiscoveryBrowser.h"
#include "OpenRemoteDialog.h"
#include "RemoteLocation.h"
#include "RemoteParametersDialog.h"
#include "UiColors.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace loftail {

namespace {

// What a row is. A GROUP row and a STATUS row are never chosen; a device and a saved host are.
enum RowKind { GroupRow = 1, StatusRow, DeviceRow, HostRow };
constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kKeyRole = Qt::UserRole + 2;    // device id, or saved host name
constexpr int kTypeRole = Qt::UserRole + 3;   // the remote type's name (device rows)

enum Column { NameColumn, TypeColumn, AddressColumn, DetailsColumn };

QString foldKey(const QString &typeName)
{
    return typeName.trimmed().toCaseFolded();
}

} // namespace

RemoteBrowserDialog::RemoteBrowserDialog(HostBookmarkStore *store, QWidget *parent)
    : QDialog(parent), m_store(store)
{
    setWindowTitle(tr("Open Remote Log"));
    setObjectName(QStringLiteral("remoteBrowserDialog"));
    setModal(true);
    resize(760, 520);

    auto *outer = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("remoteBrowserFilter"));
    m_filter->setPlaceholderText(tr("Filter by name, type, address or TXT record"));
    m_filter->setClearButtonEnabled(true);
    ensureReadablePlaceholder(m_filter);
    auto *filterLabel = new QLabel(tr("F&ilter:"), this);
    filterLabel->setBuddy(m_filter);
    top->addWidget(filterLabel);
    top->addWidget(m_filter, 1);
    m_rescan = new QPushButton(tr("Re&scan"), this);
    m_rescan->setObjectName(QStringLiteral("remoteBrowserRescan"));
    m_rescan->setToolTip(tr("Forget what has been found and look again"));
    top->addWidget(m_rescan);
    m_parameters = new QPushButton(tr("Remote &Parameters…"), this);
    m_parameters->setObjectName(QStringLiteral("remoteBrowserParameters"));
    top->addWidget(m_parameters);
    outer->addLayout(top);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("remoteBrowserTree"));
    m_tree->setHeaderLabels({tr("Name"), tr("Type"), tr("Address"), tr("Details")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(NameColumn, 220);
    m_tree->setColumnWidth(TypeColumn, 120);
    m_tree->setColumnWidth(AddressColumn, 150);
    outer->addWidget(m_tree, 1);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_log = new QComboBox(this);
    m_log->setObjectName(QStringLiteral("remoteBrowserLog"));
    m_log->setEditable(true);
    m_log->setInsertPolicy(QComboBox::NoInsert);
    m_log->lineEdit()->setPlaceholderText(QStringLiteral("/var/log/messages"));
    ensureReadablePlaceholder(m_log->lineEdit());
    auto *logLabel = new QLabel(tr("&Log file:"), this);
    logLabel->setBuddy(m_log);
    form->addRow(logLabel, m_log);

    // THE ALTERNATIVE TO DISCOVERY, beside it rather than behind a menu: a machine that
    // advertises nothing is still a machine, and typing its address is the oldest way there is.
    auto *directRow = new QHBoxLayout;
    m_direct = new QLineEdit(this);
    m_direct->setObjectName(QStringLiteral("remoteBrowserDirect"));
    m_direct->setPlaceholderText(tr("IP address, host name, or ssh:// / telnet:// address"));
    ensureReadablePlaceholder(m_direct);
    m_directButton = new QPushButton(tr("&Connect…"), this);
    m_directButton->setObjectName(QStringLiteral("remoteBrowserDirectButton"));
    m_directButton->setToolTip(tr("Settle how to sign in to this address, then open the log"));
    directRow->addWidget(m_direct, 1);
    directRow->addWidget(m_directButton);
    auto *directLabel = new QLabel(tr("Or connect &directly:"), this);
    directLabel->setBuddy(m_direct);
    form->addRow(directLabel, directRow);
    outer->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    m_openButton = buttons->button(QDialogButtonBox::Open);
    outer->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &RemoteBrowserDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_filter, &QLineEdit::textChanged, this, &RemoteBrowserDialog::applyFilter);
    connect(m_rescan, &QPushButton::clicked, this, [this] {
        reloadSources();
        startBrowsing();
    });
    connect(m_parameters, &QPushButton::clicked, this, &RemoteBrowserDialog::openParameters);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &RemoteBrowserDialog::onSelectionChanged);
    connect(m_tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) { onActivated(item); });
    connect(m_log, &QComboBox::editTextChanged, this, &RemoteBrowserDialog::updateActions);
    connect(m_direct, &QLineEdit::textChanged, this, &RemoteBrowserDialog::updateActions);
    connect(m_direct, &QLineEdit::returnPressed, this, &RemoteBrowserDialog::connectDirectly);
    connect(m_directButton, &QPushButton::clicked, this, &RemoteBrowserDialog::connectDirectly);

    reloadSources();
    startBrowsing();
    updateActions();
    m_filter->setFocus();
}

RemoteBrowserDialog::~RemoteBrowserDialog()
{
    stopBrowsing();
}

void RemoteBrowserDialog::reloadSources()
{
    stopBrowsing();
    m_types = m_store ? m_store->types() : QVector<RemoteType>();
    m_hosts = m_store ? m_store->all() : QVector<HostBookmark>();
    m_tree->clear();
    m_typeGroups.clear();

    // One group per remote type, in saved order, with a status row saying what it is doing
    // until it has something to show — an empty group would read as "nothing is there".
    for (const RemoteType &t : std::as_const(m_types)) {
        auto *group = new QTreeWidgetItem(m_tree);
        group->setText(NameColumn, t.name);
        group->setText(TypeColumn, t.discovery == RemoteType::Discovery::Onvif
                                       ? tr("ONVIF")
                                       : tr("Bonjour %1").arg(t.serviceType));
        group->setData(NameColumn, kKindRole, GroupRow);
        group->setFlags(Qt::ItemIsEnabled);
        group->setFirstColumnSpanned(false);
        QFont bold = group->font(NameColumn);
        bold.setBold(true);
        group->setFont(NameColumn, bold);
        auto *status = new QTreeWidgetItem(group);
        status->setText(NameColumn, tr("Looking…"));
        status->setData(NameColumn, kKindRole, StatusRow);
        status->setFlags(Qt::NoItemFlags);
        group->setExpanded(true);
        m_typeGroups.insert(foldKey(t.name), group);
    }
    if (m_types.isEmpty()) {
        auto *none = new QTreeWidgetItem(m_tree);
        none->setText(NameColumn, tr("No remote types yet — define one in Remote Parameters to "
                                     "find devices on the network."));
        none->setData(NameColumn, kKindRole, StatusRow);
        none->setFlags(Qt::NoItemFlags);
        none->setFirstColumnSpanned(true);
    }

    m_hostsGroup = new QTreeWidgetItem(m_tree);
    m_hostsGroup->setText(NameColumn, tr("Saved hosts"));
    m_hostsGroup->setData(NameColumn, kKindRole, GroupRow);
    m_hostsGroup->setFlags(Qt::ItemIsEnabled);
    QFont bold = m_hostsGroup->font(NameColumn);
    bold.setBold(true);
    m_hostsGroup->setFont(NameColumn, bold);
    for (const HostBookmark &h : std::as_const(m_hosts)) {
        auto *row = new QTreeWidgetItem(m_hostsGroup);
        row->setText(NameColumn, h.displayName());
        row->setText(TypeColumn, h.connection == RemoteType::Connection::Telnet ? tr("telnet")
                                                                                : tr("SSH"));
        row->setText(AddressColumn, h.port == RemoteLocation::defaultPortFor(h.transport())
                                        ? h.host
                                        : QStringLiteral("%1:%2").arg(h.host).arg(h.port));
        row->setText(DetailsColumn, h.paths.join(QStringLiteral(", ")));
        row->setData(NameColumn, kKindRole, HostRow);
        row->setData(NameColumn, kKeyRole, h.displayName());
    }
    if (m_hosts.isEmpty()) {
        auto *none = new QTreeWidgetItem(m_hostsGroup);
        none->setText(NameColumn, tr("None saved"));
        none->setData(NameColumn, kKindRole, StatusRow);
        none->setFlags(Qt::NoItemFlags);
    }
    m_hostsGroup->setExpanded(true);
    applyFilter();
}

void RemoteBrowserDialog::startBrowsing()
{
    stopBrowsing();
    for (const RemoteType &t : std::as_const(m_types)) {
        auto browser = makeDiscoveryBrowser(t, this);
        const QString name = t.name;
        connect(browser.get(), &DiscoveryBrowser::found, this, &RemoteBrowserDialog::onFound);
        connect(browser.get(), &DiscoveryBrowser::lost, this, &RemoteBrowserDialog::onLost);
        connect(browser.get(), &DiscoveryBrowser::failed, this,
                [this, name](const QString &reason) { onFailed(name, reason); });
        browser->start(t);
        m_browsers.push_back(std::move(browser));
    }
}

void RemoteBrowserDialog::stopBrowsing()
{
    for (auto &b : m_browsers) {
        b->disconnect(this);
        b->stop();
    }
    m_browsers.clear();
}

QTreeWidgetItem *RemoteBrowserDialog::groupFor(const QString &typeName) const
{
    return m_typeGroups.value(foldKey(typeName), nullptr);
}

void RemoteBrowserDialog::onFound(const DiscoveredDevice &device)
{
    QTreeWidgetItem *group = groupFor(device.typeName);
    if (!group)
        return;
    QTreeWidgetItem *row = nullptr;
    for (int i = group->childCount() - 1; i >= 0; --i) {
        QTreeWidgetItem *child = group->child(i);
        const int kind = child->data(NameColumn, kKindRole).toInt();
        if (kind == StatusRow) {
            delete child;   // something to show now
            continue;
        }
        if (kind == DeviceRow && child->data(NameColumn, kKeyRole).toString() == device.id)
            row = child;
    }
    if (!row) {
        row = new QTreeWidgetItem(group);
        row->setData(NameColumn, kKindRole, DeviceRow);
        row->setData(NameColumn, kKeyRole, device.id);
        row->setData(NameColumn, kTypeRole, device.typeName);
    }
    row->setText(NameColumn, device.displayName);
    row->setToolTip(NameColumn, device.id);
    row->setText(TypeColumn, device.typeName);
    row->setText(AddressColumn, device.port > 0
                                    ? QStringLiteral("%1:%2").arg(device.host).arg(device.port)
                                    : device.host);
    row->setText(DetailsColumn, device.details.join(QStringLiteral("  ")));
    row->setToolTip(DetailsColumn, device.details.join(u'\n'));
    applyFilter();
}

void RemoteBrowserDialog::onLost(const QString &typeName, const QString &id)
{
    QTreeWidgetItem *group = groupFor(typeName);
    if (!group)
        return;
    for (int i = group->childCount() - 1; i >= 0; --i) {
        QTreeWidgetItem *child = group->child(i);
        if (child->data(NameColumn, kKindRole).toInt() == DeviceRow
            && child->data(NameColumn, kKeyRole).toString() == id) {
            delete child;
        }
    }
    updateActions();
}

void RemoteBrowserDialog::onFailed(const QString &typeName, const QString &reason)
{
    QTreeWidgetItem *group = groupFor(typeName);
    if (!group)
        return;
    // The status row SAYS WHY, in the backend's own words — a group that simply stays empty
    // would read as "there are no such devices", which is a claim about the network nobody made.
    for (int i = 0; i < group->childCount(); ++i) {
        QTreeWidgetItem *child = group->child(i);
        if (child->data(NameColumn, kKindRole).toInt() == StatusRow) {
            child->setText(NameColumn, reason);
            child->setToolTip(NameColumn, reason);
            child->setFirstColumnSpanned(true);
            return;
        }
    }
    auto *status = new QTreeWidgetItem(group);
    status->setText(NameColumn, reason);
    status->setToolTip(NameColumn, reason);
    status->setData(NameColumn, kKindRole, StatusRow);
    status->setFlags(Qt::NoItemFlags);
    status->setFirstColumnSpanned(true);
}

QTreeWidgetItem *RemoteBrowserDialog::selectedRow() const
{
    const auto items = m_tree->selectedItems();
    if (items.isEmpty())
        return nullptr;
    QTreeWidgetItem *item = items.first();
    const int kind = item->data(NameColumn, kKindRole).toInt();
    return kind == DeviceRow || kind == HostRow ? item : nullptr;
}

void RemoteBrowserDialog::onSelectionChanged()
{
    // THE LOG FIELD FOLLOWS THE SELECTION: a device of a type offers the type's default log,
    // a saved host every log remembered on it. What was typed is replaced only by something
    // to replace it with, so a path typed before choosing a row with nothing to offer stays.
    QTreeWidgetItem *row = selectedRow();
    if (!row) {
        updateActions();
        return;
    }
    QStringList offers;
    if (row->data(NameColumn, kKindRole).toInt() == DeviceRow) {
        const int at = HostBookmarkStore::indexOfType(m_types,
                                                      row->data(NameColumn, kTypeRole).toString());
        if (at >= 0 && !m_types.at(at).defaultLogPath.isEmpty())
            offers.append(m_types.at(at).defaultLogPath);
    } else {
        const int at = HostBookmarkStore::indexOfName(m_hosts,
                                                      row->data(NameColumn, kKeyRole).toString());
        if (at >= 0)
            offers = m_hosts.at(at).paths;
    }
    if (!offers.isEmpty()) {
        const QSignalBlocker block(m_log);
        m_log->clear();
        m_log->addItems(offers);
        m_log->setEditText(offers.first());
    }
    updateActions();
}

void RemoteBrowserDialog::onActivated(QTreeWidgetItem *item)
{
    const int kind = item ? item->data(NameColumn, kKindRole).toInt() : 0;
    if (kind != DeviceRow && kind != HostRow)
        return;
    m_tree->setCurrentItem(item);
    if (m_log->currentText().trimmed().isEmpty()) {
        // Nothing to open yet: say where the missing half goes rather than doing nothing.
        m_log->setFocus();
        return;
    }
    accept();
}

void RemoteBrowserDialog::applyFilter()
{
    const QString needle = m_filter->text().trimmed();
    const auto matches = [&needle](const QTreeWidgetItem *item) {
        if (needle.isEmpty())
            return true;
        for (int c = 0; c < 4; ++c) {
            if (item->text(c).contains(needle, Qt::CaseInsensitive))
                return true;
        }
        return item->data(NameColumn, kKeyRole).toString().contains(needle, Qt::CaseInsensitive);
    };
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_tree->topLevelItem(g);
        // A filter that matches the GROUP (a type's name) shows the whole group.
        const bool groupMatches = !needle.isEmpty() && matches(group);
        int visible = 0;
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem *child = group->child(i);
            const int kind = child->data(NameColumn, kKindRole).toInt();
            const bool show = kind == StatusRow ? needle.isEmpty() : (groupMatches || matches(child));
            child->setHidden(!show);
            if (show)
                ++visible;
        }
        group->setHidden(!needle.isEmpty() && visible == 0 && !groupMatches);
    }
    // A selection the filter just hid is not something Open should act on.
    if (QTreeWidgetItem *row = selectedRow(); row && row->isHidden())
        m_tree->clearSelection();
    updateActions();
}

void RemoteBrowserDialog::updateActions()
{
    m_openButton->setEnabled(selectedRow() && !m_log->currentText().trimmed().isEmpty());
    m_directButton->setEnabled(!m_direct->text().trimmed().isEmpty());
}

void RemoteBrowserDialog::connectDirectly()
{
    const QString address = m_direct->text().trimmed();
    if (address.isEmpty())
        return;
    // THE HOST FORM SETTLES THE REST — connection, account, port, the console preset — and
    // its "Settings from" picker is how a type or another saved host fills it in, so an
    // address typed here need not have every setting typed as well.
    OpenRemoteDialog form(m_store, this);
    form.presetAddress(address, m_log->currentText().trimmed());
    if (form.exec() != QDialog::Accepted || form.chosenUrl().isEmpty()) {
        reloadSources();   // the form may have saved or removed a host
        startBrowsing();
        return;
    }
    m_choice = Choice::Direct;
    m_chosenUrl = form.chosenUrl();
    m_chosenConsolePreset = form.chosenConsolePreset();
    QDialog::accept();
}

void RemoteBrowserDialog::openParameters()
{
    RemoteParametersDialog parameters(m_store, this);
    parameters.exec();
    reloadSources();
    startBrowsing();
}

void RemoteBrowserDialog::accept()
{
    QTreeWidgetItem *row = selectedRow();
    const QString path = m_log->currentText().trimmed();
    if (!row || path.isEmpty())
        return;   // a guard only: Open is disabled while this can happen

    if (row->data(NameColumn, kKindRole).toInt() == DeviceRow) {
        // BY REFERENCE: the type and the device's own id, never the address it was found at.
        RemoteLocation location;
        location.transport = RemoteLocation::Transport::Discovered;
        location.discoveryType = row->data(NameColumn, kTypeRole).toString();
        location.host = row->data(NameColumn, kKeyRole).toString();
        location.path = path.startsWith(u'/') || path.startsWith(u'~') ? path : u'/' + path;
        m_choice = Choice::Discovered;
        m_chosenUrl = location.toString();
    } else {
        m_choice = Choice::SavedHost;
        m_chosenHost = row->data(NameColumn, kKeyRole).toString();
        m_chosenPath = path;
    }
    QDialog::accept();
}

} // namespace loftail
