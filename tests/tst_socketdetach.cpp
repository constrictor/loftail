#include <QtTest>

#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "SocketDetach.h"

#if defined(Q_OS_WIN)
#  include <winsock2.h>
#else
#  include <sys/socket.h>
#  include <unistd.h>
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

    static void spinEventLoop(int ms)
    {
        // What a modal password dialog does, and what the main window does forever.
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
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
    }

    void qtStealsBytesFromTheDescriptorItHolds();
    void aDetachedDescriptorKeepsItsBytes();
    void aDetachedSocketIsStillTwoWay();
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

QTEST_MAIN(TestSocketDetach)
#include "tst_socketdetach.moc"
