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

#include "RecentRemotes.h"

#include "RemoteLocation.h"

namespace loftail {

QString recentRemoteMachineKey(const QString &address)
{
    const std::optional<RemoteLocation> location = RemoteLocation::parse(address);
    if (!location)
        return {};
    switch (location->transport) {
    case RemoteLocation::Transport::Ssh:
    case RemoteLocation::Transport::Telnet:
    case RemoteLocation::Transport::Discovered:
        return location->target();
    case RemoteLocation::Transport::Serial:
        break;   // File ▸ Serial lists devices; Open Remote never does
    }
    return {};
}

QStringList withRecentRemoteMachine(QStringList recent, const QString &address)
{
    const QString key = recentRemoteMachineKey(address);
    if (key.isEmpty())
        return recent;
    recent.removeAll(key);
    recent.prepend(key);
    while (recent.size() > kMaxRecentRemoteMachines)
        recent.removeLast();
    return recent;
}

} // namespace loftail
