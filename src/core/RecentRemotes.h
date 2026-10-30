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

#include <QString>
#include <QStringList>

namespace loftail {

// The machines remote logs were most recently opened on, newest first — what File ▸ Open
// Remote lists first in each of its groups (SPEC.md §3).
//
// A MACHINE AND NOT A LOG: two logs on one device are one entry, so the list is keyed by
// RemoteLocation::target(), the key a connect is already filed under. That is the type and
// the device's own id for a discovered device — never the address it was found at, so a
// device that moved keeps its place — and the transport, account, host and port for an SSH
// or telnet host, which is exactly what HostBookmarkStore::indexOfTarget() matches a saved
// host by. A key never carries a password: target() has none to give.
//
// Pure: the list is kept by whoever owns the settings (MainWindow, beside the recent files)
// and handed to the dialog, so neither the rule nor the dialog needs a settings file to test.
inline constexpr int kMaxRecentRemoteMachines = 20;

// The key `address` is filed under, or empty for anything Open Remote cannot list: a local
// path, a serial device, an address that does not parse.
QString recentRemoteMachineKey(const QString &address);

// `recent` with the machine `address` names moved to the front, and the oldest dropped past
// kMaxRecentRemoteMachines. Unchanged when the address names no such machine.
QStringList withRecentRemoteMachine(QStringList recent, const QString &address);

} // namespace loftail
