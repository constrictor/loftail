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

// WS-Discovery: parseWsProbeMatches() over whatever datagram arrives on the probe's socket. A
// UDP answer to a multicast probe can come from ANY host on the network segment, so this is
// untrusted input by construction, reaching the parser before anything has been connected to.
//
// Properties, beyond not crashing: every match handed back has an endpoint (a match with none
// has no identity a saved address could name), and every match turns into a device with an id
// and a non-empty display name — what the Open Remote list shows, and never a blank row.

#include "FuzzSupport.h"

#include "DiscoveryMatch.h"

#include <QByteArray>

using namespace loftail;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    if (size > kMaxFuzzInput)
        return 0;
    const QByteArray datagram(reinterpret_cast<const char *>(data), qsizetype(size));
    const QList<WsProbeMatch> matches = parseWsProbeMatches(datagram);
    for (const WsProbeMatch &m : matches) {
        FUZZ_CHECK(!m.endpoint.isEmpty(), "a match with no endpoint was handed back");
        const DiscoveredDevice d = deviceFromProbeMatch(QStringLiteral("T"), m);
        FUZZ_CHECK(d.id == m.endpoint, "the device id is not the endpoint");
        FUZZ_CHECK(!d.displayName.isEmpty(), "a device with no name to show");
    }
    // And the relates-to filter never ADDS a match.
    FUZZ_CHECK(parseWsProbeMatches(datagram, QStringLiteral("urn:uuid:x")).size()
                   <= matches.size(),
               "filtering by RelatesTo produced more matches than not filtering");
    return 0;
}
