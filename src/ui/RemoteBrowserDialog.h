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

#pragma once

#include "HostBookmarkStore.h"
#include "RemoteType.h"

#include <QDialog>
#include <QHash>
#include <QVector>

#include <memory>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
QT_END_NAMESPACE

namespace loftail {

class DiscoveryBrowser;
struct DiscoveredDevice;

// File ▸ Open Remote (SPEC.md §3): find a machine and open a log on it.
//
// Three ways to name the machine, one list to choose from: the devices every remote type is
// finding on the network RIGHT NOW (browsing starts when the dialog opens and stops when it
// closes), the hosts saved by address, and — beside the list — an address typed outright, which
// opens the host form to settle the rest. The filter narrows all of them at once.
//
// A DISCOVERED DEVICE IS OPENED BY REFERENCE: what comes back is a `discover://Type/id/path`
// address naming the type and the device's own id, never the address it was found at, so the
// next launch finds it again wherever it has gone. A saved host comes back as its NAME and path
// rather than as an address, MainWindow::openRemoteBookmark()'s contract, so its poll cadence,
// tail-start and compression apply.
//
// Activation is itemActivated, never itemDoubleClicked — one signal is both the double-click and
// Return, the welcome page's rule — and every control has an object name.
//
// THE FILTER DRIVES THE LIST FROM THE KEYBOARD: every edit selects the first result, Up and
// Down step through the results without the focus leaving the field, and Return opens the
// selected one exactly as Return on its row would — so type, arrow, Return is the whole gesture.
class RemoteBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RemoteBrowserDialog(HostBookmarkStore *store, QWidget *parent = nullptr);
    // `recentMachines` is the recency list RecentRemotes.h keeps, newest first: the devices and
    // saved hosts it names are listed first in their groups, in that order, and everything
    // else after them in the order it would have had anyway.
    RemoteBrowserDialog(HostBookmarkStore *store, const QStringList &recentMachines,
                        QWidget *parent = nullptr);
    ~RemoteBrowserDialog() override;

    enum class Choice { None, Discovered, SavedHost, Direct };

    // What was chosen, valid once exec() returned Accepted.
    Choice choice() const { return m_choice; }
    // Discovered and Direct: the address to open, in normal form.
    QString chosenUrl() const { return m_chosenUrl; }
    // SavedHost: which host and which log on it.
    QString chosenHost() const { return m_chosenHost; }
    QString chosenPath() const { return m_chosenPath; }
    // Direct over telnet: the console preset chosen in the host form.
    QString chosenConsolePreset() const { return m_chosenConsolePreset; }

private:
    void reloadSources();
    void startBrowsing();
    void stopBrowsing();
    void onFound(const DiscoveredDevice &device);
    void onLost(const QString &typeName, const QString &id);
    void onFailed(const QString &typeName, const QString &reason);
    void onSelectionChanged();
    void onActivated(QTreeWidgetItem *item);
    void onFilterEdited();
    void applyFilter();
    void stepSelection(int step);
    void selectRow(QTreeWidgetItem *row);
    void updateActions();
    void connectDirectly();
    void openParameters();
    void accept() override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    QTreeWidgetItem *groupFor(const QString &typeName) const;
    // Where a row stands in the recency list: its index, or past the end for one not in it.
    int recencyRank(const QString &machineKey) const;
    QTreeWidgetItem *selectedRow() const;
    // The device and saved-host rows the filter leaves showing, in the order they are listed.
    QList<QTreeWidgetItem *> visibleResults() const;

    HostBookmarkStore    *m_store = nullptr;
    QStringList           m_recent;   // machine keys, newest first
    QVector<RemoteType>   m_types;
    QVector<HostBookmark> m_hosts;

    QLineEdit   *m_filter = nullptr;
    QPushButton *m_rescan = nullptr;
    QPushButton *m_parameters = nullptr;
    QTreeWidget *m_tree = nullptr;
    QComboBox   *m_log = nullptr;
    QLineEdit   *m_direct = nullptr;
    QPushButton *m_directButton = nullptr;
    QPushButton *m_openButton = nullptr;

    QTreeWidgetItem *m_hostsGroup = nullptr;
    QHash<QString, QTreeWidgetItem *> m_typeGroups;   // keyed case-folded, as a type name is
    std::vector<std::unique_ptr<DiscoveryBrowser>> m_browsers;

    Choice  m_choice = Choice::None;
    QString m_chosenUrl;
    QString m_chosenHost;
    QString m_chosenPath;
    QString m_chosenConsolePreset;
};

} // namespace loftail
