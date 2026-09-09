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
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "SocketDetach.h"

#if defined(Q_OS_WIN)
#  include <winsock2.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <cerrno>
#endif

using namespace loftail;

// Why the SSH transport takes its socket away from Qt (SocketDetach.h).
//
// This pins a property of QT, not of loftail and not of libssh2, which is why it is
// ungated and runs in a build with no SSH support at all: the bug it prevents cost a
// working remote-log feature, and the only cheap way to notice Qt changing underneath
// is a test that fails when it does.
//
// The failure it describes was reported as "ssh doesn't work": a correct password was
// accepted, then the open hung and ended with libssh2's "Timed out waiting on socket".
// The cause is below — Qt had already eaten the bytes libssh2 was waiting for.
//
// Loopback only. No network, no server, no credentials.
class TestSocketDetach : public QObject
{
    Q_OBJECT

private:
    QTcpServer  m_server;
    QTcpSocket  m_client;
    QTcpSocket *m_peer = nullptr;

    // A connected loopback pair, with `m_client` the one whose descriptor a third-party
    // library would be handed.
    bool connectPair()
    {
        if (!m_server.listen(QHostAddress::LocalHost, 0))
            return false;
        m_client.connectToHost(QHostAddress::LocalHost, m_server.serverPort());
        if (!m_client.waitForConnected(3000))
            return false;
        if (!m_server.waitForNewConnection(3000))
            return false;
        m_peer = m_server.nextPendingConnection();
        return m_peer != nullptr;
    }

#if !defined(Q_OS_WIN)
    // A PORT WHOSE ACCEPT QUEUE IS FULL, so that a connect to it STALLS instead of being
    // refused or accepted: the kernel has nowhere to put the connection, drops the SYN,
    // and the client retries about a second later. That is a connect slower than one
    // slice of anybody's connect loop, built out of loopback and a listen backlog — no
    // network, no unreachable address, no timing luck, and nothing that depends on how
    // busy the runner is.
    //
    // POSIX only: it needs the raw listening socket, because QTcpServer accepts
    // everything that arrives and would keep the queue empty.
    bool stallingPortOpen()
    {
        m_stall = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_stall < 0)
            return false;
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(m_stall, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
            return false;
        socklen_t length = sizeof(address);
        if (::getsockname(m_stall, reinterpret_cast<sockaddr *>(&address), &length) != 0)
            return false;
        m_stallPort = quint16(ntohs(address.sin_port));
        // The smallest backlog the kernel will take. It is a floor rather than a
        // promise, which is why the queue is filled by counting connections rather than
        // by trusting this number.
        if (::listen(m_stall, 0) != 0)
            return false;
        for (int i = 0; i < 8; ++i) {
            const int filler = ::socket(AF_INET, SOCK_STREAM, 0);
            if (filler < 0)
                return false;
            ::fcntl(filler, F_SETFL, O_NONBLOCK);
            ::connect(filler, reinterpret_cast<sockaddr *>(&address), sizeof(address));
            m_fillers.append(filler);
        }
        return true;
    }

    // Take one connection off the queue, which lets the next SYN retry through.
    void freeOneStallSlot()
    {
        sockaddr_in from {};
        socklen_t length = sizeof(from);
        const int taken = ::accept(m_stall, reinterpret_cast<sockaddr *>(&from), &length);
        if (taken >= 0)
            m_fillers.append(taken);
    }

    void closeStallingPort()
    {
        for (int fd : m_fillers)
            ::close(fd);
        m_fillers.clear();
        if (m_stall >= 0)
            ::close(m_stall);
        m_stall = -1;
    }

    int          m_stall = -1;
    quint16      m_stallPort = 0;
    QList<int>   m_fillers;
#endif

    static void spinEventLoop(int ms)
    {
        // What a modal password dialog does, and what the main window does forever.
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    }

    // Whether a read on `fd` would park a blocking reader — "nothing yet, ask again" —
    // as opposed to ending it, however it ends. The two platforms end it differently and
    // only the ending matters (SshSession::abort).
    static bool readWouldBlock(qintptr fd)
    {
        char buffer[64];
        const qint64 got = rawRead(fd, buffer, sizeof(buffer));
        if (got >= 0)
            return false; // data, or end of stream: either way the reader moves on
#if defined(Q_OS_WIN)
        return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
    }

    // A read that does not block, the way libssh2 polls a non-blocking descriptor.
    static qint64 rawRead(qintptr fd, char *buffer, qint64 size)
    {
#if defined(Q_OS_WIN)
        u_long nonBlocking = 1;
        ::ioctlsocket(SOCKET(fd), FIONBIO, &nonBlocking);
        return ::recv(SOCKET(fd), buffer, int(size), 0);
#else
        return ::recv(int(fd), buffer, size_t(size), MSG_DONTWAIT);
#endif
    }

private slots:
    void cleanup()
    {
        m_client.abort();
        m_server.close();
        m_peer = nullptr;
#if !defined(Q_OS_WIN)
        closeStallingPort();
#endif
    }

    void qtStealsBytesFromTheDescriptorItHolds();
    void aDetachedDescriptorKeepsItsBytes();
    void aDetachedSocketIsStillTwoWay();
    void shuttingDownUnblocksAReadWithoutFreeingTheDescriptor();
    void aTimedOutWaitForConnectedAbandonsTheAttempt();
    void aConnectSlowerThanOneSliceStillConnects();
    void aConnectNobodyWantsIsGivenUpBetweenSlices();
    void aRefusedConnectIsReportedRatherThanWaitedOut();
};

void TestSocketDetach::qtStealsBytesFromTheDescriptorItHolds()
{
    // THE BUG, stated as a fact about Qt. If this ever starts failing, Qt has changed
    // and SocketDetach is no longer necessary — which is worth being told.
    QVERIFY(connectPair());
    const qintptr fd = m_client.socketDescriptor();
    QVERIFY(fd >= 0);

    m_peer->write("SSH-2.0-loftail\r\n");
    QVERIFY(m_peer->waitForBytesWritten(2000));

    spinEventLoop(250);

    // Qt has taken them, because it still has a notifier on this descriptor…
    QVERIFY2(m_client.bytesAvailable() > 0, "Qt did not buffer — the premise is gone");

    char buffer[64];
    const qint64 got = rawRead(fd, buffer, sizeof(buffer));
    // …so the library holding the same descriptor sees nothing and waits. libssh2
    // waits until its timeout and then reports "Timed out waiting on socket".
    QVERIFY2(got < 0, "a raw read still saw the bytes — SocketDetach may be unnecessary");
}

void TestSocketDetach::aDetachedDescriptorKeepsItsBytes()
{
    QVERIFY(connectPair());

    const qintptr owned = detachSocketFromQt(m_client);
    QVERIFY(owned >= 0);
    // Qt has genuinely let go, but the connection is untouched — abort() drops Qt's own
    // descriptor without the FIN that disconnectFromHost() would send.
    QCOMPARE(m_client.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(m_peer->state(), QAbstractSocket::ConnectedState);

    m_peer->write("SSH-2.0-loftail\r\n");
    QVERIFY(m_peer->waitForBytesWritten(2000));

    spinEventLoop(250); // the same event loop that broke it before

    char buffer[64];
    const qint64 got = rawRead(owned, buffer, sizeof(buffer));
    QVERIFY2(got > 0, "the detached descriptor lost its bytes anyway");
    QCOMPARE(QByteArray(buffer, int(got)), QByteArray("SSH-2.0-loftail\r\n"));

    closeDetachedSocket(owned);
}

void TestSocketDetach::aDetachedSocketIsStillTwoWay()
{
    // Reading is only half of a session: the handshake also has to be able to write.
    QVERIFY(connectPair());
    const qintptr owned = detachSocketFromQt(m_client);
    QVERIFY(owned >= 0);

#if defined(Q_OS_WIN)
    QCOMPARE(::send(SOCKET(owned), "hello\n", 6, 0), 6);
#else
    QCOMPARE(::send(int(owned), "hello\n", 6, 0), ssize_t(6));
#endif
    QVERIFY(m_peer->waitForReadyRead(2000));
    QCOMPARE(m_peer->readAll(), QByteArray("hello\n"));

    closeDetachedSocket(owned);
}

void TestSocketDetach::shuttingDownUnblocksAReadWithoutFreeingTheDescriptor()
{
    // How a connect in progress is abandoned when its tab is closed (SshSession::abort).
    // A blocking read inside libssh2 would otherwise sit there until the session timeout,
    // which is the whole twenty seconds the async open exists to stop anyone waiting for.
    //
    // Two halves, and the second is the one that makes it safe to call across threads:
    // the read must return, and the descriptor must still be VALID afterwards. Closing
    // it here instead would free the number while another thread is inside libssh2
    // holding it, and the next socket the process opened could inherit it.
    QVERIFY(connectPair());
    const qintptr owned = detachSocketFromQt(m_client);
    QVERIFY(owned >= 0);

    // Before: an idle connected socket says "nothing yet, ask again" — which is exactly
    // what a blocking reader inside libssh2 is sitting in.
    QVERIFY2(readWouldBlock(owned), "an idle connected socket should have nothing to say");

    shutdownDetachedSocket(owned);

    // After: the read stops saying that, so a blocking reader stops waiting. WHAT it says
    // instead differs by platform and does not matter — POSIX reports end of stream (0),
    // Winsock fails the call with WSAESHUTDOWN, and libssh2 abandons the read either way.
    // Asserting the POSIX answer specifically is what failed on Windows.
    QVERIFY2(!readWouldBlock(owned), "a shut-down socket still parked the reader");

    // Still a descriptor this process owns — shutdown() is not close(). Asking for a
    // socket option is the cheapest question that distinguishes the two.
    //
    // The length is declared per platform because its TYPE differs: `socklen_t` is
    // POSIX, and Winsock's getsockopt() takes a plain `int *`. Declaring it once outside
    // the #if is what broke the Windows build.
    int optval = 0;
#if defined(Q_OS_WIN)
    int optlen = sizeof(optval);
    const int rc = ::getsockopt(SOCKET(owned), SOL_SOCKET, SO_TYPE,
                                reinterpret_cast<char *>(&optval), &optlen);
#else
    socklen_t optlen = sizeof(optval);
    const int rc = ::getsockopt(int(owned), SOL_SOCKET, SO_TYPE, &optval, &optlen);
#endif
    QCOMPARE(rc, 0);

    // Idempotent: requestStop() may well be called more than once.
    shutdownDetachedSocket(owned);
    closeDetachedSocket(owned);
}


// A slice of the connect wait, small enough that a whole test is bounded by the connect
// rather than by the slicing, and far below the ~1 s a stalled SYN takes to be retried.
static constexpr int kSliceMs = 250;

void TestSocketDetach::aTimedOutWaitForConnectedAbandonsTheAttempt()
{
    // WHY awaitSocketConnected() EXISTS, stated as a fact about Qt — the shape of
    // qtStealsBytesFromTheDescriptorItHolds above, one call over. If this ever starts
    // failing, Qt has begun keeping the attempt alive across its own timeout and a
    // connect really can be sliced with waitForConnected(), which is worth being told.
    //
    // Until then: the expiry does not merely report "not yet". It sets
    // SocketTimeoutError, drops the socket to UnconnectedState and resets the socket
    // layer, so the connection being waited for is gone and every later call returns
    // false at once. A loop that reads that error as "still trying" therefore kills the
    // connect after its first slice and then spins out the whole budget — which is how
    // the first open of any host slower than 250 ms reported "Socket operation timed
    // out" about a machine that answers ssh in 40 ms.
#if defined(Q_OS_WIN)
    QSKIP("needs a raw listening socket to stall a connect");
#else
    QVERIFY(stallingPortOpen());

    QTcpSocket socket;
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), m_stallPort);
    QVERIFY(!socket.waitForConnected(kSliceMs));
    QCOMPARE(socket.error(), QAbstractSocket::SocketTimeoutError);
    QCOMPARE(socket.state(), QAbstractSocket::UnconnectedState);

    // And it stays gone: the second slice does not resume anything, it answers off a
    // socket that is no longer connecting. Immediately, which is the busy-spin.
    QElapsedTimer spun;
    spun.start();
    QVERIFY(!socket.waitForConnected(kSliceMs));
    QVERIFY2(spun.elapsed() < kSliceMs / 2,
             "the second wait actually waited — Qt may have kept the attempt");
#endif
}

void TestSocketDetach::aConnectSlowerThanOneSliceStillConnects()
{
    // THE FIX. A connect that needs several slices has to survive them, or a host that
    // is merely not on the local network cannot be opened at all.
#if defined(Q_OS_WIN)
    QSKIP("needs a raw listening socket to stall a connect");
#else
    QVERIFY(stallingPortOpen());

    QTcpSocket socket;
    QElapsedTimer elapsed;
    elapsed.start();
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), m_stallPort);
    // Room for the connection only once the wait is under way, so it genuinely spans
    // more than one slice however fast the runner is.
    QTimer::singleShot(kSliceMs, this, [this] { freeOneStallSlot(); });

    QCOMPARE(awaitSocketConnected(socket, 20000, kSliceMs, nullptr), SocketWait::Connected);
    QCOMPARE(socket.state(), QAbstractSocket::ConnectedState);
    QVERIFY2(elapsed.elapsed() > kSliceMs,
             "the connect did not outlast a slice — the fixture stalled nothing");

    // And what the SSH transport does next still works on it.
    const qintptr owned = detachSocketFromQt(socket);
    QVERIFY(owned >= 0);
    closeDetachedSocket(owned);
#endif
}

void TestSocketDetach::aConnectNobodyWantsIsGivenUpBetweenSlices()
{
    // Closing a tab while its host is not answering costs a slice, not the whole connect
    // budget — which is the reason the wait is sliced at all (SshSession::connectTo).
#if defined(Q_OS_WIN)
    QSKIP("needs a raw listening socket to stall a connect");
#else
    QVERIFY(stallingPortOpen());

    QTcpSocket socket;
    QElapsedTimer elapsed;
    elapsed.start();
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), m_stallPort);

    bool wanted = true;
    QTimer::singleShot(kSliceMs, this, [&wanted] { wanted = false; });

    QCOMPARE(awaitSocketConnected(socket, 20000, kSliceMs, [&wanted] { return !wanted; }),
             SocketWait::Abandoned);
    QVERIFY2(elapsed.elapsed() < 20000 / 4, "gave up on the budget rather than on the ask");

    // The budget is honoured too, and is what a host that never answers costs.
    QTcpSocket patient;
    elapsed.restart();
    patient.connectToHost(QHostAddress(QHostAddress::LocalHost), m_stallPort);
    QCOMPARE(awaitSocketConnected(patient, 3 * kSliceMs, kSliceMs, nullptr),
             SocketWait::TimedOut);
    QVERIFY(elapsed.elapsed() >= 3 * kSliceMs);
#endif
}

void TestSocketDetach::aRefusedConnectIsReportedRatherThanWaitedOut()
{
    // The other end of it: a refusal is not a slow connect and must not be paid for as
    // one. Qt has already phrased it, so the caller reports socket.errorString().
    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost, 0));
    const quint16 port = closed.serverPort();
    closed.close();

    QTcpSocket socket;
    QElapsedTimer elapsed;
    elapsed.start();
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    QCOMPARE(awaitSocketConnected(socket, 20000, kSliceMs, nullptr), SocketWait::Failed);
    QVERIFY(elapsed.elapsed() < 20000 / 4);
    QVERIFY(!socket.errorString().isEmpty());
}

QTEST_MAIN(TestSocketDetach)
#include "tst_socketdetach.moc"
