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

class QCheckBox;
class QPlainTextEdit;

namespace loftail {

// One record, whole (SPEC.md §5): every line of it, however many the table's display cap
// or a column's width left off screen. The "expands on request" half of a capped record,
// and the way to read a line too long for the table at all.
//
// A window of its own rather than a taller row, because a record's height in the table is
// fixed by the two-level prefix sums (invariant #6): letting one record grow past
// kDisplayLineCap would mean a per-record override in every geometry path, exact and
// estimated, for a gesture that is about reading one record and not about the table.
// Holds a COPY of the text, taken when it opened — a snapshot, which is what a record
// already written is; nothing here points back into the log.
//
// Non-modal and self-deleting, so several can be open and the log goes on tailing behind
// them. Object names are the test contract: `recordViewText`, `recordViewWrap`.
class RecordViewDialog : public QDialog
{
    Q_OBJECT

public:
    RecordViewDialog(const QString &title, const QString &text, const QFont &font,
                     QWidget *parent = nullptr);

    QString text() const;

private:
    QPlainTextEdit *m_text = nullptr;
    QCheckBox      *m_wrap = nullptr;
};

} // namespace loftail
