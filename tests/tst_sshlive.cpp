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
#include "SourceSpool.h"
#include "SshPrompter.h"
#include "SshSession.h"

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
    void aConfigFileIsReadAndWrittenWholeOverSftp();
    void writingAConfigKeepsItsPermissions();
    void theExecFallbackWritesTheSameBytes();
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

QTEST_GUILESS_MAIN(TestSshLive)
#include "tst_sshlive.moc"
