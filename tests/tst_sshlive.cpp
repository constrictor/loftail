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
#include "SourceSpool.h"
#include "SshPrompter.h"

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
