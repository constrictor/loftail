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

#include <atomic>

namespace loftail {

// libssh2's own error codes, MIRRORED NUMERICALLY.
//
// This file is compiled in every configuration, including one with no libssh2 headers
// anywhere, for the reason SshExecCommands and SshRetryPolicy beside it are: the
// classification below is the only judgement the transport makes about its own health,
// and a rule compiled in one build configuration is a rule tested in one build
// configuration. The numbers are libssh2's public ABI and have not moved since 1.x;
// SshSession.cpp — the one translation unit that can see the real header — carries a
// static_assert per constant, so a mirror that drifts is a compile error there rather
// than a misclassification nobody notices.
//
// A VALUE MAY BE NEWER THAN THE OLDEST HEADER THE TREE BUILDS AGAINST, and kMacFailure is.
// The list is append-only, so the number is settled the moment it is assigned; what a given
// libssh2 decides is whether the NAME exists. Ubuntu 24.04's 1.11.0 stops at -51, so the
// assert for anything below that is guarded over there while the value stays here — the
// two releases share a soname, and a build made against the older one is told -52 by the
// newer one at run time all the same.
namespace SshError {

constexpr int kNone                  = 0;
constexpr int kSocketNone            = -1;
constexpr int kBannerRecv            = -2;
constexpr int kBannerSend            = -3;
constexpr int kInvalidMac            = -4;
constexpr int kKexFailure            = -5;
constexpr int kAlloc                 = -6;
constexpr int kSocketSend            = -7;
constexpr int kKeyExchangeFailure    = -8;
constexpr int kTimeout               = -9;
constexpr int kDecrypt               = -12;
constexpr int kSocketDisconnect      = -13;
constexpr int kProto                 = -14;
constexpr int kAuthenticationFailed  = -18;
constexpr int kChannelFailure        = -21;
constexpr int kChannelRequestDenied  = -22;
constexpr int kChannelClosed         = -26;
constexpr int kChannelEofSent        = -27;
constexpr int kZlib                  = -29;
constexpr int kSocketTimeout         = -30;
constexpr int kSftpProtocol          = -31;
constexpr int kRequestDenied         = -32;
constexpr int kEagain                = -37;
constexpr int kBadUse                = -39;
constexpr int kCompress              = -40;
constexpr int kSocketRecv            = -43;
constexpr int kEncrypt               = -44;
constexpr int kBadSocket             = -45;
constexpr int kMacFailure            = -52;

} // namespace SshError

// Whether `code` means the SESSION is finished, as opposed to this one request having
// failed on a session that is still good.
//
// The distinction is the whole of bugs.md 30. SshSession::isConnected() used to test a
// POINTER, which is cleared only by teardown() — so once a connect had succeeded the
// answer was permanently true, the fetcher's "the link dropped, let go of it and
// reconnect" branch could never be taken, and a tab whose machine went away polled a
// corpse for the rest of its life, reporting "not readable right now" once per session
// timeout and never recovering. Asking the transport rather than the pointer is what
// this is for.
//
// TERMINAL means the socket or the encrypted stream underneath is gone or untrustworthy:
// nothing sent on this session will be understood again, and the only way back is a new
// connection. NOT terminal is everything the far end says about a REQUEST — SFTP's own
// status codes (which libssh2 reports as kSftpProtocol, with the FX code fetched
// separately), a channel the server refused, an authentication that failed, a would-block
// on a non-blocking read. Those all arrive on a healthy link and must leave it alone, or
// a log that is merely missing would be answered by tearing the connection down.
//
// kTimeout is deliberately on the terminal side, and it is the one judgement here that
// costs something: a command that genuinely takes longer than the session timeout — the
// `wc` size rung on a large file over a slow link — is read as a dropped link and answered
// with a reconnect. That is the honest reading (libssh2 cannot tell the two apart, which
// is exactly what the SFTP-probe comment in SshSession::connectTo says), the recovery is
// automatic and bounded, and the alternative is the bug: a session that has stopped
// answering entirely is reported as nothing but a slow one, for ever.
bool sshErrorEndsSession(int code);

// The latch itself: one session's answer to "are you still usable?".
//
// LATCHED, NEVER ACTED ON. Setting this frees nothing and closes nothing — the thread
// inside libssh2 owns the session and tears it down when its call returns, which is the
// same discipline SshSession::abort() follows and for the same reason: a session in use
// must never be freed under its user. All this does is stop isConnected() lying, so that
// the fetcher's existing "let go and reconnect" branch becomes reachable.
//
// std::atomic because it is written by whichever thread is inside a libssh2 call and may
// be read by another (a fetcher publishes its session for abort()); relaxed is enough,
// as nothing else is published through it.
class SessionHealth
{
public:
    // Report the outcome of a libssh2 call. A terminal code sets the flag; ANY OTHER CODE
    // LEAVES IT EXACTLY AS IT WAS, success included — because a code that is not terminal
    // is not by itself evidence that the far end is there. An EAGAIN, a channel refusal
    // and a zero-length read all arrive just as readily on a link that has gone. Saying
    // the session is alive again is therefore a separate, deliberate statement, made by
    // markAlive() from the sites that got a whole answer back.
    void noteError(int code)
    {
        if (sshErrorEndsSession(code))
            m_dead.store(true, std::memory_order_relaxed);
    }

    // A call that demonstrably got an answer OUT of the far end and back. Two kinds of
    // caller, and both are load-bearing rather than decoration.
    //
    // A connect that got all the way in: connectTo() probes for SFTP and reads a plain
    // kTimeout from a server that accepts the subsystem channel with no sftp-server
    // behind it, then falls back to shell commands and returns success (§6.3.1). Without
    // the clear, the fallback's own probe would condemn the exec session it just
    // established, and the transport would be dead on arrival on exactly the servers it
    // exists for.
    //
    // And a `stat` that answered: SshSession::statPath() is the call SshFetcher::pollOnce()
    // makes first and the only one whose failure it answers by asking isConnected(), so
    // "the stat that just replied proves the link" is both the narrowest true statement
    // available and the one that runs once per poll ahead of everything else. It has to be
    // said, because the flag is set from calls that can latch WITHOUT failing: a
    // 256 KB SFTP read that delivered 200 KB and then timed out hands its caller a
    // positive byte count, the spool advances and the tab tails on normally — so an
    // unclearable latch would sit there invisibly and then fire on the first perfectly
    // benign stat failure afterwards (the gap during a `logrotate`, or M13's "the log is
    // not there yet"), reporting a dropped link about a link that never dropped and
    // paying a full connect + host-key check + authentication + re-fetch from offset 0
    // for it, once per poll, on somebody else's machine.
    //
    // Clearing it cannot re-open bugs.md 30, and the reason is one sentence: when the link
    // is genuinely gone nothing succeeds, so nothing calls this. A latch is only ever
    // meaningful while nothing can get an answer.
    void markAlive() { m_dead.store(false, std::memory_order_relaxed); }

    bool dead() const { return m_dead.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> m_dead{false};
};

} // namespace loftail
