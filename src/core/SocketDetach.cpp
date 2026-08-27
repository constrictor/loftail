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

#include <QTcpSocket>

#if defined(Q_OS_WIN)
#  include <winsock2.h>
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace loftail {

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
