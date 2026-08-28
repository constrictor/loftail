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

#include "SshRetryPolicy.h"

using namespace loftail;

// Whether an unattended reconnect keeps trying after the far end suddenly wants a person
// (SPEC.md §3, ARCHITECTURE.md §6.5, SshRetryPolicy.h).
//
// UNGATED, and the situation it stands for is one nothing else in the tree can reach: a
// remote machine rebooting, which passes through a window where sshd answers but nothing
// loftail has will authenticate yet — /home not mounted, PAM not up, a regenerated host
// key that is briefly not in known_hosts. Reproducing that needs a machine, a reboot and
// several minutes, so the clock is a parameter instead and the whole window is walked in
// microseconds.
class TestSshRetry : public QObject
{
    Q_OBJECT

private slots:
    void aTabThatNeverSignedInGivesUpAtOnce();
    void aHostThatSignedInIsRetriedThroughTheWindow();
    void theWindowIsBoundedSoALatchStillHappens();
    void signingInAgainRestoresTheWholeWindow();
    void theFirstFailureOfAnOutageIsAlwaysRetried();
};

// The session-restore-onto-an-unknown-host case, and the reason the grace is gated on
// having signed in rather than granted to everybody: with no evidence that these
// credentials work at all, "needs a person" is a standing state and one attempt is the
// right price.
void TestSshRetry::aTabThatNeverSignedInGivesUpAtOnce()
{
    ReconnectGrace grace(1000);
    QVERIFY(!grace.signedInOnce());
    QVERIFY(!grace.keepTrying(0));
    QVERIFY(!grace.keepTrying(1));
    QVERIFY(!grace.keepTrying(500));
}

// The bug this was written for: the log had been open and working, the far end rebooted,
// and one NeedsPerson on the way back up latched the tab off for good.
void TestSshRetry::aHostThatSignedInIsRetriedThroughTheWindow()
{
    ReconnectGrace grace(1000);
    grace.signedIn();
    QVERIFY(grace.signedInOnce());

    QVERIFY(grace.keepTrying(10000)); // sshd is answering, authorized_keys is not there yet
    QVERIFY(grace.keepTrying(10200));
    QVERIFY(grace.keepTrying(10900));

    // …and the box finishes booting. The next attempt gets in, which is the whole point:
    // nothing had to be clicked.
    grace.signedIn();
}

// Bounded, so a server that has genuinely changed its mind still reaches the state the
// diagnostic log calls out rather than being probed for the rest of the day.
void TestSshRetry::theWindowIsBoundedSoALatchStillHappens()
{
    ReconnectGrace grace(1000);
    grace.signedIn();
    QVERIFY(grace.keepTrying(0));
    QVERIFY(grace.keepTrying(999));
    QVERIFY(!grace.keepTrying(1000));
    QVERIFY(!grace.keepTrying(5000));
}

// A log that has been up for a week must survive its SECOND reboot as well as its first,
// which is why success closes the window instead of merely being ignored.
void TestSshRetry::signingInAgainRestoresTheWholeWindow()
{
    ReconnectGrace grace(1000);
    grace.signedIn();
    QVERIFY(grace.keepTrying(0));
    QVERIFY(grace.keepTrying(900));
    grace.signedIn();

    // The clock has not been rewound; the window has.
    QVERIFY(grace.keepTrying(50000));
    QVERIFY(grace.keepTrying(50999));
    QVERIFY(!grace.keepTrying(51000));
}

// The clock starts at the first failure, so measuring the window from it would give that
// very attempt zero and give up before anything had been retried at all.
void TestSshRetry::theFirstFailureOfAnOutageIsAlwaysRetried()
{
    ReconnectGrace grace(0);
    grace.signedIn();
    QVERIFY(grace.keepTrying(1234));
    QVERIFY(!grace.keepTrying(1234));
}

QTEST_GUILESS_MAIN(TestSshRetry)
#include "tst_sshretry.moc"
