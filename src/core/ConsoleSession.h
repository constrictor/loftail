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

#include <atomic>
#include <memory>
#include <mutex>

namespace loftail {

// The byte pipe under a login console (ARCHITECTURE.md §6.11, §6.12).
//
// A serial port and a telnet connection differ in exactly this and in nothing above it: both
// carry one stream that a login prompt arrives on, a shell answers on, and whatever else the
// device felt like printing is mixed into. Everything that makes such a stream readable —
// the login conversation, the framing, the byte-safety probe, the reboot watch — is the
// ConsoleSession's, and the link only moves bytes.
//
// Shaped after QIODevice's blocking calls because both implementations are one: QSerialPort,
// and QTcpSocket behind a telnet filter. Called only with the session's mutex held, from the
// fetcher's worker thread, which is the one thread that owns the link.
class ConsoleLink
{
public:
    virtual ~ConsoleLink() = default;

    virtual bool       isOpen() const = 0;
    virtual qint64     write(const QByteArray &bytes) = 0;
    virtual bool       waitForBytesWritten(int ms) = 0;
    virtual qint64     bytesAvailable() const = 0;
    virtual bool       waitForReadyRead(int ms) = 0;
    virtual QByteArray readAll() = 0;
    virtual void       close() = 0;
};

// One console session: a link, the login on it, and every command that goes over it.
//
// EXTRACTED FROM SerialDevice RATHER THAN COPIED, ExecTransport's own argument one layer up:
// the second transport over a console (telnet) needs every judgement here and none of the
// port, and two copies of the login loop and the framing would be two places to fix the next
// time a device answers in a way nobody expected. tst_serialdevice passing unaltered across
// the extraction is the evidence that it was inert.
//
// What a subclass supplies is how to OPEN the link, how long a read of N bytes may take on
// it, how big a chunk is worth asking for, and the area its diagnostic lines go under. The
// health split, the frame token and the reboot handling are all here, and are described on
// SerialDevice, whose class comment is the original statement of them.
class ConsoleSession
{
public:
    virtual ~ConsoleSession();

    // Connect the link, run the login conversation and settle the byte-safety probe, if that
    // has not already been done by whichever log got here first.
    //
    // `samplePath` is used only for the byte-safety comparison, which is a property of the
    // LINE rather than of the log — so the first caller's path settles it and later callers
    // skip it.
    struct Ready
    {
        bool    ok = false;
        // Not there: the device is not reachable, or the login has not finished. A WAIT.
        bool    waiting = false;
        // Refused outright and worth no more polling: no `stty raw` and no `base64`, or the
        // login was told no. A refusal that keeps its tab and says why (M17).
        bool    refused = false;
        QString reason;
    };
    Ready ensureReady(const QString &samplePath, const SerialFetchOptions &options,
                      const RemoteLocation &location, const QByteArray &password);

    // ExecTransport::RunCommand. TWO-WAY: false is the LINK, true with empty output is the
    // request. See SerialDevice's class comment — this is where the shared-session health
    // split lives.
    bool run(const QString &command, QByteArray *stdOut);

    // Bytes of `path` at `offset`, through whichever encoding the probe settled on.
    qint64 readBytes(const QString &path, qint64 offset, char *buffer, qint64 length);

    ByteSafety byteSafety() const;

    // Run one restart script on the device (SPEC.md §4, §6.9). KEEPS stderr and takes its own
    // deadline; see SerialDevice.h for why both.
    bool runScript(const QString &command, qint64 deadlineMs, QByteArray *output, int *exitCode);

    // Drop the login and the link. What a reboot, a dropped line and a setting change all
    // mean, and the one thing that condemns the session for every log on the device.
    void dropSession();

    // Whether the reboot pattern has fired since this was last asked. CONSUMED by the ask.
    bool takeReboot();

    // Stop a blocking read at once, for a tab closing on a device that is not answering.
    // Latched, never acted on.
    void abort() { m_aborted = true; }

    // What the tab says while the login is running. Empty once it is done.
    QString waitingText() const;

    // The options the session was last made ready with.
    SerialProfile profileInUse() const;

    // How big a chunk a fetcher should ask for at a time on this link. A chunk is one step of
    // committedSize, so the bound is in TIME (SshFetcher.h): about a second of the link.
    virtual qint64 chunkBytes() const = 0;

    // The name the session goes by in messages and diagnostic lines: a device id, a host.
    const QString &name() const { return m_name; }

protected:
    ConsoleSession(QString name, const char *diagArea);

    // Open the link and store it with setLink(). On failure fill `out` — `waiting` for
    // something that may turn up, `refused` for something that will not — and answer false.
    // Called with the mutex held.
    virtual bool openLinkLocked(const SerialFetchOptions &options, const RemoteLocation &location,
                                Ready *out) = 0;

    // How long a framed answer carrying `bytes` of payload may take to arrive.
    virtual qint64 readDeadlineMsLocked(qint64 bytes) const = 0;

    // How long to let the far end SPEAK FIRST on a fresh link before the login machine nudges
    // it. Zero for a serial line, which says nothing until spoken to; a telnet server prints
    // its `login:` the moment it accepts, and a nudge sent before that has arrived makes it
    // print a SECOND prompt — which lands while the user name is in flight, reads as the
    // name being refused, and spends an attempt (or the whole login) on nothing.
    virtual int greetingWaitMsLocked() const { return 0; }

    void setLink(std::unique_ptr<ConsoleLink> link);
    const SerialFetchOptions &optionsLocked() const { return m_options; }
    const char *diagArea() const { return m_diagArea; }

private:
    // Everything below is called with m_mutex held.
    bool loginLocked(const SerialFetchOptions &options, const RemoteLocation &location,
                     const QByteArray &password, Ready *out);
    bool settleByteSafetyLocked(const QString &samplePath, Ready *out);
    bool runLocked(const QString &command, QByteArray *stdOut, qint64 expectedPayload,
                   bool keepStdErr, QByteArray *rawPayload, qint64 deadlineOverrideMs = 0,
                   int *exitCode = nullptr);
    void dropSessionLocked();

    QString                 m_name;
    const char             *m_diagArea;
    mutable std::mutex      m_mutex;
    std::unique_ptr<ConsoleLink>        m_link;
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
    QString                 m_linkedHost;   // where the link was opened, for ensureReady()
    int                     m_linkedPort = 0;
    std::atomic<bool>       m_aborted{false};
};

} // namespace loftail
