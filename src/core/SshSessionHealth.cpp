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

#include "SshSessionHealth.h"

namespace loftail {

bool sshErrorEndsSession(int code)
{
    switch (code) {
    // The socket itself: gone, never established, or shut down under us — which is what
    // SshSession::abort() does deliberately, so an aborted call lands here too.
    case SshError::kSocketNone:
    case SshError::kSocketSend:
    case SshError::kSocketRecv:
    case SshError::kSocketDisconnect:
    case SshError::kBadSocket:
    // Silence. The far end stopped answering; see the note on kTimeout in the header.
    case SshError::kTimeout:
    case SshError::kSocketTimeout:
    // The handshake never finished, so there is no transport to keep.
    case SshError::kBannerRecv:
    case SshError::kBannerSend:
    case SshError::kKexFailure:
    case SshError::kKeyExchangeFailure:
    // The encrypted stream is out of step or untrustworthy. These are the ones where
    // carrying on is worse than reconnecting rather than merely futile: after a MAC or
    // decrypt failure the packet stream cannot be resynchronised, and a rekey that fails
    // mid-session leaves the same wreckage a failed initial one does.
    case SshError::kInvalidMac:
    case SshError::kMacFailure:
    case SshError::kDecrypt:
    case SshError::kEncrypt:
    case SshError::kProto:
    case SshError::kZlib:
    case SshError::kCompress:
        return true;
    default:
        // EVERYTHING ELSE IS ABOUT THE REQUEST, not the link, and the default has to sit
        // this way round. A code this function has never heard of is far more likely to
        // be a new thing a server can say about a file than a new way for a socket to
        // die, and guessing "terminal" would answer it by dropping a working connection
        // — on a poll that runs once a second, for ever.
        return false;
    }
}

} // namespace loftail
