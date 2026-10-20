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
#include <QByteArrayView>

#include <array>

namespace loftail {

// The telnet layer (RFC 854/855) between a TCP socket and a login console, and NOTHING ELSE
// (ARCHITECTURE.md §6.12).
//
// PURE: no socket, no clock, no QObject — bytes in, bytes out — so the whole of the protocol
// is driven by tst_telnetprotocol and fuzz_telnetprotocol with nothing listening. Everything
// above it (the login conversation, the framing, the byte-safety probe) is ConsoleSession's
// and is shared with the serial transport, which is why this has to hand back exactly what a
// serial port would: the bytes the far end's shell printed, with every IAC sequence gone.
//
// THREE RULES THAT ARE EASY TO UNDO, each silent when broken.
//
//   * STATE CROSSES READS. An IAC sequence split across two TCP segments is the ordinary case
//     on a slow link, not a corner, and a decoder that resets per call lets the second half
//     through as data — a stray `\xfb\x01` in front of a login prompt, which the prompt regex
//     then fails to match only on the links that fragment. Same reason the login machine
//     matches against a window rather than the arriving chunk.
//   * A REPLY IS SENT ONLY ON A CHANGE OF STATE. Answering every WILL with a DO, including a
//     WILL that confirms our own request, is how two endpoints negotiate one option for ever —
//     RFC 854's loop, and RFC 1143 is the long version of avoiding it.
//   * BINARY IS ASKED FOR IN BOTH DIRECTIONS, and the rest of the transport depends on it being
//     granted: in binary mode the far end's telnetd passes bytes through untouched, so a raw
//     log read arrives as the bytes the file holds. Where it is refused, an NVT `CR NUL` is
//     folded back to `CR` — and the byte-safety probe, which never believes anything it has
//     not round-tripped, is what decides whether reads may travel raw at all.
//
//   * A NEWLINE GOES OUT AS A BARE LF, IN BOTH MODES, AND THAT IS A DELIBERATE DEPARTURE FROM
//     RFC 854's CR LF. The byte-safety probe runs `stty raw -echo`, after which the far end's
//     terminal no longer turns a CR into a newline — and telnetd hands the CR on (netkit and
//     busybox fold CR LF to CR before the pty sees it). So CR LF works until the probe, then
//     every command arrives with a stray `\r` on its last word, the frame's end marker never
//     matches, and the log never arrives. A bare LF is passed through untouched by every
//     telnetd, raw mode or not. Found in a manual run against a real pty, which no fake that
//     treats CR and LF alike could show.
class TelnetProtocol
{
public:
    // Option codes this side ever agrees to. Anything else is refused.
    static constexpr unsigned char kBinary = 0;
    static constexpr unsigned char kEcho = 1;
    static constexpr unsigned char kSuppressGoAhead = 3;

    // Commands, for tests and for the encoder.
    static constexpr unsigned char kIac = 255;
    static constexpr unsigned char kDont = 254;
    static constexpr unsigned char kDo = 253;
    static constexpr unsigned char kWont = 252;
    static constexpr unsigned char kWill = 251;
    static constexpr unsigned char kSb = 250;
    static constexpr unsigned char kSe = 240;

    // What a client opens with: ask for binary both ways and for no go-aheads. Recorded as
    // REQUESTED, so the far end's agreement is taken as an answer and not answered again.
    QByteArray openingNegotiation();

    // Feed bytes as they arrived from the socket. Returns the data with every telnet command
    // removed; appends to `*reply` anything this side owes the far end in answer.
    QByteArray receive(QByteArrayView arrived, QByteArray *reply);

    // Bytes we want the far end's shell to read, made safe to put on the wire: every 0xFF
    // doubled, and — only while we are NOT in binary mode — a bare `\r` sent as `\r\0`.
    // A `\n` goes out as itself in both modes (see the class comment).
    QByteArray encode(QByteArrayView data) const;

    // Whether each direction has been granted binary mode.
    bool weSendBinary() const { return m_us[kBinary]; }
    bool theySendBinary() const { return m_him[kBinary]; }
    // Whether the far end has said it will echo what it is sent.
    bool theyEcho() const { return m_him[kEcho]; }

private:
    enum class State { Data, Iac, Option, Sub, SubIac };

    static bool weAccept(unsigned char option);
    static bool theyMayDo(unsigned char option);
    static void command(QByteArray *out, unsigned char verb, unsigned char option);
    void negotiate(unsigned char verb, unsigned char option, QByteArray *reply);

    State         m_state = State::Data;
    unsigned char m_verb = 0;
    bool          m_pendingCr = false;   // a CR seen last call, awaiting its NUL or LF
    std::array<bool, 256> m_us{};       // options WE perform
    std::array<bool, 256> m_him{};      // options THEY perform
    std::array<bool, 256> m_askedUs{};  // we sent WILL and have not had the answer
    std::array<bool, 256> m_askedHim{}; // we sent DO and have not had the answer
};

} // namespace loftail
