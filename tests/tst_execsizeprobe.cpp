#include <QtTest>

#include <QProcess>

#include "ExecSizeProbe.h"

using namespace loftail;

// The size ladder the exec transport settles on, and the read-path proof that settles it
// (ARCHITECTURE.md §6.3.1).
//
// UNGATED, and this is the only automated coverage the exec fallback's DECISION has ever
// had. The transport that uses it needs libssh2, a server with no working SFTP, and — for
// the rungs below `stat` — a server missing a utility that every developer machine has.
// None of those is reachable from CI. So the two seams take the place of the network: the
// command seam runs through a real /bin/sh against real files, and the tools the far end
// is supposed to have are simply declared.
class TestExecSizeProbe : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    static bool haveShell()
    {
        return QFileInfo::exists(QStringLiteral("/bin/sh"));
    }

    static QByteArray runSh(const QString &command)
    {
        QProcess sh;
        sh.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
        if (!sh.waitForFinished(10000))
            return {};
        return sh.readAllStandardOutput();
    }

    // The command seam, wired to a real shell.
    static ExecSizeProbe::RunCommand shellRunner()
    {
        return [](const QString &command, QByteArray *out) {
            *out = runSh(command);
            return true;
        };
    }

    // The read seam, wired to the real readCommand() through a real shell — so the proof
    // in settle() runs an actual `tail -c +N | head -c 1`.
    static ExecSizeProbe::ReadAt shellReader(const QString &path)
    {
        return [path](qint64 offset, qint64 length) -> qint64 {
            return runSh(readCommand(path, offset, length)).size();
        };
    }

    static ExecTools tools(bool stat, bool ls, bool wc)
    {
        ExecTools out;
        out.ok = true;
        out.hasStat = stat;
        out.hasLs = ls;
        out.hasWc = wc;
        return out;
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

    void statIsPreferredWhenItIsThere();
    void fallsToLsWhenThereIsNoStat();
    void fallsToWcWhenThereIsNeither();
    void nothingToMeasureWithSettlesOnNothing();
    void aRungThatOverReportsIsRejected();
    void aSizeThatWentStaleLowIsStillAccepted();
    void anEmptyFileSettlesWithoutReadingAnything();
    void aReadPathThatDeliversNothingIsRefused();
    void aDeadChannelIsNotAMissingFile();
    void wcWillNotTakeOnALargeFile();
    void aNewlineInThePathDisqualifiesLs();
};

void TestExecSizeProbe::statIsPreferredWhenItIsThere()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    const QString path = write(QStringLiteral("app.log"), QByteArray(1500, 'x'));
    QVERIFY(!path.isEmpty());

    ExecSizeProbe probe(path, tools(true, true, true), shellRunner(), shellReader(path));
    ExecAttrs settled;
    QCOMPARE(probe.settle(&settled), SizeSource::Stat);
    QVERIFY(settled.ok);
    QCOMPARE(settled.size, 1500);
    // The one rung that also answers "when", which is what the ordinary rotation check
    // in SshFetcher wants.
    QVERIFY(settled.mtime > 1700000000);
}

void TestExecSizeProbe::fallsToLsWhenThereIsNoStat()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    // The case this milestone exists for: an embedded image with a shell, `tail` and
    // `head`, and no `stat` at all.
    const QString path = write(QStringLiteral("no-stat.log"), QByteArray(2048, 'y'));
    QVERIFY(!path.isEmpty());

    ExecSizeProbe probe(path, tools(false, true, true), shellRunner(), shellReader(path));
    ExecAttrs settled;
    QCOMPARE(probe.settle(&settled), SizeSource::Ls);
    QCOMPARE(settled.size, 2048);
    QCOMPARE(settled.mtime, kUnknownMtime);
    QVERIFY(!probe.channelDied());

    // And the settled rung keeps answering, which is what every poll then does.
    QCOMPARE(probe.query(SizeSource::Ls).size, 2048);
}

void TestExecSizeProbe::fallsToWcWhenThereIsNeither()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    const QString path = write(QStringLiteral("wc-only.log"), QByteArray(333, 'z'));
    QVERIFY(!path.isEmpty());

    ExecSizeProbe probe(path, tools(false, false, true), shellRunner(), shellReader(path));
    ExecAttrs settled;
    QCOMPARE(probe.settle(&settled), SizeSource::Wc);
    QCOMPARE(settled.size, 333);
    QCOMPARE(settled.mtime, kUnknownMtime);
}

void TestExecSizeProbe::nothingToMeasureWithSettlesOnNothing()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    const QString path = write(QStringLiteral("unmeasurable.log"), QByteArray(10, 'q'));
    QVERIFY(!path.isEmpty());

    ExecSizeProbe probe(path, tools(false, false, false), shellRunner(), shellReader(path));
    QCOMPARE(probe.settle(), SizeSource::None);
    // Nothing was attempted, so the CHANNEL is not what is wrong. The caller has to tell
    // these apart: one is reconnected, the other is waited for.
    QVERIFY(!probe.channelDied());
}

void TestExecSizeProbe::aRungThatOverReportsIsRejected()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    const QString path = write(QStringLiteral("liar.log"), QByteArray(500, 'a'));
    QVERIFY(!path.isEmpty());

    // An `ls` whose output parses perfectly and names a size past the end of the file —
    // the shape of every accident this proof exists to catch, whatever caused it. The
    // read is real, so the file itself gets the last word.
    auto lyingRunner = [](const QString &command, QByteArray *out) {
        if (command.contains(QStringLiteral("ls -lnLd"))) {
            *out = QByteArrayLiteral("-rw-r--r-- 1 0 0 99999 Aug  6 10:00 /liar.log\n");
            return true;
        }
        *out = runSh(command);
        return true;
    };

    ExecSizeProbe probe(path, tools(false, true, true), lyingRunner, shellReader(path));
    ExecAttrs settled;
    QCOMPARE(probe.settle(&settled), SizeSource::Wc); // fell past the rung that lied
    QCOMPARE(settled.size, 500);
}

void TestExecSizeProbe::aSizeThatWentStaleLowIsStillAccepted()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    // A log is being written to while it is being measured, which is the ordinary case
    // and not an error. Between the size query and the proof, the file grows — so the
    // size is already stale by the time it is checked, and it must still be accepted:
    // under-reporting costs one poll of latency, and it is indistinguishable from growth
    // anyway. Only over-reading is worth refusing.
    const QString path = write(QStringLiteral("growing.log"), QByteArray(100, 'g'));
    QVERIFY(!path.isEmpty());

    auto growThenRead = [path](qint64 offset, qint64 length) -> qint64 {
        QFile f(path);
        if (f.open(QIODevice::Append)) {
            f.write(QByteArray(4096, 'h'));
            f.close();
        }
        return runSh(readCommand(path, offset, length)).size();
    };

    ExecSizeProbe probe(path, tools(false, true, false), shellRunner(), growThenRead);
    ExecAttrs settled;
    QCOMPARE(probe.settle(&settled), SizeSource::Ls);
    QCOMPARE(settled.size, 100);
}

void TestExecSizeProbe::anEmptyFileSettlesWithoutReadingAnything()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    // A log that exists and is empty is a perfectly ordinary thing to open, and there is
    // no byte in it to prove anything with. Reading anyway would reject every rung and
    // leave the tab waiting for a file that is right there.
    const QString path = write(QStringLiteral("empty.log"), QByteArray());
    QVERIFY(!path.isEmpty());

    bool read = false;
    auto neverRead = [&read](qint64, qint64) -> qint64 {
        read = true;
        return 0;
    };

    ExecSizeProbe probe(path, tools(false, true, false), shellRunner(), neverRead);
    ExecAttrs settled;
    QCOMPARE(probe.settle(&settled), SizeSource::Ls);
    QCOMPARE(settled.size, 0);
    QVERIFY2(!read, "an empty file must not be read to be believed");
}

void TestExecSizeProbe::aReadPathThatDeliversNothingIsRefused()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    // `head -c` is NOT POSIX — POSIX head has only -n — so a box can have `head` on its
    // PATH and still deliver nothing for the command the transport builds. Before this
    // proof existed, such a box opened the log and showed an empty view forever, because
    // zero bytes back is exactly what EOF looks like. Refusing to open says why instead.
    const QString path = write(QStringLiteral("no-head.log"), QByteArray(64, 'n'));
    QVERIFY(!path.isEmpty());

    auto deliversNothing = [](qint64, qint64) -> qint64 { return 0; };

    ExecSizeProbe probe(path, tools(true, true, true), shellRunner(), deliversNothing);
    QCOMPARE(probe.settle(), SizeSource::None);
    // Every rung ran and answered; it is the reading that is broken, not the link.
    QVERIFY(!probe.channelDied());
}

void TestExecSizeProbe::aDeadChannelIsNotAMissingFile()
{
    const QString path = m_dir.filePath(QStringLiteral("whatever.log"));
    auto noChannel = [](const QString &, QByteArray *) { return false; };
    auto neverRead = [](qint64, qint64) -> qint64 { return -1; };

    ExecSizeProbe probe(path, tools(true, true, true), noChannel, neverRead);
    QCOMPARE(probe.settle(), SizeSource::None);
    // The distinction the two-way command seam exists for. Collapsed into one answer,
    // a dropped connection would be waited out as a log that has not been written yet —
    // forever, because nothing would ever reconnect.
    QVERIFY(probe.channelDied());
}

void TestExecSizeProbe::wcWillNotTakeOnALargeFile()
{
    // Measuring by reading the whole file is exact and it is the last rung for a reason:
    // observing a log must not disturb the machine producing it (invariant #5). Past the
    // ceiling the rung declines rather than being allowed with a warning attached.
    const QString path = m_dir.filePath(QStringLiteral("huge.log"));
    constexpr qint64 huge = ExecSizeProbe::kWcSettleCeiling + 1;

    // No capture: `huge` is a constant expression, so naming it inside the lambda odr-uses
    // nothing — and clang treats capturing it anyway as an unused capture.
    auto bigRunner = [](const QString &command, QByteArray *out) {
        if (command.startsWith(QStringLiteral("wc -c <")))
            *out = QByteArray::number(huge) + "\n";
        return true; // every other rung runs and prints nothing: not available
    };
    bool read = false;
    auto neverRead = [&read](qint64, qint64) -> qint64 {
        read = true;
        return 1;
    };

    ExecSizeProbe probe(path, tools(false, false, true), bigRunner, neverRead);
    QCOMPARE(probe.settle(), SizeSource::None);
    QVERIFY2(!read, "the ceiling must be applied before the file is touched");
    QVERIFY(!probe.channelDied());

    // Just under it is fine.
    auto okRunner = [](const QString &command, QByteArray *out) {
        if (command.startsWith(QStringLiteral("wc -c <")))
            *out = QByteArray::number(ExecSizeProbe::kWcSettleCeiling) + "\n";
        return true;
    };
    auto oneByte = [](qint64, qint64) -> qint64 { return 1; };
    ExecSizeProbe under(path, tools(false, false, true), okRunner, oneByte);
    QCOMPARE(under.settle(), SizeSource::Wc);
}

void TestExecSizeProbe::aNewlineInThePathDisqualifiesLs()
{
    // `ls` prints the name back and the answer is taken from the last line, so a filename
    // containing a newline can carry a complete, plausible `ls` line of its own and name
    // whatever size it likes. The rung steps aside instead; the other two print no name.
    const QString path = QStringLiteral("/var/log/a\n-rw-r--r-- 1 0 0 4096 Aug 6 10:00 x");

    QStringList ran;
    auto recorder = [&ran](const QString &command, QByteArray *out) {
        ran.append(command);
        *out = QByteArrayLiteral("4096\n"); // whatever wc would have said
        return true;
    };
    auto oneByte = [](qint64, qint64) -> qint64 { return 1; };

    ExecSizeProbe probe(path, tools(false, true, true), recorder, oneByte);
    QCOMPARE(probe.settle(), SizeSource::Wc);
    for (const QString &command : ran)
        QVERIFY2(!command.contains(QStringLiteral("ls -lnLd")), "the ls rung was tried");
}

QTEST_GUILESS_MAIN(TestExecSizeProbe)
#include "tst_execsizeprobe.moc"
