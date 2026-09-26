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
#include "SerialProfile.h"

namespace loftail {

// The per-device settings a serial fetcher is built with (SPEC.md §3).
//
// THE HANDOFF, AND THE GAP IT MUST NOT REPEAT. SshFetchOptions carries a host's poll cadence
// and tail-start the same way, and ARCHITECTURE.md §8 records what went wrong with it: those
// reach the transport only from the Open Remote dialog and the Remote Hosts menu, so the same
// URL arriving from the command line, the recent-files list or a restored session gets the
// defaults, silently. So this is primed from MainWindow::openFile()'s SINGLE FUNNEL, beside
// primeRemoteCredentials(), and every way in therefore carries it.
//
// ALWAYS COMPILED, exactly as SshFetchOptions.cpp is, so the UI needs no `#if` of its own to
// set an option on a build that cannot open a serial log.
struct SerialFetchOptions
{
    SerialProfile profile;
    qint64        tailStartBytes = 0;   // 0 = read the whole log, as SSH does
};

// Remember the options for an address, and read them back.
//
// THE STORE IS MUTEX-GUARDED, and that is a correction rather than caution. SshFetchOptions'
// hash is unsynchronised and is only correct because it is written before the fetcher exists
// and never read again — but a serial fetcher re-reads its options ON EVERY POLL, which is
// what makes "change the settings on the fly" work at all, so the same shape would be a
// cross-thread race no sanitizer would reach (no test edits a preset while a tab is open).
// std::mutex and never QMutex: this is src/core (§13.1).
void setSerialFetchOptions(const RemoteLocation &location, const SerialFetchOptions &options);
SerialFetchOptions serialFetchOptions(const RemoteLocation &location);

// Forget everything, for a test that must not answer the next test's question — the shape
// tst_sshcredentials' own poisoning took.
void clearSerialFetchOptions();

} // namespace loftail
