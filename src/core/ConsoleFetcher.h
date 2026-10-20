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

#include "ConsoleSession.h"
#include "EndpointResolver.h"
#include "RemoteLocation.h"
#include "SerialFetchOptions.h"
#include "SourceFetcher.h"

#include <functional>
#include <memory>

namespace loftail {

// What one transport over a login console supplies to the fetcher that reads a log through it
// (ARCHITECTURE.md §6.11, §6.12).
//
// The fetcher — the tail loop, the rotation verdict, the spool generations, the grace window
// after a reboot — was SerialFetcher's and is now the console's, because telnet needs every
// line of it and none of the port. What differs per transport is which shared session an
// address resolves to and where its options are kept, and that is all this carries.
struct ConsoleFetcherBinding
{
    // The diagnostic log's area for this transport's lines ("serial", "telnet").
    const char *area = "console";

    // The shared, reference-counted session for this address. ONE PER DEVICE OR HOST, however
    // many logs are open on it (SerialDevice.h's class comment). Released when the FETCHER is
    // destroyed, never earlier.
    std::function<std::shared_ptr<ConsoleSession>(const RemoteLocation &)> acquire;

    // The options this address was primed with. RE-READ ON EVERY POLL, which is what makes
    // "change the settings on the fly" work — so the store behind it must be thread-safe.
    std::function<SerialFetchOptions(const RemoteLocation &)> options;

    // Where to sign in THIS TIME, asked before every login when set, and whether the far end
    // signed in to is still the device's, asked every poll (EndpointResolver.h). Unset for an
    // address a person typed, which then signs in where it says and never asks again.
    EndpointSource endpoint;
};

// Always compiled: it names no QSerialPort and no socket type, only the two seams above.
std::unique_ptr<SourceFetcher> makeConsoleFetcher(const RemoteLocation &location,
                                                  ConsoleFetcherBinding binding);

} // namespace loftail
