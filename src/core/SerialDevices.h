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

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace loftail {

// What is known about one serial port, as a plain value (SPEC.md §3, ARCHITECTURE.md
// §6.11).
//
// THE STRUCT AND THE DECISIONS ARE ALWAYS COMPILED; ONLY THE FILLING OF IT IS GATED.
// QSerialPortInfo cannot be constructed synthetically, so a filter written against it
// directly would be a filter no test could reach — and "which of these is a UART" and
// "what is this device's stable name" are the whole decision. That is the
// SshExecCommands / ExecSizeProbe / SshRetryPolicy / PathTrouble / SshSessionHealth
// argument made for the sixth time, and SerialEnumerator.cpp is the gated half whose only
// job is filling this in.
struct SerialDeviceInfo
{
    // What the OS calls the port: `ttyUSB0`, `ttyACM0`, `COM3`, `cu.usbserial-A50285BI`.
    QString portName;
    // What to open: `/dev/ttyUSB0`, `\\.\COM3`.
    QString systemLocation;
    QString description;
    QString manufacturer;
    QString serialNumber;

    quint16 vendorId = 0;
    quint16 productId = 0;
    // Asked rather than inferred from a non-zero id, because 0 is a legal vendor id and
    // "no vendor id at all" is the thing being tested.
    bool    hasVendorId = false;

    // The base names of every /dev/serial/by-id link that resolves to `systemLocation`,
    // sorted, empty where there are none (a machine with no udev, a container). Filled by
    // the enumerator; the derivation below is what decides which one to use.
    QStringList byIdLinks;
};

// Whether this port is worth offering under File ▸ Serial (requirement 1).
//
// THE TEST IS A VENDOR IDENTIFIER, not a name blacklist. A USB-serial adapter and a
// CDC-ACM device both have one; a motherboard's 16550 does not. That is the one portable,
// non-heuristic discriminator there is, and it beats enumerating the names of every UART
// anybody ever shipped — which is what makes a list of built-in ttys a belt-and-braces
// exclusion below rather than the rule.
//
// TWO PLATFORM RULES RIDE WITH IT AND BOTH ARE EASY TO LOSE.
//
// A macOS `tty.*` device is EXCLUDED and its `cu.*` twin kept, although they are the same
// hardware. Opening the `tty.` node BLOCKS until the far end asserts carrier detect, so a
// board that is powered off — the very case requirement 7 is about — would hang the open
// rather than wait for it, which is exactly the freeze M17 exists to have removed.
//
// A name of the form `ttyS<digits>` is excluded even WITH a vendor id, because a PC
// enumerates dozens of them and a list of eight dead ports is worse than no list.
bool looksLikeUart(const SerialDeviceInfo &info);

// The name a serial address carries for this device (SPEC.md §3).
//
// PREFER A STABLE ID, because the alternative silently repoints a tab. `ttyUSB0` is
// allocated in plug order, so unplugging two adapters and plugging them back the other way
// round makes the address name the OTHER board — and with it that board's remembered
// format, its filters and its settings record, with nothing on screen to say so. A
// /dev/serial/by-id name is derived from the adapter's own vendor, product and serial
// number, so it follows the hardware.
//
// The ladder is: a by-id name where udev made one, else the manufacturer's serial number,
// else the plain port name. The last rung is not a failure — plenty of adapters ship with
// no serial number, and a plain name is what such a device has — it is just not stable,
// which is a fact about the adapter rather than about loftail.
QString stableIdFor(const SerialDeviceInfo &info);

// Whether a string may be the device half of a `serial://` address.
//
// `[A-Za-z0-9._-]+` AND NOTHING ELSE, which is narrow on purpose. It covers every real
// name on every platform — `ttyUSB0`, `ttyACM0`, `usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0`,
// `COM3`, `cu.usbserial-A50285BI` — and because none of those characters means anything to
// a URL, the address's normal form is idempotent by construction with no encoding to get
// wrong in either direction. Anything else is refused where it is parsed, which is decided
// with no I/O and is therefore an M17 refusal naming the address rather than a tab that
// waits for a device that cannot exist.
bool isValidSerialDeviceName(const QString &name);

// The SHORT name for a device id — what a tab bracket can afford.
//
// A stable id is a vendor string: `usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0` is forty-five
// characters, which is the right thing to key settings on (it follows the hardware) and an
// unreadable thing to put beside a log's name. This answers the PORT NAME the id currently
// resolves to — `ttyUSB0`, which is what a person recognises and what the menu shows in brackets
// — and falls back to the id where nothing resolves, because a long name is better than none.
//
// PURE, over a list the caller supplies, for the reason every decision in this header is: the
// enumeration cannot be constructed by a test.
QString shortSerialDeviceName(const QString &deviceId, const QList<SerialDeviceInfo> &devices);

// What the menu calls this device: its description and its port name, or just the port
// name. NOT the address — a by-id name is 40 characters of vendor string, which is the
// right thing to key settings on and the wrong thing to put on a menu.
QString serialDeviceLabel(const SerialDeviceInfo &info);

} // namespace loftail
