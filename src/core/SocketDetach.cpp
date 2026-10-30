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

#include "SocketDetach.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTcpSocket>
#include <QTimer>

#if defined(Q_OS_WIN)
#  include <winsock2.h>
#  include <windows.h>
#else
#  include <cerrno>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace loftail {

namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::SocketDetach)
};

// Whether the kernel says `descriptor` has a peer — which every connected TCP socket does —
// and, when it has not, why: the socket's pending error. Asked of a socket Qt has called
// connected (SocketDetach.h on why Qt's word is not enough).
bool kernelSeesAPeer(qintptr descriptor, QString *why)
{
    sockaddr_storage peer {};
    int error = 0;
#if defined(Q_OS_WIN)
    int length = sizeof(peer);
    if (::getpeername(SOCKET(descriptor), reinterpret_cast<sockaddr *>(&peer), &length) == 0)
        return true;
    int size = sizeof(error);
    ::getsockopt(SOCKET(descriptor), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error),
                 &size);
    const bool refused = error == WSAECONNREFUSED;
#else
    socklen_t length = sizeof(peer);
    if (::getpeername(int(descriptor), reinterpret_cast<sockaddr *>(&peer), &length) == 0)
        return true;
    socklen_t size = sizeof(error);
    ::getsockopt(int(descriptor), SOL_SOCKET, SO_ERROR, &error, &size);
    const bool refused = error == ECONNREFUSED;
#endif
    // Qt's own wording for a refusal, so the sentence does not depend on which of the two
    // routes reported it; anything else is named by its number, there being no portable
    // thread-safe way to phrase an errno here and nothing the reader would do differently.
    if (refused)
        *why = Tr::tr("Connection refused");
    else if (error != 0)
        *why = Tr::tr("The connection was not established (system error %1).").arg(error);
    else
        *why = Tr::tr("The connection was not established.");
    return false;
}

} // namespace

SocketWait awaitSocketConnected(QTcpSocket &socket, int timeoutMs, int sliceMs,
                                const std::function<bool()> &abandoned, QString *failure)
{
    QElapsedTimer elapsed;
    elapsed.start();

    for (;;) {
        // Asked FIRST, so a socket that connected or failed synchronously — a loopback
        // peer, a literal address, a name lookup already in the cache — never enters a
        // loop it would have to be woken out of.
        switch (socket.state()) {
        case QAbstractSocket::ConnectedState: {
            // ...and believed only with a peer (SocketDetach.h): Qt 6.8 on macOS 15.8 calls
            // a refused connect connected.
            QString why;
            if (kernelSeesAPeer(socket.socketDescriptor(), &why))
                return SocketWait::Connected;
            socket.abort();
            if (failure)
                *failure = why;
            return SocketWait::Failed;
        }
        case QAbstractSocket::UnconnectedState:
            // Refused, unreachable, or a name that does not resolve. Qt has phrased it.
            if (failure)
                *failure = socket.errorString();
            return SocketWait::Failed;
        default:
            break;
        }

        if (abandoned && abandoned())
            return SocketWait::Abandoned;

        const qint64 left = qint64(timeoutMs) - elapsed.elapsed();
        if (left <= 0)
            return SocketWait::TimedOut;

        // The two signals are what makes a healthy connect cost one turn of this loop
        // rather than a whole slice, and the timer is what bounds a connect that is
        // still in flight. `errorOccurred` does not necessarily mean the attempt is
        // over — QAbstractSocket works through the addresses a name resolved to — so the
        // decision is taken off state() at the top rather than off the signal that woke
        // us.
        QEventLoop loop;
        const QMetaObject::Connection onConnected =
            QObject::connect(&socket, &QTcpSocket::connected, &loop, &QEventLoop::quit);
        const QMetaObject::Connection onError =
            QObject::connect(&socket, &QAbstractSocket::errorOccurred, &loop, &QEventLoop::quit);
        QTimer::singleShot(int(qMin(qint64(sliceMs), left)), &loop, &QEventLoop::quit);
        loop.exec();
        QObject::disconnect(onConnected);
        QObject::disconnect(onError);
    }
}

qintptr detachSocketFromQt(QTcpSocket &socket)
{
    const qintptr original = socket.socketDescriptor();
    if (original < 0)
        return -1;

#if defined(Q_OS_WIN)
    // Windows sockets are not file descriptors and cannot be dup()'d. WSADuplicateSocket
    // is the documented equivalent, and duplicating into the calling process is a
    // supported use of it.
    WSAPROTOCOL_INFOW info;
    if (::WSADuplicateSocketW(SOCKET(original), ::GetCurrentProcessId(), &info) != 0)
        return -1;
    const SOCKET copy = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                     FROM_PROTOCOL_INFO, &info, 0, 0);
    // abort(), not disconnectFromHost(): the latter is a graceful shutdown that would
    // send FIN and tear down the connection we are trying to keep. abort() drops Qt's
    // own descriptor without touching the peer.
    socket.abort();
    return copy == INVALID_SOCKET ? -1 : qintptr(copy);
#else
    const int copy = ::dup(int(original));
    socket.abort();
    return copy < 0 ? -1 : qintptr(copy);
#endif
}

void shutdownDetachedSocket(qintptr descriptor)
{
    if (descriptor < 0)
        return;
#if defined(Q_OS_WIN)
    ::shutdown(SOCKET(descriptor), SD_BOTH);
#else
    ::shutdown(int(descriptor), SHUT_RDWR);
#endif
}

void closeDetachedSocket(qintptr descriptor)
{
    if (descriptor < 0)
        return;
#if defined(Q_OS_WIN)
    ::closesocket(SOCKET(descriptor));
#else
    ::close(int(descriptor));
#endif
}

} // namespace loftail
