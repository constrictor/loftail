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

#include <QtTest>

#include <QByteArray>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>

#include <functional>

#include "Document.h"
#include "LiveController.h"
#include "LogModel.h"
#include "LogSource.h"
#include "RemoteLocation.h"
#include "SshExecCommands.h"

// std::thread and not QThread: ARCHITECTURE.md §13.1 — a QThread join goes through a
// QWaitCondition, which breaks TSan's happens-before chain at the unannotated system Qt.
#include <chrono>
#include <thread>
#include "SourceSpool.h"
#include "SshPrompter.h"
#include "SshSession.h"
#include "SshSessionCache.h"
#include "SshWorkerPool.h"

using namespace loftail;

// M11 — the ONLY test that touches a real SSH server, and the only place the libssh2
// code is exercised at all. Everything above the transport is covered without a
// network by tst_spooledsource and tst_remotetail; what cannot be faked is here:
// the handshake, host-key verification, agent/key authentication, and whether a real
// sftp-server's FSTAT follows the handle (which is what stands in for an inode when
// detecting a rotation).
//
// CI NEVER RUNS THIS. It is skipped unless LOFTAIL_TEST_SSH_URL names a writable
// remote path, e.g.
//
//   LOFTAIL_TEST_SSH_URL=ssh://me@host/tmp/loftail-test.log ./build/tests/tst_sshlive
//
// The file is created and overwritten by the test, so point it somewhere disposable.
// Authentication must be non-interactive (agent or key): there is no prompter here,
// so a password-only host skips rather than hangs.
class TestSshLive : public QObject
{
    Q_OBJECT

private:
    QString m_url;
    QString m_remotePath;
    RemoteLocation m_location;

    // Runs a command on the remote host over the system ssh client. Used only to SET
    // UP the fixture — the code under test is loftail's own libssh2 path.
    bool remoteShell(const QString &command)
    {
        QStringList args;
        args << QStringLiteral("-o") << QStringLiteral("BatchMode=yes");
        if (m_location.port != RemoteLocation::kDefaultPort)
            args << QStringLiteral("-p") << QString::number(m_location.port);
        args << (m_location.user.isEmpty() ? m_location.host
                                           : m_location.user + u'@' + m_location.host);
        args << command;

        QProcess ssh;
        ssh.start(QStringLiteral("ssh"), args);
        if (!ssh.waitForFinished(30000))
            return false;
        return ssh.exitStatus() == QProcess::NormalExit && ssh.exitCode() == 0;
    }

    // Same, but hands back what the command printed. The exec-fallback case needs the
    // OUTPUT of the very commands the transport builds, not just their exit status.
    QByteArray remoteShellOutput(const QString &command)
    {
        QStringList args;
        args << QStringLiteral("-o") << QStringLiteral("BatchMode=yes");
        if (m_location.port != RemoteLocation::kDefaultPort)
            args << QStringLiteral("-p") << QString::number(m_location.port);
        args << (m_location.user.isEmpty() ? m_location.host
                                           : m_location.user + u'@' + m_location.host);
        args << command;

        QProcess ssh;
        ssh.start(QStringLiteral("ssh"), args);
        if (!ssh.waitForFinished(30000))
            return {};
        return ssh.readAllStandardOutput();
    }

    bool writeRemote(const QByteArray &content)
    {
        return remoteShell(QStringLiteral("cat > %1").arg(m_remotePath), content);
    }

    bool remoteShell(const QString &command, const QByteArray &stdinData)
    {
        QStringList args;
        args << QStringLiteral("-o") << QStringLiteral("BatchMode=yes");
        if (m_location.port != RemoteLocation::kDefaultPort)
            args << QStringLiteral("-p") << QString::number(m_location.port);
        args << (m_location.user.isEmpty() ? m_location.host
                                           : m_location.user + u'@' + m_location.host);
        args << command;

        QProcess ssh;
        ssh.start(QStringLiteral("ssh"), args);
        if (!ssh.waitForStarted(15000))
            return false;
        ssh.write(stdinData);
        ssh.closeWriteChannel();
        if (!ssh.waitForFinished(30000))
            return false;
        return ssh.exitStatus() == QProcess::NormalExit && ssh.exitCode() == 0;
    }

    // Wait until `predicate` holds or the timeout expires — the fetcher polls on its
    // own thread, so a remote change arrives after a delay, not instantly.
    static bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 15000)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            if (predicate())
                return true;
            QTest::qWait(200);
        }
        return predicate();
    }

private slots:
    void initTestCase();
    void cleanup();
    void connectsAndReadsTheRemoteFile();
    void followsAppendsFromTheRealServer();
    void detectsRealRotation();
    void reportsAnUnreachableHostClearly();
    void theExecFallbackReadsTheSameBytes();
    void theExecFallbackSizesWithoutStat();

    // The seek elision in SshSession::readAt(). Only a real server executes any of it —
    // the subject is libssh2's own read cursor, so there is nothing to fake.
    void sequentialReadsLandWhereTheyAskedWithNoSeekBetweenThem();
    void aConfigFileIsReadAndWrittenWholeOverSftp();
    void writingAConfigKeepsItsPermissions();
    void theExecFallbackWritesTheSameBytes();

    // M23 — the restart script over a real link. The only place SshSession::runScript()
    // is executed at all: CI has no server, so everything above it is compiled and never
    // run until somebody sets LOFTAIL_TEST_SSH_URL.
    void aRestartScriptRunsOnTheFarEndAndKeepsItsStderr();
    void aRestartScriptOutlivesTheConnectTimeout();
    void abortingARemoteScriptReturnsAtOnce();
    void aRestartScriptRunsOnAnExecOnlyConnect();

    // bugs.md 30 — the only execution of the dead-session latch. The classification
    // behind it is pinned without a server (tst_sshsessionhealth); that a real libssh2
    // actually reports one of those codes when the link goes, and that an ordinary
    // missing file does not, can only be found out against a real one.
    void aDroppedLinkIsNoticedRatherThanPolledForEver();

    // The idle session cache, end to end (§6.3). DECLARED LAST on purpose: it finishes by
    // draining, which latches the process's one cache shut for good, so anything after it
    // would be testing a build with the feature turned off.
    void oneConnectionServesSeveralErrandsAndTheDrainLetsItGo();
};

void TestSshLive::initTestCase()
{
    m_url = QProcessEnvironment::systemEnvironment().value(QStringLiteral("LOFTAIL_TEST_SSH_URL"));
    if (m_url.isEmpty()) {
        QSKIP("Set LOFTAIL_TEST_SSH_URL=ssh://user@host/tmp/disposable.log to run the "
              "live SSH tests. They are never run in CI.");
    }
    const auto parsed = RemoteLocation::parse(m_url);
    QVERIFY2(parsed.has_value(), "LOFTAIL_TEST_SSH_URL is not a valid ssh:// URL");
    m_location = *parsed;
    m_remotePath = m_location.path;

    // No prompter: authentication must be non-interactive, so a wedged test cannot
    // sit forever waiting for a dialog nobody is looking at.
    setSshPrompter(nullptr);
    QVERIFY2(remoteShell(QStringLiteral("true")),
             "Cannot reach the test host non-interactively (agent or key auth needed)");
}

void TestSshLive::aConfigFileIsReadAndWrittenWholeOverSftp()
{
    // The config-file editor's two operations against a REAL server (SPEC.md §4). This
    // is the only place they are exercised at all: CI never runs this file, and nothing
    // in it can be faked usefully — a stub returns instantly and would satisfy the
    // contract exactly as well whether or not any of this works.
    const QString cfg = m_remotePath + QStringLiteral(".properties");
    QVERIFY(remoteShell(QStringLiteral("rm -f %1").arg(cfg)));

    SshSession session;
    QString error;
    QVERIFY2(session.connectTo(m_location, nullptr, 20000, &error), qPrintable(error));

    // NOT THERE is a success with `existed` false, not a failure: the editor opens empty
    // on it and saving creates it.
    QByteArray got;
    bool existed = true;
    QVERIFY2(session.readFileAt(cfg, &got, &existed, &error), qPrintable(error));
    QVERIFY(!existed);
    QVERIFY(got.isEmpty());

    const QByteArray body = "log4cplus.rootLogger=DEBUG, STDOUT\n"
                            "log4cplus.logger.app.db=WARN\n";
    QVERIFY2(session.writeFileAt(cfg, body, &error), qPrintable(error));
    QCOMPARE(remoteShellOutput(QStringLiteral("cat %1").arg(cfg)), body);

    // And back, whole, with the existence flag now true — which is what stops the editor
    // calling a file that IS there a new one.
    got.clear();
    existed = false;
    QVERIFY2(session.readFileAt(cfg, &got, &existed, &error), qPrintable(error));
    QVERIFY(existed);
    QCOMPARE(got, body);

    // A SHORTER rewrite, because truncation is where a write that only ever appends or
    // overwrites in place would leave the old tail behind and produce a file that is
    // half of each version.
    const QByteArray shorter = "log4cplus.rootLogger=OFF\n";
    QVERIFY2(session.writeFileAt(cfg, shorter, &error), qPrintable(error));
    QCOMPARE(remoteShellOutput(QStringLiteral("cat %1").arg(cfg)), shorter);

    QVERIFY(remoteShell(QStringLiteral("rm -f %1").arg(cfg)));
}

void TestSshLive::writingAConfigKeepsItsPermissions()
{
    // THE CLAIM THAT ONLY A REAL SERVER CAN SETTLE, and the one with the worst failure:
    // a config that was readable only by its owner must not come back world-readable
    // because a log viewer saved it. Nothing on screen would say that it had happened,
    // and the file in question is the one that decides what an application logs.
    //
    // The write is in place — the existing inode truncated, never replaced — which is
    // what preserves the mode, and the owner and group a rename could not.
    const QString cfg = m_remotePath + QStringLiteral(".perm");
    QVERIFY(remoteShell(QStringLiteral("printf 'a=1\n' > %1 && chmod 600 %1").arg(cfg)));
    const QByteArray before =
        remoteShellOutput(QStringLiteral("ls -l %1 | cut -c1-10").arg(cfg)).trimmed();
    QCOMPARE(before, QByteArray("-rw-------"));

    SshSession session;
    QString error;
    QVERIFY2(session.connectTo(m_location, nullptr, 20000, &error), qPrintable(error));
    QVERIFY2(session.writeFileAt(cfg, "a=2\n", &error), qPrintable(error));

    QCOMPARE(remoteShellOutput(QStringLiteral("cat %1").arg(cfg)), QByteArray("a=2\n"));
    const QByteArray after =
        remoteShellOutput(QStringLiteral("ls -l %1 | cut -c1-10").arg(cfg)).trimmed();
    QCOMPARE(after, before);

    QVERIFY(remoteShell(QStringLiteral("rm -f %1").arg(cfg)));
}

void TestSshLive::theExecFallbackWritesTheSameBytes()
{
    // The other transport, driven directly — a server offering SFTP will never choose
    // the fallback on its own. The exec write is `cat > 'path'`, whose in-place truncate
    // gives the same permission guarantee the SFTP path gets explicitly.
    const QString cfg = m_remotePath + QStringLiteral(".exec");
    QVERIFY(remoteShell(QStringLiteral("printf 'old=1\n' > %1 && chmod 640 %1").arg(cfg)));

    SshSession exec;
    QString error;
    QVERIFY2(exec.connectTo(m_location, nullptr, 20000, &error, nullptr), qPrintable(error));
    if (exec.mode() != SshSession::Mode::Exec) {
        // The transport is chosen by probing, so it cannot be asked for. Run this against
        // a server whose sshd has no `Subsystem sftp` line to exercise it.
        QSKIP("this server offers SFTP, so the exec fallback is not in use");
    }

    const QByteArray body = "log4cplus.rootLogger=INFO\n";
    QVERIFY2(exec.writeFileAt(cfg, body, &error), qPrintable(error));
    QCOMPARE(remoteShellOutput(QStringLiteral("cat %1").arg(cfg)), body);
    QCOMPARE(remoteShellOutput(QStringLiteral("ls -l %1 | cut -c1-10").arg(cfg)).trimmed(),
             QByteArray("-rw-r-----"));

    QByteArray got;
    bool existed = false;
    QVERIFY2(exec.readFileAt(cfg, &got, &existed, &error), qPrintable(error));
    QVERIFY(existed);
    QCOMPARE(got, body);

    QVERIFY(remoteShell(QStringLiteral("rm -f %1").arg(cfg)));
}

void TestSshLive::theExecFallbackReadsTheSameBytes()
{
    // The exec transport against a REAL server (ARCHITECTURE.md §6.3.1). Everything
    // above SshSession is identical either way, so the claim worth checking here is the
    // narrow one: driven directly, the exec mode's five operations return what the SFTP
    // mode's do. Reached by talking to SshSession itself, since a server that offers
    // SFTP will never choose the fallback on its own.
    const QByteArray content =
        "2026-07-21 00:00:01,000 [t0] INFO  logger.a - exec channel line one\n"
        "2026-07-21 00:00:02,000 [t1] WARN  logger.b - exec channel line two\n";
    QVERIFY(writeRemote(content));

    SshSession sftp;
    QString error;
    QVERIFY2(sftp.connectTo(m_location, nullptr, 20000, &error), qPrintable(error));
    QCOMPARE(sftp.mode(), SshSession::Mode::Sftp);
    QVERIFY2(sftp.openFile(&error), qPrintable(error));
    const SshSession::Attrs viaSftp = sftp.statPath();
    QVERIFY(viaSftp.valid);
    QCOMPARE(viaSftp.size, qint64(content.size()));

    QByteArray sftpBytes(content.size(), '\0');
    QCOMPARE(sftp.readAt(0, sftpBytes.data(), sftpBytes.size(), &error), qint64(content.size()));
    QCOMPARE(sftpBytes, content);
    sftp.close();

    // Now the same file through the fallback. execShell() runs the very commands the
    // transport builds, so this also proves they work on THIS server's stat and tail
    // rather than only on the author's.
    const ExecAttrs viaExec = parseStatOutput(remoteShellOutput(statCommand(m_remotePath)));
    QVERIFY2(viaExec.ok, "this server's stat matched neither the GNU nor the BSD form");
    QCOMPARE(viaExec.size, viaSftp.size);
    // mtime granularity is seconds on both sides, so they must agree exactly.
    QCOMPARE(viaExec.mtime, viaSftp.mtime);

    // Reads at an offset, which is the property SCP could not have provided.
    QCOMPARE(remoteShellOutput(readCommand(m_remotePath, 0, 20)), content.left(20));
    QCOMPARE(remoteShellOutput(readCommand(m_remotePath, 20, 30)), content.mid(20, 30));
    QCOMPARE(remoteShellOutput(readCommand(m_remotePath, content.size(), 10)), QByteArray());

    // And the probe agrees this server could host the fallback at all.
    const ExecTools tools = parseProbeOutput(remoteShellOutput(probeCommand()));
    QVERIFY2(tools.ok, "this server has no tail and head for the fallback to use");
    QVERIFY2(tools.anySizeTool(), "this server offers nothing to measure the log with");
}

void TestSshLive::theExecFallbackSizesWithoutStat()
{
    // The rungs below `stat`, against a REAL server (M16, §6.3.1). Nothing in CI has an
    // `ls` other than this machine's, and the column layout is the one thing about this
    // transport that varies by userland — so a real remote `ls` is worth the round trip
    // even though the server under test almost certainly does have `stat`.
    const QByteArray content =
        "2026-07-21 00:00:01,000 [t0] INFO  logger.a - measured without stat\n";
    QVERIFY(writeRemote(content));

    SshSession sftp;
    QString error;
    QVERIFY2(sftp.connectTo(m_location, nullptr, 20000, &error), qPrintable(error));
    QVERIFY2(sftp.openFile(&error), qPrintable(error));
    const SshSession::Attrs viaSftp = sftp.statPath();
    QVERIFY(viaSftp.valid);
    sftp.close();

    const ExecTools tools = parseProbeOutput(remoteShellOutput(probeCommand()));
    if (tools.hasLs) {
        const ExecAttrs viaLs =
            parseLsSizeOutput(remoteShellOutput(lsSizeCommand(m_remotePath)));
        QVERIFY2(viaLs.ok, "this server's ls -lnLd printed a shape the parser rejects");
        QCOMPARE(viaLs.size, viaSftp.size);
        // No epoch here, which is what puts SshFetcher on the stalled-size rotation rule.
        QCOMPARE(viaLs.mtime, kUnknownMtime);
    }
    if (tools.hasWc) {
        const ExecAttrs viaWc =
            parseWcSizeOutput(remoteShellOutput(wcSizeCommand(m_remotePath)));
        QVERIFY(viaWc.ok);
        QCOMPARE(viaWc.size, viaSftp.size);
        QCOMPARE(viaWc.mtime, kUnknownMtime);
    }
}

void TestSshLive::sequentialReadsLandWhereTheyAskedWithNoSeekBetweenThem()
{
    // readAt() issues libssh2_sftp_seek64() only when libssh2's cursor is not already at
    // the offset asked for. That is a performance decision — a seek flushes the handle's
    // read-ahead and drops its outstanding requests, so seeking per chunk restarted the
    // pipeline cold on the strictly forward reads SshFetcher::fetchForward() makes — and
    // the saving is not observable from out here. What IS observable, and what this pins,
    // is the risk the decision takes on: a tracked position that outlives the handle it
    // described makes a read skip a seek it needed, so bytes from one place are written
    // into the spool at the offset of another, silently, under an indexer walking that
    // spool by offset. There is no fake worth writing for this, the whole subject being
    // libssh2's cursor, so it lives here with the rest of what needs a real sftp-server.
    QByteArray content;
    for (int i = 0; content.size() < 40000; ++i) {
        content += "2026-07-21 00:00:01,000 [t0] INFO  logger.a - chunked read line "
            + QByteArray::number(i).rightJustified(6, '0') + "\n";
    }
    QVERIFY(writeRemote(content));

    SshSession session;
    QString error;
    QVERIFY2(session.connectTo(m_location, nullptr, 20000, &error), qPrintable(error));
    QCOMPARE(session.mode(), SshSession::Mode::Sftp);
    QVERIFY2(session.openFile(&error), qPrintable(error));

    const auto readChunk = [&](qint64 off, qint64 len) {
        QByteArray buffer(int(len), '\0');
        const qint64 n = session.readAt(off, buffer.data(), len, &error);
        if (n < 0)
            return QByteArray();
        buffer.resize(int(n));
        return buffer;
    };

    // The path the elision is for: every offset is the previous one plus what came back,
    // so no seek is issued after the first.
    QByteArray gathered;
    qint64 offset = 0;
    while (offset < content.size()) {
        const QByteArray chunk = readChunk(offset, 4096);
        QVERIFY2(!chunk.isEmpty(), qPrintable(error));
        gathered += chunk;
        offset += chunk.size();
    }
    QCOMPARE(gathered, content);

    // And the path it is not: offsets that jump about, each of which must still seek.
    QCOMPARE(readChunk(content.size() - 100, 100), content.right(100));
    QCOMPARE(readChunk(0, 512), content.left(512));
    QCOMPARE(readChunk(12345, 777), content.mid(12345, 777));
    // Straight on from where that landed, which is the elided case again.
    QCOMPARE(readChunk(12345 + 777, 777), content.mid(12345 + 777, 777));

    // THE ONE THAT MATTERS. Read forward, then take a fresh handle — which is what a
    // rotation does — and ask for the same offset again. A position that survived the
    // reopen would skip the seek and hand back the head of the file while claiming to be
    // 4096 bytes in, which is exactly how the wrong bytes reach the spool.
    QCOMPARE(readChunk(0, 4096), content.left(4096));
    QVERIFY2(session.openFile(&error), qPrintable(error));
    QCOMPARE(readChunk(4096, 4096), content.mid(4096, 4096));

    // The same claim through closeFile(), the other route by which a handle goes away.
    QCOMPARE(readChunk(8192, 4096), content.mid(8192, 4096));
    session.closeFile();
    QVERIFY2(session.openFile(&error), qPrintable(error));
    QCOMPARE(readChunk(12288, 4096), content.mid(12288, 4096));

    session.close();
}

void TestSshLive::cleanup()
{
    SourceSpoolRegistry::instance().clear();
}

void TestSshLive::connectsAndReadsTheRemoteFile()
{
    const QByteArray content = "2026-07-21 00:00:01,000 [t0] INFO  logger.a - hello over ssh\n";
    QVERIFY(writeRemote(content));

    Document doc;
    QVERIFY2(doc.open(m_url, QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                      Encoding::Utf8, QTimeZone::utc()),
             qPrintable(doc.lastError()));

    QCOMPARE(doc.index().records.size(), 1);
    QCOMPARE(doc.messageText(doc.index().records.at(0)), QStringLiteral("hello over ssh"));
    // The spool is a local file, so random access survives the round trip.
    QVERIFY(doc.source()->isRandomAccess());
}

void TestSshLive::followsAppendsFromTheRealServer()
{
    QVERIFY(writeRemote("2026-07-21 00:00:01,000 [t0] INFO  logger.a - first\n"));

    Document doc;
    QVERIFY(doc.open(m_url, QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                     Encoding::Utf8, QTimeZone::utc()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 1);

    QVERIFY(remoteShell(
        QStringLiteral("printf '%s' '2026-07-21 00:00:02,000 [t1] WARN  logger.b - second\\n' >> %1")
            .arg(m_remotePath)));

    // The fetcher notices on its own schedule; checkNow() then ingests what landed.
    QVERIFY2(waitFor([&] {
                 live.checkNow();
                 return model.rowCount() == 2;
             }),
             "the appended record never arrived");
    QCOMPARE(doc.messageText(doc.index().records.at(1)), QStringLiteral("second"));
}

void TestSshLive::detectsRealRotation()
{
    QVERIFY(writeRemote("2026-07-21 00:00:01,000 [t0] INFO  logger.old - before\n"));

    Document doc;
    QVERIFY(doc.open(m_url, QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
                     Encoding::Utf8, QTimeZone::utc()));
    LogModel model(&doc);
    LiveController live(&doc, &model);
    live.start();
    QCOMPARE(model.rowCount(), 1);

    int rescans = 0;
    connect(&live, &LiveController::rescanned, &live, [&] { ++rescans; });

    // A real logrotate: rename away, then create a new file at the same path. This is
    // the case the fstat-vs-stat comparison exists for, and the one no fake can prove.
    QVERIFY(remoteShell(QStringLiteral("mv %1 %1.1").arg(m_remotePath)));
    QVERIFY(writeRemote("2026-07-21 00:00:05,000 [t9] ERROR logger.new - after rotate\n"));

    QVERIFY2(waitFor([&] {
                 live.checkNow();
                 return rescans > 0 && model.rowCount() == 1
                     && doc.index().loggers.names().contains(QStringLiteral("logger.new"));
             }),
             "the rotation was never detected");

    remoteShell(QStringLiteral("rm -f %1.1").arg(m_remotePath));
}

void TestSshLive::reportsAnUnreachableHostClearly()
{
    Document doc;
    QVERIFY(!doc.open(QStringLiteral("ssh://nobody@127.0.0.1:1/tmp/nothing.log"),
                      QStringLiteral("%m%n"), Encoding::Utf8, QTimeZone::utc()));
    // A connection failure must reach the status bar as something a person can act
    // on, not as a bare "cannot open".
    QVERIFY(!doc.lastError().isEmpty());
    QVERIFY(doc.lastError().contains(QStringLiteral("127.0.0.1")));
}

void TestSshLive::aRestartScriptRunsOnTheFarEndAndKeepsItsStderr()
{
    // THE ONLY EXECUTION of the streaming channel loop. It differs from runCommand() in
    // three ways at once — stderr kept, session non-blocking, timeout suspended — and
    // none of the three can be faked usefully: a stub returns instantly and satisfies the
    // contract exactly as well whether or not any of it works.
    const QString marker = m_remotePath + QStringLiteral(".restart-ran");
    QVERIFY(remoteShell(QStringLiteral("rm -f %1").arg(marker)));

    SshSession session;
    QString error;
    QVERIFY2(session.connectTo(m_location, nullptr, 20000, &error), qPrintable(error));

    const QList<QPair<QString, QString>> vars = {
        {QStringLiteral("LOGFILE"), m_remotePath},
        {QStringLiteral("MEMBER"), QStringLiteral("var/log/app.log")},
    };
    const QString script = QStringLiteral(
        "printf 'out:%s' \"$LOGFILE\"\n"
        "printf 'err:%s' \"$MEMBER\" >&2\n"
        "touch %1\n"
        "exit 5\n").arg(marker);

    QByteArray out, err;
    int code = -1;
    const bool ran = session.runScript(
        restartScriptCommand(script, vars),
        [&out, &err](const QByteArray &bytes, bool isStdErr) {
            (isStdErr ? err : out).append(bytes);
        },
        &code, &error);
    QVERIFY2(ran, qPrintable(error));

    // The two streams stay apart — which is what the dialog's success rule is built on —
    // and the script's own exit status survives the brace group and the redirect.
    QCOMPARE(out, ("out:" + m_remotePath).toUtf8());
    QCOMPARE(err, QByteArray("err:var/log/app.log"));
    QCOMPARE(code, 5);
    QCOMPARE(remoteShellOutput(QStringLiteral("test -e %1 && printf yes").arg(marker)),
             QByteArray("yes"));

    QVERIFY(remoteShell(QStringLiteral("rm -f %1").arg(marker)));
}

void TestSshLive::aRestartScriptOutlivesTheConnectTimeout()
{
    // connectTo() leaves libssh2's session timeout at the connect budget. Without the
    // suspension inside runScript(), any restart taking longer than that is reported as a
    // dropped link — and most interesting restarts do. Twenty-five seconds against a
    // twenty-second budget, which is the smallest gap that proves anything.
    SshSession session;
    QString error;
    QVERIFY2(session.connectTo(m_location, nullptr, 20000, &error), qPrintable(error));

    QElapsedTimer clock;
    clock.start();
    int code = -1;
    const bool ran = session.runScript(
        restartScriptCommand(QStringLiteral("sleep 25\nprintf late\n"), {}), nullptr, &code,
        &error);
    QVERIFY2(ran, qPrintable(error));
    QCOMPARE(code, 0);
    QVERIFY2(clock.elapsed() > 20000,
             "the script did not actually outlast the connect budget");
}

void TestSshLive::abortingARemoteScriptReturnsAtOnce()
{
    // abort() shuts the socket, which is the only thing that unblocks a read inside
    // libssh2 — there is no cancel flag to poll in there. What the far end makes of the
    // resulting hangup is the far end's business, which is why SPEC.md §4 says an abort
    // means loftail stopped waiting and never that the command stopped.
    SshSession session;
    QString error;
    QVERIFY2(session.connectTo(m_location, nullptr, 20000, &error), qPrintable(error));

    std::thread stopper([&session]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        session.abort();
    });

    QElapsedTimer clock;
    clock.start();
    int code = -1;
    session.runScript(restartScriptCommand(QStringLiteral("sleep 120\n"), {}), nullptr, &code,
                      &error);
    const qint64 elapsed = clock.elapsed();
    stopper.join();

    QVERIFY2(elapsed < 5000,
             qPrintable(QStringLiteral("abort took %1 ms").arg(elapsed)));
}

void TestSshLive::aRestartScriptRunsOnAnExecOnlyConnect()
{
    // THE ONLY EXECUTION of Need::ExecOnly. It is a connect that stops after the login —
    // no libssh2_sftp_init(), no shell probe — and the claim it has to support is that
    // runScript() still works on what comes back, because that is the whole of what
    // File ▸ Restart App does with the session.
    //
    // WHY THE PARAMETER EXISTS, which this cannot measure here and which is worth saying:
    // on a server that ACCEPTS the SFTP subsystem channel with no `sftp-server` behind it,
    // the init that has just been skipped waits out the whole connect budget — twenty
    // seconds — for a version packet that never comes, before the probe rescues it. This
    // case runs against a server that presumably answers SFTP promptly, so the saving it
    // demonstrates is only the channel open; the twenty seconds need the stripped-down
    // image the exec fallback exists for, which is the same thing
    // theExecFallbackWritesTheSameBytes() says about itself.
    const QString marker = m_remotePath + QStringLiteral(".execonly-marker");
    QVERIFY(remoteShell(QStringLiteral("rm -f %1").arg(marker)));

    SshSession session;
    QString error;
    QVERIFY2(session.connectTo(m_location, nullptr, 20000, &error, nullptr,
                               SshSession::Need::ExecOnly),
             qPrintable(error));
    QVERIFY(session.isConnected());
    // Exec, although this server was very likely never asked: the answer means "this
    // session talks over exec channels", not "this server refused SFTP" (SshSession.h).
    QCOMPARE(session.mode(), SshSession::Mode::Exec);
    // No handle was opened and none can be: an ExecOnly session settles no transport.
    QVERIFY(!session.hasFile());
    QVERIFY(!session.fstatTracksHandle());

    QByteArray out;
    int code = -1;
    const bool ran = session.runScript(
        restartScriptCommand(QStringLiteral("printf ran\ntouch '%1'\n").arg(marker), {}),
        [&out](const QByteArray &bytes, bool isStdErr) {
            if (!isStdErr)
                out.append(bytes);
        },
        &code, &error);
    QVERIFY2(ran, qPrintable(error));
    QCOMPARE(code, 0);
    QCOMPARE(out, QByteArray("ran"));
    QCOMPARE(remoteShellOutput(QStringLiteral("test -e %1 && printf yes").arg(marker)),
             QByteArray("yes"));

    // And the other half of the contract: every file operation refuses BY NAME rather
    // than answering. Each of these would otherwise fail silently — readAt() with the 0
    // that means end of file, readFileAt() by quietly reading the config with `cat` on a
    // server whose SFTP is perfectly good.
    QString why;
    QVERIFY(!session.openFile(&why));
    QVERIFY(!why.isEmpty());
    why.clear();
    QVERIFY(!session.readFileAt(m_remotePath, nullptr, nullptr, &why));
    QVERIFY(!why.isEmpty());
    why.clear();
    // A scratch path and not the log: if the guard were ever removed this case must fail
    // rather than silently overwrite the file every other case in this class reads.
    QVERIFY(!session.writeFileAt(m_remotePath + QStringLiteral(".execonly-write"),
                                 QByteArray("no"), &why));
    QVERIFY(!why.isEmpty());
    QCOMPARE(remoteShellOutput(QStringLiteral("test -e %1.execonly-write && printf yes")
                                   .arg(m_remotePath)),
             QByteArray());
    why.clear();
    char scratch[8] = {};
    QCOMPARE(session.readAt(0, scratch, sizeof(scratch), &why), qint64(-1));
    QVERIFY(!why.isEmpty());
    QVERIFY(!session.statPath().valid);
    QVERIFY(!session.statHandle().valid);
    // The log is untouched by all of that — a refusal writes nothing.
    QVERIFY(remoteShell(QStringLiteral("rm -f %1").arg(marker)));
}

void TestSshLive::aDroppedLinkIsNoticedRatherThanPolledForEver()
{
    // isConnected() used to test a POINTER, so once a connect had succeeded it answered
    // true for the life of the object — and SshFetcher::pollOnce()'s "the link dropped,
    // let go of it and reconnect" branch could never be taken. The tab reported the log
    // as unreadable once per session timeout for ever and did not recover when the host
    // came back (bugs.md 30).
    QVERIFY(writeRemote(QByteArray("one line\n")));

    SshSession session;
    QString error;
    QVERIFY2(session.connectTo(m_location, nullptr, 20000, &error), qPrintable(error));
    QVERIFY2(session.openFile(&error), qPrintable(error));
    QVERIFY(session.isConnected());
    QVERIFY(session.statPath().valid);

    // FIRST the half that must not regress: a path that is not there is the server
    // answering about a FILE, on a link that is perfectly healthy. Condemning the session
    // for it would tear a working connection down once a second on a log that has merely
    // not been created yet — the state M13 exists to wait in.
    bool existed = true;
    QVERIFY2(session.readFileAt(m_remotePath + QStringLiteral(".no-such-file"), nullptr,
                                &existed, &error),
             qPrintable(error));
    QVERIFY(!existed);
    QVERIFY2(session.isConnected(), "a missing file was taken for a dropped link");

    // Now take the link away. abort() shuts the socket and frees nothing, which is what
    // the far end going down looks like from in here — and it is the only way to stage
    // one without a machine to unplug.
    session.abort();

    // The next operation is what notices. statPath() is the one pollOnce() runs first,
    // and it is the call whose failure the fetcher answers by asking isConnected().
    QVERIFY(!session.statPath().valid);
    QVERIFY2(!session.isConnected(),
             "the session still reports itself connected after the link went away");

    // And it stays condemned. The latch IS clearable — a `stat` that answers un-says it,
    // which is what stops a flag set by a call that did not fail (a partial read that
    // timed out) firing much later on an unrelated stat failure — but clearing it needs
    // an answer out of the far end, and after this there are none. So asking again is the
    // whole of the safety argument for that clear, in one line.
    QVERIFY(!session.statPath().valid);
    QVERIFY2(!session.isConnected(),
             "a second failed stat un-said the latch instead of leaving it set");
}

// The whole of the cache, against a real server (SshSessionCache.h, §6.3).
//
// EVERYTHING BELOW IS PINNED WITHOUT A SERVER EXCEPT THE ONE THING THAT MATTERS: that an
// SshSession which has already done an errand is still good for the next one. The
// bookkeeping — the checkout, the single ownership, the deadline, the cap, the latch — is
// tst_sshsessioncache's, driven against a fake. Whether a real libssh2 session survives a
// whole-file SFTP write, then an exec channel with the timeout suspended and the session
// flipped to non-blocking and back, and then another SFTP read, cannot be faked at all: a
// fake is good for as many errands as you like by construction.
//
// The claim is stated as POINTER IDENTITY, which is the narrowest observation available —
// the second and third errands ran on the very object the first one made, so no connect,
// no key exchange, no host-key check and no authentication happened for either.
void TestSshLive::oneConnectionServesSeveralErrandsAndTheDrainLetsItGo()
{
    const QString cfg = m_remotePath + QStringLiteral(".reuse.properties");
    QVERIFY(remoteShell(QStringLiteral("rm -f %1").arg(cfg)));

    RemoteLocation configLocation = m_location;
    configLocation.path = cfg;
    const QString address = configLocation.toString();

    auto shared = std::make_shared<SshWorkerShared>();
    QCOMPARE(sshSessionCache().size(), 0);

    const QByteArray body = "log4cplus.rootLogger=INFO, STDOUT\n";

    SshSession *first = nullptr;
    QString error = withSshSession(
        address, nullptr, shared, SshSession::Need::LogTransport, SshErrandRepeat::Allowed,
        [&first, &body](SshSession &session, const QString &path) {
            first = &session;
            QString why;
            if (!session.writeFileAt(path, body, &why))
                return why;
            return QString();
        });
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(first != nullptr);
    // Handed back rather than destroyed, which is what the next errand will find.
    QCOMPARE(sshSessionCache().size(), 1);

    // THE ONE-WAY BORROW. A restart wants ExecOnly; what is sitting there is a Transport
    // session, which can open an exec channel as readily — and taking it is what makes
    // "save the config, then bounce the service" cost one connect rather than two.
    SshSession *second = nullptr;
    QByteArray  said;
    int         code = -1;
    error = withSshSession(
        address, nullptr, shared, SshSession::Need::ExecOnly, SshErrandRepeat::Never,
        [&second, &said, &code](SshSession &session, const QString &) {
            second = &session;
            QString why;
            if (!session.runScript(
                    QStringLiteral("printf bounced"),
                    [&said](const QByteArray &bytes, bool isStdErr) {
                        if (!isStdErr)
                            said.append(bytes);
                    },
                    &code, &why)) {
                return why;
            }
            return QString();
        });
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(second, first);
    QCOMPARE(code, 0);
    QCOMPARE(said, QByteArray("bounced"));

    // And back to SFTP on the same session, which is the half runScript() could have
    // broken: it suspends the session timeout and flips the session to non-blocking, so a
    // restore that did not happen would show up here and nowhere else.
    SshSession *third = nullptr;
    QByteArray  got;
    bool        existed = false;
    error = withSshSession(
        address, nullptr, shared, SshSession::Need::LogTransport, SshErrandRepeat::Allowed,
        [&third, &got, &existed](SshSession &session, const QString &path) {
            third = &session;
            QString why;
            if (!session.readFileAt(path, &got, &existed, &why))
                return why;
            return QString();
        });
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(third, first);
    QVERIFY(existed);
    QCOMPARE(got, body);
    // Filed back under what it IS, not under what the ExecOnly errand asked for — or this
    // third read would have missed and connected again.
    QCOMPARE(sshSessionCache().size(), 1);

    // The shutdown rule, which is the other half nothing without a server can execute: the
    // window's drain has to leave no live socket behind, because Qt's globals go with the
    // application object and a QTcpSocket torn down after that is the SEGV SshWorkerPool.h
    // records. This latches the cache shut for the rest of the process, which is why this
    // case is the last one declared.
    drainSshWorkers();
    QVERIFY(sshSessionCache().closed());
    QCOMPARE(sshSessionCache().size(), 0);

    QVERIFY(remoteShell(QStringLiteral("rm -f %1").arg(cfg)));
}

QTEST_GUILESS_MAIN(TestSshLive)
#include "tst_sshlive.moc"
