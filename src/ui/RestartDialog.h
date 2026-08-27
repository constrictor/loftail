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

#include "RestartRunner.h"
#include "RestartTarget.h"

#include <QDialog>
#include <QString>

QT_BEGIN_NAMESPACE
class QLabel;
class QPlainTextEdit;
class QPushButton;
QT_END_NAMESPACE

namespace loftail {

class MessageLabel;

// What a restart script did (SPEC.md §4).
//
// A dialog rather than the status bar or the notice strip, and modal, because this is the
// one thing loftail does that CHANGES SOMETHING ON A MACHINE. A gesture with an effect
// outside the file being read is owed an account of itself, in front of the person who
// asked for it, and it is owed the ability to be stopped.
//
// IT CLOSES ITSELF ON A CLEAN RUN, after a second — long enough to be seen to have
// worked, short enough not to be a thing to dismiss. "Clean" is exit 0 AND nothing on
// standard error: a script that tidies up after a failure can still exit 0, and a warning
// nobody read is exactly what an auto-closing dialog would hide.
class RestartDialog : public QDialog
{
    Q_OBJECT

public:
    // `logName` is what to call the log — logSourceDisplayName(), never a raw address.
    RestartDialog(const QString &logName, RestartTarget target, QWidget *parent = nullptr);
    ~RestartDialog() override;

    // Begin. Separate from the constructor because a test may never exec() a modal: it
    // builds one on the stack, calls this, and spins the event loop.
    void run();

    // Exit 0, nothing on stderr, not aborted, and it actually ran.
    bool succeeded() const { return m_finished && m_result.succeeded(); }
    bool isFinished() const { return m_finished; }

protected:
    // Escape, the ✕ and Abort are ONE path. Three ways out that could disagree about
    // whether the run was stopped is three ways to leave a script running unattended.
    void reject() override;
    void closeEvent(QCloseEvent *event) override;

private:
    void appendOutput(const QByteArray &bytes, bool isStdErr);
    void onFinished(const RestartResult &result);
    void requestAbort();
    QString statusSentence() const;

    RestartTarget   m_target;
    RestartRunner  *m_runner = nullptr;
    RestartResult   m_result;
    bool            m_finished = false;
    bool            m_aborting = false;

    QLabel         *m_headline = nullptr;
    QPlainTextEdit *m_output = nullptr;
    MessageLabel   *m_status = nullptr;
    QPushButton    *m_abort = nullptr;
    QPushButton    *m_close = nullptr;
};

} // namespace loftail
