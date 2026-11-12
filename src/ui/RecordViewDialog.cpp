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


#include "RecordViewDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace loftail {

RecordViewDialog::RecordViewDialog(const QString &title, const QString &text,
                                   const QFont &font, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("recordViewDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);

    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("recordViewText"));
    m_text->setReadOnly(true);
    // The log's own face and size, so a record reads here as it does in the table.
    m_text->setFont(font);
    m_text->setPlainText(text);

    // Wrapped by default: the window exists for the record too long to read in the table,
    // and that is overwhelmingly a long LINE. Off gives the lines as the log wrote them.
    m_wrap = new QCheckBox(tr("&Wrap lines"), this);
    m_wrap->setObjectName(QStringLiteral("recordViewWrap"));
    m_wrap->setChecked(true);
    connect(m_wrap, &QCheckBox::toggled, this, [this](bool on) {
        m_text->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *copy = buttons->addButton(tr("&Copy"), QDialogButtonBox::ActionRole);
    copy->setObjectName(QStringLiteral("recordViewCopy"));
    connect(copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_text->toPlainText()); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_wrap);
    bottom->addStretch(1);
    bottom->addWidget(buttons);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_text, 1);
    layout->addLayout(bottom);

    // Most of the screen it opens on, which is what a record too long for the table wants;
    // the window manager is free to place it.
    if (const QScreen *screen = parent ? parent->screen() : QApplication::primaryScreen()) {
        const QSize avail = screen->availableSize();
        resize(avail.width() * 2 / 3, avail.height() * 2 / 3);
    }
}

QString RecordViewDialog::text() const
{
    return m_text->toPlainText();
}

} // namespace loftail
