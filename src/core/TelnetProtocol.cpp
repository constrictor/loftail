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

#include "TelnetProtocol.h"

namespace loftail {

bool TelnetProtocol::weAccept(unsigned char option)
{
    // What WE will do when asked: send binary, and not send go-aheads. Never ECHO — a client
    // echoing the far end's output back to it would be a loop with a shell on the other side.
    return option == kBinary || option == kSuppressGoAhead;
}

bool TelnetProtocol::theyMayDo(unsigned char option)
{
    // What we let THEM do. ECHO is accepted rather than refused because nearly every telnetd
    // insists on it for a login, and the login machine and the framing both already cope with
    // a console that echoes — a serial tty echoes too, which is why the BEGIN marker is matched
    // as a whole line rather than as a substring of the echoed command.
    return option == kBinary || option == kEcho || option == kSuppressGoAhead;
}

void TelnetProtocol::command(QByteArray *out, unsigned char verb, unsigned char option)
{
    out->append(char(kIac));
    out->append(char(verb));
    out->append(char(option));
}

QByteArray TelnetProtocol::openingNegotiation()
{
    QByteArray out;
    command(&out, kDo, kBinary);
    command(&out, kWill, kBinary);
    command(&out, kDo, kSuppressGoAhead);
    m_askedHim[kBinary] = true;
    m_askedUs[kBinary] = true;
    m_askedHim[kSuppressGoAhead] = true;
    return out;
}

void TelnetProtocol::negotiate(unsigned char verb, unsigned char option, QByteArray *reply)
{
    // RFC 1143 reduced to what a client that offers almost nothing needs: a reply only where
    // the state CHANGES, and never to an answer to our own question.
    switch (verb) {
    case kWill:
        if (!theyMayDo(option)) {
            command(reply, kDont, option);
        } else if (!m_him[option]) {
            m_him[option] = true;
            if (!m_askedHim[option])
                command(reply, kDo, option);
        }
        m_askedHim[option] = false;
        break;
    case kWont:
        if (m_him[option]) {
            m_him[option] = false;
            if (!m_askedHim[option])
                command(reply, kDont, option);
        }
        m_askedHim[option] = false;
        break;
    case kDo:
        if (!weAccept(option)) {
            command(reply, kWont, option);
        } else if (!m_us[option]) {
            m_us[option] = true;
            if (!m_askedUs[option])
                command(reply, kWill, option);
        }
        m_askedUs[option] = false;
        break;
    case kDont:
        if (m_us[option]) {
            m_us[option] = false;
            if (!m_askedUs[option])
                command(reply, kWont, option);
        }
        m_askedUs[option] = false;
        break;
    default:
        break;
    }
}

QByteArray TelnetProtocol::receive(QByteArrayView arrived, QByteArray *reply)
{
    QByteArray data;
    data.reserve(arrived.size());
    QByteArray scratch;
    QByteArray *answer = reply ? reply : &scratch;

    for (const char ch : arrived) {
        const auto c = static_cast<unsigned char>(ch);
        switch (m_state) {
        case State::Data:
            if (c == kIac) {
                m_pendingCr = false;
                m_state = State::Iac;
                break;
            }
            if (m_pendingCr) {
                m_pendingCr = false;
                // NVT: a bare CR is sent as `CR NUL`. Fold it back, so the console sees the CR
                // the far end's program printed — and only outside binary mode, where a NUL
                // after a CR is a NUL the file holds.
                if (c == 0)
                    break;
            }
            data.append(ch);
            if (c == '\r' && !theySendBinary())
                m_pendingCr = true;
            break;
        case State::Iac:
            switch (c) {
            case kIac:
                // A doubled IAC is one 0xFF byte of data.
                data.append(ch);
                m_state = State::Data;
                break;
            case kWill:
            case kWont:
            case kDo:
            case kDont:
                m_verb = c;
                m_state = State::Option;
                break;
            case kSb:
                m_state = State::Sub;
                break;
            default:
                // NOP, GA, AYT, a stray SE and the rest: commands with no option byte, none of
                // which carries anything this side acts on.
                m_state = State::Data;
                break;
            }
            break;
        case State::Option:
            negotiate(m_verb, c, answer);
            m_state = State::Data;
            break;
        case State::Sub:
            // A subnegotiation is skipped whole. Nothing this side accepts has one, so any that
            // arrives is for an option already refused.
            if (c == kIac)
                m_state = State::SubIac;
            break;
        case State::SubIac:
            m_state = c == kSe ? State::Data : State::Sub;
            break;
        }
    }
    return data;
}

QByteArray TelnetProtocol::encode(QByteArrayView data) const
{
    QByteArray out;
    out.reserve(data.size() + data.size() / 16 + 4);
    const bool binary = weSendBinary();
    for (const char ch : data) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == kIac) {
            out.append(char(kIac));
            out.append(char(kIac));
        } else if (c == '\r' && !binary) {
            out.append("\r\0", 2);
        } else {
            out.append(ch);
        }
    }
    return out;
}

} // namespace loftail
