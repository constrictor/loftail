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

#include "LogProfile.h"

#include <QByteArray>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
QT_END_NAMESPACE

namespace loftail {

class FormatEditor;

// Everything one node of the settings tree holds, on one page (SPEC.md §4): the format
// (through the existing FormatEditor — pattern, encoding, source zone, live preview and
// Detect), the run-start pattern and its two flags, how timestamps are displayed, the
// wrap mode a new view of the log starts in, where the log's config file is, and the
// script that restarts the application writing it.
//
// A wrapper rather than more controls inside FormatEditor. That class's contract is that
// a trip through it cannot reset a choice made elsewhere, so it stashes the fields it
// does not own and hands them back untouched — and that stash is exactly what keeps
// `timeDisplay` and the run-start triple alive through a pattern edit. Growing it would
// mean deleting the stash, and then the trap it was written against comes back the day
// someone adds an eighth field.
//
// ORDER MATTERS in profile(): FormatEditor::settings() builds a FRESH struct, so this
// editor's own fields are written over the top of it, never before.
class LogProfileEditor : public QWidget
{
    Q_OBJECT

public:
    explicit LogProfileEditor(QWidget *parent = nullptr);

    // The bytes the preview and Detect run over. May be empty — the preview is then
    // blank and Detect is disabled, which is what editing a node with no log open
    // looks like.
    void setSample(const QByteArray &sample);
    void setPreviewCaption(const QString &text);

    // Forwarded to the format editor: whether the previewed sample is this node's log.
    void setSampleBelongsHere(bool own);

    void setProfile(const LogProfile &p);
    LogProfile profile() const;

private:
    FormatEditor *m_format = nullptr;
    QLineEdit    *m_runStart = nullptr;
    QCheckBox    *m_runRegex = nullptr;
    QCheckBox    *m_runCase = nullptr;
    QComboBox    *m_timeDisplay = nullptr;
    QComboBox    *m_wrap = nullptr;
    QLineEdit    *m_configPath = nullptr;
    QPlainTextEdit *m_restartScript = nullptr;
};

} // namespace loftail
