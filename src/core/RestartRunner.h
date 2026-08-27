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

#include "RestartTarget.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class QFile;
class QProcess;
class QTemporaryFile;
class QTimer;

namespace loftail {

struct SshWorkerShared;

// What became of a restart script (SPEC.md §4).
struct RestartResult
{
    // It ran to completion, as against failing to start at all. A script that ran and
    // exited 1 is `ok` with `exitCode` 1 — the distinction the exec transport's
    // runCommand() draws for the same reason: "the shell said no" and "there was no
    // shell" want different sentences.
    bool    ok = false;
    bool    aborted = false;
    int     exitCode = -1;

    // Whether the output was longer than loftail will keep. Said in the dialog rather
    // than left to look like a script that stopped mid-sentence.
    bool    truncated = false;

    // Whether anything at all was written to stderr. One of the TWO failure signals, and
    // the one that has nothing to do with the exit status: a script that tidies up after
    // a failure can still exit 0, and a warning the reader has not seen is exactly what
    // an auto-closing dialog would hide.
    bool    sawStdErr = false;

    QString error; // `ok` false only

    // Whether this run is one the dialog may close by itself.
    bool succeeded() const { return ok && !aborted && exitCode == 0 && !sawStdErr; }
};

// Runs one restart script, wherever the log is (ARCHITECTURE.md §6.9).
//
// ONE CLASS FOR BOTH TRANSPORTS, so the dialog above it knows only that a run produces
// output and then finishes. Locally that is a QProcess on this very thread; remotely it is
// a connect and an exec channel on a thread of its own, abandoned rather than joined.
//
// Nothing here starts by itself. A restart happens when somebody presses the item, never
// at open, on a session restore, on a rotation or on a watch tick.
class RestartRunner : public QObject
{
    Q_OBJECT

public:
    explicit RestartRunner(QObject *parent = nullptr);
    ~RestartRunner() override;

    // Begin. `finished` is emitted exactly once afterwards, unless this object is
    // destroyed first — including for a target this build cannot run, which is reported
    // through the same signal on the next turn of the event loop rather than as a return
    // value, so a caller has one path to render.
    void start(const RestartTarget &target);

    // Stop it. Idempotent, and safe while nothing is running.
    //
    // Remotely this shuts the channel; what the far end makes of the resulting hangup is
    // the far end's business, and SPEC.md §4 says so rather than promising otherwise.
    void abort();

    bool isRunning() const { return m_running; }

signals:
    // On the application thread, in arrival order. Bytes, not a QString, because a
    // multi-byte character can straddle a chunk boundary — decoding is the reader's job
    // and it holds the whole of what has arrived.
    void outputAppended(QByteArray bytes, bool isStdErr);

    // On the application thread, exactly once per start().
    void finished(RestartResult result);

private:
    void startLocal(const RestartTarget &target);
    void startRemote(const RestartTarget &target);
    void reportLater(const RestartResult &result);
    void drainLocalOutput();
    void publish(const QByteArray &bytes, bool isStdErr);
    void releaseProcess();

    bool                             m_running = false;
    bool                             m_aborting = false;
    bool                             m_sawStdErr = false;
    qint64                           m_bytesPublished = 0;
    bool                             m_truncated = false;

    QProcess                        *m_process = nullptr;
    QTemporaryFile                  *m_scriptFile = nullptr; // Windows only; see the .cpp

    // WHERE THE CHILD'S OUTPUT GOES, and not a pipe — see startLocal(). Held open at a
    // remembered offset and re-read on a timer, which is what streaming costs once the
    // pipes are given up.
    QTemporaryFile                  *m_outFile = nullptr;
    QTemporaryFile                  *m_errFile = nullptr;
    QFile                           *m_outReader = nullptr;
    QFile                           *m_errReader = nullptr;
    QTimer                          *m_pump = nullptr;

    std::shared_ptr<SshWorkerShared> m_shared;
};

} // namespace loftail
