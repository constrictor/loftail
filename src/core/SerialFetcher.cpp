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

#include "SerialFetcher.h"

#include "ConsoleFetcher.h"
#include "SerialDevice.h"
#include "SerialFetchOptions.h"

#include <QCoreApplication>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::SerialFetcher)
};

} // namespace

#if !defined(LOFTAIL_HAVE_SERIAL)

std::unique_ptr<SourceFetcher> makeSerialFetcher(const RemoteLocation &location, QString *error)
{
    Q_UNUSED(location);
    if (error) {
        // A REFUSAL DECIDED WITH NO I/O, so it fails the open outright and says why (M17) —
        // the same sentence shape a build with no SSH or no archive support gives.
        *error = Tr::tr("Serial support is not built into this copy of loftail, so a log on a "
                        "device cannot be opened.");
    }
    return nullptr;
}

#else

std::unique_ptr<SourceFetcher> makeSerialFetcher(const RemoteLocation &location, QString *error)
{
    if (location.transport != RemoteLocation::Transport::Serial) {
        if (error)
            *error = Tr::tr("Not a serial log address: %1").arg(location.toString());
        return nullptr;
    }
    // THE WHOLE OF WHAT IS SERIAL ABOUT A SERIAL FETCHER: which session a device name
    // resolves to, and where its options are kept. Everything else is the console's
    // (ConsoleFetcher.h), and is shared with telnet.
    ConsoleFetcherBinding binding;
    binding.area = "serial";
    binding.acquire = [](const RemoteLocation &loc) -> std::shared_ptr<ConsoleSession> {
        return SerialDeviceRegistry::instance().acquire(loc.host);
    };
    binding.options = [](const RemoteLocation &loc) { return serialFetchOptions(loc); };
    return makeConsoleFetcher(location, std::move(binding));
}

#endif // LOFTAIL_HAVE_SERIAL

} // namespace loftail
