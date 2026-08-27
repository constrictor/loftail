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

#include "SshFetcher.h"

#include <QHash>

namespace loftail {

// Per-location fetch tuning, kept in a translation unit that is ALWAYS compiled even
// though the fetcher that consumes it is not. The Open Remote dialog and the Remote
// Hosts menu set these on every build, so that the UI needs no #if of its own — the
// whole point of putting the transport behind SourceFetcher. Without SSH support the
// values are simply stored and never read.
namespace {

QHash<QString, SshFetchOptions> &optionsStore()
{
    static QHash<QString, SshFetchOptions> store;
    return store;
}

} // namespace

void setSshFetchOptions(const RemoteLocation &location, const SshFetchOptions &options)
{
    optionsStore().insert(location.toString(), options);
}

SshFetchOptions sshFetchOptions(const RemoteLocation &location)
{
    return optionsStore().value(location.toString(), SshFetchOptions{});
}

} // namespace loftail
