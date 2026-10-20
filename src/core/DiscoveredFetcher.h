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

#include "DiscoveryMatch.h"
#include "EndpointResolver.h"
#include "RemoteLocation.h"
#include "RemoteType.h"
#include "SourceFetcher.h"

#include <QVector>

#include <memory>
#include <optional>

namespace loftail {

// A log on a discovered device: `discover://Type/id/path` (SPEC.md §3, ARCHITECTURE.md §6.13).
//
// NOT A FOURTH TRANSPORT. The device is reached over SSH or telnet, whichever its type says, by
// the fetcher that already reads those — handed an EndpointResolver that finds the device again
// before every connect. What is new is only the lookup, and the rule that the address names the
// TYPE and the DEVICE and never where either of them is.

// The remote types as hosts.json holds them, process-wide and thread-safe: what a discovered
// fetcher reads its type out of, BY NAME and on every connect, so an edit to a type reaches the
// logs already open through it at their next connect. MainWindow::refreshRemoteHostsMenu()
// sets it from its one enumeration of hosts.json, the same pass that feeds the menu.
void setKnownRemoteTypes(const QVector<RemoteType> &types);
std::optional<RemoteType> knownRemoteType(const QString &name);

// The concrete far end for `device` of `type`, reading `address`'s log: the type's connection,
// account and port rule, the device's current address, and the type's credential key — so a
// password is filed per TYPE, not per address the device happens to have.
RemoteLocation endpointFor(const RemoteType &type, const DiscoveredDevice &device,
                           const RemoteLocation &address);

// What a discovered fetcher is built with (EndpointResolver.h).
//
// `resolve`: where a standing watch is browsing the type (DiscoveryWatch.h), the device it
// last saw, with no lookup at all; otherwise look the device up NOW, and where that finds
// nothing fall back to where it was last seen — a device that has stopped answering discovery
// may still answer a connect.
//
// `stillCurrent`: false only when a standing watch has the device at a set of addresses the
// endpoint is not in, or at another port, for a whole grace period (kMoveGraceMs) — so a
// backend that reports a multi-homed device one interface at a time, or a fresh browser whose
// first answer is a partial set, does not tear down a working connection on the way.
EndpointSource discoveryEndpointSource(const RemoteLocation &address,
                                       int lookupTimeoutMs = 4000);

// The resolver half alone, run once on the calling thread: what Restart App and the config
// editor ask before their one connect. Needs the thread's Qt event loop, as every lookup does.
std::optional<RemoteLocation> resolveDiscoveredEndpoint(const RemoteLocation &address,
                                                        const std::function<bool()> &abandoned,
                                                        QString *why);

// How a `discover://` address is REACHED: its type's connection, Ssh or Telnet — or
// Discovered itself when no type of that name is configured, which every caller treats as
// "cannot be reached at all". Any other address answers its own transport.
RemoteLocation::Transport reachedOver(const RemoteLocation &address);

// How long a device must be seen elsewhere before an open log follows it.
inline constexpr qint64 kMoveGraceMs = 3000;

// The fetcher for a `discover://` address. Refuses, with no I/O, an address whose type is not
// configured — the type is what says SSH or telnet, so without it there is no fetcher to build.
std::unique_ptr<SourceFetcher> makeDiscoveredFetcher(const RemoteLocation &location,
                                                     QString *error);

} // namespace loftail
