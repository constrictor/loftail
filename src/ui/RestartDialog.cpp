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

#include "RestartDialog.h"

#include "Fonts.h"
#include "MessageLabel.h"
#include "UiColors.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace loftail {

namespace {
// How long a clean run stays on screen before it closes itself.
//
// Not zero. A dialog that appears and vanishes inside one frame reads as a flicker rather
// than as an answer, and the whole value of showing the output at all is that the reader
// SEES it worked. A second is long enough for that and short enough that nobody reaches
// for the mouse.
constexpr int kAutoCloseMs = 1000;

// How much output the pane keeps. The runner caps the bytes it publishes; this is the
// display-side twin, so a single enormous line cannot do what a million small ones cannot.
constexpr int kMaxOutputBlocks = 5000;
} // namespace

RestartDialog::RestartDialog(const QString &logName, RestartTarget target, QWidget *parent)
    : QDialog(parent), m_target(std::move(target))
{
    setObjectName(QStringLiteral("restartDialog")); // findChild, for tests
    setWindowTitle(tr("Restart App"));
    setWindowModality(Qt::ApplicationModal);

    auto *root = new QVBoxLayout(this);

    m_headline = new QLabel(this);
    m_headline->setObjectName(QStringLiteral("restartHeadline")); // findChild, for tests
    m_headline->setTextFormat(Qt::PlainText); // a log name is a path; `<` is not markup
    m_headline->setText(m_target.remote && m_target.host
                            ? tr("Restarting the application behind %1, on %2…")
                                  .arg(logName, m_target.host->displayHost())
                            : tr("Restarting the application behind %1…").arg(logName));

    // What the script was told, on the tooltip rather than in the way: it is the first
    // thing worth checking when a script does nothing, and the last thing worth reading
    // when it works.
    QStringList vars;
    vars.reserve(m_target.variables.size());
    for (const auto &v : m_target.variables)
        vars.append(v.first + QLatin1Char('=') + v.second);
    if (!vars.isEmpty())
        m_headline->setToolTip(vars.join(QLatin1Char('\n')));
    root->addWidget(m_headline);

    m_output = new QPlainTextEdit(this);
    m_output->setObjectName(QStringLiteral("restartOutput")); // findChild, for tests
    m_output->setReadOnly(true);
    m_output->setFont(logTextFont()); // it is program output, in the log's own face and size
    m_output->setMaximumBlockCount(kMaxOutputBlocks);
    m_output->setPlaceholderText(tr("(no output yet)"));
    ensureReadablePlaceholder(m_output);
    root->addWidget(m_output, 1);

    m_status = new MessageLabel(this);
    m_status->setObjectName(QStringLiteral("restartStatus")); // findChild, for tests
    root->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(this);
    buttons->setObjectName(QStringLiteral("restartButtons")); // findChild, for tests

    // TWO BUTTONS, not one that changes its text. A test may never identify a widget by
    // its visible text, so a swapping label would make "which state is this in" a
    // question nothing outside the dialog could answer.
    m_abort = buttons->addButton(tr("&Abort"), QDialogButtonBox::RejectRole);
    m_abort->setObjectName(QStringLiteral("restartAbortButton")); // findChild, for tests
    m_close = buttons->addButton(tr("&Close"), QDialogButtonBox::AcceptRole);
    m_close->setObjectName(QStringLiteral("restartCloseButton")); // findChild, for tests
    m_close->setVisible(false);
    connect(m_abort, &QPushButton::clicked, this, &RestartDialog::requestAbort);
    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);
    root->addWidget(buttons);

    resize(560, 380);
}

RestartDialog::~RestartDialog() = default;

void RestartDialog::run()
{
    m_status->setText(tr("Running…"));

    m_runner = new RestartRunner(this); // destroying the dialog abandons the run
    connect(m_runner, &RestartRunner::outputAppended, this, &RestartDialog::appendOutput);
    connect(m_runner, &RestartRunner::finished, this, &RestartDialog::onFinished);
    m_runner->start(m_target);
}

void RestartDialog::appendOutput(const QByteArray &bytes, bool isStdErr)
{
    if (bytes.isEmpty())
        return;

    // Follow the tail only when the reader is already at it, so scrolling back to read a
    // line does not get yanked away by the next chunk.
    QScrollBar *bar = m_output->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 2;

    QTextCursor cursor(m_output->document());
    cursor.movePosition(QTextCursor::End);
    QTextCharFormat format;
    // WHICH STREAM SAID IT, in colour rather than with a prefix: a prefix would be part of
    // the text somebody copies out of here to paste into a bug report.
    format.setForeground(isStdErr ? errorColor(palette()) : palette().text().color());
    cursor.insertText(QString::fromLocal8Bit(bytes), format);

    if (atEnd)
        bar->setValue(bar->maximum());
}

QString RestartDialog::statusSentence() const
{
    if (!m_result.ok && !m_result.aborted)
        return tr("Could not be run: %1").arg(m_result.error);

    if (m_result.aborted) {
        // Says what an abort DOES and does not promise. On a remote host loftail shut the
        // channel; whether the command over there stopped is the far end's business, and a
        // reader who believes otherwise will run the whole thing again.
        return m_target.remote
            ? tr("Aborted — the command on %1 may still be running.")
                  .arg(m_target.host ? m_target.host->displayHost() : QString())
            : tr("Aborted.");
    }

    if (m_result.exitCode == 0 && !m_result.sawStdErr)
        return tr("Restarted.");
    if (m_result.exitCode == 0)
        return tr("Exited normally, but wrote to standard error.");
    if (m_result.sawStdErr)
        return tr("Exited with status %1, and wrote to standard error.").arg(m_result.exitCode);
    return tr("Exited with status %1.").arg(m_result.exitCode);
}

void RestartDialog::onFinished(const RestartResult &result)
{
    m_result = result;
    m_finished = true;

    QString text = statusSentence();
    if (result.truncated)
        text += QLatin1Char(' ') + tr("Output was longer than loftail will show; it was cut.");
    m_status->setText(text);

    m_abort->setVisible(false);
    m_close->setVisible(true);
    m_close->setDefault(true);

    if (succeeded()) {
        // `this` as the CONTEXT object, so a dialog closed by hand in the meantime
        // disarms the timer rather than being accepted after it has gone.
        QTimer::singleShot(kAutoCloseMs, this, &QDialog::accept);
        return;
    }

    // Focus the output, not the button: on a failure the thing worth doing next is
    // reading and copying the reason, and this makes that reachable from the keyboard.
    m_output->setFocus();
}

void RestartDialog::requestAbort()
{
    // QDialog::reject() and not this class's, which would call back into here: the
    // Abort button is hidden once a run has finished, so this branch is only reachable
    // defensively — and routing it through the override makes the two mutually recursive
    // for no gain.
    if (m_finished) {
        QDialog::reject();
        return;
    }
    if (m_aborting)
        return;
    m_aborting = true;
    m_abort->setEnabled(false);
    m_status->setText(tr("Aborting…"));
    if (m_runner)
        m_runner->abort();
    // NOT closed here. The run has been told to stop and the reader is owed what it says
    // about having stopped — which arrives on finished(), a moment later.
}

void RestartDialog::reject()
{
    // Escape and the ✕ mean "stop it", not "leave it running unattended". Once it has
    // stopped, they mean what they usually do.
    if (m_runner && !m_finished) {
        requestAbort();
        return;
    }
    QDialog::reject();
}

void RestartDialog::closeEvent(QCloseEvent *event)
{
    if (m_runner && !m_finished) {
        requestAbort();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

} // namespace loftail
