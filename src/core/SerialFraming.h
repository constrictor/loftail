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

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace loftail {

// Turning one command on a serial console into one answer (ARCHITECTURE.md §6.11).
//
// AN EXEC CHANNEL GIVES THREE THINGS A TTY DOES NOT: a private stdout, a separate stderr,
// and an exit status. A console gives one stream carrying loftail's own commands echoed
// back, the answer, the shell's prompt, and whatever else the device felt like printing
// while all that was happening. This manufactures the exec channel's contract out of that.
//
// The shape is `printf BEGIN; <command> 2>/dev/null; printf END $?`, and the answer is what
// lies between — but HOW the end is found is the one decision in this file that matters.
struct SerialFrame
{
    // A token unique to this run: the session's own random value plus a counter. Per run
    // rather than per session, so a device replaying its scrollback, or a log holding an
    // earlier run's token, cannot close this one.
    QByteArray token;

    // How many bytes of payload to expect, or -1 where the command's output length is not
    // known in advance (every metadata command — a `stat`, a probe, a `test`).
    qint64 expected = -1;
};

// The bytes to write for `command`, framed.
//
// `2>/dev/null` IS PART OF THE FRAME rather than the caller's business, because it is
// runCommand()'s own rule one transport over: a far end's complaint is not the answer we
// asked for, and on one stream there is no other way to keep the two apart. The one command
// that must NOT discard it — a restart script, whose stderr is half of what makes a run
// clean — says so by asking for it, which is why this takes a flag rather than assuming.
QByteArray framedCommand(const QString &command, const SerialFrame &frame,
                         bool keepStdErr = false);

// What one framed run produced, once its end marker has arrived.
struct FramedAnswer
{
    enum class State {
        Incomplete,  // the end marker has not arrived; read more
        Complete,    // `output` and `exitCode` are the answer
        Corrupt,     // the frame arrived malformed — retry rather than believe it
    };

    // Whether the payload came back SHORTER than the length the command was bounded to.
    //
    // NOT AN ERROR, AND TREATING IT AS ONE WAS A DESIGN MISTAKE THIS FOUND. `head -c L` gives
    // fewer bytes whenever the file holds fewer, which is the ordinary end of every file — and
    // it is what the byte-safety probe does deliberately, asking for a 4 KB sample of a log that
    // may be a hundred bytes long. So the frame reports it and the CALLER decides: a fetch that
    // bounded its request by a size it had just observed learns the file shrank underneath it,
    // and a probe learns the log is small. Neither is corruption.
    bool       shortPayload = false;

    State      state = State::Incomplete;
    QByteArray output;
    int        exitCode = 0;
    // How many bytes of `buffer` the frame consumed, so the caller can drop them and keep
    // whatever the device printed afterwards.
    qsizetype  consumed = 0;
};

// Read one framed answer out of everything received so far.
//
// THE PAYLOAD IS LENGTH-DELIMITED WHEREVER ITS LENGTH IS KNOWN, AND THAT IS THE MOST
// IMPORTANT RULE IN THIS FILE. `readCommand()` carries `| head -c L`, so a log read's exact
// byte count is known before the run starts — and a log is arbitrary bytes, which means it
// may contain this run's token. Scanning a binary payload for the end marker is therefore
// how a log truncates itself at whatever point it happens to mention the token, silently,
// on exactly the logs nobody thinks to test. Where the length is NOT known the output is
// text a shell printed and scanning is safe.
//
// THE BEGIN MARKER IS MATCHED AS A WHOLE LINE, NEVER AS A SUBSTRING. With echo on — which
// is the state of a console before `stty -echo` has taken, and the state of one where it
// never will — the command loftail just typed comes back FIRST, and it contains the begin
// token. A `contains()` here silently loses the first read of every command, on every device
// with echo on and only on those.
FramedAnswer readFramedAnswer(const QByteArray &buffer, const SerialFrame &frame);

// AND THE COMPLEMENT OF THE LENGTH RULE, WHICH IS WHY A BASE64 READ MAY LEAVE `expected` AT
// -1. Length-delimiting is what a RAW payload needs, because raw is arbitrary bytes and may
// contain the token. A base64 payload cannot: the alphabet is `A-Za-z0-9+/=` and both markers
// carry a `-`, which is not in it — so no amount of encoded log can spell one, and scanning is
// provably safe there.
//
// That matters because a base64 payload's length is NOT knowable in advance: GNU `base64`
// wraps at 76 columns and busybox's does not, so `4*ceil(n/3)` is right about the data and
// wrong about the newlines. Requiring an exact count would make every encoded read Incomplete
// on one implementation and Corrupt on the other. The marker holding a character the alphabet
// cannot produce is what lets the two rules coexist, and it is the reason the markers are
// spelled `LFB-`/`LFE-` rather than as bare hex.
constexpr char kMarkerCharOutsideBase64 = '-';

// How much of `buffer` is worth keeping while waiting for a frame to complete.
//
// A BOOTING DEVICE PRINTS MEGABYTES BEFORE ANYTHING ANSWERS, which is precisely the input
// requirement 7 is about — so an accumulating buffer is an out-of-memory on the one case
// this transport exists to tolerate. The cap is generous enough for a whole log chunk plus
// a burst of noise; past it the OLDEST bytes go, because the answer is always the newest.
constexpr qsizetype kFrameBufferCap = 4 * 1024 * 1024;

// Trim `buffer` to that cap in place.
//
// IT DROPS ONLY WHAT PRECEDES THE FRAME'S BEGIN MARKER once that has arrived, which is why it
// needs the frame: trimming the front of a buffer whose frame has already started cuts the
// marker off, every later read answers Incomplete, and the command times out against a device
// that answered it correctly. Reachable on a booting board, which is the case the cap exists
// for in the first place.
void capFrameBuffer(QByteArray *buffer, const SerialFrame &frame);

} // namespace loftail
