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

#include "SshSessionCache.h"

#include <memory>

using namespace loftail;

// The bookkeeping behind reusing an SSH connection between errands (ARCHITECTURE.md §6.3).
//
// UNGATED, and for the fifth time on the argument SshRetryPolicy, ExecSizeProbe and
// SshSessionHealth already carry: the transport that consumes this is reachable in one
// build configuration on one kind of server, so a rule compiled only there is a rule tested
// only there. What is decided here — who owns a session, when it stops being reusable, how
// many may be held, and what happens at shutdown — needs no libssh2 at all, because the
// cache deliberately holds sessions behind CachedSshSession and never names one.
//
// The clock is a parameter, so the whole sixty-second deadline is walked in microseconds.
class TestSshSessionCache : public QObject
{
    Q_OBJECT

private slots:
    void aSessionComesBackToTheNextErrandOnTheSameHost();
    void theCheckoutIsTheExclusionSoTwoErrandsNeverShareOne();
    void aDifferentHostIsADifferentConnection();
    void anExecOnlyErrandBorrowsAReadSessionButNeverTheOtherWayRound();
    void anExactRoleIsPreferredToABorrow();
    void aSessionIdleTooLongIsLetGoRatherThanHandedOut();
    void aSessionThatWentUnhealthyIsNeverStoredAndNeverHandedOut();
    void theCacheIsBoundedAndTheOldestGoesFirst();
    void closingLetsEveryConnectionGoAndStaysClosed();
    void everySessionIsCutLooseBeforeItIsDestroyed();
    void checkingInDropsTheFinishedErrandsHold();
};

namespace {

// What the cache's three questions are asked OF. Every answer is recorded, because each of
// them stands for something that costs a socket or a twenty-second write in the real thing.
class FakeSession final : public CachedSshSession
{
public:
    struct Tally
    {
        int  destroyed = 0;
        int  cutLoose = 0;
        int  detached = 0;
        // In order, so a test can say WHICH session was let go rather than only how many.
        QList<int> destroyedIds;
        // Set for the one thing the fake cannot express afterwards: cutLoose() must come
        // BEFORE the destructor, or a real session's farewell packet blocks.
        bool cutLooseAfterDestroy = false;
    };

    FakeSession(int id, Tally *tally) : m_id(id), m_tally(tally) {}
    ~FakeSession() override
    {
        ++m_tally->destroyed;
        m_tally->destroyedIds.push_back(m_id);
        m_destroyed = true;
    }

    int  id() const { return m_id; }
    void die() { m_healthy = false; }

    bool healthy() const override { return m_healthy; }
    void cutLoose() override
    {
        ++m_tally->cutLoose;
        if (m_destroyed)
            m_tally->cutLooseAfterDestroy = true;
        m_cutLoose = true;
    }
    void detachOwner() override { ++m_tally->detached; }

    bool wasCutLoose() const { return m_cutLoose; }

private:
    int    m_id;
    Tally *m_tally;
    bool   m_healthy = true;
    bool   m_cutLoose = false;
    bool   m_destroyed = false;
};

std::unique_ptr<CachedSshSession> make(int id, FakeSession::Tally *tally)
{
    return std::make_unique<FakeSession>(id, tally);
}

int idOf(const SshSessionCache::Checkout &out)
{
    return out.session ? static_cast<FakeSession *>(out.session.get())->id() : -1;
}

const QString kHost = QStringLiteral("me@host-a:22");
const QString kOther = QStringLiteral("me@host-b:22");

} // namespace

// The whole point: the second errand against a machine loftail is already connected to
// pays no connect at all.
void TestSshSessionCache::aSessionComesBackToTheNextErrandOnTheSameHost()
{
    FakeSession::Tally tally;
    SshSessionCache    cache;

    cache.checkIn(kHost, SshSessionRole::Transport, make(1, &tally), 0);
    QCOMPARE(cache.size(), 1);
    QCOMPARE(tally.destroyed, 0);

    const auto taken = cache.checkOut(kHost, SshSessionRole::Transport, 10);
    QVERIFY(taken);
    QCOMPARE(idOf(taken), 1);
    QCOMPARE(taken.role, SshSessionRole::Transport);

    // TAKEN OUT, not borrowed in place: the cache is empty while the errand runs.
    QCOMPARE(cache.size(), 0);
}

// SshSession is one-thread-at-a-time and SshConnectHold guards a CONNECT, which a hit does
// not perform — so the removal is the only thing standing between two workers and one
// session. The second one has to miss and go and connect.
void TestSshSessionCache::theCheckoutIsTheExclusionSoTwoErrandsNeverShareOne()
{
    FakeSession::Tally tally;
    SshSessionCache    cache;
    cache.checkIn(kHost, SshSessionRole::Transport, make(1, &tally), 0);

    const auto first = cache.checkOut(kHost, SshSessionRole::Transport, 1);
    const auto second = cache.checkOut(kHost, SshSessionRole::Transport, 1);
    QVERIFY(first);
    QVERIFY(!second);
}

void TestSshSessionCache::aDifferentHostIsADifferentConnection()
{
    FakeSession::Tally tally;
    SshSessionCache    cache;
    cache.checkIn(kHost, SshSessionRole::Transport, make(1, &tally), 0);

    QVERIFY(!cache.checkOut(kOther, SshSessionRole::Transport, 1));
    QCOMPARE(cache.size(), 1);
    QVERIFY(cache.checkOut(kHost, SshSessionRole::Transport, 1));
}

// The asymmetry that makes "save the config, then restart the service" cost one connect. A
// Transport session settled a transport and can open an exec channel too; an ExecOnly one
// never asked the server anything and has no SFTP handle, so handing it to a reader would
// hand it a session every read operation refuses by name (SshSession.h).
void TestSshSessionCache::anExecOnlyErrandBorrowsAReadSessionButNeverTheOtherWayRound()
{
    FakeSession::Tally tally;
    SshSessionCache    cache;

    cache.checkIn(kHost, SshSessionRole::Transport, make(1, &tally), 0);
    const auto borrowed = cache.checkOut(kHost, SshSessionRole::ExecOnly, 1);
    QVERIFY(borrowed);
    QCOMPARE(idOf(borrowed), 1);
    // AND IT REPORTS WHAT IT IS, not what was asked for, so the caller files it back under
    // Transport and the next config read can have it.
    QCOMPARE(borrowed.role, SshSessionRole::Transport);

    cache.checkIn(kHost, SshSessionRole::ExecOnly, make(2, &tally), 2);
    QVERIFY(!cache.checkOut(kHost, SshSessionRole::Transport, 3));
    QCOMPARE(cache.size(), 1);
}

// The borrow is a fallback, not a preference: leaving the Transport session where it is,
// is what keeps a config read that follows the restart free as well.
void TestSshSessionCache::anExactRoleIsPreferredToABorrow()
{
    FakeSession::Tally tally;
    SshSessionCache    cache;
    cache.checkIn(kHost, SshSessionRole::Transport, make(1, &tally), 0);
    cache.checkIn(kHost, SshSessionRole::ExecOnly, make(2, &tally), 0);

    const auto taken = cache.checkOut(kHost, SshSessionRole::ExecOnly, 1);
    QVERIFY(taken);
    QCOMPARE(idOf(taken), 2);
    QCOMPARE(taken.role, SshSessionRole::ExecOnly);

    const auto rest = cache.checkOut(kHost, SshSessionRole::Transport, 1);
    QVERIFY(rest);
    QCOMPARE(idOf(rest), 1);
}

// A socket held open against somebody else's machine for an errand that has finished, which
// is invariant #5's spirit one step out from the log. With no event loop on a worker thread
// there is no timer, so the deadline is enforced by every touch of the cache — and the
// expired entry is DESTROYED rather than handed over.
void TestSshSessionCache::aSessionIdleTooLongIsLetGoRatherThanHandedOut()
{
    FakeSession::Tally tally;
    SshSessionCache    cache(1000, 4);

    // A millisecond inside the deadline is still a hit. (The checkout hands the session to
    // the caller, whose scope ends here — so session 1 is destroyed by the TEST, not by the
    // cache, which is exactly what an errand does with one it could not give back.)
    cache.checkIn(kHost, SshSessionRole::Transport, make(1, &tally), 5000);
    QVERIFY(cache.checkOut(kHost, SshSessionRole::Transport, 5999));
    QCOMPARE(tally.destroyedIds, QList<int>{1});

    cache.checkIn(kHost, SshSessionRole::Transport, make(2, &tally), 6000);
    QVERIFY(!cache.checkOut(kHost, SshSessionRole::Transport, 7000));
    QCOMPARE(tally.destroyedIds, (QList<int>{1, 2}));
    QCOMPARE(cache.size(), 0);

    // A check-in sweeps too, or a burst of errands against one host would let every other
    // host's expired connection sit there until the process quit.
    cache.checkIn(kOther, SshSessionRole::Transport, make(3, &tally), 8000);
    cache.checkIn(kHost, SshSessionRole::Transport, make(4, &tally), 9500);
    QCOMPARE(cache.size(), 1);
    QCOMPARE(tally.destroyedIds, (QList<int>{1, 2, 3}));
}

// A session can latch itself dead during the very errand that hands it back — a read that
// delivered its bytes and then timed out reports success — so it is asked on the way in AND
// on the way out.
void TestSshSessionCache::aSessionThatWentUnhealthyIsNeverStoredAndNeverHandedOut()
{
    FakeSession::Tally tally;
    SshSessionCache    cache;

    auto  dead = make(1, &tally);
    auto *deadRaw = static_cast<FakeSession *>(dead.get());
    deadRaw->die();
    cache.checkIn(kHost, SshSessionRole::Transport, std::move(dead), 0);
    QCOMPARE(cache.size(), 0);
    QCOMPARE(tally.destroyed, 1);

    // And one that dies while it sits here.
    auto  later = make(2, &tally);
    auto *laterRaw = static_cast<FakeSession *>(later.get());
    cache.checkIn(kHost, SshSessionRole::Transport, std::move(later), 1);
    laterRaw->die();
    QVERIFY(!cache.checkOut(kHost, SshSessionRole::Transport, 2));
    QCOMPARE(tally.destroyed, 2);
}

void TestSshSessionCache::theCacheIsBoundedAndTheOldestGoesFirst()
{
    FakeSession::Tally tally;
    SshSessionCache    cache(60000, 2);

    cache.checkIn(QStringLiteral("a"), SshSessionRole::Transport, make(1, &tally), 0);
    cache.checkIn(QStringLiteral("b"), SshSessionRole::Transport, make(2, &tally), 1);
    QCOMPARE(cache.size(), 2);
    QCOMPARE(tally.destroyed, 0);

    cache.checkIn(QStringLiteral("c"), SshSessionRole::Transport, make(3, &tally), 2);
    QCOMPARE(cache.size(), 2);
    QCOMPARE(tally.destroyedIds, QList<int>{1});
    QVERIFY(!cache.checkOut(QStringLiteral("a"), SshSessionRole::Transport, 3));
    QVERIFY(cache.checkOut(QStringLiteral("b"), SshSessionRole::Transport, 3));
    QVERIFY(cache.checkOut(QStringLiteral("c"), SshSessionRole::Transport, 3));
}

// The shutdown rule. The release is the obvious half; the LATCH is the half that is easy to
// leave out, and without it a worker finishing after the drain gave up waiting puts a live
// socket back into a process whose application object is going away.
void TestSshSessionCache::closingLetsEveryConnectionGoAndStaysClosed()
{
    FakeSession::Tally tally;
    SshSessionCache    cache;
    cache.checkIn(kHost, SshSessionRole::Transport, make(1, &tally), 0);
    cache.checkIn(kOther, SshSessionRole::Transport, make(2, &tally), 0);

    cache.close();
    QVERIFY(cache.closed());
    QCOMPARE(cache.size(), 0);
    QCOMPARE(tally.destroyed, 2);

    cache.checkIn(kHost, SshSessionRole::Transport, make(3, &tally), 1);
    QCOMPARE(cache.size(), 0);
    QCOMPARE(tally.destroyed, 3); // taken and destroyed, never handed back to the caller
    QVERIFY(!cache.checkOut(kHost, SshSessionRole::Transport, 1));
}

// Destroying a real session writes a farewell packet bounded only by the session timeout —
// twenty seconds — and two of the three places that happens cannot afford it: a checkout
// sweep is on the thread the user is waiting on, and close() is on the application thread
// inside the shutdown drain. Cutting the socket first is what makes destruction prompt, so
// the order matters as much as the call.
void TestSshSessionCache::everySessionIsCutLooseBeforeItIsDestroyed()
{
    FakeSession::Tally tally;
    {
        SshSessionCache cache(1000, 1);
        cache.checkIn(kHost, SshSessionRole::Transport, make(1, &tally), 0);
        cache.checkIn(kOther, SshSessionRole::Transport, make(2, &tally), 1); // over the cap
        cache.checkIn(kHost, SshSessionRole::Transport, make(3, &tally), 5000); // expiry
        cache.checkIn(kOther, SshSessionRole::Transport, make(4, &tally), 5001); // over again
        cache.close();
    }
    QCOMPARE(tally.destroyed, 4);
    QCOMPARE(tally.cutLoose, 4);
    QVERIFY(!tally.cutLooseAfterDestroy);
}

// A worker gives its session an abandon check that captures that worker's shared block.
// Left on, a stored session keeps a finished errand alive and then answers the NEXT one
// with somebody else's cancellation.
void TestSshSessionCache::checkingInDropsTheFinishedErrandsHold()
{
    FakeSession::Tally tally;
    SshSessionCache    cache;
    cache.checkIn(kHost, SshSessionRole::Transport, make(1, &tally), 0);
    QCOMPARE(tally.detached, 1);
}

QTEST_GUILESS_MAIN(TestSshSessionCache)
#include "tst_sshsessioncache.moc"
