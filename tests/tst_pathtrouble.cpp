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

#include "PathTrouble.h"

using namespace loftail;

// WHY a log on another machine is not there, in the one place that words it
// (SPEC.md §3, ARCHITECTURE.md §6.5).
//
// UNGATED and core-only, like tst_sshexec and tst_execsizeprobe: a build without libssh2
// says these sentences too — a restored session naming a remote log reaches them — and a
// wording tested in one configuration is a wording that rots in the other.
//
// What is asserted is that each answer is DISTINGUISHABLE from the others, never the
// exact prose: a `tr()` string is not a contract and every one of these will be reworded.
// The one thing spelled out is the pair this whole change exists to separate — an absence
// must not describe itself as something that cannot be read, which is the sentence that
// sent the reader looking in the wrong place.
class TestPathTrouble : public QObject
{
    Q_OBJECT

private slots:
    void everyReasonGetsItsOwnSentenceAndNoneOfThemSaysUnreadable();
    void anAnswerNobodyRecognisedQuotesTheServerRatherThanGuessing();
    void theFolderIsCutTheWayTheFarEndWouldCutIt();

private:
    static QString say(LogPresence presence, const QString &path = QStringLiteral("/var/log/a.log"))
    {
        RemotePathReport report;
        report.known = true;
        report.presence = presence;
        return remotePathTroubleText(report, path, QStringLiteral("web1"));
    }
};

void TestPathTrouble::everyReasonGetsItsOwnSentenceAndNoneOfThemSaysUnreadable()
{
    const QString absent = say(LogPresence::Absent);
    const QString noFolder = say(LogPresence::NoDirectory);
    const QString denied = say(LogPresence::Unreadable);
    const QString folder = say(LogPresence::NotAFile);

    // Four answers, four sentences. Folding any two of them is the defect: the exec
    // transport used to say "it is missing, or the account cannot read it" — both at
    // once — and the poll loop said "is not readable right now" about a file that was
    // simply gone.
    const QStringList all{absent, noFolder, denied, folder};
    for (const QString &one : all) {
        QVERIFY2(!one.isEmpty(), "every reason has to say something");
        QVERIFY2(one.contains(QStringLiteral("web1")), qPrintable(one));
        QVERIFY2(one.contains(QStringLiteral("/var/log/a.log")), qPrintable(one));
    }
    QCOMPARE(QSet<QString>(all.begin(), all.end()).size(), all.size());

    // THE PAIR THIS EXISTS TO SEPARATE. An absence may not describe itself as something
    // that cannot be read — that is what sends somebody to check permissions on a file
    // that is not there — and the sentence about a folder must name the folder.
    QVERIFY2(!absent.contains(QStringLiteral("read")), qPrintable(absent));
    QVERIFY2(denied.contains(QStringLiteral("read")), qPrintable(denied));
    QVERIFY2(noFolder.contains(QStringLiteral("/var/log")), qPrintable(noFolder));
    QVERIFY2(folder.contains(QStringLiteral("folder")), qPrintable(folder));

    // Present is a real answer and not a failure to get one: it is what a server says
    // when the trouble was somewhere other than the path, and claiming one of the four
    // above there would be a sentence about the wrong subject.
    const QString fine = say(LogPresence::Present);
    QVERIFY(!fine.isEmpty());
    QVERIFY(!all.contains(fine));
}

void TestPathTrouble::anAnswerNobodyRecognisedQuotesTheServerRatherThanGuessing()
{
    RemotePathReport unknown;
    unknown.known = false;
    unknown.detail = QStringLiteral("SFTP status 4 (failure)");
    const QString said = remotePathTroubleText(unknown, QStringLiteral("/var/log/a.log"),
                                               QStringLiteral("web1"));
    QVERIFY2(said.contains(unknown.detail), qPrintable(said));

    // And with nothing to quote it says THAT, rather than picking one of the four. The
    // whole argument for the `known` flag is that a status nobody recognised has no
    // classification, and inventing one is how the folded sentence arose.
    RemotePathReport silent;
    silent.known = false;
    const QString nothing = remotePathTroubleText(silent, QStringLiteral("/var/log/a.log"),
                                                  QStringLiteral("web1"));
    QVERIFY(!nothing.isEmpty());
    QVERIFY(!nothing.contains(QStringLiteral("no such")));
    QVERIFY(nothing != said);
}

void TestPathTrouble::theFolderIsCutTheWayTheFarEndWouldCutIt()
{
    // POSIX, always, whatever this machine thinks a separator is: the answer is fed to a
    // `test -d` on somebody else's box and to a stat over SFTP, so a Windows-flavoured
    // cut would ask about a folder the far end has never heard of.
    QCOMPARE(remoteParentFolderOf(QStringLiteral("/var/log/a.log")), QStringLiteral("/var/log"));
    // The root's parent is the root, not the empty string — otherwise the sentence reads
    // "there is no folder  either" and the stat asks about nothing.
    QCOMPARE(remoteParentFolderOf(QStringLiteral("/a.log")), QStringLiteral("/"));
    // A backslash is an ordinary character in a POSIX path and must not cut anything.
    QCOMPARE(remoteParentFolderOf(QStringLiteral("/var/log/a\\b.log")), QStringLiteral("/var/log"));
    // Nothing to name: a relative path, resolved by the far end against a home directory
    // loftail knows nothing about.
    QVERIFY(remoteParentFolderOf(QStringLiteral("a.log")).isEmpty());

    // And with no folder to name, the missing-folder answer falls back to the plain
    // absence rather than printing a blank one.
    const QString said = say(LogPresence::NoDirectory, QStringLiteral("a.log"));
    QCOMPARE(said, say(LogPresence::Absent, QStringLiteral("a.log")));
}

QTEST_MAIN(TestPathTrouble)
#include "tst_pathtrouble.moc"
