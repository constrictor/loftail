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

#include "DiscoveredFetcher.h"

#include "DiscoveryBrowser.h"
#include "DiscoveryWatch.h"
#include "HostBookmarkStore.h"
#include "SshFetcher.h"
#include "TelnetSession.h"

#include <QCoreApplication>

#include <chrono>
#include <memory>
#include <mutex>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::DiscoveredFetcher)
};

std::mutex &typesMutex()
{
    static std::mutex m;
    return m;
}

QVector<RemoteType> &types()
{
    static QVector<RemoteType> t;
    return t;
}

} // namespace

void setKnownRemoteTypes(const QVector<RemoteType> &list)
{
    const std::scoped_lock lock(typesMutex());
    types() = list;
}

std::optional<RemoteType> knownRemoteType(const QString &name)
{
    const std::scoped_lock lock(typesMutex());
    const int at = HostBookmarkStore::indexOfType(types(), name);
    if (at < 0)
        return std::nullopt;
    return types().at(at);
}

RemoteLocation endpointFor(const RemoteType &type, const DiscoveredDevice &device,
                           const RemoteLocation &address)
{
    RemoteLocation out;
    out.transport = type.transport();
    out.user = type.user;
    out.host = device.host;
    out.port = type.effectivePort(device.port);
    out.path = address.path;
    out.credentialOverride = type.credentialTarget();
    return out;
}

namespace {

std::optional<RemoteLocation> resolveNow(const RemoteLocation &address, int lookupTimeoutMs,
                                         const std::function<bool()> &abandoned, QString *why)
{
    const auto type = knownRemoteType(address.discoveryType);
    if (!type) {
        if (why) {
            *why = Tr::tr("There is no remote type called %1 any more — add it again in "
                          "File ▸ Remote Parameters.")
                       .arg(address.discoveryType);
        }
        return std::nullopt;
    }
    // A STANDING WATCH IS ALREADY LOOKING, so what it saw last is as current as a fresh
    // lookup would be and costs nothing — where the lookup costs its whole timeout for a device
    // that is switched off, on every retry.
    std::optional<DiscoveredDevice> device = watchedDevice(type->name, address.host);
    QString lookupFailure;
    if (!device) {
        device = resolveDiscoveredDevice(*type, address.host, lookupTimeoutMs, abandoned,
                                         &lookupFailure);
    }
    // A DEVICE THAT STOPPED ANSWERING DISCOVERY MAY STILL ANSWER A CONNECT: plenty of
    // responders go quiet under load or drop off a multicast group after a network change.
    // Its last address is worth one try, and a connect that fails is a wait like any other.
    if (!device)
        device = lastSeenDevice(type->name, address.host);
    if (!device) {
        if (why) {
            *why = lookupFailure.isEmpty()
                ? Tr::tr("%1 is not on the network right now").arg(address.displayHost())
                : lookupFailure;
        }
        return std::nullopt;
    }
    if (device->host.isEmpty()) {
        if (why) {
            *why = Tr::tr("%1 answered discovery without an address to connect to")
                       .arg(address.displayHost());
        }
        return std::nullopt;
    }
    return endpointFor(*type, *device, address);
}

} // namespace

EndpointSource discoveryEndpointSource(const RemoteLocation &address, int lookupTimeoutMs)
{
    EndpointSource source;
    source.resolve = [address, lookupTimeoutMs](const std::function<bool()> &abandoned,
                                                QString *why) {
        return resolveNow(address, lookupTimeoutMs, abandoned, why);
    };

    // The fetcher's own worker is the only caller, so the grace clock needs no lock; it is
    // shared only so that the function object stays copyable.
    struct Grace
    {
        bool                                  elsewhere = false;
        std::chrono::steady_clock::time_point since;
    };
    auto grace = std::make_shared<Grace>();
    source.stillCurrent = [address, grace](const RemoteLocation &endpoint, QString *where) {
        const auto type = knownRemoteType(address.discoveryType);
        // NOT KNOWING IS NEVER A MOVE: no type, no watch, a device the watch has not seen, or
        // one it saw leave (which leaves the cache exactly as it was).
        const std::optional<DiscoveredDevice> device =
            type ? watchedDevice(type->name, address.host) : std::nullopt;
        if (!type || !device || device->host.isEmpty()) {
            grace->elsewhere = false;
            return true;
        }
        const RemoteLocation now = endpointFor(*type, *device, address);
        // The address SET, not the preferred host: a device on two interfaces is the same
        // device on either, and must not be reconnected every time the backend names the
        // other one. The port is a single value and compares directly.
        const bool here = device->addresses.contains(endpoint.host, Qt::CaseInsensitive)
            && now.port == endpoint.port;
        if (here) {
            grace->elsewhere = false;
            return true;
        }
        // SEEN ELSEWHERE FOR A WHOLE GRACE PERIOD before it counts — measured in elapsed time,
        // never in polls (M13's rule): a fresh browser's first answer may name one address of
        // several, and the rest arrive milliseconds later.
        const auto clock = std::chrono::steady_clock::now();
        if (!grace->elsewhere) {
            grace->elsewhere = true;
            grace->since = clock;
            return true;
        }
        if (clock - grace->since < std::chrono::milliseconds(kMoveGraceMs))
            return true;
        grace->elsewhere = false;
        if (where)
            *where = QStringLiteral("%1:%2").arg(now.host).arg(now.port);
        return false;
    };
    return source;
}

RemoteLocation::Transport reachedOver(const RemoteLocation &address)
{
    if (address.transport != RemoteLocation::Transport::Discovered)
        return address.transport;
    const auto type = knownRemoteType(address.discoveryType);
    return type ? type->transport() : RemoteLocation::Transport::Discovered;
}

std::optional<RemoteLocation> resolveDiscoveredEndpoint(const RemoteLocation &address,
                                                        const std::function<bool()> &abandoned,
                                                        QString *why)
{
    return resolveNow(address, 4000, abandoned, why);
}

std::unique_ptr<SourceFetcher> makeDiscoveredFetcher(const RemoteLocation &location,
                                                     QString *error)
{
    const auto type = knownRemoteType(location.discoveryType);
    if (!type) {
        if (error) {
            *error = Tr::tr("There is no remote type called %1. Add it in File ▸ Remote "
                            "Parameters to open logs found through it.")
                         .arg(location.discoveryType);
        }
        return nullptr;
    }
    if (type->connection == RemoteType::Connection::Telnet)
        return makeTelnetFetcher(location, error, discoveryEndpointSource(location));
#if defined(LOFTAIL_HAVE_SSH)
    return makeSshFetcher(location, error, discoveryEndpointSource(location));
#else
    if (error) {
        *error = Tr::tr("%1 is reached over SSH, and SSH support is not built into this copy "
                        "of loftail.")
                     .arg(location.displayHost());
    }
    return nullptr;
#endif
}

} // namespace loftail
