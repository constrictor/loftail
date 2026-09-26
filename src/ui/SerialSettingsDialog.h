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

#include "SerialProfile.h"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace loftail {

class SerialProfileStore;

// The named serial presets (SPEC.md §3, requirement 2).
//
// A PRESET IS NOT BOUND TO A DEVICE, and this dialog is the whole of what that means: a list of
// named settings on the left, the settings on the right, and no device anywhere in it. WHICH log
// uses which preset is answered in Preferences, by the ordinary three-level tree — so a
// `serial://*` pattern saying "every board of ours is a Linux box" is one entry rather than one
// per device.
//
// APPLIES NOTHING ITSELF. The caller reads the presets back after Accepted and writes them, then
// re-primes the open serial tabs — PreferencesDialog's own posture, and for its reason: a tab
// already reading a log is re-settled by the fetcher on its next poll rather than from in here.
class SerialSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SerialSettingsDialog(SerialProfileStore *store, QWidget *parent = nullptr);

    // The edited list, after Accepted. The caller writes it; this dialog does not.
    QVector<SerialProfile> presets() const { return m_presets; }

private:
    void buildUi();
    void reload();
    void showPreset(int row);
    void commitCurrent();
    void addPreset();
    void duplicatePreset();
    void removePreset();
    void updateActions();
    void updatePatternNotes();
    SerialProfile currentFields() const;

    SerialProfileStore     *m_store = nullptr;
    QVector<SerialProfile>  m_presets;
    int                     m_current = -1;
    bool                    m_loading = false;

    QListWidget *m_list = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_duplicate = nullptr;
    QPushButton *m_remove = nullptr;

    QLineEdit   *m_name = nullptr;
    QComboBox   *m_baud = nullptr;
    QComboBox   *m_dataBits = nullptr;
    QComboBox   *m_parity = nullptr;
    QComboBox   *m_stopBits = nullptr;
    QComboBox   *m_flow = nullptr;
    QSpinBox    *m_pollMs = nullptr;

    QLineEdit   *m_loginPrompt = nullptr;
    QLineEdit   *m_passwordPrompt = nullptr;
    QSpinBox    *m_promptTimeout = nullptr;
    QSpinBox    *m_attempts = nullptr;
    QPlainTextEdit *m_startup = nullptr;
    QLineEdit   *m_rebootPattern = nullptr;
    QLabel      *m_patternNote = nullptr;
};

} // namespace loftail
