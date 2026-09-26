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

// Whether a log's bytes survive the way back, and what to do when they do not
// (ARCHITECTURE.md §6.11).
//
// UNGATED, and tst_execsizeprobe's shape exactly: the two seams take the place of the far
// end, the command seam runs through a real /bin/sh against real files, and what the far end
// is supposed to have is DECLARED — because "a server with no base64" and "a line that
// mangles bytes" are neither of them reachable from CI, and the decision is the part worth
// pinning anyway.

#include <QtTest>

#include <QProcess>

#include "ExecByteSafety.h"

using namespace loftail;

class TestExecByteSafety : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    static bool haveShell() { return QFileInfo::exists(QStringLiteral("/bin/sh")); }

    static QByteArray runSh(const QString &command)
    {
        QProcess sh;
        sh.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
        if (!sh.waitForFinished(10000))
            return {};
        return sh.readAllStandardOutput();
    }

    // A far end that runs what it is asked, except that the named commands are pretended
    // absent — which is how "this box has no base64" is expressed without uninstalling one.
    static ExecByteSafety::RunCommand shellRunnerWithout(const QStringList &missing)
    {
        return [missing](const QString &command, QByteArray *out) {
            for (const QString &tool : missing) {
                if (command.contains(tool)) {
                    // Absent, not broken: the command runs and prints nothing, which is
                    // what a shell does with a name it cannot find. NOT false, which would
                    // mean the channel died — the two-way rule.
                    out->clear();
                    return true;
                }
            }
            *out = runSh(command);
            return true;
        };
    }

    // The read seam, wired to the real readCommand() through a real shell, so the sample the
    // comparison runs over is an actual `tail -c +N | head -c L`.
    static ExecByteSafety::ReadRaw shellReader(const QString &path)
    {
        return [path](qint64 offset, qint64 length) {
            return runSh(readCommand(path, offset, length, ByteSafety::Raw));
        };
    }

    // The same read, over the mangling line. See manglingRunner(): the two seams must agree
    // about what the line does, because in production they are one line.
    static ExecByteSafety::ReadRaw manglingReader(const QString &path)
    {
        return [path](qint64 offset, qint64 length) {
            return runSh(readCommand(path, offset, length, ByteSafety::Raw)
                         + QStringLiteral(" | sed 's/$/\r/'"));
        };
    }

    // A far end whose line MANGLES bytes, which is the whole subject: `sed` stands in for the
    // line discipline, turning LF into CR-LF exactly as a cooked tty does.
    //
    // BOTH SEAMS HAVE TO BE MANGLED, and getting that wrong is a fixture that proves the
    // opposite of what it claims: in production the raw read and the base64 read cross the
    // SAME line, so a fixture whose reader bypasses the mangling compares a clean sample
    // against a decoded-and-therefore-also-clean one, and settles on Raw over a line that
    // corrupts everything.
    static ExecByteSafety::RunCommand manglingRunner(const QStringList &missing)
    {
        return [missing](const QString &command, QByteArray *out) {
            for (const QString &tool : missing) {
                if (command.contains(tool)) {
                    out->clear();
                    return true;
                }
            }
            // The base64 pipeline's own output is 7-bit text and survives the mangling
            // untouched, which is exactly why it is the fallback.
            *out = runSh(command + QStringLiteral(" | sed 's/$/\\r/'"));
            return true;
        };
    }

    QString writeFile(const QString &name, const QByteArray &bytes)
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
    void initTestCase();
    void aLineThatCarriesBytesIntactSettlesOnRaw();
    void aLineThatMangesBytesFallsBackToBase64();
    void aFarEndWithNoEncoderIsVerifiedByAGeneratedPatternInstead();
    void aBinaryLogWithNoEncoderIsRefusedRatherThanGuessedAt();
    void aFarEndWithNeitherIsRefusedAndNamesBoth();
    void anEmptyLogIsNotEvidenceEitherWay();
    void settlingAgainLeavesNoStaleAnswerBehind();
};

void TestExecByteSafety::initTestCase()
{
    QVERIFY(m_dir.isValid());
    if (!haveShell())
        QSKIP("no /bin/sh");
}

void TestExecByteSafety::aLineThatCarriesBytesIntactSettlesOnRaw()
{
    // An SSH exec channel, which is what every server rung has always used: nothing between
    // the bytes and the reader. Raw, and verified against the encoder rather than assumed.
    const QString path = writeFile(QStringLiteral("clean.log"),
                                   QByteArrayLiteral("2026-01-01 INFO one\n"
                                                     "2026-01-01 INFO two\n"));
    QVERIFY(!path.isEmpty());
    ExecByteSafety probe(path, shellRunnerWithout({}), shellReader(path));
    QString refusal;
    const auto answer = probe.settle(&refusal);
    QVERIFY2(answer.has_value(), qUtf8Printable(refusal));
    QCOMPARE(*answer, ByteSafety::Raw);
    QVERIFY(probe.hasEncoder());
    // AND THE STRONG TEST IS THE ONE THAT RAN. That distinction is the point of the
    // accessor: the generated-pattern test cannot see a NUL, so which test settled it is
    // what the next decision turns on.
    QVERIFY(probe.verifiedAgainstEncoder());
}

void TestExecByteSafety::aLineThatMangesBytesFallsBackToBase64()
{
    // THE CASE THE WHOLE LADDER EXISTS FOR. A tty turns `\n` into `\r\n`, so the log arrives
    // quietly corrupted — not refused, not short, just wrong. Nothing about the read reports
    // it, which is why it has to be probed for rather than waited for.
    const QString path = writeFile(QStringLiteral("mangled.log"),
                                   QByteArrayLiteral("first line\nsecond line\n"));
    QVERIFY(!path.isEmpty());
    ExecByteSafety probe(path, manglingRunner({}), manglingReader(path));
    QString refusal;
    const auto answer = probe.settle(&refusal);
    QVERIFY2(answer.has_value(), qUtf8Printable(refusal));
    QCOMPARE(*answer, ByteSafety::Base64);
    QVERIFY(probe.hasEncoder());
}

void TestExecByteSafety::aFarEndWithNoEncoderIsVerifiedByAGeneratedPatternInstead()
{
    // No encoder to compare against, so the fallback is to produce the awkward bytes with
    // `printf` and require them back. Weaker, and the accessor says so.
    const QString path = writeFile(QStringLiteral("plain.log"), QByteArrayLiteral("hello\n"));
    QVERIFY(!path.isEmpty());
    ExecByteSafety probe(path, shellRunnerWithout({QStringLiteral("base64")}),
                         shellReader(path));
    QString refusal;
    const auto answer = probe.settle(&refusal);
    QVERIFY2(answer.has_value(), qUtf8Printable(refusal));
    QCOMPARE(*answer, ByteSafety::Raw);
    QVERIFY(!probe.hasEncoder());
    QVERIFY(!probe.verifiedAgainstEncoder());
}

void TestExecByteSafety::aBinaryLogWithNoEncoderIsRefusedRatherThanGuessedAt()
{
    // THE LIMIT, STATED RATHER THAN HIDDEN. A NUL cannot be carried through a shell argument,
    // so the generated pattern cannot prove one survives — and a log full of NULs is a
    // UTF-16 log, which is invariant #8's own subject and the case where getting this wrong
    // makes the whole file unreadable rather than merely odd. So it is REFUSED, by name,
    // naming the one utility that would fix it.
    QByteArray utf16;
    for (const char c : QByteArrayLiteral("hello")) {
        utf16.append(c);
        utf16.append('\0');
    }
    const QString path = writeFile(QStringLiteral("utf16.log"), utf16);
    QVERIFY(!path.isEmpty());
    ExecByteSafety probe(path, shellRunnerWithout({QStringLiteral("base64")}),
                         shellReader(path));
    QString refusal;
    QVERIFY(!probe.settle(&refusal).has_value());
    QVERIFY(!refusal.isEmpty());
    QVERIFY2(refusal.contains(QStringLiteral("base64")), qUtf8Printable(refusal));

    // And WITH an encoder the very same log settles perfectly well, which is what says the
    // refusal is about the missing utility and not about the log.
    ExecByteSafety withEncoder(path, shellRunnerWithout({}), shellReader(path));
    QString ignored;
    const auto answer = withEncoder.settle(&ignored);
    QVERIFY(answer.has_value());
    QCOMPARE(*answer, ByteSafety::Raw);
}

void TestExecByteSafety::aFarEndWithNeitherIsRefusedAndNamesBoth()
{
    // A mangling line and no encoder: there is nothing to be done, so it says so rather than
    // reading the log wrongly. A refusal that keeps its tab and explains itself (M17).
    const QString path = writeFile(QStringLiteral("hopeless.log"),
                                   QByteArrayLiteral("first\nsecond\n"));
    QVERIFY(!path.isEmpty());
    ExecByteSafety probe(path, manglingRunner({QStringLiteral("base64")}),
                         manglingReader(path));
    QString refusal;
    QVERIFY(!probe.settle(&refusal).has_value());
    QVERIFY(refusal.contains(QStringLiteral("stty")));
    QVERIFY(refusal.contains(QStringLiteral("base64")));
}

void TestExecByteSafety::anEmptyLogIsNotEvidenceEitherWay()
{
    // No bytes to mangle, so no comparison is possible and raw is taken — ExecSizeProbe skips
    // its own proof read for a zero size for the same reason. A log that is empty now will be
    // read again when it grows, and this re-settles on every login in any case.
    const QString path = writeFile(QStringLiteral("empty.log"), QByteArray());
    QVERIFY(!path.isEmpty());
    ExecByteSafety probe(path, manglingRunner({}), manglingReader(path));
    QString refusal;
    const auto answer = probe.settle(&refusal);
    QVERIFY2(answer.has_value(), qUtf8Printable(refusal));
    QCOMPARE(*answer, ByteSafety::Raw);
    // And the weaker flag says the strong test did NOT run, which is the honest answer.
    QVERIFY(!probe.verifiedAgainstEncoder());
}

void TestExecByteSafety::settlingAgainLeavesNoStaleAnswerBehind()
{
    // ExecSizeProbe's rule: this re-settles on every login, so a flag left over from the
    // previous one describes the previous session's line discipline. Cleared at the top.
    const QString path = writeFile(QStringLiteral("again.log"), QByteArrayLiteral("x\n"));
    QVERIFY(!path.isEmpty());
    ExecByteSafety probe(path, shellRunnerWithout({}), shellReader(path));
    QString refusal;
    QVERIFY(probe.settle(&refusal).has_value());
    QVERIFY(probe.hasEncoder());
    QVERIFY(probe.verifiedAgainstEncoder());

    // The same object, now against a far end with no encoder. Both flags must follow.
    ExecByteSafety second(path, shellRunnerWithout({QStringLiteral("base64")}),
                          shellReader(path));
    QVERIFY(second.settle(&refusal).has_value());
    QVERIFY(!second.hasEncoder());
    QVERIFY(!second.verifiedAgainstEncoder());
}

QTEST_MAIN(TestExecByteSafety)
#include "tst_execbytesafety.moc"
