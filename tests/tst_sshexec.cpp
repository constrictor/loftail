#include <QtTest>

#include <QProcess>

#include "SshExecCommands.h"

using namespace loftail;

// The exec transport's commands and parsing (ARCHITECTURE.md §6.3.1).
//
// UNGATED, and deliberately so. The quoting these tests cover is the security boundary
// of the whole exec transport — a remote path arrives from a URL somebody typed and ends
// up inside a command line a shell on another machine will interpret — and a boundary
// that is only compiled in one build configuration is one that is only tested in one.
//
// The injection cases do not merely compare strings: they run the generated command
// through a REAL /bin/sh against real temporary files, because the only claim worth
// making is that a hostile path stays inert when a shell actually parses it.
class TestSshExec : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    static bool haveShell()
    {
        return QFileInfo::exists(QStringLiteral("/bin/sh"));
    }

    // Run `command` through a real shell and return stdout.
    static QByteArray runSh(const QString &command, int *exitCode = nullptr)
    {
        QProcess sh;
        sh.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
        if (!sh.waitForFinished(10000))
            return {};
        if (exitCode)
            *exitCode = sh.exitCode();
        return sh.readAllStandardOutput();
    }

    QString write(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(bytes);
        f.close();
        return path;
    }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void quotingNeutralisesShellMetacharacters();
    void anInjectingPathCannotRunAnything();
    void statReportsSizeAndMtime();
    void readHonoursTheOneBasedOffset();
    void statOutputParsingRejectsRubbish();
};

void TestSshExec::quotingNeutralisesShellMetacharacters()
{
    QCOMPARE(shellQuote(QStringLiteral("/var/log/app.log")),
             QStringLiteral("'/var/log/app.log'"));
    // A quote is the one character single quotes cannot contain, and this is the only
    // correct POSIX spelling of it.
    QCOMPARE(shellQuote(QStringLiteral("a'b")), QStringLiteral("'a'\\''b'"));
    // Everything else is inert inside single quotes and must be passed through as-is
    // rather than escaped a second time.
    QCOMPARE(shellQuote(QStringLiteral("$HOME `id` \"x\" *")),
             QStringLiteral("'$HOME `id` \"x\" *'"));
}

void TestSshExec::anInjectingPathCannotRunAnything()
{
    if (!haveShell())
        QSKIP("no /bin/sh to prove the quoting against");

    // The canary: if the path escapes its quotes, this file gets created.
    const QString canary = m_dir.filePath(QStringLiteral("pwned"));
    QVERIFY(!QFileInfo::exists(canary));

    // A path that closes the quote and appends a command — the exact shape of the
    // attack the quoting exists to stop.
    const QString hostile =
        QStringLiteral("/tmp/x'; touch %1; echo '").arg(canary);

    int code = 0;
    runSh(statCommand(hostile), &code);
    QVERIFY2(!QFileInfo::exists(canary), "shell injection through statCommand");

    runSh(readCommand(hostile, 0, 16), &code);
    QVERIFY2(!QFileInfo::exists(canary), "shell injection through readCommand");

    // Substitution must not happen either: a path naming $HOME stays literal.
    const QByteArray out = runSh(
        QStringLiteral("echo %1").arg(shellQuote(QStringLiteral("$HOME/`id`"))));
    QCOMPARE(out.trimmed(), QByteArray("$HOME/`id`"));
}

void TestSshExec::statReportsSizeAndMtime()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    // A name with a space and a quote in it, because those are the paths that break
    // command construction, and they are legal filenames.
    const QString path = write(QStringLiteral("od d app.log"), QByteArray(1234, 'x'));
    QVERIFY(!path.isEmpty());

    const ExecAttrs attrs = parseStatOutput(runSh(statCommand(path)));
    QVERIFY2(attrs.ok, "stat output did not parse — is this a GNU or BSD stat?");
    QCOMPARE(attrs.size, 1234);
    // Written just now; a wildly wrong epoch means the wrong field was parsed.
    QVERIFY(attrs.mtime > 1700000000);
}

void TestSshExec::readHonoursTheOneBasedOffset()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    const QString path = write(QStringLiteral("read.log"), QByteArrayLiteral("0123456789"));
    QVERIFY(!path.isEmpty());

    // Offset 0 is the start of the file, NOT one byte in: `tail -c +N` counts from one,
    // and this is the off-by-one readCommand() exists to hide.
    QCOMPARE(runSh(readCommand(path, 0, 4)), QByteArray("0123"));
    QCOMPARE(runSh(readCommand(path, 4, 3)), QByteArray("456"));
    // Asking past the end yields nothing rather than an error.
    QCOMPARE(runSh(readCommand(path, 10, 5)), QByteArray());
    // A length beyond the end is clamped by the file itself.
    QCOMPARE(runSh(readCommand(path, 8, 99)), QByteArray("89"));
}

void TestSshExec::statOutputParsingRejectsRubbish()
{
    // Everything a broken server can print has to read as "no attributes" rather than
    // as a plausible size — a wrong size here would truncate or over-read a log.
    QVERIFY(!parseStatOutput(QByteArray()).ok);
    QVERIFY(!parseStatOutput("\n\n").ok);
    QVERIFY(!parseStatOutput("stat: cannot stat '/x': No such file or directory\n").ok);
    QVERIFY(!parseStatOutput("1234\n").ok);            // one field
    QVERIFY(!parseStatOutput("1234 5678 9\n").ok);     // three
    QVERIFY(!parseStatOutput("abc def\n").ok);         // not numbers
    QVERIFY(!parseStatOutput("-1 1700000000\n").ok);   // negative size

    const ExecAttrs good = parseStatOutput("  4096   1700000000  \n");
    QVERIFY(good.ok);
    QCOMPARE(good.size, 4096);
    QCOMPARE(good.mtime, 1700000000);

    // A login banner on stdout must not defeat the parse: the answer is the last line.
    const ExecAttrs banner = parseStatOutput("Welcome to prod-web!\nLast login: today\n77 12345\n");
    QVERIFY(banner.ok);
    QCOMPARE(banner.size, 77);
}

QTEST_GUILESS_MAIN(TestSshExec)
#include "tst_sshexec.moc"
