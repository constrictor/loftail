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

#include "SerialDevice.h"

#include "DiagnosticLog.h"
#include "ExecByteSafety.h"
#include "SerialEnumerator.h"

#if defined(LOFTAIL_HAVE_SERIAL)
#include <QSerialPort>
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QRandomGenerator>

#include <utility>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::SerialDevice)
};

#if defined(LOFTAIL_HAVE_SERIAL)
QSerialPort::Parity toQt(SerialProfile::Parity p)
{
    switch (p) {
    case SerialProfile::Parity::None:  return QSerialPort::NoParity;
    case SerialProfile::Parity::Even:  return QSerialPort::EvenParity;
    case SerialProfile::Parity::Odd:   return QSerialPort::OddParity;
    case SerialProfile::Parity::Mark:  return QSerialPort::MarkParity;
    case SerialProfile::Parity::Space: return QSerialPort::SpaceParity;
    }
    return QSerialPort::NoParity;
}

QSerialPort::StopBits toQt(SerialProfile::StopBits s)
{
    switch (s) {
    case SerialProfile::StopBits::One:        return QSerialPort::OneStop;
    case SerialProfile::StopBits::OneAndHalf: return QSerialPort::OneAndHalfStop;
    case SerialProfile::StopBits::Two:        return QSerialPort::TwoStop;
    }
    return QSerialPort::OneStop;
}

QSerialPort::FlowControl toQt(SerialProfile::Flow f)
{
    switch (f) {
    case SerialProfile::Flow::None:     return QSerialPort::NoFlowControl;
    case SerialProfile::Flow::Hardware: return QSerialPort::HardwareControl;
    case SerialProfile::Flow::Software: return QSerialPort::SoftwareControl;
    }
    return QSerialPort::NoFlowControl;
}
#endif

// One slice of waiting. Short enough that abort() and a reboot are noticed promptly, long
// enough that a quiet line is not a busy loop.
constexpr int kSliceMs = 100;

} // namespace

qint64 serialChunkBytes(int baud)
{
    // One second's worth at ten bits per byte — the bound SshFetcher.h states in TIME rather
    // than in bytes, applied to this line's rate. Floored so a 300-baud line still makes
    // progress in units worth a round trip, capped so a fast line does not spend a megabyte of
    // granularity on a count nobody is watching that closely.
    const qint64 perSecond = qint64(qMax(300, baud)) / 10;
    return qBound<qint64>(4096LL, perSecond, 65536LL);
}

qint64 serialReadDeadlineMs(int baud, qint64 bytes)
{
    // Ten bits per byte at the line's rate, times three for a device that is also doing
    // something else, plus a floor for the round trip itself. A FIXED timeout cannot work: at
    // 9600 baud a chunk legitimately takes a minute, and a deadline shorter than the transfer
    // turns every read into a retry that will also time out.
    const qint64 rate = qMax<qint64>(300, baud) / 10;
    return 5000 + (qMax<qint64>(0, bytes) * 3000) / qMax<qint64>(1, rate);
}

// --- the port, kept out of the header ---------------------------------------

struct SerialDevice::Port
{
#if defined(LOFTAIL_HAVE_SERIAL)
    QSerialPort port;
#endif
};

SerialDevice::SerialDevice(QString deviceName, std::function<QString()> resolveNode)
    : m_deviceName(std::move(deviceName))
    , m_resolveNode(std::move(resolveNode))
{
    // Per device, per process: the frame token's stable half. A device replaying its scrollback
    // cannot answer a frame from a previous run of loftail.
    m_tokenBase = QByteArray::number(QRandomGenerator::global()->generate64(), 16);
}

SerialDevice::~SerialDevice()
{
    const std::scoped_lock lock(m_mutex);
    dropSessionLocked();
}

ByteSafety SerialDevice::byteSafety() const
{
    const std::scoped_lock lock(m_mutex);
    return m_safety;
}

SerialProfile SerialDevice::profileInUse() const
{
    const std::scoped_lock lock(m_mutex);
    return m_options.profile;
}

QString SerialDevice::waitingText() const
{
    const std::scoped_lock lock(m_mutex);
    return m_waitingText;
}

bool SerialDevice::takeReboot()
{
    const std::scoped_lock lock(m_mutex);
    // CONSUMED BY THE ASK, so the fetcher that notices acts once. Two fetchers on one device
    // both need to know, and both find out — the first through this, the rest through the
    // session having been dropped, which fails their next command as a LINK failure. That is
    // the health split doing its job rather than a second notification mechanism.
    const bool was = m_rebooted;
    m_rebooted = false;
    return was;
}

void SerialDevice::dropSession()
{
    const std::scoped_lock lock(m_mutex);
    dropSessionLocked();
}

void SerialDevice::dropSessionLocked()
{
#if defined(LOFTAIL_HAVE_SERIAL)
    if (m_port && m_port->port.isOpen())
        m_port->port.close();
#endif
    m_port.reset();
    m_login.reset();
    m_ready = false;
    m_safetySettled = false;   // settled per LOGIN, where the size ladder settles per open
    m_pending.clear();
    m_waitingText.clear();
    m_aborted = false;
}

qint64 SerialDevice::readDeadlineMsLocked(qint64 bytes) const
{
    return serialReadDeadlineMs(m_options.profile.baud, bytes);
}

// --- opening the port -------------------------------------------------------

bool SerialDevice::openPortLocked(const SerialFetchOptions &options, Ready *out)
{
#if !defined(LOFTAIL_HAVE_SERIAL)
    Q_UNUSED(options);
    out->refused = true;
    out->reason = Tr::tr("This copy of loftail was built without serial support, so a log on "
                         "a device cannot be opened.");
    return false;
#else
    const QString node = m_resolveNode();
    if (node.isEmpty()) {
        // NOT PLUGGED IN, AND THAT IS A WAIT rather than a refusal: an absent device is a log
        // that has not turned up, which M13 already handles and which costs nothing new. It is
        // also why a stable id is worth preferring — plugging the board back in resolves to
        // the same address, where a port name may have renumbered.
        out->waiting = true;
        out->reason = Tr::tr("%1 is not plugged in — waiting for it").arg(m_deviceName);
        return false;
    }

    m_port = std::make_unique<Port>();
    m_port->port.setPortName(node);
    if (!m_port->port.open(QIODevice::ReadWrite)) {
        const QString why = m_port->port.errorString();
        m_port.reset();
        // A PERMISSION IS A WAIT, NOT A REFUSAL, for LogPresence::Unreadable's reason: it is
        // there, so nothing has to appear, and a permission is granted as readily as a file is
        // written. On Linux this is almost always group membership, so the sentence names it —
        // "resource busy" is the other common answer and means another program has the port.
        out->waiting = true;
        out->reason = Tr::tr("Cannot open %1 — %2. On Linux this usually means the account is "
                             "not in the `dialout` group.")
                          .arg(m_deviceName, why);
        return false;
    }

    const SerialProfile &p = options.profile;
    m_port->port.setBaudRate(p.baud);
    m_port->port.setDataBits(QSerialPort::DataBits(p.dataBits));
    m_port->port.setParity(toQt(p.parity));
    m_port->port.setStopBits(toQt(p.stopBits));
    m_port->port.setFlowControl(toQt(p.flow));
    // Anything the device said before loftail was listening is not an answer to anything.
    m_port->port.clear();
    m_pending.clear();
    diagLog("serial", QStringLiteral("%1: opened %2 at %3 %4%5%6")
                          .arg(m_deviceName, node)
                          .arg(p.baud)
                          .arg(p.dataBits)
                          .arg(p.parity == SerialProfile::Parity::None ? QStringLiteral("N")
                                                                      : QStringLiteral("P"))
                          .arg(p.stopBits == SerialProfile::StopBits::One ? QStringLiteral("1")
                                                                         : QStringLiteral("2")));
    return true;
#endif
}

// --- the login --------------------------------------------------------------

bool SerialDevice::loginLocked(const SerialFetchOptions &options, const RemoteLocation &location,
                               const QByteArray &password, Ready *out)
{
#if !defined(LOFTAIL_HAVE_SERIAL)
    Q_UNUSED(options); Q_UNUSED(location); Q_UNUSED(password); Q_UNUSED(out);
    return false;
#else
    if (!m_login) {
        m_login = std::make_unique<SerialLoginMachine>(options.profile, m_deviceName,
                                                       location.effectiveUser(), password,
                                                       QDateTime::currentMSecsSinceEpoch());
    }

    QElapsedTimer overall;
    overall.start();
    // Bounded by the conversation's own budget rather than by a number here: every step has a
    // timeout and a count of attempts, so the machine gives up on its own — this is only a
    // backstop against a machine that somehow neither progresses nor fails.
    const qint64 budget = qint64(options.profile.promptTimeoutMs)
        * qint64(qMax(1, options.profile.attempts)) * 8;

    while (!m_aborted) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        LoginAction action = m_login->tick(now);
        if (action.kind == LoginAction::Kind::Wait && m_port->port.bytesAvailable() == 0)
            m_port->port.waitForReadyRead(kSliceMs);

        const QByteArray arrived = m_port->port.readAll();
        if (!arrived.isEmpty())
            action = m_login->feed(arrived, QDateTime::currentMSecsSinceEpoch());

        switch (action.kind) {
        case LoginAction::Kind::Write:
            // THE TRANSCRIPT HONOURS `sensitive`, and SPEC.md §3's Diagnostics promises by name
            // that loftail's own log "never records a password, a passphrase, a key" — in the
            // file meant to be attachable to a bug report as it stands.
            diagLog("serial", QStringLiteral("%1: %2 -> %3")
                                  .arg(m_deviceName,
                                       QString::fromLatin1(loginStepName(m_login->step())),
                                       action.sensitive
                                           ? QStringLiteral("(password)")
                                           : QString::fromUtf8(action.bytes).trimmed()));
            m_port->port.write(action.bytes);
            m_port->port.waitForBytesWritten(kSliceMs * 10);
            break;
        case LoginAction::Kind::Done:
            m_waitingText.clear();
            diagLog("serial", QStringLiteral("%1: signed in").arg(m_deviceName));
            return true;
        case LoginAction::Kind::Fail:
            // Told no. A refusal that keeps its tab and says why (M17) rather than something
            // to poll for: a wrong password in the preset does not fix itself.
            out->refused = true;
            out->reason = action.reason;
            dropSessionLocked();
            return false;
        case LoginAction::Kind::Reboot:
            // Mid-login. Start the conversation again rather than reporting anything — which
            // is exactly what a board that reboots while being connected to needs.
            m_rebooted = true;
            m_login->reset(QDateTime::currentMSecsSinceEpoch());
            break;
        case LoginAction::Kind::Wait:
            break;
        }

        m_waitingText = m_login->waitingText();
        if (overall.elapsed() > budget)
            break;
    }

    out->waiting = true;
    out->reason = m_login ? m_login->waitingText()
                          : Tr::tr("Signing in to %1").arg(m_deviceName);
    if (out->reason.isEmpty())
        out->reason = Tr::tr("Signing in to %1").arg(m_deviceName);
    return false;
#endif
}

// --- running one command ----------------------------------------------------

bool SerialDevice::runLocked(const QString &command, QByteArray *stdOut, qint64 expectedPayload,
                             bool keepStdErr, QByteArray *rawPayload, qint64 deadlineOverrideMs,
                             int *exitCode)
{
#if !defined(LOFTAIL_HAVE_SERIAL)
    Q_UNUSED(command); Q_UNUSED(stdOut); Q_UNUSED(expectedPayload);
    Q_UNUSED(keepStdErr); Q_UNUSED(rawPayload);
    return false;
#else
    if (!m_port || !m_port->port.isOpen())
        return false;   // LINK: there is nothing to run it on.

    SerialFrame frame;
    frame.token = m_tokenBase + QByteArray::number(++m_frameCounter, 16);
    frame.expected = expectedPayload;

    const QByteArray line = framedCommand(command, frame, keepStdErr);
    if (m_port->port.write(line) < 0)
        return false;   // LINK
    if (!m_port->port.waitForBytesWritten(kSliceMs * 20))
        return false;   // LINK: the far end is not taking bytes at all.

    // WHAT THE DEVICE PRINTED SINCE THE LAST FRAME IS KEPT, not thrown away: a line it emitted
    // between two commands is noise to the framing and would be noise to anything else, but
    // dropping the buffer here would also drop the beginning of THIS frame on a device that
    // answered faster than the write returned.
    QByteArray buffer = std::move(m_pending);
    m_pending.clear();

    QElapsedTimer clock;
    clock.start();
    const qint64 deadline = deadlineOverrideMs > 0
        ? deadlineOverrideMs
        : readDeadlineMsLocked(qMax<qint64>(0, expectedPayload));

    while (clock.elapsed() < deadline) {
        if (m_aborted)
            return false;   // LINK, effectively: the tab is going away.

        const FramedAnswer answer = readFramedAnswer(buffer, frame);
        if (answer.state == FramedAnswer::State::Complete) {
            m_pending = buffer.mid(answer.consumed);
            capFrameBuffer(&m_pending, frame);
            if (rawPayload)
                *rawPayload = answer.output;
            if (stdOut)
                *stdOut = answer.output;
            if (exitCode)
                *exitCode = answer.exitCode;
            return true;
        }
        if (answer.state == FramedAnswer::State::Corrupt) {
            // A REQUEST FAILURE, not a link one: the far end answered, and what it answered
            // cannot be believed. True with empty output is ExecTransport::RunCommand's own
            // spelling for that, and it is what keeps a damaged frame from condemning the
            // session another log is using.
            m_pending = buffer.mid(answer.consumed);
            capFrameBuffer(&m_pending, frame);
            diagLog("serial", QStringLiteral("%1: a framed answer arrived damaged")
                                  .arg(m_deviceName));
            if (stdOut)
                stdOut->clear();
            return true;
        }

        if (m_port->port.bytesAvailable() == 0)
            m_port->port.waitForReadyRead(kSliceMs);
        const QByteArray arrived = m_port->port.readAll();
        if (arrived.isEmpty()) {
            if (!m_port->port.isOpen())
                return false;   // LINK: the device went away mid-command.
            continue;
        }
        buffer.append(arrived);
        capFrameBuffer(&buffer, frame);

        // THE REBOOT PATTERN IS WATCHED ON EVERY COMMAND, not only during the login — because
        // Ready is the state a reboot actually happens in, and a machine that only looked
        // during the login would be a machine that never noticed one.
        //
        // FED THROUGH THE LOGIN MACHINE, which is safe and is the point: in its Ready step
        // feed() asks the reboot pattern and answers nothing else, so there is one place that
        // knows what a reboot looks like rather than a second copy of the match here.
        //
        // It is a LINK failure by definition — the shell that was going to answer this no
        // longer exists — so it condemns the session for every log on the device, which is
        // correct: the board really did restart.
        if (m_login) {
            const LoginAction seen = m_login->feed(arrived, QDateTime::currentMSecsSinceEpoch());
            if (seen.kind == LoginAction::Kind::Reboot) {
                m_rebooted = true;
                diagLog("serial", QStringLiteral("%1: rebooted while a command was running")
                                      .arg(m_deviceName));
                dropSessionLocked();
                return false;
            }
        }
    }

    // Nothing came back inside a deadline derived from the line's own rate. That is the link,
    // not the request: a device that answers nothing at all is not telling us about a file.
    m_pending = std::move(buffer);
    capFrameBuffer(&m_pending, frame);
    diagLog("serial", QStringLiteral("%1: no answer within %2 ms").arg(m_deviceName).arg(deadline));
    return false;
#endif
}

bool SerialDevice::run(const QString &command, QByteArray *stdOut)
{
    const std::scoped_lock lock(m_mutex);
    if (!m_ready)
        return false;
    return runLocked(command, stdOut, /*expectedPayload=*/-1, /*keepStdErr=*/false, nullptr);
}

bool SerialDevice::runScript(const QString &command, qint64 deadlineMs, QByteArray *output,
                            int *exitCode)
{
    const std::scoped_lock lock(m_mutex);
    if (!m_ready)
        return false;
    // KEEPS STDERR — the one place the frame's `2>/dev/null` is wrong, because here a complaint
    // is a failure signal rather than noise — and carries its own deadline, a service restart
    // outliving any read the line's rate would allow for.
    return runLocked(command, output, /*expectedPayload=*/-1, /*keepStdErr=*/true, nullptr,
                     deadlineMs, exitCode);
}

qint64 SerialDevice::readBytes(const QString &path, qint64 offset, char *buffer, qint64 length)
{
    const std::scoped_lock lock(m_mutex);
    if (!m_ready || length <= 0)
        return 0;

    // THE ENCODING DECIDES WHETHER THE PAYLOAD IS LENGTH-DELIMITED OR SCANNED, and the two are
    // not interchangeable. Raw is arbitrary bytes and may contain the frame's token, so it must
    // be bounded by the count `| head -c L` guarantees. Base64 cannot contain either marker —
    // the alphabet has no `-` — so it is scanned, which is what lets the two implementations of
    // `base64` (GNU wraps at 76 columns, busybox does not) both work.
    const bool encoded = m_safety == ByteSafety::Base64;
    QByteArray payload;
    if (!runLocked(readCommand(path, offset, length, m_safety), nullptr,
                   encoded ? -1 : length, /*keepStdErr=*/false, &payload)) {
        return -1;
    }
    if (encoded) {
        QByteArray decoded;
        if (!decodeBase64Payload(payload, &decoded)) {
            // A REQUEST failure: the far end answered and the answer did not decode. Nothing is
            // handed back, because a short buffer here is indistinguishable from end of file one
            // layer up — which is how a cut-off answer becomes a silently truncated log.
            diagLog("serial", QStringLiteral("%1: a read did not decode").arg(m_deviceName));
            return -1;
        }
        payload = decoded;
    }
    const qint64 got = qMin<qint64>(payload.size(), length);
    if (got > 0)
        memcpy(buffer, payload.constData(), size_t(got));
    return got;
}

SerialDevice::Ready SerialDevice::ensureReady(const QString &samplePath,
                                             const SerialFetchOptions &options,
                                             const RemoteLocation &location,
                                             const QByteArray &password)
{
    const std::scoped_lock lock(m_mutex);
    Ready out;

    // WHAT AN EDITED PRESET COSTS IS DECIDED HERE, once, for every log on the device — because
    // the line settings and the login belong to the device rather than to a log. A tier that
    // needs the port reopened drops the session; one that needs the login run again drops it
    // too, the login being the only thing above the port; and an in-place change is simply
    // stored. See serialChangeCost().
    if (m_ready || m_port) {
        switch (serialChangeCost(m_options.profile, options.profile)) {
        case SerialChangeCost::ReopenPort:
        case SerialChangeCost::Relogin:
            diagLog("serial", QStringLiteral("%1: settings changed — starting over")
                                  .arg(m_deviceName));
            dropSessionLocked();
            break;
        case SerialChangeCost::InPlace:
        case SerialChangeCost::Nothing:
            break;
        }
    }
    m_options = options;

    if (m_ready) {
        out.ok = true;
        return out;
    }

    if (!m_port && !openPortLocked(options, &out))
        return out;
    if (!loginLocked(options, location, password, &out))
        return out;
    if (!m_safetySettled && !settleByteSafetyLocked(samplePath, &out))
        return out;

    m_ready = true;
    out.ok = true;
    return out;
}

// --- the byte-safety probe --------------------------------------------------

bool SerialDevice::settleByteSafetyLocked(const QString &samplePath, Ready *out)
{
    // SETTLED PER LOGIN, where the size ladder settles per OPEN — two cadences on purpose: a
    // rotation gets a fresh size rung, a reboot gets a fresh verdict about the line. And it is
    // a property of the LINE rather than of the log, so whichever log gets here first settles
    // it for every log on the device.
    ExecByteSafety probe(
        samplePath,
        [this](const QString &command, QByteArray *output) {
            return runLocked(command, output, /*expectedPayload=*/-1, /*keepStdErr=*/false,
                             nullptr);
        },
        [this, &samplePath](qint64 offset, qint64 length) {
            // RAW, EXPLICITLY, whatever m_safety currently holds: the probe's whole question is
            // whether raw survives, so a seam that already applied the answer would be
            // comparing the encoder against itself.
            QByteArray payload;
            if (!runLocked(readCommand(samplePath, offset, length, ByteSafety::Raw), nullptr,
                           length, /*keepStdErr=*/false, &payload)) {
                return QByteArray();
            }
            return payload;
        });

    QString refusal;
    const auto answer = probe.settle(&refusal);
    if (!answer) {
        // Neither raw nor base64 can be had. A refusal that keeps its tab and says what is
        // missing (M17) — reading a log through a line that mangles it is worse than not
        // reading it, because nothing about the result says it was mangled.
        out->refused = true;
        out->reason = refusal;
        return false;
    }
    m_safety = *answer;
    m_safetySettled = true;
    diagLog("serial", QStringLiteral("%1: bytes travel %2%3")
                          .arg(m_deviceName,
                               *answer == ByteSafety::Raw ? QStringLiteral("raw")
                                                          : QStringLiteral("base64-encoded"),
                               probe.verifiedAgainstEncoder()
                                   ? QStringLiteral(" (verified against the encoder)")
                                   : QStringLiteral(" (verified against a generated pattern)")));
    return true;
}

// --- the registry -----------------------------------------------------------

SerialDeviceRegistry &SerialDeviceRegistry::instance()
{
    static SerialDeviceRegistry registry;
    return registry;
}

std::shared_ptr<SerialDevice> SerialDeviceRegistry::acquire(const QString &deviceName)
{
    const std::scoped_lock lock(m_mutex);
    // WEAK, so the last fetcher letting go closes the port. Held strongly here instead and a
    // device that is no longer open would keep its line claimed for the life of the process.
    if (const auto existing = m_devices.value(deviceName).lock())
        return existing;
    // Captured by value, and it captures the RESOLVER rather than the resolved node: a device
    // that is unplugged and plugged back in must resolve again, which is the whole reason a
    // stable id is worth preferring over a port name that renumbers.
    NodeResolver resolver = m_resolver;
    auto resolve = [deviceName, resolver] {
        return resolver ? resolver(deviceName) : serialSystemLocationFor(deviceName);
    };
    auto device = std::shared_ptr<SerialDevice>(new SerialDevice(deviceName, std::move(resolve)));
    m_devices.insert(deviceName, device);
    return device;
}

void SerialDeviceRegistry::setNodeResolver(NodeResolver resolver)
{
    const std::scoped_lock lock(m_mutex);
    m_resolver = std::move(resolver);
    // Existing devices keep the resolver they were built with, which is why this also empties
    // the table: a test installing one must not find a device wired to the production resolution.
    m_devices.clear();
}

void SerialDeviceRegistry::clear()
{
    const std::scoped_lock lock(m_mutex);
    m_devices.clear();
}

} // namespace loftail
