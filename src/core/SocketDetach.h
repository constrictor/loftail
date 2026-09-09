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

#pragma once

#include <QtGlobal>

#include <functional>

QT_BEGIN_NAMESPACE
class QTcpSocket;
QT_END_NAMESPACE

namespace loftail {

// What awaitSocketConnected() settled on.
enum class SocketWait {
    Connected,  // the socket is in ConnectedState and its descriptor may be taken
    Failed,     // the connection was refused, unreachable, unresolvable — socket.errorString()
    TimedOut,   // `timeoutMs` ran out with the attempt still in flight
    Abandoned,  // `abandoned()` answered true between slices
};

// Wait for a connect started with QTcpSocket::connectToHost(), in slices of `sliceMs`,
// giving up after `timeoutMs` or as soon as `abandoned` (which may be null) says so.
//
// NOT QTcpSocket::waitForConnected(), AND THAT IS THE WHOLE POINT OF THIS FUNCTION —
// pinned, like everything else in this header, by a test that fails if Qt ever changes
// its mind (tst_socketdetach). waitForConnected() does not keep connecting through its
// own timeout: when it expires it sets SocketTimeoutError, puts the socket into
// UnconnectedState and RESETS THE SOCKET LAYER, so the attempt is dead and every later
// call returns false at once off an unconnected socket. Slicing a connect with it
// therefore does not slice anything — it kills the connection after the first slice and
// then busy-spins for the rest of the budget, reporting "Socket operation timed out"
// about a host that answers ssh in 40 ms. Any connect slower than one slice, which a
// cold name lookup can be on its own, could never succeed at all.
//
// A nested QEventLoop is what waits without destroying anything. It runs the same Qt
// machinery waitForConnected() runs — so this, like that, must happen BEFORE the
// descriptor is detached below and never after — and it drives the name lookup as well
// as the connect, which is why the caller may pass a host name rather than an address.
//
// It needs the Qt event dispatcher of the thread it is called on, exactly as
// QAbstractSocket does everywhere else (see startSshWorker's comment on why the SSH
// workers are QThreads and not std::threads).
SocketWait awaitSocketConnected(QTcpSocket &socket, int timeoutMs, int sliceMs,
                                const std::function<bool()> &abandoned);

// Take a connected socket AWAY FROM Qt, leaving the connection itself intact.
//
// THIS IS NOT AN OPTIMISATION — for the SSH transport it is the difference between
// working and not. A QTcpSocket keeps a read notifier armed on its descriptor, so the
// moment ANY Qt event loop runs, Qt drains the socket into its own buffer. A third-party
// library reading the same descriptor then finds nothing, blocks, and eventually times
// out, with the bytes it needed sitting in a QByteArray it cannot see. libssh2 reports
// exactly that as "Timed out waiting on socket".
//
// Two event loops did it in loftail, and both are ordinary rather than exotic. The modal
// password prompt runs one in the middle of authentication — which is why key and agent
// logins were unaffected and password logins failed immediately after the password was
// accepted. And once connected, the session is handed to a fetcher thread while the
// QTcpSocket still belongs to the GUI thread, so every turn of the main event loop could
// take bytes from a tail already in progress.
//
// Duplicating the descriptor and letting Qt close its own copy fixes both: a socket
// lives as long as any descriptor references it, and the survivor is one Qt has never
// heard of. QTcpSocket still resolves the name, applies the connect timeout and phrases
// the connection error — it just does not get to keep the socket afterwards.
//
// ALWAYS COMPILED, unlike the SSH transport that needs it, so the behaviour it depends
// on is pinned by a test in every build configuration (tst_socketdetach). The property
// belongs to Qt, not to libssh2, and a Qt that stopped behaving this way would silently
// make the whole workaround unnecessary — worth learning from a failing test.
//
// Returns the caller-owned descriptor, or -1 if it could not be duplicated. `socket` is
// left unconnected either way.
qintptr detachSocketFromQt(QTcpSocket &socket);

// Close a descriptor returned by detachSocketFromQt(). A no-op for -1.
void closeDetachedSocket(qintptr descriptor);

// Break a detached socket's connection WITHOUT closing the descriptor, so that a
// blocking read or write on another thread returns instead of waiting out its timeout.
//
// This is how a connect in progress is abandoned when the tab it belongs to is closed
// (SshSession::abort). Shutting down rather than closing is the whole point: the
// descriptor stays valid, so the thread that owns it can still fail, report and tear
// down in its own time, and no descriptor number is freed for something else to reuse
// while another thread is inside libssh2 holding it.
//
// A no-op for -1. Safe to call more than once.
void shutdownDetachedSocket(qintptr descriptor);

} // namespace loftail
