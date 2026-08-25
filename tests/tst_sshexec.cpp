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
    void readCommandSurvivesAPercentInThePath();
    void statOutputParsingRejectsRubbish();
    void lsReportsTheSizeOfARegularFile();
    void lsFollowsASymlinkToTheLog();
    void lsRejectsWhatIsNotAPlainFile();
    void lsSizeParsingRejectsRubbish();
    void wcReportsTheExactSize();
    void wcSizeParsingRejectsRubbish();
    void theProbeRequiresHeadAsWellAsTail();
    void aConfigFileIsReadWholeAndWrittenWhole();
    void writingInPlaceKeepsTheFilesPermissions();
    void existenceTellsAnEmptyFileFromAMissingOne();
    void aHostileConfigPathCannotRunAnything();
    void theProbeReportsWhichToolsExist();

    // M23 — the restart script. The one command loftail builds whose payload is CODE
    // rather than data, so the whole of what these pin is the asymmetry: the values are
    // quoted and the script is not.
    void aRestartScriptRunsAsAWholeWithItsVariablesExported();
    void aVariableThatDoesNotApplyIsNotExportedAtAll();
    void aHostileLogPathCannotRunAnythingInARestartScript();
    void aRestartScriptKeepsItsOwnExitStatus();
    void aRestartScriptThatReadsStandardInputDoesNotHang();
    void aScriptStoredWithWindowsLineEndingsStillRuns();

    // The rotation ladder: what one poll's stat justifies doing about it. Ungated and
    // here for the same reason the quoting and the size ladder are — it is the only
    // judgement the remote transport makes on its own, and a rule compiled in one
    // configuration is a rule tested in one configuration.
    void aShrinkBelowWhatWeReadIsCertain();
    void aHandleNameDisagreementIsCertain();
    void aPlainAppendCostsNothing();
    void growthIsAlwaysWorthAContentCheck();
    void aChangeWithoutGrowthIsCheckedAtOnce();
    void aServerWithNoMtimeAlwaysPaces();
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

    runSh(lsSizeCommand(hostile), &code);
    QVERIFY2(!QFileInfo::exists(canary), "shell injection through lsSizeCommand");

    runSh(wcSizeCommand(hostile), &code);
    QVERIFY2(!QFileInfo::exists(canary), "shell injection through wcSizeCommand");

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

void TestSshExec::readCommandSurvivesAPercentInThePath()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    // A percent is a legal filename character, and a URL-decoded %251 arrives here as a
    // literal %1. Chaining arg() made the LENGTH argument substitute itself into the
    // path — a different file, or none — because the second call rescans the whole
    // string including what the first one inserted.
    const QString path = write(QStringLiteral("%1-%2-%3.log"),
                               QByteArrayLiteral("0123456789"));
    QVERIFY(!path.isEmpty());

    QCOMPARE(runSh(readCommand(path, 0, 4)), QByteArray("0123"));
    QCOMPARE(runSh(readCommand(path, 4, 3)), QByteArray("456"));
    // The path must appear in the command exactly as given, markers and all.
    QVERIFY(readCommand(path, 0, 4).contains(QStringLiteral("%1-%2-%3.log")));
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

void TestSshExec::lsReportsTheSizeOfARegularFile()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    // The same awkward name statReportsSizeAndMtime() uses: a space and a quote are
    // legal, and they are what breaks command construction.
    const QString path = write(QStringLiteral("od d app.log"), QByteArray(4321, 'x'));
    QVERIFY(!path.isEmpty());

    const ExecAttrs attrs = parseLsSizeOutput(runSh(lsSizeCommand(path)));
    QVERIFY2(attrs.ok, "ls -lnLd output did not parse — what does this ls print?");
    QCOMPARE(attrs.size, 4321);
    // `ls` prints a human date, and an old file loses the clock from it entirely, so
    // there is nothing here an mtime comparison could use.
    QCOMPARE(attrs.mtime, kUnknownMtime);
}

void TestSshExec::lsFollowsASymlinkToTheLog()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    const QString target = write(QStringLiteral("real.log"), QByteArray(999, 'y'));
    QVERIFY(!target.isEmpty());
    const QString link = m_dir.filePath(QStringLiteral("current.log"));
    if (!QFile::link(target, link))
        QSKIP("this filesystem will not make a symlink");

    // -L must beat -d. POSIX says it does; this pins it, because without -L the answer
    // would be the LENGTH OF THE TARGET PATH — a plausible small number, and wrong.
    const ExecAttrs attrs = parseLsSizeOutput(runSh(lsSizeCommand(link)));
    QVERIFY2(attrs.ok, "a symlinked log was not dereferenced");
    QCOMPARE(attrs.size, 999);
}

void TestSshExec::lsRejectsWhatIsNotAPlainFile()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    // A directory: -d stops it listing the contents, and the type char rejects it. Both
    // are needed — without -d, lastNonEmptyLine() would take a member for the answer.
    QVERIFY(!parseLsSizeOutput(runSh(lsSizeCommand(m_dir.path()))).ok);

    // A character device, whose size column is not a byte count but "1, 3". Rejected on
    // the type char, before the shifted column can be read as a size.
    if (QFileInfo::exists(QStringLiteral("/dev/null")))
        QVERIFY(!parseLsSizeOutput(runSh(lsSizeCommand(QStringLiteral("/dev/null")))).ok);

    // A dangling symlink: -L falls back to lstat and prints an `l` line, which is right
    // — there are no bytes there to read.
    const QString dangling = m_dir.filePath(QStringLiteral("gone.log"));
    if (QFile::link(m_dir.filePath(QStringLiteral("never-written.log")), dangling))
        QVERIFY(!parseLsSizeOutput(runSh(lsSizeCommand(dangling))).ok);
}

void TestSshExec::lsSizeParsingRejectsRubbish()
{
    // A wrong size here would truncate or over-read a log, so everything a strange
    // server can print has to read as "no attributes" rather than as a plausible number.
    QVERIFY(!parseLsSizeOutput(QByteArray()).ok);
    QVERIFY(!parseLsSizeOutput("Welcome to the router\n").ok);
    QVERIFY(!parseLsSizeOutput("ls: /x: No such file or directory\n").ok);
    // A human-readable size, which is what a remote /etc/profile setting BLOCK_SIZE
    // produces. The command defends against it; the parser must refuse it regardless.
    QVERIFY(!parseLsSizeOutput("-rw-r--r-- 1 0 0 1.0K Aug  6 10:00 /x.log\n").ok);
    QVERIFY(!parseLsSizeOutput("drwxr-xr-x 2 0 0 4096 Aug  6 10:00 /var/log\n").ok);
    QVERIFY(!parseLsSizeOutput("lrwxrwxrwx 1 0 0 11 Aug  6 10:00 /x -> /y\n").ok);
    QVERIFY(!parseLsSizeOutput("crw-rw-rw- 1 0 0 1, 3 Jan  1 00:00 /dev/null\n").ok);
    // Short of a full line: no time and no name, so the columns are not what they look
    // like and field 4 is not necessarily a size.
    QVERIFY(!parseLsSizeOutput("-rw-r--r-- 1 0 0 4096 Aug  6\n").ok);
    QVERIFY(!parseLsSizeOutput("-rw-r--r-- 1 0 0 -5 Aug  6 10:00 /x.log\n").ok);
    // Not numeric where -n guarantees numbers: the columns are not where they should be.
    QVERIFY(!parseLsSizeOutput("-rw-r--r-- 1 root root 4096 Aug  6 10:00 /x.log\n").ok);

    // Busybox pads its columns differently from GNU; simplified() is what makes the two
    // the same parse. And an ACL or SELinux marker after the mode must not matter.
    const ExecAttrs busybox =
        parseLsSizeOutput("-rw-r--r--    1 0        0             8192 Aug  6 10:00 /x\n");
    QVERIFY(busybox.ok);
    QCOMPARE(busybox.size, 8192);
    const ExecAttrs acl = parseLsSizeOutput("-rw-rw-r--+ 1 0 0 77 Aug  6 10:00 /x.log\n");
    QVERIFY(acl.ok);
    QCOMPARE(acl.size, 77);

    // And a banner in front of the answer, the way a login shell delivers it.
    const ExecAttrs banner =
        parseLsSizeOutput("MOTD\n-rw-r--r-- 1 0 0 12 Aug  6 10:00 /x.log\n");
    QVERIFY(banner.ok);
    QCOMPARE(banner.size, 12);
}

void TestSshExec::wcReportsTheExactSize()
{
    if (!haveShell())
        QSKIP("no /bin/sh");
    const QString path = write(QStringLiteral("od d app.log"), QByteArray(2222, 'z'));
    QVERIFY(!path.isEmpty());

    const ExecAttrs attrs = parseWcSizeOutput(runSh(wcSizeCommand(path)));
    QVERIFY(attrs.ok);
    QCOMPARE(attrs.size, 2222);
    QCOMPARE(attrs.mtime, kUnknownMtime);

    // A missing file prints nothing on stdout — the shell's complaint goes to stderr —
    // which is the whole reason this rung uses a redirect rather than an operand.
    const QString missing = m_dir.filePath(QStringLiteral("not-there.log"));
    QVERIFY(!parseWcSizeOutput(runSh(wcSizeCommand(missing))).ok);
}

void TestSshExec::wcSizeParsingRejectsRubbish()
{
    QVERIFY(!parseWcSizeOutput(QByteArray()).ok);
    QVERIFY(!parseWcSizeOutput("\n \n").ok);
    // Two fields means `wc` was given an operand rather than a redirect, so this is not
    // the command we built and its first field is not necessarily what we think.
    QVERIFY(!parseWcSizeOutput("1234 /var/log/app.log\n").ok);
    QVERIFY(!parseWcSizeOutput("wc: /x: No such file\n").ok);
    QVERIFY(!parseWcSizeOutput("-3\n").ok);

    const ExecAttrs good = parseWcSizeOutput("   4096  \n");
    QVERIFY(good.ok);
    QCOMPARE(good.size, 4096);
}

void TestSshExec::theProbeRequiresHeadAsWellAsTail()
{
    // readCommand() is `tail -c +N ... | head -c L`, and for a long time the probe asked
    // only about `tail`. A box with tail and no head passed, opened the log, and then
    // returned zero bytes from every read — which is exactly what EOF looks like, so it
    // opened empty and never said why.
    const QString probe = probeCommand();
    QVERIFY(probe.contains(QStringLiteral("command -v tail")));
    QVERIFY(probe.contains(QStringLiteral("command -v head")));
}

void TestSshExec::theProbeReportsWhichToolsExist()
{
    // The probe decides whether the exec transport is available AT ALL, and it is only
    // ever reached on servers with no working SFTP — which in practice are the small,
    // heavily-customised ones whose shells greet you. Comparing the whole of stdout
    // against the marker would have failed on every one of them.
    QCOMPARE(lastNonEmptyLine(probeMarker() + "\n"), probeMarker());
    QCOMPARE(lastNonEmptyLine("Welcome to the router\n\n" + probeMarker() + "\n"),
             probeMarker());
    QCOMPARE(lastNonEmptyLine("  " + probeMarker() + "  "), probeMarker());
    // No trailing newline at all, which is what a shell that does not add one gives.
    QCOMPARE(lastNonEmptyLine("banner\n" + probeMarker()), probeMarker());

    // And it must still be able to say no: a shell that answered without running the
    // utilities prints its own last line, not ours.
    QVERIFY(lastNonEmptyLine(QByteArray()).isEmpty());
    QVERIFY(lastNonEmptyLine("\n \n\t\n").isEmpty());
    QVERIFY(lastNonEmptyLine("Welcome to the router\n") != probeMarker());

    // Reading and measuring are separate questions. The marker alone answers the first
    // and says no to the second, which is a server that gets its own message.
    const ExecTools readOnly = parseProbeOutput(probeMarker() + "\n");
    QVERIFY(readOnly.ok);
    QVERIFY(!readOnly.anySizeTool());

    const ExecTools noStat = parseProbeOutput("MOTD\n" + probeMarker() + " ls wc\n");
    QVERIFY(noStat.ok);
    QVERIFY(!noStat.hasStat);
    QVERIFY(noStat.hasLs);
    QVERIFY(noStat.hasWc);

    // An unknown name is ignored rather than fatal, so an older loftail and a newer
    // probe (or the reverse) do not refuse each other.
    const ExecTools future = parseProbeOutput(probeMarker() + " stat perl\n");
    QVERIFY(future.ok);
    QVERIFY(future.hasStat);

    QVERIFY(!parseProbeOutput(QByteArray()).ok);
    QVERIFY(!parseProbeOutput("Welcome to the router\n").ok);
    // The marker is the HEAD of the line, not somewhere in it: a shell echoing back the
    // command it was given must not read as an answer.
    QVERIFY(!parseProbeOutput("echo " + probeMarker() + "\n").ok);

    if (!haveShell())
        QSKIP("no /bin/sh to run the probe through");
    // The real command, through a real shell. `ls` is asserted and `stat` deliberately
    // is not: a box without stat is the case this whole ladder exists for.
    const ExecTools real = parseProbeOutput(runSh(probeCommand()));
    QVERIFY2(real.ok, "the probe did not find tail and head on this machine");
    QVERIFY(real.hasLs);
    QVERIFY(real.anySizeTool());

    // And with a banner in front of it, the way a login shell would deliver it.
    const ExecTools chatty = parseProbeOutput(
        runSh(QStringLiteral("echo 'MOTD: be careful'; %1").arg(probeCommand())));
    QVERIFY(chatty.ok);
    QVERIFY(chatty.hasLs);

    // With nothing on PATH it must still be able to say no rather than half-answer.
    QVERIFY(!parseProbeOutput(runSh(QStringLiteral("PATH=/nonexistent; %1")
                                        .arg(probeCommand())))
                 .ok);
}

// --- the rotation ladder ----------------------------------------------------

// A remote log's poll, with the fields a case does not care about left at their defaults.
// `consumed` defaults to the smaller of the two sizes, so the shrink test never fires by
// accident in a case that is about something else.
static RemoteObservation obs(qint64 size, qint64 lastSize, qint64 mtime, qint64 lastMtime)
{
    RemoteObservation o;
    o.size = size;
    o.lastSize = lastSize;
    o.mtime = mtime;
    o.lastMtime = lastMtime;
    o.consumed = qMin(size, lastSize);
    return o;
}

void TestSshExec::aShrinkBelowWhatWeReadIsCertain()
{
    RemoteObservation o = obs(500, 1000, 200, 100);
    o.consumed = 1000; // we had already read 1000 bytes; there are now 500
    QCOMPARE(rotationVerdict(o), RotationVerdict::Rotated);

    // And it outranks everything else: a server whose FSTAT agrees perfectly still
    // rotated if the file is now shorter than what we have handed out.
    o.fstatTracksHandle = true;
    o.handleValid = true;
    o.handleSize = 500;
    QCOMPARE(rotationVerdict(o), RotationVerdict::Rotated);
}

void TestSshExec::aHandleNameDisagreementIsCertain()
{
    // The inode substitute: the handle still names the file we opened while stat
    // re-resolves the name. This is what catches the SAME-SIZE rotate that a size check
    // cannot see at all.
    RemoteObservation o = obs(1000, 1000, 200, 100);
    o.fstatTracksHandle = true;
    o.handleValid = true;
    o.handleSize = 4096;
    QCOMPARE(rotationVerdict(o), RotationVerdict::Rotated);
}

void TestSshExec::aPlainAppendCostsNothing()
{
    // Nothing moved at all: an idle log on a server with a working stat. The commonest
    // poll there is, and it must not spend a byte.
    RemoteObservation o = obs(1000, 1000, 100, 100);
    o.fstatTracksHandle = true;
    o.handleValid = true;
    o.handleSize = 1000;
    QCOMPARE(rotationVerdict(o), RotationVerdict::Nothing);

    // Same, on a server whose handle cannot be trusted.
    QCOMPARE(rotationVerdict(obs(1000, 1000, 100, 100)), RotationVerdict::Nothing);
}

void TestSshExec::growthIsAlwaysWorthAContentCheck()
{
    // THE case this ladder was rebuilt for. `cp bigger.log app.log` on the far end moves
    // neither the inode nor the size below what we read, and on the SFTP rung the handle
    // and the name still agree — because they are still the same file. Only the content
    // moved. Read growth as a plain append and the pre-rewrite records stay on screen for
    // ever, with the new bytes parsed from the middle of a record.
    //
    // Paced, not immediate: an ordinary append looks exactly the same from here, and a
    // log being actively written must not pay a network read on every poll (invariant #5).
    RemoteObservation sftp = obs(2000, 1000, 200, 100);
    sftp.fstatTracksHandle = true;
    sftp.handleValid = true;
    sftp.handleSize = 2000; // agrees — no rename happened
    QCOMPARE(rotationVerdict(sftp), RotationVerdict::ComparePaced);

    // And on the weaker rung, where there is no handle to ask.
    QCOMPARE(rotationVerdict(obs(2000, 1000, 200, 100)), RotationVerdict::ComparePaced);
}

void TestSshExec::aChangeWithoutGrowthIsCheckedAtOnce()
{
    // The mtime moved and the size did not. No append produces that, so it is worth a
    // read the moment it is seen rather than waiting out the pacing window.
    QCOMPARE(rotationVerdict(obs(1000, 1000, 200, 100)), RotationVerdict::CompareNow);
}

void TestSshExec::aServerWithNoMtimeAlwaysPaces()
{
    // A box with no `stat`: the ls and wc rungs report a size and nothing else, so
    // "did it change without growing" cannot be asked at all.
    //
    // THE ORDER OF THE TESTS IS WHAT THIS PINS. kUnknownMtime is -1 and -1 > -1 is false,
    // so a ladder that compared mtimes before checking for their absence would answer
    // Nothing here for ever — switching rotation detection off on exactly the servers
    // that have the least of it to begin with.
    QCOMPARE(rotationVerdict(obs(1000, 1000, kUnknownMtime, kUnknownMtime)),
             RotationVerdict::ComparePaced);
    QCOMPARE(rotationVerdict(obs(2000, 1000, kUnknownMtime, kUnknownMtime)),
             RotationVerdict::ComparePaced);
}

void TestSshExec::aConfigFileIsReadWholeAndWrittenWhole()
{
    if (!haveShell())
        QSKIP("no /bin/sh to prove the commands against");

    const QByteArray body = "log4cplus.rootLogger=DEBUG, STDOUT\n# a comment\n";
    const QString path = write(QStringLiteral("cfg.properties"), body);
    QCOMPARE(runSh(configReadCommand(path)), body);

    // The write takes stdin, so it is driven the way the transport drives it: bytes in,
    // nothing out.
    const QByteArray replacement = "log4cplus.rootLogger=WARN, STDOUT\n";
    QProcess sh;
    sh.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), configWriteCommand(path)});
    QVERIFY(sh.waitForStarted(5000));
    sh.write(replacement);
    sh.closeWriteChannel();
    QVERIFY(sh.waitForFinished(10000));
    QCOMPARE(sh.exitCode(), 0);
    QCOMPARE(runSh(configReadCommand(path)), replacement);
}

void TestSshExec::writingInPlaceKeepsTheFilesPermissions()
{
    if (!haveShell())
        QSKIP("no /bin/sh to prove the commands against");

    // THE WHOLE REASON THE WRITE IS A REDIRECT rather than a temp-and-move. A shell `>`
    // truncates the existing file instead of unlinking and recreating it, so the inode
    // survives and with it the owner, the group and the mode. A config that was 0640
    // must not come back 0644 because a log viewer saved it — and nothing on screen
    // would say that it had.
    const QString path = write(QStringLiteral("perm.properties"), "a=1\n");
    const QFile::Permissions restricted =
        QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup;
    QVERIFY(QFile::setPermissions(path, restricted));

    QProcess sh;
    sh.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), configWriteCommand(path)});
    QVERIFY(sh.waitForStarted(5000));
    sh.write("a=2\n");
    sh.closeWriteChannel();
    QVERIFY(sh.waitForFinished(10000));

    QCOMPARE(runSh(configReadCommand(path)), QByteArray("a=2\n"));
    QCOMPARE(QFile::permissions(path) & (QFile::ReadOther | QFile::WriteOther),
             QFile::Permissions());
    QVERIFY(QFile::permissions(path).testFlag(QFile::ReadGroup));
}

void TestSshExec::existenceTellsAnEmptyFileFromAMissingOne()
{
    if (!haveShell())
        QSKIP("no /bin/sh to prove the commands against");

    // An EMPTY FILE and a MISSING one are the same empty stdout from a read, and telling
    // them apart is the whole of whether the editor says "new file" and whether saving
    // is creating something. That is why existence is its own round trip.
    const QString empty = write(QStringLiteral("empty.properties"), QByteArray());
    bool exists = false;
    QVERIFY(parseConfigExistsOutput(runSh(configExistsCommand(empty)), &exists));
    QVERIFY(exists);
    QCOMPARE(runSh(configReadCommand(empty)), QByteArray());

    const QString missing = m_dir.filePath(QStringLiteral("not-there.properties"));
    QVERIFY(parseConfigExistsOutput(runSh(configExistsCommand(missing)), &exists));
    QVERIFY(!exists);
    QCOMPARE(runSh(configReadCommand(missing)), QByteArray());

    // And a banner ahead of the answer does not become the answer: on the machines this
    // transport exists for, stdout is not private.
    QVERIFY(parseConfigExistsOutput(QByteArray("Welcome to the box\nloftail-cfg 1\n"), &exists));
    QVERIFY(exists);
    QVERIFY(!parseConfigExistsOutput(QByteArray("Welcome to the box\n"), &exists));
    QVERIFY(!parseConfigExistsOutput(QByteArray("loftail-cfg maybe\n"), &exists));
}

void TestSshExec::aHostileConfigPathCannotRunAnything()
{
    if (!haveShell())
        QSKIP("no /bin/sh to prove the quoting against");

    // The same claim the read path already makes, for the three commands that are new.
    // A remote path arrives from a URL somebody typed or was handed, and it ends up
    // inside a command a shell on someone else's machine interprets: without the
    // quoting this is not a strange filename, it is remote code execution.
    const QString canary = m_dir.filePath(QStringLiteral("canary"));
    const QString hostile =
        m_dir.filePath(QStringLiteral("x'; touch %1; echo '").arg(canary));

    for (const QString &command : {configReadCommand(hostile), configExistsCommand(hostile),
                                   configWriteCommand(hostile)}) {
        int code = 0;
        runSh(command, &code);
        QVERIFY2(!QFileInfo::exists(canary), qPrintable(command));
    }

    // And the other half of the claim, which the case above cannot make: the quoting
    // does not merely stop things running, it passes the whole string through as ONE
    // filename. Checked on a name that is legal — the hostile one above embeds a path,
    // so it names directories that are not there and could not be created whatever the
    // quoting did.
    const QString weird = m_dir.filePath(QStringLiteral("a b;$(id)`id`'q'.properties"));
    QProcess sh;
    sh.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), configWriteCommand(weird)});
    QVERIFY(sh.waitForStarted(5000));
    sh.write("k=v\n");
    sh.closeWriteChannel();
    QVERIFY(sh.waitForFinished(10000));
    QVERIFY2(QFileInfo::exists(weird), qPrintable(weird));
    QCOMPARE(runSh(configReadCommand(weird)), QByteArray("k=v\n"));

    bool exists = false;
    QVERIFY(parseConfigExistsOutput(runSh(configExistsCommand(weird)), &exists));
    QVERIFY(exists);
}


void TestSshExec::aRestartScriptRunsAsAWholeWithItsVariablesExported()
{
    if (!haveShell())
        QSKIP("no /bin/sh to run the script through");

    // AS A WHOLE, not line by line: an `if` opened on one line and closed on another, and
    // a variable set on one and read on the next, are what "as a whole" means in practice.
    const QList<QPair<QString, QString>> vars = {
        {QStringLiteral("LOGFILE"), QStringLiteral("/var/log/my app's $HOME `id`.log")},
        {QStringLiteral("ARCHIVE"), QStringLiteral("/srv/b u n.zip")},
        {QStringLiteral("MEMBER"), QStringLiteral("var/log/app.log")},
    };
    const QString script = QStringLiteral(
        "#!/bin/sh\n"
        "n=''\n"
        "for x in a b c; do n=\"${n}${x}\"; done\n"
        "if [ -n \"$LOGFILE\" ]; then\n"
        "  printf '%s|%s|%s|%s' \"$LOGFILE\" \"$ARCHIVE\" \"$MEMBER\" \"$n\"\n"
        "fi\n");

    int code = -1;
    const QByteArray out = runSh(restartScriptCommand(script, vars), &code);
    QCOMPARE(code, 0);
    QCOMPARE(QString::fromUtf8(out),
             QStringLiteral("/var/log/my app's $HOME `id`.log|/srv/b u n.zip|"
                            "var/log/app.log|abc"));
}

void TestSshExec::aVariableThatDoesNotApplyIsNotExportedAtAll()
{
    if (!haveShell())
        QSKIP("no /bin/sh to run the script through");

    // UNSET, never assigned empty, so `${ARCHIVE-unset}` tells an archived log from a
    // plain one. RestartTarget's half of the same promise is pinned in tst_restarttarget;
    // this is the half that would break silently if the builder started writing empties.
    const QList<QPair<QString, QString>> vars = {
        {QStringLiteral("LOGFILE"), QStringLiteral("/var/log/app.log")}};
    const QByteArray out = runSh(restartScriptCommand(
        QStringLiteral("printf '%s' \"${ARCHIVE-unset}${MEMBER-unset}\"\n"), vars));
    QCOMPARE(out, QByteArray("unsetunset"));
}

void TestSshExec::aHostileLogPathCannotRunAnythingInARestartScript()
{
    if (!haveShell())
        QSKIP("no /bin/sh to prove the quoting against");

    // THE VALUES ARE DATA. A log path arrives from a URL somebody typed or was handed,
    // and here it lands in a command a shell on somebody else's machine interprets —
    // without the quoting that is remote code execution from a filename.
    const QString canary = m_dir.filePath(QStringLiteral("script-canary"));
    const QString hostile = QStringLiteral("/var/log/x'; touch %1; echo '").arg(canary);

    const QList<QPair<QString, QString>> vars = {
        {QStringLiteral("LOGFILE"), hostile},
        {QStringLiteral("ARCHIVE"), hostile},
        {QStringLiteral("MEMBER"), hostile},
    };
    const QString command =
        restartScriptCommand(QStringLiteral("printf '%s' \"$LOGFILE\"\n"), vars);
    const QByteArray out = runSh(command);

    QVERIFY2(!QFileInfo::exists(canary), qPrintable(command));
    // And the other half of the claim: the quoting does not merely stop things running,
    // it passes the whole string through as one value.
    QCOMPARE(QString::fromUtf8(out), hostile);
}

void TestSshExec::aRestartScriptKeepsItsOwnExitStatus()
{
    if (!haveShell())
        QSKIP("no /bin/sh to run the script through");

    // The brace group and the /dev/null redirect must not eat the status: the exit code
    // is one of the two things the dialog decides success from.
    int code = -1;
    runSh(restartScriptCommand(QStringLiteral("exit 7\n"), {}), &code);
    QCOMPARE(code, 7);

    code = -1;
    runSh(restartScriptCommand(QStringLiteral("true\n"), {}), &code);
    QCOMPARE(code, 0);
}

void TestSshExec::aRestartScriptThatReadsStandardInputDoesNotHang()
{
    if (!haveShell())
        QSKIP("no /bin/sh to run the script through");

    // An exec channel's stdin is the channel, so a script that reads it would wait for
    // ever on a link nobody is writing to. `< /dev/null` is what makes `read` return at
    // once instead — and the 10 s bound inside runSh() is what would catch its removal.
    QElapsedTimer clock;
    clock.start();
    int code = -1;
    const QByteArray out = runSh(
        restartScriptCommand(QStringLiteral("read line\nprintf 'after'\n"), {}), &code);
    QCOMPARE(out, QByteArray("after"));
    QVERIFY2(clock.elapsed() < 5000, qPrintable(QString::number(clock.elapsed())));
}

void TestSshExec::aScriptStoredWithWindowsLineEndingsStillRuns()
{
    if (!haveShell())
        QSKIP("no /bin/sh to run the script through");

    // A script typed on Windows reaches a POSIX shell as `printf\r` — "command not
    // found", about a command that is plainly right on screen.
    int code = -1;
    const QByteArray out =
        runSh(restartScriptCommand(QStringLiteral("printf 'ok'\r\nexit 0\r\n"), {}), &code);
    QCOMPARE(out, QByteArray("ok"));
    QCOMPARE(code, 0);
}

QTEST_GUILESS_MAIN(TestSshExec)
#include "tst_sshexec.moc"
