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

#include <QDir>

#include "RemoteLocation.h"
#include "RestartTarget.h"

using namespace loftail;

// Where a log's restart script runs, and what it is told about the log (SPEC.md §4).
//
// THE SAME KIND OF TEST tst_configlocation is, and for a sharper reason: a wrong answer
// there writes a file at a path nobody asked for; a wrong answer HERE runs a script on a
// machine nobody asked for, or hands one a path that names nothing. Both fail by
// succeeding somewhere else, which is the only kind of failure worth this much coverage.
//
// UNGATED and core-only, for the reason ArchiveLocation's and RemoteLocation's own tests
// are: deriving and refusing a restart target must be identical in a build with SSH
// support and one without, or the two disagree about what a settings file means.
class TestRestartTarget : public QObject
{
    Q_OBJECT

private slots:
    void nothingConfiguredIsNotARefusal();
    void aPlainLocalLogNamesItselfAsLogfile();
    void aRemoteLogRunsOnItsOwnHostAndNamesThePathThere();
    void anArchivedLogRunsOutsideAndNamesTheContainerAndTheMember();
    void aRemoteArchiveRunsOnTheHostAndNamesTheContainerThere();
    void aBareCompressedStreamIsStillAnArchiveWithNoMember();
    void anAbsentVariableIsOmittedRatherThanEmpty();
    void aLogAddressThatDoesNotParseIsRefused();
    void thereIsNoLogToRestartFor();
    void noValueIsEverAUrlOrANestedArchiveAddress();
    void noHalfOfAnyAnswerEverCarriesAPassword();
    void whetherThisBuildCanRunItIsASeparateQuestion();

private:
    // "/srv/x" is NOT absolute on Windows — it has no drive — so an expectation spelled
    // that way misses by a drive letter. Built from QDir::rootPath(), which is already
    // absolute on both, exactly as tst_configlocation::root() does.
    static QString root(const QString &tail) { return QDir::rootPath() + tail; }

    static QString valueOf(const RestartTarget &t, const char *name)
    {
        for (const auto &v : t.variables) {
            if (v.first == QLatin1String(name))
                return v.second;
        }
        return {};
    }
    static bool has(const RestartTarget &t, const char *name)
    {
        for (const auto &v : t.variables) {
            if (v.first == QLatin1String(name))
                return true;
        }
        return false;
    }
};

void TestRestartTarget::nothingConfiguredIsNotARefusal()
{
    // Unset is a THIRD state and not a refusal: it is what makes the menu item explain
    // itself and offer Preferences. Folding it into Refused puts an error strip above the
    // document well where an invitation belongs.
    for (const QString &empty : {QString(), QStringLiteral("   \n\t ")}) {
        const auto t = resolveRestartTarget(root(QStringLiteral("var/log/app.log")), empty);
        QCOMPARE(t.state, RestartTarget::State::Unset);
        QVERIFY(t.variables.isEmpty());
        QVERIFY(t.script.isEmpty());
        QVERIFY(t.reason.isEmpty());
    }
}

void TestRestartTarget::aPlainLocalLogNamesItselfAsLogfile()
{
    const QString log = root(QStringLiteral("var/log/app.log"));
    const auto t = resolveRestartTarget(log, QStringLiteral("systemctl restart app"));

    QCOMPARE(t.state, RestartTarget::State::Resolved);
    QVERIFY(!t.remote);
    QVERIFY(!t.host.has_value());
    QCOMPARE(t.script, QStringLiteral("systemctl restart app"));
    QCOMPARE(valueOf(t, "LOGFILE"), log);
    QVERIFY(!has(t, "ARCHIVE"));
    QVERIFY(!has(t, "MEMBER"));
}

void TestRestartTarget::aRemoteLogRunsOnItsOwnHostAndNamesThePathThere()
{
    const auto t = resolveRestartTarget(QStringLiteral("ssh://web1/var/log/app.log"),
                                        QStringLiteral("true"));
    QCOMPARE(t.state, RestartTarget::State::Resolved);
    QVERIFY(t.remote);
    QVERIFY(t.host.has_value());
    QCOMPARE(t.host->host, QStringLiteral("web1"));

    // THE PATH ON THAT HOST, never the URL. A shell over there cannot open an ssh:// URL,
    // so handing one over would be a variable that silently names nothing.
    QCOMPARE(valueOf(t, "LOGFILE"), QStringLiteral("/var/log/app.log"));
}

void TestRestartTarget::anArchivedLogRunsOutsideAndNamesTheContainerAndTheMember()
{
    const QString container = root(QStringLiteral("srv/bundle.zip"));
    const auto t = resolveRestartTarget(container + QStringLiteral("/var/log/app.log"),
                                        QStringLiteral("true"));

    QCOMPARE(t.state, RestartTarget::State::Resolved);
    // THE OUTER SYSTEM. There is no service running inside a zip file.
    QVERIFY(!t.remote);
    QCOMPARE(valueOf(t, "LOGFILE"), container);
    QCOMPARE(valueOf(t, "ARCHIVE"), container);
    QCOMPARE(valueOf(t, "MEMBER"), QStringLiteral("var/log/app.log"));
}

void TestRestartTarget::aRemoteArchiveRunsOnTheHostAndNamesTheContainerThere()
{
    // The case a LENGTH-based peel gets wrong: split() returns the container in normal
    // form with the port spelled out, so it comes back strictly LONGER than what went in.
    const auto t = resolveRestartTarget(QStringLiteral("ssh://h/srv/b.tar.gz/app.log"),
                                        QStringLiteral("true"));
    QCOMPARE(t.state, RestartTarget::State::Resolved);
    QVERIFY(t.remote);
    QCOMPARE(t.host->host, QStringLiteral("h"));
    QCOMPARE(valueOf(t, "LOGFILE"), QStringLiteral("/srv/b.tar.gz"));
    QCOMPARE(valueOf(t, "ARCHIVE"), QStringLiteral("/srv/b.tar.gz"));
    QCOMPARE(valueOf(t, "MEMBER"), QStringLiteral("app.log"));
}

void TestRestartTarget::aBareCompressedStreamIsStillAnArchiveWithNoMember()
{
    // The commonest archived shape of all, and the one a `member.isEmpty()` test for
    // "is this an archive?" would leave with no ARCHIVE at all: a `.gz` has exactly one
    // member and its name is implied rather than spelled.
    const QString log = root(QStringLiteral("logs/app.log.gz"));
    const auto t = resolveRestartTarget(log, QStringLiteral("true"));

    QCOMPARE(t.state, RestartTarget::State::Resolved);
    QCOMPARE(valueOf(t, "LOGFILE"), log);
    QCOMPARE(valueOf(t, "ARCHIVE"), log);
    QVERIFY(!has(t, "MEMBER"));
}

void TestRestartTarget::anAbsentVariableIsOmittedRatherThanEmpty()
{
    // OMITTED, never assigned empty, so a script can write `${ARCHIVE-}` and tell an
    // archived log from a plain one. Both executors have to agree about this; the remote
    // half's half of the promise is pinned in tst_sshexec.
    const auto plain = resolveRestartTarget(root(QStringLiteral("var/log/app.log")),
                                            QStringLiteral("true"));
    QCOMPARE(plain.variables.size(), 1);
    QCOMPARE(plain.variables.first().first, QStringLiteral("LOGFILE"));
}

void TestRestartTarget::aLogAddressThatDoesNotParseIsRefused()
{
    // A remote-shaped address with no host is not something to guess at: running a script
    // on the wrong machine is the failure this whole file exists to prevent.
    const auto t = resolveRestartTarget(QStringLiteral("ssh://"), QStringLiteral("true"));
    QCOMPARE(t.state, RestartTarget::State::Refused);
    QVERIFY(!t.reason.isEmpty());
    QVERIFY(t.variables.isEmpty());
}

void TestRestartTarget::thereIsNoLogToRestartFor()
{
    const auto t = resolveRestartTarget(QString(), QStringLiteral("true"));
    QCOMPARE(t.state, RestartTarget::State::Refused);
    QVERIFY(!t.reason.isEmpty());
}

void TestRestartTarget::noValueIsEverAUrlOrANestedArchiveAddress()
{
    const QStringList logs = {
        root(QStringLiteral("var/log/app.log")),
        root(QStringLiteral("srv/bundle.zip/var/log/app.log")),
        root(QStringLiteral("logs/app.log.gz")),
        QStringLiteral("ssh://web1/var/log/app.log"),
        QStringLiteral("ssh://h:2222/srv/b.tar.gz/app.log"),
    };
    for (const QString &log : logs) {
        const auto t = resolveRestartTarget(log, QStringLiteral("true"));
        QCOMPARE(t.state, RestartTarget::State::Resolved);
        for (const auto &v : t.variables) {
            // A value is a path on the machine the script runs on. Not a URL, and not
            // loftail's own nested address — neither opens on the far end.
            QVERIFY2(!v.second.contains(QStringLiteral("://")), qPrintable(v.second));
            QVERIFY2(!v.second.contains(QStringLiteral(".zip/")), qPrintable(v.second));
            QVERIFY2(!v.second.contains(QStringLiteral(".tar.gz/")), qPrintable(v.second));
        }
    }
}

void TestRestartTarget::noHalfOfAnyAnswerEverCarriesAPassword()
{
    // The last case tst_configlocation carries, for the same reason: the branch reached
    // when parse() FAILED is the one that never went through parse()'s own dropping, and
    // it is the branch that puts an address on screen verbatim.
    const QStringList logs = {
        QStringLiteral("ssh://deploy:hunter2@web1/var/log/app.log"),
        QStringLiteral("ssh://deploy:hunter2@web1"), // no path: does not parse
        QStringLiteral("ssh://deploy:hunter2@"),     // no host either
    };
    for (const QString &log : logs) {
        const auto t = resolveRestartTarget(log, QStringLiteral("true"));
        QVERIFY2(!t.reason.contains(QStringLiteral("hunter2")), qPrintable(t.reason));
        for (const auto &v : t.variables)
            QVERIFY2(!v.second.contains(QStringLiteral("hunter2")), qPrintable(v.second));
        if (t.host)
            QVERIFY(!t.host->toString().contains(QStringLiteral("hunter2")));
    }
}

void TestRestartTarget::whetherThisBuildCanRunItIsASeparateQuestion()
{
    // The resolution above must answer IDENTICALLY with and without libssh2 — the rule
    // RemoteLocation.h and ArchiveLocation.h both state, so the two builds cannot
    // disagree about what a settings file means. What a build CAN DO is asked separately,
    // exactly as configAddressIsWritable() asks it for a config file.
    const auto here = resolveRestartTarget(root(QStringLiteral("var/log/app.log")),
                                           QStringLiteral("true"));
    QString why;
    QVERIFY(restartTargetIsRunnable(here, &why));
    QVERIFY(why.isEmpty());

    const auto there = resolveRestartTarget(QStringLiteral("ssh://web1/var/log/app.log"),
                                            QStringLiteral("true"));
    QCOMPARE(there.state, RestartTarget::State::Resolved); // resolved either way
    why.clear();
#if defined(LOFTAIL_HAVE_SSH)
    QVERIFY(restartTargetIsRunnable(there, &why));
#else
    QVERIFY(!restartTargetIsRunnable(there, &why));
    // Named, not silent: a missing dependency is a sentence somebody can act on.
    QVERIFY(!why.isEmpty());
#endif
}

QTEST_APPLESS_MAIN(TestRestartTarget)
#include "tst_restarttarget.moc"
