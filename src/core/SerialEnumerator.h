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

#include "SerialDevices.h"

#include <QList>
#include <QString>

namespace loftail {

// Every serial port this machine has, filled from QSerialPortInfo.
//
// THE ONLY GATED HALF OF THE ENUMERATION, and it holds no decisions: which ports are worth
// offering and what a device is called are pure functions over SerialDeviceInfo
// (SerialDevices.h), because QSerialPortInfo cannot be constructed synthetically and a filter
// written against it directly would be a filter no test could reach.
//
// Without serial support this answers an empty list, so the menu is present, disabled and
// says why — the File menu's own rule about a vanished entry reading as a feature that does
// not exist.
QList<SerialDeviceInfo> enumerateSerialPorts();

// Every port that looks like a UART, which is what the menu offers.
QList<SerialDeviceInfo> availableSerialDevices();

// What to open for a device NAME, which may be a stable id or a plain port name.
//
// RESOLVED BY RE-ENUMERATING, never by string surgery: a stable id is a `/dev/serial/by-id`
// link whose target moves with the plug order, which is the whole point of preferring it. An
// empty answer means the device is not plugged in, and THAT IS A WAIT rather than a refusal —
// an unplugged board is a log that has not turned up, which M13 already handles.
QString serialSystemLocationFor(const QString &deviceName);

} // namespace loftail
