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

#include <QHash>
#include <QString>

#include <functional>
#include <memory>
#include <mutex>

namespace loftail {

// One device's serial console: the port, the login, and every command that goes over it
// (ARCHITECTURE.md §6.11).
//
// ONE DEVICE, ONE SESSION, N LOGS — and this class is the whole of that. A serial line permits
// exactly ONE opener, so two tabs reading two logs off one board are two spools, two fetchers
// and would be two QSerialPort::open() calls on one node. Either the second fails, or — far
// worse — both succeed and two command streams interleave, splicing one log's bytes into the
// other's spool at offsets an indexer is walking.
//
// ARCHITECTURE.md §6.3's "one connection per open file, which is what `scp` does anyway" is
// simply FALSE for a UART, and that sentence has been amended. The device is reference-counted
// per id, logged in once however many logs are open, and a mutex serialises commands: two logs
// then halve each other's throughput, which is honest on a line moving ~11 KB/s and is stated
// in SPEC.md rather than left to be found.
//
// SHARING A SESSION IS WHAT SshSessionCache.h RECORDS AS REJECTED FOR FETCHERS, so the clause
// that does not carry over has to be answered rather than ignored, and it is the health latch.
// Its objection is that one errand's failure latches health and sends the other log's tail
// into a reconnect and a re-fetch from zero. The answer is already in the tree:
// sshErrorEndsSession() is "the one answer to whether this is about the link or about the
// request", and the same split governs here —
//
//   * a LINK failure (the port would not open, the port went away, the device rebooted, the
//     login is gone) condemns the session for everyone, which is CORRECT: the board really did
//     reboot, and both logs really are affected;
//   * a REQUEST failure (this log is missing, this read was refused, this frame arrived
//     damaged) is the asking fetcher's alone and touches no shared state.
//
// The second is expressed through ExecTransport::RunCommand's own two-way rule — false means
// the channel died, true with empty output means it ran and printed nothing — so the split is
// carried by the interface rather than by a second flag anybody has to remember.
class SerialDevice final : public ConsoleSession
{
public:
    ~SerialDevice() override;

    // One second of the line at ten bits per byte (serialChunkBytes()).
    qint64 chunkBytes() const override;

    // Everything else a fetcher asks of the device — ensureReady(), run(), readBytes(),
    // runScript(), the reboot latch — is ConsoleSession's, which this class's comment above
    // is the original statement of. What is left here is the PORT: which node a device name
    // resolves to, the line settings, and the arithmetic that derives a deadline from a baud.

protected:
    bool openLinkLocked(const SerialFetchOptions &options, const RemoteLocation &location,
                        Ready *out) override;
    qint64 readDeadlineMsLocked(qint64 bytes) const override;

private:
    friend class SerialDeviceRegistry;
    SerialDevice(QString deviceName, std::function<QString()> resolveNode);

    // Resolved through the registry, so the one seam above covers the device too.
    std::function<QString()> m_resolveNode;
};

// One SerialDevice per device NAME, reference-counted, SourceSpoolRegistry's own shape.
//
// The last handle dropping closes the port — and it is dropped when the FETCHER is destroyed,
// which the reaper runs once the worker thread has exited, not when the spool is let go. That
// is spoolDirName()'s serial-number problem restated: a reopen that raced the previous
// fetcher's wind-down would find the port still held.
class SerialDeviceRegistry
{
public:
    static SerialDeviceRegistry &instance();

    std::shared_ptr<SerialDevice> acquire(const QString &deviceName);

    // Where a device NAME resolves to, overridable.
    //
    // THE SEAM SourceSpoolRegistry::setFetcherFactory() ALREADY ESTABLISHES, and for the same
    // reason: the thing on the other end cannot be created by a test. A serial port is hardware,
    // and the one substitute that behaves like one is a pty — which no enumeration will ever
    // list, because it is not a serial port. Without this, the transport's whole conversation
    // would be reachable only from a real board, which is the position the SSH exec transport
    // spent three milestones in.
    //
    // Null restores the ordinary resolution (SerialEnumerator.h).
    using NodeResolver = std::function<QString(const QString &deviceName)>;
    void setNodeResolver(NodeResolver resolver);

    // For a test that must not answer the next test's question.
    void clear();

private:
    std::mutex m_mutex;
    QHash<QString, std::weak_ptr<SerialDevice>> m_devices;
    NodeResolver m_resolver;
};

// How big a chunk to read at a time, and how long to wait for one, DERIVED FROM THE BAUD
// rather than written down.
//
// SSH reads 1 MB at a time, and the argument behind that number is in SshFetcher.h: a chunk is
// one step of committedSize, so it is also how coarsely a catch-up fills the view, and the
// bound is in TIME — a chunk that takes longer than about a second to arrive reads as a frozen
// count rather than as a log loading. At 115200 baud the line delivers ~11 KB/s, so ONE SECOND
// IS ELEVEN KILOBYTES: borrowing 1 MB here would freeze the count for a minute and a half per
// chunk. The same rule, applied to a line four orders of magnitude slower, gives a different
// number — which is why it is computed and not copied.
qint64 serialChunkBytes(int baud);

// And the deadline for one read, from the same arithmetic: ten bits per byte at the line's
// rate, times a slack factor for a busy device, plus a floor. A fixed timeout cannot work
// here — at 9600 baud a chunk legitimately takes a minute — and a timeout shorter than the
// transfer turns every read into a retry.
qint64 serialReadDeadlineMs(int baud, qint64 bytes);

} // namespace loftail
