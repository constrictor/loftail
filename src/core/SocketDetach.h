#pragma once

#include <QtGlobal>

QT_BEGIN_NAMESPACE
class QTcpSocket;
QT_END_NAMESPACE

namespace loftail {

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

} // namespace loftail
