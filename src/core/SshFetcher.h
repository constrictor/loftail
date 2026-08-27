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

#include "SourceFetcher.h"
#include "RemoteLocation.h"

#include <memory>

namespace loftail {

// Per-file fetch tuning. The Open Remote dialog sets these before opening; anything
// not set uses the defaults below.
struct SshFetchOptions
{
    // Fetch only the last N bytes of the remote file rather than all of it, for a log
    // far too large to copy down. 0 means "the whole file", which is the default so
    // that a remote log behaves exactly like a local one. When non-zero the view
    // starts mid-file, and the status bar has to say so.
    qint64 tailStartBytes = 0;

    // How often to ask the server whether the file changed. This is the NETWORK
    // cadence; the GUI-side watch tick is separate and much cheaper (§6.3).
    int pollMs = 1000;

    // Bounds the connect and every subsequent SFTP call.
    int timeoutMs = 20000;
};

// Above this, the Open Remote dialog offers to fetch only the tail rather than the
// whole file. Advisory — it changes what is suggested, never what is allowed.
constexpr qint64 kSshLargeFileThreshold = 256LL * 1024 * 1024;

// Remember options for a location, consulted when its fetcher is next built.
void setSshFetchOptions(const RemoteLocation &location, const SshFetchOptions &options);
SshFetchOptions sshFetchOptions(const RemoteLocation &location);

// Build a fetcher that reads `location` over SSH. Returns nullptr with `error` filled
// where SSH support is not compiled in. Connecting happens in start(), not here.
std::unique_ptr<SourceFetcher> makeSshFetcher(const RemoteLocation &location, QString *error);

} // namespace loftail
