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
#include "HostBookmarkStore.h"
#include "SshFetcher.h"
#include "TelnetSession.h"

#include <QCoreApplication>

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

EndpointResolver discoveryResolver(const RemoteLocation &address, int lookupTimeoutMs)
{
    return [address, lookupTimeoutMs](const std::function<bool()> &abandoned,
                                      QString *why) -> std::optional<RemoteLocation> {
        const auto type = knownRemoteType(address.discoveryType);
        if (!type) {
            if (why) {
                *why = Tr::tr("There is no remote type called %1 any more — add it again in "
                              "File ▸ Remote Parameters.")
                           .arg(address.discoveryType);
            }
            return std::nullopt;
        }
        QString lookupFailure;
        std::optional<DiscoveredDevice> device =
            resolveDiscoveredDevice(*type, address.host, lookupTimeoutMs, abandoned,
                                    &lookupFailure);
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
    };
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
        return makeTelnetFetcher(location, error, discoveryResolver(location));
#if defined(LOFTAIL_HAVE_SSH)
    return makeSshFetcher(location, error, discoveryResolver(location));
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
