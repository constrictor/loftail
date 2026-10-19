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

// The telnet layer: TelnetProtocol over whatever bytes a far end chooses to send. A telnet
// server is somebody else's machine, and these bytes reach the login console before anything
// has been authenticated, so this is a layer that eats untrusted input by construction.
//
// THREE PROPERTIES, each one a rule TelnetProtocol.h states:
//
//   * STATE CROSSES READS. Feeding the input whole and feeding it in two pieces split at a
//     fuzzer-chosen point must give the same data and the same replies — the property a
//     decoder that resets per call breaks only on links that fragment.
//   * IT ONLY EVER REMOVES. The data handed back is never longer than what arrived, and the
//     replies are bounded by three bytes per byte received; a negotiation that answered an
//     answer would grow without bound, and that is RFC 854's loop.
//   * ENCODE IS RECEIVE'S INVERSE IN BINARY MODE. Whatever this side sends through encode()
//     comes back out of a peer's receive() byte for byte, 0xFF included — the doubled IAC is
//     the one transformation binary mode keeps.

#include "FuzzSupport.h"

#include "TelnetProtocol.h"

#include <QByteArray>
#include <QByteArrayView>

using namespace loftail;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    if (size == 0 || size > kMaxFuzzInput)
        return 0;
    const QByteArrayView input(reinterpret_cast<const char *>(data) + 1, qsizetype(size - 1));
    const qsizetype cut = input.isEmpty() ? 0 : qsizetype(data[0]) % (input.size() + 1);

    TelnetProtocol whole;
    QByteArray wholeReply;
    const QByteArray wholeData = whole.receive(input, &wholeReply);

    TelnetProtocol split;
    QByteArray splitReply;
    QByteArray splitData = split.receive(input.first(cut), &splitReply);
    splitData += split.receive(input.sliced(cut), &splitReply);

    FUZZ_CHECK(wholeData == splitData, "a split read decodes differently from a whole one");
    FUZZ_CHECK(wholeReply == splitReply, "a split read negotiates differently from a whole one");
    FUZZ_CHECK(wholeData.size() <= input.size(), "receive() made data out of nothing");
    FUZZ_CHECK(wholeReply.size() <= 3 * input.size(), "negotiation answered without bound");

    // The inverse, in binary mode both ways.
    TelnetProtocol sender;
    sender.openingNegotiation();
    QByteArray grant;
    grant.append(char(TelnetProtocol::kIac));
    grant.append(char(TelnetProtocol::kDo));
    grant.append(char(TelnetProtocol::kBinary));
    sender.receive(grant, nullptr);
    TelnetProtocol receiver;
    receiver.openingNegotiation();
    QByteArray offer;
    offer.append(char(TelnetProtocol::kIac));
    offer.append(char(TelnetProtocol::kWill));
    offer.append(char(TelnetProtocol::kBinary));
    receiver.receive(offer, nullptr);
    FUZZ_CHECK(sender.weSendBinary() && receiver.theySendBinary(), "binary was not granted");
    const QByteArray payload = input.toByteArray();
    FUZZ_CHECK(receiver.receive(sender.encode(payload), nullptr) == payload,
               "encode() is not receive()'s inverse in binary mode");
    return 0;
}
