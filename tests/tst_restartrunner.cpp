#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "RestartRunner.h"
#include "RestartTarget.h"

using namespace loftail;

// Running a restart script LOCALLY (SPEC.md §4, ARCHITECTURE.md §6.9).
//
// No ssh server, no network, no libssh2 — a real shell and real scripts. The remote half
// of RestartRunner cannot be driven from here at all and says so: tst_sshexec covers the
// command it builds, and tst_sshlive covers the transport, by hand, against a real host.
//
// POSIX-only, guarded rather than ifdef'd out, for the reason tst_tail is: what is being
// asserted needs a shell that takes `-c` and a `sleep` that exists. The Windows branch of
// startLocal() is a different mechanism (a temp .cmd file) and is unexercised here, which
// is stated rather than papered over.
class TestRestartRunner : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void aScriptRunsAndReportsExitZero();
    void stdoutAndStderrArriveSeparately();
    void aNonZeroExitIsReported();
    void theVariablesReachTheScriptAsEnvironment();
    void aPathWithSpacesQuotesAndDollarsArrivesIntact();
    void anAbsentVariableIsUnsetRatherThanEmpty();
    void aScriptRunsAsAWholeAndNotLineByLine();
    void aBackgroundedChildHoldingStdoutDoesNotLookHung();
    void closingTheRunDoesNotKillWhatTheScriptStarted();
    void aScriptThatReadsStandardInputDoesNotHang();
    void abortStopsAScriptThatWouldRunForever();
    void destroyingTheRunnerMidRunDoesNotBlock();

private:
    QTemporaryDir m_dir;

    static bool haveShell() { return QFileInfo::exists(QStringLiteral("/bin/sh")); }

    // A target that runs `script` here, with `vars` in its environment.
    static RestartTarget local(const QString &script,
                               const QList<QPair<QString, QString>> &vars = {})
    {
        RestartTarget t;
        t.state = RestartTarget::State::Resolved;
        t.remote = false;
        t.script = script;
        t.variables = vars;
        return t;
    }

    // Run to completion, collecting both streams. Returns false on timeout.
    static bool run(const RestartTarget &target, RestartResult *result, QByteArray *out,
                    QByteArray *err, int timeoutMs = 10000)
    {
        RestartRunner runner;
        bool done = false;
        QObject::connect(&runner, &RestartRunner::outputAppended,
                         [out, err](const QByteArray &bytes, bool isStdErr) {
                             if (isStdErr) {
                                 if (err)
                                     err->append(bytes);
                             } else if (out) {
                                 out->append(bytes);
                             }
                         });
        QObject::connect(&runner, &RestartRunner::finished,
                         [result, &done](const RestartResult &r) {
                             if (result)
                                 *result = r;
                             done = true;
                         });
        runner.start(target);

        QElapsedTimer clock;
        clock.start();
        while (!done && clock.elapsed() < timeoutMs)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return done;
    }
};

void TestRestartRunner::aScriptRunsAndReportsExitZero()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    RestartResult r;
    QByteArray out;
    QVERIFY(run(local(QStringLiteral("printf hello\n")), &r, &out, nullptr));
    QVERIFY(r.ok);
    QCOMPARE(r.exitCode, 0);
    QVERIFY(!r.sawStdErr);
    QVERIFY(r.succeeded());
    QCOMPARE(out, QByteArray("hello"));
}

void TestRestartRunner::stdoutAndStderrArriveSeparately()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    // WHICH STREAM SAID IT is half of what this feature reports — it is the difference
    // between a dialog that closes itself and one that stays up — so the two may never be
    // merged, however much simpler one channel would be.
    RestartResult r;
    QByteArray out, err;
    QVERIFY(run(local(QStringLiteral("printf out\nprintf err >&2\n")), &r, &out, &err));
    QCOMPARE(out, QByteArray("out"));
    QCOMPARE(err, QByteArray("err"));
    QVERIFY(r.sawStdErr);

    // Exit 0 and a byte on stderr is NOT a success: a script that tidies up after a
    // failure still exits 0, and a warning nobody read is what an auto-close would hide.
    QCOMPARE(r.exitCode, 0);
    QVERIFY(!r.succeeded());
}

void TestRestartRunner::aNonZeroExitIsReported()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    RestartResult r;
    QVERIFY(run(local(QStringLiteral("exit 3\n")), &r, nullptr, nullptr));
    QVERIFY(r.ok); // it RAN; "the shell said no" is not "there was no shell"
    QCOMPARE(r.exitCode, 3);
    QVERIFY(!r.succeeded());
}

void TestRestartRunner::theVariablesReachTheScriptAsEnvironment()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    RestartResult r;
    QByteArray out;
    const QList<QPair<QString, QString>> vars = {
        {QStringLiteral("LOGFILE"), QStringLiteral("/srv/b.zip")},
        {QStringLiteral("ARCHIVE"), QStringLiteral("/srv/b.zip")},
        {QStringLiteral("MEMBER"), QStringLiteral("var/log/app.log")},
    };
    QVERIFY(run(local(QStringLiteral("printf '%s|%s|%s' \"$LOGFILE\" \"$ARCHIVE\" \"$MEMBER\"\n"),
                      vars),
                &r, &out, nullptr));
    QCOMPARE(out, QByteArray("/srv/b.zip|/srv/b.zip|var/log/app.log"));
}

void TestRestartRunner::aPathWithSpacesQuotesAndDollarsArrivesIntact()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    // THE WHOLE PAYOFF OF THE ENVIRONMENT DECISION. QProcess hands argv straight to the
    // platform, so on this path no shell parses the value at all — the quoting question
    // the remote side answers with shellQuote() is removed here rather than answered.
    const QString nasty = QStringLiteral("/var/log/my app's $HOME `id` \"q\";rm -rf x.log");
    RestartResult r;
    QByteArray out;
    QVERIFY(run(local(QStringLiteral("printf '%s' \"$LOGFILE\"\n"),
                      {{QStringLiteral("LOGFILE"), nasty}}),
                &r, &out, nullptr));
    QCOMPARE(QString::fromUtf8(out), nasty);
}

void TestRestartRunner::anAbsentVariableIsUnsetRatherThanEmpty()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    // The local half of the promise tst_restarttarget and tst_sshexec make about the
    // other two. All three have to agree, or a script written against one breaks on the
    // others with nothing to see.
    RestartResult r;
    QByteArray out;
    QVERIFY(run(local(QStringLiteral("printf '%s' \"${ARCHIVE-unset}${MEMBER-unset}\"\n"),
                      {{QStringLiteral("LOGFILE"), QStringLiteral("/var/log/app.log")}}),
                &r, &out, nullptr));
    QCOMPARE(out, QByteArray("unsetunset"));
}

void TestRestartRunner::aScriptRunsAsAWholeAndNotLineByLine()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    // Running a script a line at a time would break every `if`, every loop and every
    // variable set on one line and read on the next — which is most restart scripts.
    RestartResult r;
    QByteArray out;
    const QString script = QStringLiteral(
        "n=''\n"
        "for x in a b c; do n=\"${n}${x}\"; done\n"
        "if [ \"$n\" = abc ]; then\n"
        "  printf whole\n"
        "fi\n");
    QVERIFY(run(local(script), &r, &out, nullptr));
    QCOMPARE(out, QByteArray("whole"));
    QCOMPARE(r.exitCode, 0);
}

void TestRestartRunner::aBackgroundedChildHoldingStdoutDoesNotLookHung()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    // Completion is the process DYING and nothing else — never a stream reaching EOF.
    // A restart script very often ends by leaving something running, and such a grandchild
    // inherits the standard streams it was started with, so a stream stays open long after
    // the shell has gone. Anything that waited for one would wait for the daemon.
    //
    // The other, worse half of that hazard is what the next case covers.
    RestartResult r;
    QByteArray out;
    QElapsedTimer clock;
    clock.start();
    QVERIFY(run(local(QStringLiteral("sleep 20 &\nprintf done\n")), &r, &out, nullptr, 8000));
    QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(out, QByteArray("done"));
    QVERIFY(r.succeeded());
}

void TestRestartRunner::closingTheRunDoesNotKillWhatTheScriptStarted()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    // THE CASE THE FILE-BACKED OUTPUT EXISTS FOR, and the one no other test here can make.
    //
    // A grandchild left running by the script inherits the standard streams. With PIPES,
    // closing loftail's read end — which happens the moment the dialog goes — gives that
    // grandchild SIGPIPE on its next write, and its default action is to kill it. loftail
    // would then kill the service it had just been asked to restart, a minute later, with
    // nothing on screen connecting the two.
    //
    // A file has no reader to disappear, so the write simply succeeds. That is the whole
    // claim, and it is measured by whether the marker below was created after the run had
    // been closed and its output destination torn down.
    const QString marker = m_dir.filePath(QStringLiteral("survivor"));
    QFile::remove(marker);

    const QString script = QStringLiteral(
        "( sleep 1; printf 'still here\n'; touch %1 ) &\n"
        "printf started\n").arg(marker);

    {
        RestartResult r;
        QByteArray out;
        QVERIFY(run(local(script), &r, &out, nullptr, 8000));
        QCOMPARE(out, QByteArray("started"));
    }
    // The runner is gone: with pipes, the read end has just closed.

    QElapsedTimer clock;
    clock.start();
    while (!QFileInfo::exists(marker) && clock.elapsed() < 6000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    QVERIFY2(QFileInfo::exists(marker),
             "the process the script left running was killed by loftail closing the run");
}

void TestRestartRunner::aScriptThatReadsStandardInputDoesNotHang()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    // Standard input is the null device, so a `read` returns at once rather than waiting
    // on a terminal that is not there — which from outside is indistinguishable from a
    // slow restart.
    RestartResult r;
    QByteArray out;
    QElapsedTimer clock;
    clock.start();
    QVERIFY(run(local(QStringLiteral("read line\nprintf after\n")), &r, &out, nullptr, 8000));
    QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
    QCOMPARE(out, QByteArray("after"));
}

void TestRestartRunner::abortStopsAScriptThatWouldRunForever()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    RestartRunner runner;
    RestartResult r;
    bool done = false;
    QObject::connect(&runner, &RestartRunner::finished, [&](const RestartResult &got) {
        r = got;
        done = true;
    });
    runner.start(local(QStringLiteral("sleep 60\n")));

    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QVERIFY(!done);

    runner.abort();
    clock.restart();
    while (!done && clock.elapsed() < 8000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    QVERIFY(done);
    QVERIFY(r.aborted);
    QVERIFY(!r.succeeded());
}

void TestRestartRunner::destroyingTheRunnerMidRunDoesNotBlock()
{
    if (!haveShell())
        QSKIP("no shell to run a script in");

    // ABANDON, NEVER JOIN, and here the trap is local rather than remote: ~QProcess on a
    // live child warns, kills and BLOCKS in waitForFinished(). That is a stall on the
    // application thread — the thread the dialog is being closed on — so a running child
    // is disowned to a reaper instead. Three rounds, because a bound that passes once by
    // luck is not a bound.
    for (int round = 0; round < 3; ++round) {
        QElapsedTimer clock;
        clock.start();
        {
            RestartRunner runner;
            runner.start(local(QStringLiteral("sleep 30\n")));
            // Let it actually get started, or the destructor has nothing to disown.
            QElapsedTimer spin;
            spin.start();
            while (spin.elapsed() < 200)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            clock.restart();
        }
        QVERIFY2(clock.elapsed() < 500, qPrintable(QString::number(clock.elapsed())));
    }

    // Let the reaper's kill timer fire, so nothing is left running past the test.
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 2500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

QTEST_GUILESS_MAIN(TestRestartRunner)
#include "tst_restartrunner.moc"
