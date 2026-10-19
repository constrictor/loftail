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
#include <QVector>

#include <memory>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTabWidget;
QT_END_NAMESPACE

namespace loftail {

class DiscoveryBrowser;
class OpenRemoteDialog;
struct DiscoveredDevice;

// File ▸ Remote Parameters (SPEC.md §3, §4): everything about HOW loftail reaches other
// machines, in one place — the remote TYPES that find machines by discovery, and the saved
// HOSTS reached by address — and nothing about opening a log, which is File ▸ Open Remote.
//
// The hosts page IS the old Open Remote form, embedded in manage mode, rather than a second
// editor for the same records: two forms for one hosts.json would be two places for its rules
// (the name is the identity, the paths are pruned where they are shown, a password goes where
// the consent note said) to drift apart.
//
// Every control has an object name, the test contract; a type is saved explicitly, as a host
// is, and the button says Update when the name already names one.
class RemoteParametersDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RemoteParametersDialog(HostBookmarkStore *store, QWidget *parent = nullptr);
    ~RemoteParametersDialog() override;

    // Open on the hosts page rather than the types page.
    void showHostsPage();

private:
    QWidget *buildTypesPage();
    void reloadTypes();
    void showType(int row);
    void clearTypeForm();
    RemoteType currentType() const;
    void updateTypeActions();
    void updateDiscoveryFields();
    void updateTypePasswordField();
    void updateTypeConsent();
    void saveType();
    void removeType();
    void testDiscovery();
    void onTestFound(const DiscoveredDevice &device);

    HostBookmarkStore  *m_store = nullptr;
    QVector<RemoteType> m_types;

    QTabWidget       *m_tabs = nullptr;
    OpenRemoteDialog *m_hosts = nullptr;

    QListWidget *m_typeList = nullptr;
    QPushButton *m_typeNew = nullptr;
    QPushButton *m_typeSave = nullptr;
    QPushButton *m_typeRemove = nullptr;

    QLineEdit *m_name = nullptr;
    QComboBox *m_discovery = nullptr;
    QGroupBox *m_bonjourBox = nullptr;
    QLineEdit *m_service = nullptr;
    QLineEdit *m_domain = nullptr;
    QLineEdit *m_instanceRe = nullptr;
    QLineEdit *m_txtRe = nullptr;
    QLabel    *m_bonjourNote = nullptr;
    QGroupBox *m_onvifBox = nullptr;
    QLineEdit *m_scopeRe = nullptr;
    QComboBox *m_connection = nullptr;
    QSpinBox  *m_port = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_remember = nullptr;
    QLabel    *m_consent = nullptr;
    QLineEdit *m_defaultLog = nullptr;
    QSpinBox  *m_poll = nullptr;
    QCheckBox *m_tailOnly = nullptr;
    QSpinBox  *m_tailMb = nullptr;
    QCheckBox *m_compress = nullptr;
    QComboBox *m_consolePreset = nullptr;

    QPushButton *m_testButton = nullptr;
    QListWidget *m_testResults = nullptr;
    QLabel      *m_testStatus = nullptr;
    std::unique_ptr<DiscoveryBrowser> m_testBrowser;
};

} // namespace loftail
