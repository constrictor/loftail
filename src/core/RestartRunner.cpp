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

#include "RestartRunner.h"

#include "DiagnosticLog.h"
#include "SshExecCommands.h"
#include "SshWorkerPool.h"

#if defined(LOFTAIL_HAVE_SSH)
#include "RemoteLocation.h"
#include "SshSession.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QList>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryFile>
#include <QTimer>

namespace loftail {

namespace {
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::RestartRunner)
};

// How long a shell is given to end on its own after abort() before it is killed.
//
// A shell handed SIGTERM can run its traps and take its children with it, which is a
// better end than SIGKILL leaving a half-restarted service behind. Short, because the
// reader has already asked for it to stop.
constexpr int kKillGraceMs = 2000;

// How often the local run's two output files are re-read.
//
// A POLL, and the price of not using pipes (see startLocal()). Ten times a second is
// below what anybody reading a restart script's output can tell from instant, and it is
// two stat-and-read calls on files that are almost always unchanged.
constexpr int kPumpMs = 100;

// How much of a run's output is kept, per stream.
//
// A restart script that prints forever is a script, not a fault, and it must not be able
// to grow a QPlainTextEdit until the window stops laying out. What is dropped is said in
// the dialog rather than left looking like output that simply stopped.
constexpr qint64 kMaxOutputBytes = 1024LL * 1024;

#if defined(Q_OS_WIN)
// cmd.exe cannot be handed a multi-line command on its command line, so the script goes
// into a file and cmd is pointed at it. Deliberately NOT what the POSIX branch does: `-c`
// there takes a whole script happily, leaves no file behind, and is the exact analogue of
// what libssh2_channel_exec hands a remote login shell.
QString windowsShell()
{
    return qEnvironmentVariable("COMSPEC", QStringLiteral("cmd.exe"));
}
#else
// The user's OWN shell, which is what "the local default shell" means — a script written
// with bash arrays or fish syntax is the script they wrote. /bin/sh is the fallback for a
// process started with no SHELL in its environment, which a desktop launcher can be.
QString posixShell()
{
    const QString shell = qEnvironmentVariable("SHELL");
    return shell.isEmpty() ? QStringLiteral("/bin/sh") : shell;
}
#endif
} // namespace

RestartRunner::RestartRunner(QObject *parent) : QObject(parent) { }

namespace {
// QProcesses that were still running when their owner went.
//
// ~QProcess on a live child WARNS, KILLS, AND BLOCKS in waitForFinished() — a stall on
// the application thread, and the local twin of the deadlock ~ConfigTransfer was written
// against. So a running one is disowned into here instead and deletes itself when the
// child ends, which is what makes closing the dialog on a slow restart instant.
//
// Reachable, so the leak checker does not report one still running at exit — the device
// SourceSpool::drainRetired() and the SSH worker pool both use, and for the same reason.
QList<QProcess *> g_reapedProcesses;
} // namespace

RestartRunner::~RestartRunner()
{
    // ABANDON, NEVER JOIN, on both halves. Remotely for the reason SshWorkerShared states;
    // locally because of the destructor above.
    if (m_shared)
        m_shared->abandon();

    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->setParent(nullptr);
            g_reapedProcesses.append(m_process);
            QProcess *proc = m_process;
            QObject::connect(proc, &QProcess::finished, proc, [proc](int, QProcess::ExitStatus) {
                g_reapedProcesses.removeOne(proc);
                proc->deleteLater();
            });
            // TERMINATE, not kill: a shell handed SIGTERM runs its traps and can take its
            // children with it, which is a better end than SIGKILL leaving half a restart.
            proc->terminate();
            QTimer::singleShot(kKillGraceMs, proc, [proc]() {
                if (proc->state() != QProcess::NotRunning)
                    proc->kill();
            });
        } else {
            m_process->deleteLater();
        }
        m_process = nullptr;
    }
    releaseProcess();
}

void RestartRunner::reportLater(const RestartResult &result)
{
    // Through the event loop, never returned, so that every outcome — including one
    // decided before anything ran — reaches the caller by the same route. A dialog that
    // has to render one case from a return value and the rest from a signal is a dialog
    // with two states nobody exercises.
    QPointer<RestartRunner> self(this);
    QTimer::singleShot(0, this, [self, result]() {
        if (self)
            emit self->finished(result);
    });
}

void RestartRunner::start(const RestartTarget &target)
{
    m_running = true;
    m_aborting = false;
    m_sawStdErr = false;

    if (QString reason; !restartTargetIsRunnable(target, &reason)) {
        RestartResult out;
        out.error = reason;
        m_running = false;
        reportLater(out);
        return;
    }

    if (target.remote)
        startRemote(target);
    else
        startLocal(target);
}

void RestartRunner::publish(const QByteArray &bytes, bool isStdErr)
{
    if (bytes.isEmpty())
        return;
    if (isStdErr)
        m_sawStdErr = true; // ANY byte, and recorded before the cap can drop it

    if (m_bytesPublished >= kMaxOutputBytes) {
        m_truncated = true;
        return;
    }
    QByteArray slice = bytes;
    if (m_bytesPublished + slice.size() > kMaxOutputBytes) {
        slice.truncate(int(kMaxOutputBytes - m_bytesPublished));
        m_truncated = true;
    }
    m_bytesPublished += slice.size();
    emit outputAppended(slice, isStdErr);
}

void RestartRunner::drainLocalOutput()
{
    const auto pull = [this](QFile *reader, bool isStdErr) {
        if (!reader || !reader->isOpen())
            return;
        // The file grows under us; readAll() from the remembered position is exactly what
        // has been appended since the last pass, and the position advances with it.
        const QByteArray bytes = reader->readAll();
        publish(bytes, isStdErr);
    };
    pull(m_outReader, false);
    pull(m_errReader, true);
}

void RestartRunner::releaseProcess()
{
    if (m_pump) {
        m_pump->stop();
        m_pump->deleteLater();
        m_pump = nullptr;
    }
    delete m_outReader;
    m_outReader = nullptr;
    delete m_errReader;
    m_errReader = nullptr;
    delete m_outFile;
    m_outFile = nullptr;
    delete m_errFile;
    m_errFile = nullptr;
    delete m_scriptFile;
    m_scriptFile = nullptr;
}

void RestartRunner::startLocal(const RestartTarget &target)
{
    m_process = new QProcess(this);

    // THE CHILD'S OUTPUT GOES TO FILES, NOT PIPES, and that is the most consequential
    // decision on this path. A restart script very often ends by leaving something
    // running — `./app &`, `nohup app &`, a `su -c` that returns before its child does —
    // and such a grandchild INHERITS the standard streams it was started with. With pipes
    // that is two failures at once, and the second is the serious one:
    //
    //   - the stream never reaches EOF, so anything that waited for one — a completion
    //     test, a drain — would wait for the daemon rather than for the script;
    //   - and closing loftail's read end, which happens the moment this run is torn down,
    //     gives that grandchild SIGPIPE on its next write, whose default action kills it.
    //     loftail would then KILL THE SERVICE IT HAD JUST BEEN ASKED TO RESTART, a minute
    //     later, with nothing on screen connecting the two. That is precisely the class of
    //     harm invariant #5 exists for.
    //
    // The second is not theoretical and was measured: with pipes, a script ending
    // `( sleep 1; echo x; touch marker ) &` prints its own output correctly and the
    // marker is NEVER created — the subshell is dead by the time it writes. With files
    // it is created every time. tst_restartrunner::closingTheRunDoesNotKillWhatTheScript
    // Started is that experiment.
    //
    // A file has no reader to disappear, so neither can happen. What it costs is that
    // streaming becomes a poll (drainLocalOutput on a kPumpMs timer) rather than a signal.
    // Do not "simplify" this back to readyReadStandardOutput.
    //
    // Two files and never MergedChannels: which stream a byte came out of is half of what
    // this feature reports, and merging throws it away irrecoverably.
    m_outFile = new QTemporaryFile(QDir::tempPath() + QLatin1String("/loftail-out-XXXXXX"));
    m_errFile = new QTemporaryFile(QDir::tempPath() + QLatin1String("/loftail-err-XXXXXX"));
    if (!m_outFile->open() || !m_errFile->open()) {
        RestartResult out;
        out.error = Tr::tr("A temporary file for the output could not be created.");
        m_running = false;
        releaseProcess();
        reportLater(out);
        return;
    }
    const QString outPath = m_outFile->fileName();
    const QString errPath = m_errFile->fileName();
    m_process->setStandardOutputFile(outPath, QIODevice::Truncate);
    m_process->setStandardErrorFile(errPath, QIODevice::Truncate);

    // STDIN IS THE NULL DEVICE. A script that reads standard input — a `read`, an `ssh`,
    // an `apt` that decides to ask something — would otherwise block on a terminal that
    // is not there, which from outside is indistinguishable from a slow restart.
    m_process->setStandardInputFile(QProcess::nullDevice());

    // Both are opened, and a failure is REPORTED rather than ignored: drainLocalOutput()
    // skips a reader that is not open, so the run still finishes and still reports its
    // exit status — with the script's own words missing, which is half of what the dialog
    // has to say. Neither call is short-circuited out of the other's failure.
    m_outReader = new QFile(outPath);
    m_errReader = new QFile(errPath);
    const bool outOpened = m_outReader->open(QIODevice::ReadOnly);
    const bool errOpened = m_errReader->open(QIODevice::ReadOnly);
    if (!outOpened || !errOpened) {
        diagLog("restart", QStringLiteral("could not read back the script's output: "
                                          "stdout=%1 stderr=%2")
                               .arg(outOpened ? QStringLiteral("ok")
                                              : m_outReader->errorString(),
                                    errOpened ? QStringLiteral("ok")
                                              : m_errReader->errorString()));
    }

    m_pump = new QTimer(this);
    m_pump->setInterval(kPumpMs);
    connect(m_pump, &QTimer::timeout, this, &RestartRunner::drainLocalOutput);
    m_pump->start();

    // The variables go in the ENVIRONMENT rather than being substituted into the text, so
    // a path with a space, a quote or a `$` in it cannot break the script or smuggle a
    // command into it. QProcess hands argv straight to the platform, so on this path there
    // is NO SHELL QUOTING ANYWHERE — the question the remote side answers with
    // shellQuote() is removed here rather than answered.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const auto &v : target.variables)
        env.insert(v.first, v.second);
    m_process->setProcessEnvironment(env);

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return; // every other error is followed by finished(), which reports it
        RestartResult out;
        out.error = Tr::tr("%1 could not be started.").arg(m_process->program());
        m_running = false;
        releaseProcess();
        emit finished(out);
    });
    connect(m_process, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus status) {
                // COMPLETION IS THE PROCESS DYING, and it cannot be anything else here:
                // there is no channel to reach EOF. That is what makes the backgrounded
                // daemon above a non-event rather than a hang.
                drainLocalOutput(); // whatever landed between the last tick and now

                RestartResult result;
                result.ok = true;
                result.aborted = m_aborting;
                result.sawStdErr = m_sawStdErr;
                result.truncated = m_truncated;
                result.exitCode = status == QProcess::NormalExit ? code : -1;
                m_running = false;
                releaseProcess();
                emit finished(result);
            });

#if defined(Q_OS_WIN)
    // cmd.exe cannot be handed a multi-line command on its command line — CreateProcess
    // takes one string and cmd reads it as one command — so the script goes into a file
    // and cmd is pointed at it. Deliberately NOT what the POSIX branch does: `-c` there
    // takes a whole script as one argv element, leaves nothing on disk, and is the exact
    // analogue of what libssh2_channel_exec hands a remote login shell.
    m_scriptFile = new QTemporaryFile(QDir::tempPath() + QLatin1String("/loftail-XXXXXX.cmd"));
    if (!m_scriptFile->open()) {
        RestartResult out;
        out.error = Tr::tr("A temporary script file could not be created.");
        m_running = false;
        releaseProcess();
        reportLater(out);
        return;
    }
    // CRLF, because cmd.exe is the reader and a bare LF makes it mis-split a line — the
    // exact mirror of the LF normalisation restartScriptCommand() does for a POSIX shell.
    QString text = target.script;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\n'), QLatin1String("\r\n"));
    m_scriptFile->write(text.toLocal8Bit());
    m_scriptFile->write("\r\n");
    m_scriptFile->flush();
    // %LOGFILE%, not $LOGFILE, is how this script reads its variables. Said in the
    // Preferences tooltip and in SPEC.md §4 rather than left to be found.
    m_process->start(windowsShell(),
                     {QStringLiteral("/d"), QStringLiteral("/s"), QStringLiteral("/c"),
                      m_scriptFile->fileName()});
#else
    // ONE `-c`, the whole script as a single argv element. Not line by line: running a
    // script a line at a time would break every `if`, every loop, and every variable set
    // on one line and read on the next.
    m_process->start(posixShell(), {QStringLiteral("-c"), target.script});
#endif
}

void RestartRunner::startRemote(const RestartTarget &target)
{
#if !defined(LOFTAIL_HAVE_SSH)
    RestartResult out;
    restartTargetIsRunnable(target, &out.error);
    m_running = false;
    reportLater(out);
#else
    m_shared = std::make_shared<SshWorkerShared>();

    const QString address = target.host ? target.host->toString() : QString();
    const QString command = restartScriptCommand(target.script, target.variables);

    auto shared = m_shared;
    QPointer<RestartRunner> self(this);

    startSshWorker([address, command, shared, self]() {
        RestartResult out;
        int code = -1;

        // Chunks are marshalled to the application thread as they arrive. The QPointer is
        // carried across threads but only ever DEREFERENCED there, which is what makes
        // carrying it legal: if the owner went in the meantime, this does nothing.
        const auto onChunk = [self, shared](const QByteArray &bytes, bool isStdErr) {
            if (shared->abandoned || !QCoreApplication::instance() || bytes.isEmpty())
                return;
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, bytes, isStdErr]() {
                    // Through publish(), the same funnel the local half uses, so the cap
                    // and the "saw stderr" record are one rule rather than two.
                    if (self)
                        self->publish(bytes, isStdErr);
                },
                Qt::QueuedConnection);
        };

        // ExecOnly, and it is worth twenty seconds. runScript() opens a plain exec
        // channel, which needs the session and nothing else — so the SFTP init a
        // LogTransport connect ends with buys this errand nothing, and on the servers the
        // exec fallback exists for it costs the WHOLE connect budget before the script
        // starts: sshd accepts the subsystem channel, no `sftp-server` answers, and
        // libssh2 waits out kSshWorkerConnectTimeoutMs for a version packet that is never
        // coming (SshSession.h, §6.9). That was the reported "Restart App takes twenty
        // seconds before anything happens".
        //
        // Nothing in this lambda may reach for a file: an ExecOnly session has no SFTP
        // handle and no settled size rung, and every operation that would want one refuses
        // it by name. runScript() is the one thing here, which is the whole errand.
        const QString error = withSshSession(
            address, &shared->relay, shared, SshSession::Need::ExecOnly,
            [&out, &code, &command, &onChunk, &shared](SshSession &session, const QString &) {
                QString why;
                // The chunk callback also RECORDS whether stderr was touched, on the
                // worker, because the marshalled copy may never be delivered — an
                // abandoned run must not report a clean stderr it never actually read.
                bool sawErr = false;
                const auto tap = [&sawErr, &onChunk](const QByteArray &bytes, bool isStdErr) {
                    if (isStdErr && !bytes.isEmpty())
                        sawErr = true;
                    onChunk(bytes, isStdErr);
                };
                const bool ran = session.runScript(command, tap, &code, &why);
                out.sawStdErr = sawErr;
                if (!ran) {
                    // An interrupted read is what an abort looks like from in here, and it
                    // is not a failure to report: the owner asked for it and already knows.
                    if (shared->abandoned)
                        return QString();
                    return why;
                }
                out.ok = true;
                out.exitCode = code;
                return QString();
            });

        if (!error.isEmpty()) {
            out.ok = false;
            out.error = error;
        }
        if (shared->abandoned) {
            out.aborted = true;
            out.ok = false;
        }
        if (shared->abandoned || !QCoreApplication::instance())
            return; // the owner has gone, or asked to stop; nothing to report to
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, out]() {
                if (self) {
                    self->m_running = false;
                    RestartResult final = out;
                    final.truncated = self->m_truncated;
                    emit self->finished(final);
                }
            },
            Qt::QueuedConnection);
    });
#endif
}

void RestartRunner::abort()
{
    if (!m_running || m_aborting)
        return;
    m_aborting = true;

    if (m_process && m_process->state() != QProcess::NotRunning) {
        // TERMINATE first, then kill after a grace: a shell given SIGTERM can run its
        // traps and take its children with it. The answer still comes from finished(),
        // which is the one place a local run is reported from.
        m_process->terminate();
        QPointer<QProcess> proc(m_process);
        QTimer::singleShot(kKillGraceMs, this, [proc]() {
            if (proc && proc->state() != QProcess::NotRunning)
                proc->kill();
        });
        return;
    }

    if (m_shared) {
        // The remote worker reports nothing after this — it finds `abandoned` set — so the
        // answer is published from here instead, which is also what makes an abort instant
        // on a host that has stopped answering.
        m_shared->abandon();
        RestartResult out;
        out.aborted = true;
        out.sawStdErr = m_sawStdErr;
        out.truncated = m_truncated;
        m_running = false;
        reportLater(out);
    }
}

} // namespace loftail
