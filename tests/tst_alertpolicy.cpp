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

#include "AlertPolicy.h"

using namespace loftail;

// M19 — how often a log may interrupt (SPEC.md §7, ARCHITECTURE.md §7.5.3).
//
// This is the part of the notification action that CI can honestly cover. The delivery
// path cannot be: the runners are headless with no tray and no notification server, so
// QSystemTrayIcon::isSystemTrayAvailable() is false and showMessage() is unreachable —
// the same shape as tst_sshlive and tst_keychainlive. What decides whether a
// notification happens at all, and what it says, is right here and needs no desktop:
// the clock is a parameter, so a burst spread over an hour runs in microseconds.
class TestAlertPolicy : public QObject
{
    Q_OBJECT

private slots:
    void firstBatchNotifiesImmediately();
    void aSecondBatchInsideTheIntervalIsSuppressed();
    void suppressedMatchesAreCoalescedNotDropped();
    void theBacklogIsReleasedByPoll();
    void oneBatchOfTenThousandIsOneNotification();
    void anEmptyBatchDecidesNothing();
    void resetForgetsTheBacklog();
    void pollWithNoBacklogIsSilent();
};

void TestAlertPolicy::firstBatchNotifiesImmediately()
{
    AlertPolicy p;
    // Not delayed by an interval that has not started yet: waiting one out would make
    // the feature look broken on the very match a user tests it with.
    const auto d = p.recordBatch(0, 1);
    QVERIFY(d.notify);
    QCOMPARE(d.count, 1);
    QCOMPARE(p.pending(), 0);
}

void TestAlertPolicy::aSecondBatchInsideTheIntervalIsSuppressed()
{
    AlertPolicy p(1000);
    QVERIFY(p.recordBatch(0, 1).notify);
    QVERIFY(!p.recordBatch(500, 1).notify);  // inside
    QVERIFY(!p.recordBatch(999, 1).notify);  // still inside
    QVERIFY(p.recordBatch(1000, 1).notify);  // the boundary is admitted
}

void TestAlertPolicy::suppressedMatchesAreCoalescedNotDropped()
{
    AlertPolicy p(1000);
    QCOMPARE(p.recordBatch(0, 1).count, 1);

    // Three ticks inside the interval, seven matches between them. Dropping them would
    // make the next notification lie about what happened while it was quiet.
    p.recordBatch(100, 2);
    p.recordBatch(200, 4);
    p.recordBatch(300, 1);
    QCOMPARE(p.pending(), 7);

    const auto d = p.recordBatch(1500, 3);
    QVERIFY(d.notify);
    QCOMPARE(d.count, 10); // the seven suppressed plus this batch's three
    QCOMPARE(p.pending(), 0);
}

void TestAlertPolicy::theBacklogIsReleasedByPoll()
{
    AlertPolicy p(1000);
    QVERIFY(p.recordBatch(0, 1).notify);
    p.recordBatch(100, 5); // suppressed
    QCOMPARE(p.pending(), 5);

    // A burst followed by SILENCE produces no further ingest tick, so without a pump
    // those five would go unreported forever.
    QVERIFY(!p.poll(500).notify); // still inside the interval
    const auto d = p.poll(1200);
    QVERIFY(d.notify);
    QCOMPARE(d.count, 5);
    QCOMPARE(p.pending(), 0);
}

void TestAlertPolicy::oneBatchOfTenThousandIsOneNotification()
{
    AlertPolicy p;
    // The structural half of the rate limit: recordBatch() is called once per ingest
    // tick with a COUNT, never once per record, so this needs no special case.
    const auto d = p.recordBatch(0, 10000);
    QVERIFY(d.notify);
    QCOMPARE(d.count, 10000);
}

void TestAlertPolicy::anEmptyBatchDecidesNothing()
{
    AlertPolicy p;
    // A tick that appended records but matched nothing must not consume the first
    // notification, or the next real match would be silently suppressed.
    QVERIFY(!p.recordBatch(0, 0).notify);
    QVERIFY(p.recordBatch(1, 1).notify);
}

void TestAlertPolicy::resetForgetsTheBacklog()
{
    AlertPolicy p(1000);
    QVERIFY(p.recordBatch(0, 1).notify);
    p.recordBatch(100, 9);
    QCOMPARE(p.pending(), 9);

    // A rotation replaced every record: what was suppressed described records that no
    // longer exist, and the next log is entitled to its immediate first notification.
    p.reset();
    QCOMPARE(p.pending(), 0);
    const auto d = p.recordBatch(200, 1);
    QVERIFY(d.notify);
    QCOMPARE(d.count, 1);
}

void TestAlertPolicy::pollWithNoBacklogIsSilent()
{
    AlertPolicy p(1000);
    QVERIFY(!p.poll(0).notify);
    QVERIFY(p.recordBatch(0, 1).notify);
    // The pump runs every five seconds while the tray exists; a quiet log must cost it
    // nothing but a comparison.
    QVERIFY(!p.poll(99999).notify);
}

QTEST_APPLESS_MAIN(TestAlertPolicy)
#include "tst_alertpolicy.moc"
