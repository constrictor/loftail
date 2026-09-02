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

// How much one SshFetcher::fetchForward() read asks for, and therefore how deep
// libssh2's SFTP request pipeline runs. It lives in the header rather than beside the
// fetcher's other constants so that the one ungated test able to state a relation
// against it — tst_sshexec, on the exec transport's unbounded streaming read — can name
// it instead of copying the number and decaying in silence when it moves.
//
// WHY THIS IS THE PIPELINE CONTROL AT ALL, which is not obvious and is the whole reason
// the value is worth choosing rather than picking. libssh2 splits an SFTP read into
// FXP_READ requests of MAX_SFTP_READ_SIZE (30000 bytes) and sends them off without
// waiting for the answers — but only as far ahead as `buffer_size * 4`, where
// buffer_size is the length THIS caller hands it, capped at
// LIBSSH2_CHANNEL_WINDOW_DEFAULT * 4, i.e. 8 MB (libssh2 1.11, src/sftp.c sftp_read()).
// So the size asked for here is multiplied by four and becomes the bytes in flight:
// 256 KB, which this carried until now, was 1 MB in flight against OpenSSH's sftp
// keeping 2 MB (64 requests of 32 KB), and 1 MB is 4 MB — past the point where a
// hundred-millisecond round trip is what limits a remote log's transfer rate.
//
// IT ONLY BECAME THE PIPELINE CONTROL WHEN THE SEEK WENT, which is why it is raised now
// and not earlier: readAt() used to call libssh2_sftp_seek64() before every read, and
// that FLUSHES the read-ahead and the outstanding requests with it, so a bigger ask
// merely bought a bigger cold start (§6.3; SshSession.cpp, Impl::filePos).
//
// THE CEILING IS AT 2 MB and is worth writing down rather than rediscovering: there
// `buffer_size * 4` meets the 8 MB cap, so every byte past it widens nothing and costs
// memory and progress granularity alone. The channel window is NOT the bound it looks
// like — libssh2 grows the receive window itself when the read-ahead would exceed it
// (_libssh2_channel_receive_window_adjust, same function) — so asking past
// LIBSSH2_CHANNEL_WINDOW_DEFAULT neither helps nor hurts; it is the read-ahead cap that
// stops the widening, and nothing else.
//
// AND THE OTHER HALF OF THE TRADE, which the value this replaces was chosen for and
// which is not discarded: a chunk is one step of committedSize, so it is also how
// coarsely a long catch-up fills the view, and a chunk that takes longer than about a
// second to arrive reads as a frozen count rather than as a log loading. That bound is
// in TIME and not in bytes — on the slowest link anybody reads a remote log over, call
// it 1 MB/s, one megabyte IS that second, and on anything faster it is finer. 2 MB would
// not be, which settles the choice between the two without appeal to taste. The PRIME is
// untouched by all of it: it is its own, smaller number (kPrimeBytes in SshFetcher.cpp)
// and fetchForward() never asks for more than the span it was handed.
//
// MEMORY is paid per fetch PASS and not per open tab. fetchForward()'s buffer is a local
// that dies with the pass, and it is sized to the span actually being fetched rather
// than to this constant — so the steady-state poll of a tailing log allocates the few
// kilobytes that arrived, and a megabyte is spent only while a catch-up is running,
// which is the one time it buys anything. What is NOT local is libssh2's own read-ahead,
// which is up to four times this number and is the reason it is not simply set to the
// 2 MB ceiling.
constexpr qint64 kSshFetchChunkBytes = 1024LL * 1024;

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
