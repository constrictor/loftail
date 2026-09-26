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

#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;

namespace loftail {

// Which log to open on a device that has none remembered (SPEC.md §3).
//
// One field and nothing else. The DEVICE is already chosen — this is reached from the menu entry
// for it — and the PRESET is not asked here on purpose: which preset a log is read with belongs to
// the settings tree, so asking at open time would be a second place to answer it and the two would
// disagree the first time somebody edited a pattern.
class OpenSerialDialog : public QDialog
{
    Q_OBJECT

public:
    explicit OpenSerialDialog(const QString &deviceLabel, QWidget *parent = nullptr);

    QString chosenPath() const;

private:
    void updateActions();

    QLineEdit   *m_path = nullptr;
    QPushButton *m_open = nullptr;
};

} // namespace loftail
