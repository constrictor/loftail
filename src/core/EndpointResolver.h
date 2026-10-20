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

#include <QString>

#include <functional>
#include <optional>

namespace loftail {

// Where a fetcher should connect THIS TIME (ARCHITECTURE.md §6.13).
//
// A fetcher reads its far end out of the address it was built for, once — which is right for
// every address a person types, and wrong for a `discover://` one, whose far end is whatever
// address the device has now. So a fetcher may be handed one of these, and asks it before
// EVERY connect: the first, and each reconnect after a dropped link, which is exactly when a
// device that moved needs finding again.
//
// THE DEFAULT IS IDENTITY: a fetcher with no resolver connects to its own address, so every
// existing transport is untouched by the seam existing — and tst_sshlive's PASS list staying
// the same is the evidence that it is.
//
// nullopt means "not now": the device is not on the network, or its type is gone. `why` is
// the wait reason the tab shows, and the fetcher WAITS rather than refusing — a device that is
// switched off is a log that has not turned up (M13). `abandoned` is the fetcher's own stop
// check, so a tab closing mid-lookup costs a slice rather than the lookup's timeout.
using EndpointResolver = std::function<std::optional<RemoteLocation>(
    const std::function<bool()> &abandoned, QString *why)>;

// Whether the far end a fetcher is connected to is STILL the device's, asked on every poll.
//
// A connect-time lookup finds a device that moved only once its old link has failed, and a
// link to an address that has since been handed to another machine does not fail — it shows
// that machine's log. So a fetcher holding one of these asks it each poll, and on `false`
// drops its session and reconnects through the resolver, which is what a moved device's logs
// being REOPENED at the new address amounts to. `where`, on false, names the new address for
// the diagnostic line.
//
// TRUE WHENEVER IT DOES NOT KNOW: no standing watch, a device not seen, an address set that
// still holds the endpoint. A false is a positive statement that the device is somewhere else,
// never an absence of news — which is what keeps a device whose service blinked out of
// Bonjour, as Bonjour's do, from losing its log for it.
using EndpointCheck =
    std::function<bool(const RemoteLocation &endpoint, QString *where)>;

// The two together: what a fetcher for a moving far end is built with. Both empty is the
// default, and is identity — the fetcher connects to its own address and never asks again.
struct EndpointSource
{
    EndpointResolver resolve;
    EndpointCheck    stillCurrent;
};

} // namespace loftail
