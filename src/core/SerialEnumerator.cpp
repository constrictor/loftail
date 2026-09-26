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

#include "SerialEnumerator.h"

#if defined(LOFTAIL_HAVE_SERIAL)
#include <QSerialPortInfo>
#endif

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace loftail {

#if defined(LOFTAIL_HAVE_SERIAL)

namespace {

// Every /dev/serial/by-id link that resolves to `systemLocation`, sorted.
//
// SORTED, AND THE SORT IS LOAD-BEARING. udev may make several links to one node (`-if00` and
// `-if00-port0`), and stableIdFor() takes the front — so which one it takes has to be the same
// answer every time, or one device acquires two addresses and therefore two settings records.
QStringList byIdLinksFor(const QString &systemLocation)
{
    QStringList out;
#if defined(Q_OS_LINUX)
    const QDir byId(QStringLiteral("/dev/serial/by-id"));
    if (!byId.exists())
        return out;   // no udev, or a container: the serial-number rung is what answers then
    const QFileInfo node(systemLocation);
    const QString target = node.canonicalFilePath();
    if (target.isEmpty())
        return out;
    const QFileInfoList links = byId.entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &link : links) {
        if (link.canonicalFilePath() == target)
            out.append(link.fileName());
    }
    out.sort();
#else
    Q_UNUSED(systemLocation);
#endif
    return out;
}

SerialDeviceInfo infoFrom(const QSerialPortInfo &port)
{
    SerialDeviceInfo out;
    out.portName = port.portName();
    out.systemLocation = port.systemLocation();
    out.description = port.description();
    out.manufacturer = port.manufacturer();
    out.serialNumber = port.serialNumber();
    out.hasVendorId = port.hasVendorIdentifier();
    if (out.hasVendorId)
        out.vendorId = port.vendorIdentifier();
    if (port.hasProductIdentifier())
        out.productId = port.productIdentifier();
    out.byIdLinks = byIdLinksFor(out.systemLocation);
    return out;
}

} // namespace

QList<SerialDeviceInfo> enumerateSerialPorts()
{
    QList<SerialDeviceInfo> out;
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    out.reserve(ports.size());
    for (const QSerialPortInfo &port : ports)
        out.append(infoFrom(port));
    return out;
}

#else // !LOFTAIL_HAVE_SERIAL

QList<SerialDeviceInfo> enumerateSerialPorts()
{
    // Nothing to enumerate without the dependency, and this is what makes the menu present,
    // disabled and able to say why rather than absent.
    return {};
}

#endif

QList<SerialDeviceInfo> availableSerialDevices()
{
    QList<SerialDeviceInfo> out;
    for (const SerialDeviceInfo &info : enumerateSerialPorts()) {
        if (looksLikeUart(info))
            out.append(info);
    }
    // Stable order, so the menu does not reshuffle between two openings of it for no reason
    // the reader can see.
    std::sort(out.begin(), out.end(), [](const SerialDeviceInfo &a, const SerialDeviceInfo &b) {
        return a.portName < b.portName;
    });
    return out;
}

QString serialSystemLocationFor(const QString &deviceName)
{
    if (deviceName.isEmpty())
        return QString();

    // RE-ENUMERATED RATHER THAN COMPOSED, because a stable id names a link whose target moves
    // with the plug order — which is the entire reason it is preferred over a port name.
    for (const SerialDeviceInfo &info : enumerateSerialPorts()) {
        if (info.portName == deviceName)
            return info.systemLocation;
        if (info.byIdLinks.contains(deviceName) || info.serialNumber == deviceName)
            return info.systemLocation;
    }

#if defined(Q_OS_LINUX)
    // A by-id link udev made for a port QSerialPortInfo did not list. Worth trying: the
    // enumeration misses a port the process cannot read, and an unreadable device is a
    // different sentence from a missing one (LogPresence's whole subject).
    const QFileInfo link(QStringLiteral("/dev/serial/by-id/") + deviceName);
    if (link.exists())
        return link.canonicalFilePath();
#endif
    // Empty: not plugged in. A WAIT rather than a refusal — an unplugged board is a log that
    // has not turned up yet.
    return QString();
}

} // namespace loftail
