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

#include "OpenSerialDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace loftail {

OpenSerialDialog::OpenSerialDialog(const QString &deviceLabel, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("openSerialDialog")); // findChild, for tests
    setWindowTitle(tr("Open a Log on %1").arg(deviceLabel));

    auto *root = new QVBoxLayout(this);

    // Said in prose above the field rather than left to be discovered, which is the Open Remote
    // form's own rule about what a field cannot say for itself: the path is on the DEVICE, and
    // nothing about it resolves against this machine.
    auto *hint = new QLabel(tr("The log's path on %1, as that machine spells it.").arg(deviceLabel),
                            this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto *form = new QFormLayout;
    m_path = new QLineEdit(this);
    m_path->setObjectName(QStringLiteral("serialPathField")); // findChild, for tests
    m_path->setPlaceholderText(QStringLiteral("/var/log/messages"));
    form->addRow(tr("&Path:"), m_path);
    root->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    m_open = buttons->button(QDialogButtonBox::Open);
    m_open->setObjectName(QStringLiteral("serialOpenButton")); // findChild, for tests
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    // OPEN STAYS UNAVAILABLE UNTIL THERE IS A PATH, rather than accepting the click and doing
    // nothing — the Open Remote form's rule, and the reason it is `textChanged` and not
    // `editingFinished`: a button that becomes pressable only after the field loses focus reads
    // as broken.
    connect(m_path, &QLineEdit::textChanged, this, &OpenSerialDialog::updateActions);
    updateActions();
}

QString OpenSerialDialog::chosenPath() const
{
    return m_path->text().trimmed();
}

void OpenSerialDialog::updateActions()
{
    if (m_open)
        m_open->setEnabled(!chosenPath().isEmpty());
}

} // namespace loftail
