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

#include "SerialFetchOptions.h"

#include <QHash>

#include <mutex>

namespace loftail {
namespace {

// Keyed on the address's normal form, as SshFetchOptions is: one log, one spelling, so a
// device opened by its port name and by its stable id are two entries — which is correct,
// they are two addresses and the settings tree may well give them different presets.
std::mutex &optionsMutex()
{
    static std::mutex m;
    return m;
}

QHash<QString, SerialFetchOptions> &optionsStore()
{
    static QHash<QString, SerialFetchOptions> store;
    return store;
}

} // namespace

void setSerialFetchOptions(const RemoteLocation &location, const SerialFetchOptions &options)
{
    const std::scoped_lock lock(optionsMutex());
    optionsStore().insert(location.toString(), options);
}

SerialFetchOptions serialFetchOptions(const RemoteLocation &location)
{
    const std::scoped_lock lock(optionsMutex());
    // BY VALUE, and the lock is released before the caller looks at it: the fetcher reads
    // this from its worker thread on every poll, and handing back a reference into a hash the
    // GUI thread may rewrite is the race the lock exists to prevent.
    return optionsStore().value(location.toString());
}

void clearSerialFetchOptions()
{
    const std::scoped_lock lock(optionsMutex());
    optionsStore().clear();
}

} // namespace loftail
