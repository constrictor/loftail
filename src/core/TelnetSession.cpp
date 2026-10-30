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

#include "TelnetSession.h"

#include "ConsoleFetcher.h"
#include "DiagnosticLog.h"
#include "SocketDetach.h"
#include "TelnetProtocol.h"

#include <QCoreApplication>
#include <QTcpSocket>

#if defined(Q_OS_WIN)
#  include <winsock2.h>
#  include <windows.h>
#else
#  include <cerrno>
#  include <poll.h>
#  include <sys/socket.h>
#endif

#include <utility>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::TelnetSession)
};

// How long a connect may take. The same 20 s every attended SSH gesture gets
// (kSshWorkerConnectTimeoutMs), sliced so abort() is noticed within a quarter of a second.
constexpr int kConnectTimeoutMs = 20000;
constexpr int kConnectSliceMs = 250;

#if defined(Q_OS_WIN)
using Fd = SOCKET;
Fd toFd(qintptr d) { return SOCKET(d); }
int pollOne(Fd fd, short events, int ms)
{
    WSAPOLLFD p{};
    p.fd = fd;
    p.events = events;
    const int n = WSAPoll(&p, 1, ms);
    return n > 0 ? p.revents : (n < 0 ? -1 : 0);
}
bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
using Fd = int;
Fd toFd(qintptr d) { return int(d); }
int pollOne(Fd fd, short events, int ms)
{
    pollfd p{};
    p.fd = fd;
    p.events = events;
    int n;
    do {
        n = ::poll(&p, 1, ms);
    } while (n < 0 && errno == EINTR);
    return n > 0 ? p.revents : (n < 0 ? -1 : 0);
}
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
#endif

// A connected socket as a ConsoleLink, with TelnetProtocol in front of it.
//
// THE SOCKET IS NOT A QTcpSocket ONCE CONNECTED, and SocketDetach.h is the reason in full: a
// QTcpSocket keeps a read notifier armed in the thread that made it, and this session is shared
// by every log on the far end, so the next command may come from a different fetcher's worker
// thread entirely. Plain descriptor I/O has no thread affinity at all.
class TelnetLink final : public ConsoleLink
{
public:
    explicit TelnetLink(qintptr fd) : m_fd(fd) {}
    ~TelnetLink() override { close(); }

    TelnetProtocol &protocol() { return m_protocol; }

    bool isOpen() const override { return m_fd >= 0 && !m_closed; }

    qint64 write(const QByteArray &bytes) override
    {
        return sendAll(m_protocol.encode(bytes)) ? bytes.size() : -1;
    }

    // Writes are synchronous (sendAll() returns once every byte is with the kernel), so there
    // is nothing to wait for — answered from the link's health.
    bool waitForBytesWritten(int ms) override
    {
        Q_UNUSED(ms);
        return isOpen();
    }

    qint64 bytesAvailable() const override { return m_inbox.size(); }

    bool waitForReadyRead(int ms) override
    {
        if (!m_inbox.isEmpty())
            return true;
        pump(ms);
        return !m_inbox.isEmpty();
    }

    QByteArray readAll() override
    {
        pump(0);   // what has arrived since the last wait, without waiting for more
        return std::exchange(m_inbox, QByteArray());
    }

    void close() override
    {
        if (m_fd >= 0) {
            closeDetachedSocket(m_fd);
            m_fd = -1;
        }
        m_closed = true;
    }

    bool sendAll(const QByteArray &bytes)
    {
        if (!isOpen())
            return false;
        qint64 at = 0;
        while (at < bytes.size()) {
#if defined(Q_OS_WIN)
            const int n = ::send(toFd(m_fd), bytes.constData() + at, int(bytes.size() - at), 0);
#else
            const ssize_t n = ::send(toFd(m_fd), bytes.constData() + at, size_t(bytes.size() - at),
                                     MSG_NOSIGNAL);
#endif
            if (n > 0) {
                at += n;
                continue;
            }
            if (n < 0 && wouldBlock()) {
                // Qt left the descriptor non-blocking. A full send buffer is the far end
                // reading slowly, not a dead link: wait for room, boundedly.
                if (pollOne(toFd(m_fd), POLLOUT, 5000) <= 0) {
                    m_closed = true;
                    return false;
                }
                continue;
            }
            m_closed = true;
            return false;
        }
        return true;
    }

private:
    // Read whatever is waiting (waiting up to `ms` for the first byte), run it through the
    // protocol, send any negotiation it owes, and keep the data.
    void pump(int ms)
    {
        if (!isOpen())
            return;
        const int ready = pollOne(toFd(m_fd), POLLIN, ms);
        if (ready <= 0)
            return;
        char buffer[16384];
        for (;;) {
#if defined(Q_OS_WIN)
            const int n = ::recv(toFd(m_fd), buffer, int(sizeof buffer), 0);
#else
            const ssize_t n = ::recv(toFd(m_fd), buffer, sizeof buffer, 0);
#endif
            if (n > 0) {
                QByteArray reply;
                m_inbox.append(m_protocol.receive(QByteArrayView(buffer, n), &reply));
                if (!reply.isEmpty())
                    sendAll(reply);
                if (n < qint64(sizeof buffer))
                    return;
                continue;
            }
            if (n < 0 && wouldBlock())
                return;
            // 0 is the far end closing; anything else is the link failing. Either way the
            // console is gone, and the session finds out through isOpen().
            m_closed = true;
            return;
        }
    }

    qintptr        m_fd = -1;
    bool           m_closed = false;
    TelnetProtocol m_protocol;
    QByteArray     m_inbox;
};

} // namespace

TelnetSession::TelnetSession(QString name)
    : ConsoleSession(std::move(name), "telnet")
{
}

TelnetSession::~TelnetSession() = default;

qint64 TelnetSession::chunkBytes() const
{
    return 256 * 1024;
}

qint64 TelnetSession::readDeadlineMsLocked(qint64 bytes) const
{
    // A floor for the round trip and a busy shell, plus the payload at a pessimistic 64 KB/s.
    // Generous on purpose, for serialReadDeadlineMs()'s reason: a deadline shorter than the
    // transfer turns every read into a retry that will also time out.
    return 20000 + (qMax<qint64>(0, bytes) * 1000) / (64 * 1024);
}

bool TelnetSession::openLinkLocked(const SerialFetchOptions &options,
                                   const RemoteLocation &location, Ready *out)
{
    Q_UNUSED(options);
    QTcpSocket socket;
    socket.connectToHost(location.host, quint16(location.port));
    // Sliced, and through awaitSocketConnected() rather than waitForConnected(), which DESTROYS
    // the attempt when its timeout expires (SocketDetach.h) — a connect slower than one slice
    // would otherwise fail outright, which is every first connect to a host not yet resolved.
    QString connectFailure;
    switch (awaitSocketConnected(socket, kConnectTimeoutMs, kConnectSliceMs, {},
                                 &connectFailure)) {
    case SocketWait::Connected:
        break;
    case SocketWait::Failed:
        // UNREACHABLE IS A WAIT (M13): a machine that is down is a log that has not turned up,
        // and the next poll tries again.
        out->waiting = true;
        out->reason = Tr::tr("Cannot reach %1:%2 — %3")
                          .arg(location.host)
                          .arg(location.port)
                          .arg(connectFailure);
        return false;
    case SocketWait::TimedOut:
    case SocketWait::Abandoned:
        out->waiting = true;
        out->reason = Tr::tr("Cannot reach %1:%2 — it did not answer within %3 seconds")
                          .arg(location.host)
                          .arg(location.port)
                          .arg(kConnectTimeoutMs / 1000);
        return false;
    }

    const qintptr fd = detachSocketFromQt(socket);
    if (fd < 0) {
        out->waiting = true;
        out->reason = Tr::tr("Cannot reach %1:%2").arg(location.host).arg(location.port);
        return false;
    }
    auto link = std::make_unique<TelnetLink>(fd);
    if (!link->sendAll(link->protocol().openingNegotiation())) {
        out->waiting = true;
        out->reason = Tr::tr("%1:%2 closed the connection").arg(location.host).arg(location.port);
        return false;
    }
    setLink(std::move(link));
    diagLog("telnet", QStringLiteral("%1: connected to %2:%3")
                          .arg(name(), location.host)
                          .arg(location.port));
    return true;
}

TelnetSessionRegistry &TelnetSessionRegistry::instance()
{
    static TelnetSessionRegistry registry;
    return registry;
}

std::shared_ptr<TelnetSession> TelnetSessionRegistry::acquire(const RemoteLocation &location)
{
    const QString key = location.target();
    const std::scoped_lock lock(m_mutex);
    if (const auto existing = m_sessions.value(key).lock())
        return existing;
    // NAMED FOR THE PERSON READING, keyed for the machine: messages say `web1` or `web1:2323`,
    // while the table is keyed on target(), whose `telnet:` prefix keeps a host's telnet login
    // apart from its SSH one.
    const QString name =
        location.transport == RemoteLocation::Transport::Discovered
            || location.port == RemoteLocation::defaultPortFor(location.transport)
        ? location.displayHost()
        : QStringLiteral("%1:%2").arg(location.host).arg(location.port);
    auto session = std::shared_ptr<TelnetSession>(new TelnetSession(name));
    m_sessions.insert(key, session);
    return session;
}

void TelnetSessionRegistry::clear()
{
    const std::scoped_lock lock(m_mutex);
    m_sessions.clear();
}

std::unique_ptr<SourceFetcher> makeTelnetFetcher(const RemoteLocation &location, QString *error,
                                                 EndpointSource endpoint)
{
    ConsoleFetcherBinding binding;
    binding.area = "telnet";
    binding.acquire = [](const RemoteLocation &loc) -> std::shared_ptr<ConsoleSession> {
        return TelnetSessionRegistry::instance().acquire(loc);
    };
    binding.options = [](const RemoteLocation &loc) { return telnetFetchOptions(loc); };
    binding.endpoint = std::move(endpoint);
    Q_UNUSED(error);
    return makeConsoleFetcher(location, std::move(binding));
}

std::unique_ptr<SourceFetcher> makeTelnetFetcher(const RemoteLocation &location, QString *error)
{
    if (location.transport != RemoteLocation::Transport::Telnet) {
        if (error)
            *error = Tr::tr("Not a telnet log address: %1").arg(location.toString());
        return nullptr;
    }
    ConsoleFetcherBinding binding;
    binding.area = "telnet";
    binding.acquire = [](const RemoteLocation &loc) -> std::shared_ptr<ConsoleSession> {
        return TelnetSessionRegistry::instance().acquire(loc);
    };
    binding.options = [](const RemoteLocation &loc) { return telnetFetchOptions(loc); };
    return makeConsoleFetcher(location, std::move(binding));
}

} // namespace loftail
