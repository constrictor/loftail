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
#include "SerialEnumerator.h"

#if defined(LOFTAIL_HAVE_SERIAL)
#include <QSerialPort>
#endif

#include <QCoreApplication>

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

#if defined(LOFTAIL_HAVE_SERIAL)
namespace {

// A QSerialPort as a ConsoleLink: nothing but the six calls ConsoleSession makes.
class SerialLink final : public ConsoleLink
{
public:
    QSerialPort port;

    bool       isOpen() const override { return port.isOpen(); }
    qint64     write(const QByteArray &bytes) override { return port.write(bytes); }
    bool       waitForBytesWritten(int ms) override { return port.waitForBytesWritten(ms); }
    qint64     bytesAvailable() const override { return port.bytesAvailable(); }
    bool       waitForReadyRead(int ms) override { return port.waitForReadyRead(ms); }
    QByteArray readAll() override { return port.readAll(); }
    void       close() override { port.close(); }
};

} // namespace
#endif

SerialDevice::SerialDevice(QString deviceName, std::function<QString()> resolveNode)
    : ConsoleSession(std::move(deviceName), "serial")
    , m_resolveNode(std::move(resolveNode))
{
}

SerialDevice::~SerialDevice() = default;

qint64 SerialDevice::chunkBytes() const
{
    return serialChunkBytes(profileInUse().baud);
}

qint64 SerialDevice::readDeadlineMsLocked(qint64 bytes) const
{
    return serialReadDeadlineMs(optionsLocked().profile.baud, bytes);
}

// --- opening the port -------------------------------------------------------

bool SerialDevice::openLinkLocked(const SerialFetchOptions &options,
                                  const RemoteLocation &location, Ready *out)
{
    Q_UNUSED(location);
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
        out->reason = Tr::tr("%1 is not plugged in — waiting for it").arg(name());
        return false;
    }

    auto link = std::make_unique<SerialLink>();
    link->port.setPortName(node);
    if (!link->port.open(QIODevice::ReadWrite)) {
        const QString why = link->port.errorString();
        // A PERMISSION IS A WAIT, NOT A REFUSAL, for LogPresence::Unreadable's reason: it is
        // there, so nothing has to appear, and a permission is granted as readily as a file is
        // written. On Linux this is almost always group membership, so the sentence names it —
        // "resource busy" is the other common answer and means another program has the port.
        out->waiting = true;
        out->reason = Tr::tr("Cannot open %1 — %2. On Linux this usually means the account is "
                             "not in the `dialout` group.")
                          .arg(name(), why);
        return false;
    }

    const SerialProfile &p = options.profile;
    link->port.setBaudRate(p.baud);
    link->port.setDataBits(QSerialPort::DataBits(p.dataBits));
    link->port.setParity(toQt(p.parity));
    link->port.setStopBits(toQt(p.stopBits));
    link->port.setFlowControl(toQt(p.flow));
    // Anything the device said before loftail was listening is not an answer to anything.
    link->port.clear();
    setLink(std::move(link));
    diagLog("serial", QStringLiteral("%1: opened %2 at %3 %4%5%6")
                          .arg(name(), node)
                          .arg(p.baud)
                          .arg(p.dataBits)
                          .arg(p.parity == SerialProfile::Parity::None ? QStringLiteral("N")
                                                                      : QStringLiteral("P"))
                          .arg(p.stopBits == SerialProfile::StopBits::One ? QStringLiteral("1")
                                                                         : QStringLiteral("2")));
    return true;
#endif
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
