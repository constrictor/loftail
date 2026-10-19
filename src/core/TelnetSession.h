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

#include "ConsoleSession.h"
#include "EndpointResolver.h"
#include "RemoteLocation.h"
#include "SerialFetchOptions.h"
#include "SourceFetcher.h"

#include <QHash>
#include <QString>

#include <memory>
#include <mutex>

namespace loftail {

// A login console reached over TCP (SPEC.md §3, ARCHITECTURE.md §6.12).
//
// TELNET IS A CONSOLE AND NOTHING MORE: everything that makes a log readable through it — the
// login conversation, the framed commands, the byte-safety probe, the reboot watch, the tail
// loop — is ConsoleSession's and ConsoleFetcher's, shared with the serial transport. What is
// here is how to OPEN the pipe: resolve and connect a socket, take it away from Qt, and put
// TelnetProtocol between it and the console.
//
// ONE SESSION PER FAR END, as one device has one SerialDevice — not because TCP permits only one
// opener, which it does not, but because a login is not free and the reboot watch belongs to the
// machine rather than to the log. Keyed on RemoteLocation::target(), whose `telnet:` prefix keeps
// a host's telnet login apart from its SSH one.
class TelnetSession final : public ConsoleSession
{
public:
    ~TelnetSession() override;

    // A network chunk, not a baud-derived one: a chunk is one step of committedSize, bounded in
    // TIME (SshFetcher.h). 256 KB is about a second on the slowest link worth reading a log
    // over, and an embedded box behind telnet is often that slow on the far end's own disk.
    qint64 chunkBytes() const override;

protected:
    bool openLinkLocked(const SerialFetchOptions &options, const RemoteLocation &location,
                        Ready *out) override;
    qint64 readDeadlineMsLocked(qint64 bytes) const override;
    // A telnet server prints its prompt on accepting, so it is given the chance to.
    int greetingWaitMsLocked() const override { return 1500; }

private:
    friend class TelnetSessionRegistry;
    explicit TelnetSession(QString name);
};

// One TelnetSession per far end, reference-counted, SerialDeviceRegistry's own shape — weak
// here, so the last fetcher letting go logs out.
class TelnetSessionRegistry
{
public:
    static TelnetSessionRegistry &instance();

    // The session for `location`'s far end, keyed on its target().
    std::shared_ptr<TelnetSession> acquire(const RemoteLocation &location);

    // For a test that must not answer the next test's question.
    void clear();

private:
    std::mutex m_mutex;
    QHash<QString, std::weak_ptr<TelnetSession>> m_sessions;
};

// The per-address options a telnet fetcher reads on every poll — the console preset (only its
// conversation fields mean anything here: prompts, timeouts, attempts, startup commands, the
// reboot pattern and the poll cadence; the line settings are a serial port's) and the
// tail-start. KEPT IN SerialFetchOptions' MUTEX-GUARDED STORE, keyed by the address string, so
// the two console transports share one synchronised table rather than a second copy of it.
inline void setTelnetFetchOptions(const RemoteLocation &location,
                                  const SerialFetchOptions &options)
{
    setSerialFetchOptions(location, options);
}
inline SerialFetchOptions telnetFetchOptions(const RemoteLocation &location)
{
    return serialFetchOptions(location);
}

// The fetcher for a `telnet://` address. Always compiled: it needs only Qt Network, which the
// core library links unconditionally.
std::unique_ptr<SourceFetcher> makeTelnetFetcher(const RemoteLocation &location, QString *error);

// The same for a `discover://` address, signing in wherever `resolve` finds the device before
// every login. The session is still keyed on `location` — the DEVICE, whose address moves —
// so two logs on one discovered device share one login as two on one host do.
std::unique_ptr<SourceFetcher> makeTelnetFetcher(const RemoteLocation &location, QString *error,
                                                 EndpointResolver resolve);

} // namespace loftail
