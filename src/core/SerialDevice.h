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

#include "RemoteLocation.h"
#include "SerialFetchOptions.h"
#include "SerialFraming.h"
#include "SerialLoginMachine.h"
#include "SshExecCommands.h"

#include <QByteArray>
#include <QString>

#include <QHash>

#include <atomic>
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
class SerialDevice
{
public:
    ~SerialDevice();

    // Connect the port, run the login conversation and settle the byte-safety probe, if that
    // has not already been done by whichever log got here first.
    //
    // `samplePath` is used only for the byte-safety comparison, which is a property of the
    // LINE rather than of the log — so the first caller's path settles it and later callers
    // skip it.
    struct Ready
    {
        bool    ok = false;
        // Not there: the device is not plugged in, or the login has not finished. A WAIT.
        bool    waiting = false;
        // Refused outright and worth no more polling: no `stty raw` and no `base64`, or the
        // login was told no. A refusal that keeps its tab and says why (M17).
        bool    refused = false;
        QString reason;
    };
    Ready ensureReady(const QString &samplePath, const SerialFetchOptions &options,
                      const RemoteLocation &location, const QByteArray &password);

    // ExecTransport::RunCommand. TWO-WAY: false is the LINK, true with empty output is the
    // request. See the class comment — this is where the shared-session health split lives.
    bool run(const QString &command, QByteArray *stdOut);

    // Bytes of `path` at `offset`, through whichever encoding the probe settled on.
    qint64 readBytes(const QString &path, qint64 offset, char *buffer, qint64 length);

    ByteSafety byteSafety() const;

    // Run one restart script on the device (SPEC.md §4, §6.9).
    //
    // TWO THINGS SEPARATE IT FROM run(). It KEEPS stderr, merged into the output, because here a
    // complaint is one of the two failure signals rather than noise — the one place the frame's
    // `2>/dev/null` is wrong. And it takes its own deadline, because a service restart outlives
    // any read: a deadline derived from the line's rate would call every successful restart a
    // dropped console.
    //
    // A console carries ONE stream, so stdout and stderr cannot be told apart and the caller
    // must judge the run on its exit status alone. That relaxation is SPEC.md §4's, and
    // RestartResult::streamsMerged is how the dialog says so.
    bool runScript(const QString &command, qint64 deadlineMs, QByteArray *output, int *exitCode);

    // Drop the login and the port. What a reboot, a dropped line and a line-setting change all
    // mean, and the one thing that condemns the session for every log on the device.
    void dropSession();

    // Whether the reboot pattern has fired since this was last asked. CONSUMED by the ask, so
    // the fetcher that notices it is the one that acts on it and a second poll does not
    // reconnect twice.
    bool takeReboot();

    // Stop a blocking read at once, for a tab closing on a device that is not answering.
    // Latched, never acted on — SessionHealth's discipline, and for its reason: the thread
    // inside the port owns it.
    void abort() { m_aborted = true; }

    // What the tab says while the login is running. Empty once it is done.
    QString waitingText() const;

    // The options the device was last made ready with, so a fetcher can tell whether an edited
    // preset needs the port reopened, the login run again, or nothing.
    SerialProfile profileInUse() const;

private:
    friend class SerialDeviceRegistry;
    SerialDevice(QString deviceName, std::function<QString()> resolveNode);

    // Everything below is called with m_mutex held.
    bool openPortLocked(const SerialFetchOptions &options, Ready *out);
    bool loginLocked(const SerialFetchOptions &options, const RemoteLocation &location,
                     const QByteArray &password, Ready *out);
    bool settleByteSafetyLocked(const QString &samplePath, Ready *out);
    bool runLocked(const QString &command, QByteArray *stdOut, qint64 expectedPayload,
                   bool keepStdErr, QByteArray *rawPayload, qint64 deadlineOverrideMs = 0,
                   int *exitCode = nullptr);
    void dropSessionLocked();
    qint64 readDeadlineMsLocked(qint64 bytes) const;

    struct Port;   // the QSerialPort, hidden so this header names no Qt SerialPort type

    QString                 m_deviceName;
    // Resolved through the registry, so the one seam above covers the device too.
    std::function<QString()> m_resolveNode;
    mutable std::mutex      m_mutex;
    std::unique_ptr<Port>   m_port;
    std::unique_ptr<SerialLoginMachine> m_login;
    SerialFetchOptions      m_options;
    ByteSafety              m_safety = ByteSafety::Raw;
    bool                    m_safetySettled = false;
    bool                    m_ready = false;
    quint64                 m_frameCounter = 0;
    QByteArray              m_tokenBase;
    QByteArray              m_pending;      // bytes read past the last frame
    QString                 m_waitingText;
    bool                    m_rebooted = false;
    std::atomic<bool>       m_aborted{false};
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
