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

#include "SerialDevices.h"

#include <QRegularExpression>

namespace loftail {

bool looksLikeUart(const SerialDeviceInfo &info)
{
    if (info.portName.isEmpty())
        return false;

    // The macOS pair. `tty.usbserial-X` and `cu.usbserial-X` are one adapter, and only the
    // second may be opened without waiting for carrier detect — see the header.
    if (info.portName.startsWith(QLatin1String("tty.")))
        return false;

    // A built-in 16550, with or without a vendor id. Anchored, so `ttyS0` goes and a
    // hypothetical `ttySomething` stays.
    static const QRegularExpression builtIn(QStringLiteral("^ttyS[0-9]+$"));
    if (builtIn.match(info.portName).hasMatch())
        return false;

    return info.hasVendorId;
}

QString stableIdFor(const SerialDeviceInfo &info)
{
    // The by-id name first, and the FIRST of them rather than any of them: udev may make
    // several links to one node (`-if00-port0` and a `-if00` alias), and which one is
    // chosen has to be the same answer every time or one device acquires two addresses and
    // therefore two settings records. The enumerator sorts them; taking the front is what
    // makes that sort load-bearing rather than cosmetic.
    for (const QString &link : info.byIdLinks) {
        if (isValidSerialDeviceName(link))
            return link;
    }
    // A serial number is the adapter's own and survives a replug just as well; it is only
    // second because a by-id name also says WHAT the adapter is, which a bare serial
    // number does not.
    if (isValidSerialDeviceName(info.serialNumber))
        return info.serialNumber;
    // Not stable, and that is a fact about the adapter rather than a failure here.
    return info.portName;
}

bool isValidSerialDeviceName(const QString &name)
{
    if (name.isEmpty())
        return false;
    for (const QChar c : name) {
        // Deliberately spelled out rather than run through a regular expression: this is
        // asked on every address parse, and the set is the contract rather than a pattern
        // somebody may later widen by accident.
        const bool ok = (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')
            || (c >= u'0' && c <= u'9') || c == u'.' || c == u'_' || c == u'-';
        if (!ok)
            return false;
    }
    return true;
}

QString shortSerialDeviceName(const QString &deviceId, const QList<SerialDeviceInfo> &devices)
{
    if (deviceId.isEmpty())
        return deviceId;
    for (const SerialDeviceInfo &info : devices) {
        // Either spelling identifies it: the address may carry the port name or any of the stable
        // ids, and all of them name this one port.
        if (info.portName == deviceId || info.serialNumber == deviceId
            || info.byIdLinks.contains(deviceId)) {
            return info.portName;
        }
    }
    // NOTHING RESOLVES — the device is unplugged, or this build cannot enumerate — AND THIS IS THE
    // COMMON CASE RATHER THAN A CORNER: a restored session names a board that is not connected yet.
    // So the id has to be shortened here, and it cannot be shortened downstream: TabLabels.cpp
    // elides the path run and DELIBERATELY not the device, on the reasoning that a machine's name
    // is short. That reasoning is true of a host name and false of a vendor string, so the namer is
    // what must not hand one over.
    //
    // A by-id name has a known shape — `usb-<Vendor>_<Product>_<Serial>-if00-port0` — whose
    // distinguishing part is the serial number, which is both short and what is printed on the
    // adapter. Peel the fixed decorations and take it.
    QString name = deviceId;
    if (name.startsWith(QLatin1String("usb-")))
        name = name.mid(4);
    // The interface and port suffixes udev appends, which say nothing about which adapter this is.
    static const QRegularExpression decoration(QStringLiteral("(-if[0-9A-Fa-f]+)?(-port[0-9]+)?$"));
    name.remove(decoration);
    if (const qsizetype at = name.lastIndexOf(u'_'); at >= 0 && at + 1 < name.size())
        name = name.mid(at + 1);
    if (!name.isEmpty() && name.size() < deviceId.size())
        return name;
    // Not a shape this recognises. The id is what the address says, so it is what is answered —
    // the tab bracket is the caller's to bound, and a name is better than none.
    return deviceId;
}

QString serialDeviceLabel(const SerialDeviceInfo &info)
{
    if (info.description.isEmpty())
        return info.portName;
    // The description first, because that is what a person recognises; the port name in
    // brackets, because that is what tells two of the same adapter apart.
    return QStringLiteral("%1 (%2)").arg(info.description, info.portName);
}

} // namespace loftail
