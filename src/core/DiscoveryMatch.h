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

#include "RemoteType.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace loftail {

// One machine a discovery found (ARCHITECTURE.md §6.13).
//
// `id` IS THE IDENTITY AND THE ONLY THING A SAVED ADDRESS KEEPS: the Bonjour service instance
// name, or the ONVIF endpoint reference (`urn:uuid:…`). Both are chosen by the device and
// survive a change of address, which is the whole reason they are what `discover://` names.
// Everything else here is what the device looks like NOW and is looked up again on every
// connect.
struct DiscoveredDevice
{
    QString     typeName;     // the RemoteType that found it
    QString     id;           // stable: instance name / endpoint reference
    QString     displayName;  // what a person would call it; the instance name, or an ONVIF name scope
    QString     host;         // an address to connect to, numeric where the backend gave one
    int         port = 0;     // the advertised port, 0 for none
    // EVERY address the device answers on right now, `host` among them. A device reached over
    // two interfaces reports both, and a connection to either is a connection to the device —
    // so "the device moved" is the connected address LEAVING this set, never `host` changing,
    // or such a device is reconnected every time the backend happens to name the other one.
    QStringList addresses;
    QStringList details;      // TXT records as `key=value`, or ONVIF scope URIs

    bool operator==(const DiscoveredDevice &) const = default;
};

// THE MATCH RULES, pure, and the same for every backend: a device a backend reports is shown
// only if it passes these, so a platform's own browse API decides what is on the network and
// this decides which of it belongs to the type.
//
// An EMPTY expression matches anything. An INVALID one matches NOTHING — LogPatternNode's rule,
// and for its reason: a typo that matched everything would offer every printer on the network
// as one of the user's cameras, with nothing on screen to say the expression was the problem.
bool discoveryPatternMatches(const QString &pattern, const QString &subject);
bool discoveryPatternMatchesAny(const QString &pattern, const QStringList &subjects);

// Whether `device` belongs to `type` under its kind's rules: the instance name and at least
// one TXT record for Bonjour, at least one scope for ONVIF.
bool deviceMatchesType(const RemoteType &type, const DiscoveredDevice &device);

// The ONVIF `name` scope's value, percent-decoded — `onvif://www.onvif.org/name/Lobby%20Cam`
// is a camera its owner called "Lobby Cam". Empty when the device advertises none.
QString onvifNameFromScopes(const QStringList &scopes);

// The scopes as File ▸ Open Remote's Details column shows them: the `hardware` scope FIRST —
// which camera it is, the thing a person scanning the list is looking for, and otherwise
// wherever in the list the device happened to put it — then the rest in the order advertised.
// The `onvif://www.onvif.org/` every standard scope begins with is taken off and the rest
// percent-decoded, as onvifNameFromScopes() decodes the name; a vendor's own scope, under any
// other prefix, is shown whole. DISPLAY ONLY: a type's scope pattern is still matched against
// the scopes as advertised (deviceMatchesType()), and Test Discovery still shows those.
QStringList onvifScopesForDisplay(const QStringList &scopes);

// --- WS-Discovery (ONVIF), pure --------------------------------------------

// The Probe message for an ONVIF NetworkVideoTransmitter, with `messageId` (a `urn:uuid:…`)
// as its MessageID. Sent to 239.255.255.250:3702.
QByteArray wsDiscoveryProbe(const QString &messageId);

// One ProbeMatch out of a ProbeMatches reply.
struct WsProbeMatch
{
    QString     endpoint;   // EndpointReference/Address — the stable id
    QStringList types;
    QStringList scopes;
    QStringList xaddrs;     // service URLs, the first of which says where the device is
};

// Every ProbeMatch in `datagram`, answering only to `relatesTo` when it is non-empty (a reply
// to somebody else's probe is not an answer to ours, though the device is as real). Never
// throws, never asserts: this is a datagram anybody on the network can send, so a malformed
// one is an empty answer — fuzz_wsdiscovery holds it to that.
QList<WsProbeMatch> parseWsProbeMatches(const QByteArray &datagram,
                                        const QString &relatesTo = QString());

// A ProbeMatch as a device of `type`: the endpoint is the id, the first XAddr's host is where
// it is, and the name scope is what it is called.
DiscoveredDevice deviceFromProbeMatch(const QString &typeName, const WsProbeMatch &match);

} // namespace loftail
