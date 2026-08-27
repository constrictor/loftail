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
#include <QVector>

QT_BEGIN_NAMESPACE
class QDialogButtonBox;
class QLabel;
class QTreeWidget;
QT_END_NAMESPACE

namespace loftail {

// Which open log to take a highlight rule list from (SPEC.md §7).
//
// Highlight rules belong to the file, so a list built on one log is unreachable from
// the next — and a reader tailing one service across two hosts, or a rolled app.log
// beside app.log.1, wants the same list on both. This is the picker for the one gesture
// that moves them: pick another open log, take its whole list.
//
// It knows NOTHING about Document, DocumentContext or HighlighterSet — the caller
// renders each candidate down to a label, an address and a count, and this hands back
// an INDEX into what it was given. That is what keeps it testable with no log anywhere,
// and it is the OpenArchiveDialog shape one file over: take the data in the
// constructor, do no I/O of its own, and apply nothing.
//
// It is a REPLACE and loftail has no undo, so the disclosure is in the summary line and
// in the accept button's own word rather than in a second confirmation stacked on top
// of a picker.
class CopyHighlightersDialog : public QDialog
{
    Q_OBJECT

public:
    // One candidate log, already rendered for display. `address` is what a row's
    // tooltip shows and must be a DISPLAY path — logSourceDisplayPath(), never the raw
    // address, which for a remote log can carry a password.
    struct Source
    {
        QString label;          // the log's tab label
        QString address;        // logSourceDisplayPath(), for the row's tooltip
        int     ruleCount = 0;
        bool    seeded = false; // its rules are the three a log arrives with
    };

    CopyHighlightersDialog(QVector<Source> sources, const QString &targetLabel,
                           int targetRuleCount, QWidget *parent = nullptr);

    // The chosen source's index into the vector passed in, valid once exec() returned
    // Accepted; -1 otherwise.
    int chosenIndex() const { return m_chosen; }

    // The whole gesture: show the picker and answer with an index, or -1 when the user
    // cancelled. Cancelling must change nothing, exactly as cancelling Preferences does.
    static int chooseSource(const QVector<Source> &sources, const QString &targetLabel,
                            int targetRuleCount, QWidget *parent);

    // Public because QDialog's is: a test drives the modal from a timer, and refusing
    // an empty selection lives here rather than in a disabled button so that the
    // refusal is reachable at all.
    void accept() override;

private:
    // Where the selection starts: the first candidate whose rules are NOT the seed.
    // Copying the three level colours onto a log is almost never what was meant —
    // propagating a list somebody built is the whole feature — but this is a starting
    // selection and never a filter, so a seeded source is still there to be picked.
    static int preselect(const QVector<Source> &sources);

    QVector<Source>   m_sources;
    QTreeWidget      *m_list = nullptr;
    QLabel           *m_summary = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    int               m_chosen = -1;
};

} // namespace loftail
