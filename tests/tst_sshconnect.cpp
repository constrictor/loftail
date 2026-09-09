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

#include <QElapsedTimer>
#include <QTemporaryDir>

#include <atomic>

// std::thread and not QThread: ARCHITECTURE.md §13.1, and here for a second reason —
// the fixture's helper must run whether or not the code under test happens to be
// turning an event loop, which is precisely the thing in question.
#include <chrono>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "RemoteLocation.h"
#include "SourceFetcher.h"
#include "SshFetcher.h"
#include "RetryCountdown.h"
#include "SshSession.h"

using namespace loftail;

// SshSession::connectTo()'s TCP half, WITH NO SSH SERVER ANYWHERE.
//
// tst_sshlive needs three containers and is skipped everywhere else, including in the
// ordinary CI job that builds this transport — so the one thing the connect is asked to
// do before libssh2 is reached had no coverage that ever ran. What it costs is in
// CLAUDE.md ("A CONNECT CANNOT BE SLICED WITH `waitForConnected()`"): the 250 ms slices
// were taken with QTcpSocket::waitForConnected(), whose expiry resets the socket layer,
// so every connect slower than one slice was killed after 250 ms, busy-spun out the rest
// of its 20 s budget and reported Qt's own "Socket operation timed out". A cold name
// lookup is 250 ms by itself, so the first open of a host that was not on the local
// network could not succeed — and it never got as far as the host-key or password
// prompt, which is what the user saw.
//
// tst_socketdetach pins the wait itself, ungated and one layer down. THIS pins the
// WIRING: that connectTo() reaches libssh2 at all over a connection slower than a slice.
// The discriminator is deliberately not a message — a phrasing changes and a tr() string
// is not a contract — but the SSH banner arriving at the far end, which is a thing that
// either happened or did not.
//
// The far end is a raw listening socket that says nothing back, so the handshake fails.
// That is the point: the case is about how far the connect got, not about logging in.
class TestSshConnect : public QObject
{
    Q_OBJECT

private:
    // A PORT WHOSE ACCEPT QUEUE IS FULL, so a connect to it STALLS rather than being
    // refused or accepted: the kernel has nowhere to put it, drops the SYN, and the
    // client retries about a second later. That is a connect slower than any slice,
    // built out of loopback and a listen backlog — no network, no unreachable address,
    // and nothing that depends on how busy the runner is. tst_socketdetach builds the
    // same fixture for the same reason; it is small enough that sharing it between two
    // binaries would cost more than it saves.
    int        m_listen = -1;
    quint16    m_port = 0;
    QList<int> m_held;

    bool stallingPortOpen()
    {
        m_listen = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listen < 0)
            return false;
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(m_listen, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
            return false;
        socklen_t length = sizeof(address);
        if (::getsockname(m_listen, reinterpret_cast<sockaddr *>(&address), &length) != 0)
            return false;
        m_port = quint16(ntohs(address.sin_port));
        if (::listen(m_listen, 0) != 0)
            return false;
        for (int i = 0; i < 8; ++i) {
            const int filler = ::socket(AF_INET, SOCK_STREAM, 0);
            if (filler < 0)
                return false;
            ::fcntl(filler, F_SETFL, O_NONBLOCK);
            ::connect(filler, reinterpret_cast<sockaddr *>(&address), sizeof(address));
            m_held.append(filler);
        }
        return true;
    }

    // Take one connection off the queue, which lets the next SYN retry through. WHICH
    // connection comes back is not this fixture's to choose — accept() hands over the
    // oldest waiting, which is one of the fillers — so the caller looks for the one it
    // wants among everything that can be accepted afterwards.
    int acceptOne()
    {
        sockaddr_in from {};
        socklen_t length = sizeof(from);
        const int taken = ::accept(m_listen, reinterpret_cast<sockaddr *>(&from), &length);
        if (taken >= 0)
            m_held.append(taken);
        return taken;
    }

    // What the first client to say anything said. The queue holds the fillers as well,
    // and they say nothing at all, so this is how the connection under test is told from
    // them: it is the one that speaks first.
    QByteArray firstThingSaid(int timeoutMs)
    {
        ::fcntl(m_listen, F_SETFL, O_NONBLOCK);
        for (;;) {
            const int taken = acceptOne();
            if (taken < 0)
                break;
            const QByteArray said = readSome(taken, 50);
            if (!said.isEmpty())
                return said;
        }
        // Nothing pending has spoken; give the one still arriving its chance.
        for (int waited = 0; waited < timeoutMs; waited += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const int taken = acceptOne();
            if (taken < 0)
                continue;
            const QByteArray said = readSome(taken, 50);
            if (!said.isEmpty())
                return said;
        }
        // Everything already accepted may have spoken since.
        for (int fd : m_held) {
            const QByteArray said = readSome(fd, 0);
            if (!said.isEmpty())
                return said;
        }
        return {};
    }

    static QByteArray readSome(int fd, int timeoutMs)
    {
        pollfd waiting { fd, POLLIN, 0 };
        if (::poll(&waiting, 1, timeoutMs) <= 0)
            return {};
        char buffer[256];
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        return got > 0 ? QByteArray(buffer, int(got)) : QByteArray();
    }

    RemoteLocation stallingLocation() const
    {
        RemoteLocation where;
        where.user = QStringLiteral("loftail");
        where.host = QStringLiteral("127.0.0.1");
        where.port = int(m_port);
        where.path = QStringLiteral("/tmp/never-read.log");
        return where;
    }

private slots:
    void cleanup()
    {
        for (int fd : m_held)
            ::close(fd);
        m_held.clear();
        if (m_listen >= 0)
            ::close(m_listen);
        m_listen = -1;
    }

    void aConnectSlowerThanOneSliceReachesTheHandshake();
    void aRefusedConnectIsReportedWithoutWaitingOutTheBudget();
    void aConnectNobodyStillWantsIsGivenUpOnLongBeforeTheBudget();
    void aFetcherThatCouldNotReachItsHostPublishesWhenItWillTryAgain();
};

void TestSshConnect::aConnectSlowerThanOneSliceReachesTheHandshake()
{
    QVERIFY(stallingPortOpen());

    // Room for the connection only after a slice has passed, so it genuinely spans more
    // than one however fast the runner is. From a THREAD and not a QTimer: a timer would
    // only fire if the code under test turns an event loop, which is the very thing this
    // is trying to establish rather than something it may assume.
    std::atomic_int served { -1 };
    std::thread opener([this, &served] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        served = acceptOne();
    });

    SshSession session;
    QString error;
    SshSession::Failure failure = SshSession::Failure::None;
    QElapsedTimer elapsed;
    elapsed.start();
    // A short budget: the far end never answers the banner, so the handshake spends the
    // whole of it before giving up. That is the cost of the case, and it is why this
    // does not use the 20 s a real connect gets.
    const bool ok = session.connectTo(stallingLocation(), nullptr, 3000, &error, &failure);
    opener.join();

    QVERIFY2(!ok, "a raw listening socket cannot complete an SSH handshake");
    QVERIFY2(served.load() >= 0, "the fixture never accepted anything — nothing was stalled");
    QVERIFY2(elapsed.elapsed() > 400,
             "the connect did not outlast a slice — the fixture stalled nothing");

    // THE ASSERTION. libssh2 opens with its version banner, so these bytes are proof
    // that connectTo() got through the TCP connect and into the handshake. Sliced with
    // waitForConnected() nothing is ever sent: the attempt is dead 250 ms in and the
    // rest of the budget is spent spinning on a socket that is no longer connecting.
    const QByteArray banner = firstThingSaid(2000);
    QVERIFY2(banner.startsWith("SSH-2.0"),
             qPrintable(QStringLiteral("the far end never saw an SSH banner (got %1); "
                                       "connectTo said: %2")
                            .arg(QString::fromLatin1(banner.left(40)), error)));
}

void TestSshConnect::aRefusedConnectIsReportedWithoutWaitingOutTheBudget()
{
    // The other end of it: a refusal is not a slow connect and must not be paid for as
    // one — which is what the busy spin did to it, since a socket Qt has reset answers
    // every later slice at once and only the elapsed budget ends the loop.
    const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    QVERIFY(probe >= 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    QCOMPARE(::bind(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    socklen_t length = sizeof(address);
    QCOMPARE(::getsockname(probe, reinterpret_cast<sockaddr *>(&address), &length), 0);
    RemoteLocation where;
    where.user = QStringLiteral("loftail");
    where.host = QStringLiteral("127.0.0.1");
    where.port = int(ntohs(address.sin_port));
    where.path = QStringLiteral("/tmp/never-read.log");
    ::close(probe); // nothing is listening on it now

    SshSession session;
    QString error;
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(!session.connectTo(where, nullptr, 20000, &error));
    QVERIFY2(elapsed.elapsed() < 5000,
             qPrintable(QStringLiteral("a refusal took %1 ms").arg(elapsed.elapsed())));
    QVERIFY(!error.isEmpty());
}

void TestSshConnect::aConnectNobodyStillWantsIsGivenUpOnLongBeforeTheBudget()
{
    // Why the wait is sliced at all (M17): closing a tab whose host is not answering
    // costs a slice, not the connect budget. The slices survived the fix — what changed
    // is that they no longer kill the connection they are slicing.
    QVERIFY(stallingPortOpen());

    SshSession session;
    std::atomic_bool wanted { true };
    session.setAbandonCheck([&wanted] { return !wanted.load(); });
    std::thread giveUp([&wanted] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        wanted = false;
    });

    QString error;
    QElapsedTimer elapsed;
    elapsed.start();
    const bool ok = session.connectTo(stallingLocation(), nullptr, 20000, &error);
    giveUp.join();

    QVERIFY(!ok);
    QVERIFY2(elapsed.elapsed() < 5000,
             qPrintable(QStringLiteral("gave up after %1 ms of a 20000 ms budget")
                            .arg(elapsed.elapsed())));
}


void TestSshConnect::aFetcherThatCouldNotReachItsHostPublishesWhenItWillTryAgain()
{
    // THE PUBLISH ITSELF, against a real SshFetcher and still with no server anywhere.
    // The countdown a waiting tab shows is rendered from FetchStatus::retryAtMs, and
    // everything above this line is driven through tests/FakeFetcher.h — so the one
    // statement that fills the field in production, inside tailLoop() immediately before
    // it sleeps, is reachable only from here and from the container harness. A refused
    // port is enough: what is being asked is not how the connect failed but that a
    // fetcher which is going to try again says when.
    const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    QVERIFY(probe >= 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    QCOMPARE(::bind(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    socklen_t length = sizeof(address);
    QCOMPARE(::getsockname(probe, reinterpret_cast<sockaddr *>(&address), &length), 0);
    const quint16 port = ntohs(address.sin_port);
    ::close(probe); // bound and released: nothing is listening on it now

    const auto location = RemoteLocation::parse(
        QStringLiteral("ssh://127.0.0.1:%1/var/log/app.log").arg(port));
    QVERIFY(location.has_value());

    QString error;
    auto fetcher = makeSshFetcher(*location, &error);
    QVERIFY2(fetcher, qPrintable(error));

    QTemporaryDir spoolDir;
    QVERIFY(spoolDir.isValid());
    QVERIFY2(fetcher->start(spoolDir.path(), &error), qPrintable(error));

    // The first attempt is refused at once, and the loop then publishes its deadline
    // before it waits. Generous, because what is being waited for is a worker thread's
    // turn round a loop and not a network.
    QTRY_VERIFY_WITH_TIMEOUT(fetcher->status().retryAtMs > 0, 10000);

    const FetchStatus status = fetcher->status();
    // WHAT KIND OF DEADLINE IT IS. In the future, on the clock the renderer reads, and
    // within the slow cadence — a value taken off a wall clock or published as a
    // remaining duration would pass none of these.
    const qint64 now = fetchMonotonicMs();
    QVERIFY2(status.retryAtMs > now,
             qPrintable(QStringLiteral("deadline %1 is not ahead of now %2")
                            .arg(status.retryAtMs)
                            .arg(now)));
    QVERIFY2(status.retryAtMs - now <= 15000,
             qPrintable(QStringLiteral("deadline is %1 ms out, which is not a retry "
                                       "cadence")
                            .arg(status.retryAtMs - now)));
    // And it is a state that RETRIES: a fetcher that had given up would publish none.
    QVERIFY(status.state == FetchStatus::State::Waiting
            || status.state == FetchStatus::State::Error);
    QVERIFY(!status.error.isEmpty());

    // Non-blocking, exactly as every other caller must treat it (SourceFetcher.h).
    fetcher->requestStop();
}

QTEST_MAIN(TestSshConnect)
#include "tst_sshconnect.moc"
