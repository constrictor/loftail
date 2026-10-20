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

#include "DiscoveryMatch.h"
#include "RemoteType.h"

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace loftail {

// Watching the network for machines of one remote type (ARCHITECTURE.md §6.13).
//
// THE PLATFORM DECIDES WHAT IS ON THE NETWORK; DiscoveryMatch DECIDES WHAT IS SHOWN. A Bonjour
// browser is the platform's own API — Avahi on Linux, dns_sd on macOS, DnsServiceBrowse on
// Windows — because each of those already shares one mDNS responder with every other program
// on the machine, where a second responder of loftail's own would fight it for port 5353. An
// ONVIF browser is loftail's own WS-Discovery probe, because no platform has one.
//
// A QObject, used on the thread that made it: every backend drives itself from that thread's
// event loop (socket notifiers, timers, or callbacks marshalled back onto it). That is what lets
// the same browser serve the Open Remote dialog on the application thread and a fetcher's
// connect-time lookup on its worker QThread.
class DiscoveryBrowser : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~DiscoveryBrowser() override = default;

    // Begin watching for machines of `type`. Every device reported has already passed
    // deviceMatchesType(). Calling it again restarts the watch.
    virtual void start(const RemoteType &type) = 0;
    virtual void stop() = 0;

protected:
    // What every backend calls with a device it saw: drops it unless it belongs to `type`
    // (deviceMatchesType()), remembers it in the last-seen cache, and emits found(). One place,
    // so no backend can show a device the rules exclude or forget to feed the cache.
    void report(const RemoteType &type, DiscoveredDevice device);

signals:
    void found(const loftail::DiscoveredDevice &device);
    void lost(const QString &typeName, const QString &id);
    // The backend cannot browse at all — no daemon, no network, not compiled in. Said once,
    // in words a person can act on.
    void failed(const QString &reason);
};

// A browser for `type`'s kind of discovery, or one that reports failed() if that kind cannot
// be had in this build. Never null.
std::unique_ptr<DiscoveryBrowser> makeDiscoveryBrowser(const RemoteType &type,
                                                       QObject *parent = nullptr);

// THE SEAM SourceSpoolRegistry::setFetcherFactory() ESTABLISHES, for the reason it does: the far
// end cannot be created by a test. A test installs a factory handing back a scripted browser;
// null restores the platform's.
using DiscoveryBrowserFactory =
    std::function<std::unique_ptr<DiscoveryBrowser>(const RemoteType &type, QObject *parent)>;
void setDiscoveryBrowserFactory(DiscoveryBrowserFactory factory);

// Whether Bonjour discovery is compiled in, and if not, the sentence saying so. ONVIF needs
// nothing optional and is always available.
bool bonjourDiscoveryAvailable(QString *why = nullptr);
// "avahi", "dns_sd", "windns" or empty — for the About box and the diagnostic log.
QString bonjourBackendName();

// --- the last-seen cache ----------------------------------------------------

// Where each device was last seen, process-wide and thread-safe, fed by every browser that
// reports one — so the Open Remote dialog's browsing makes the connect that follows it instant,
// and a device that has stopped answering discovery but is still reachable at its last address
// can still be connected to.
void rememberDiscoveredDevice(const DiscoveredDevice &device);
std::optional<DiscoveredDevice> lastSeenDevice(const QString &typeName, const QString &id);
void clearDiscoveredDevices();   // for a test that must not answer the next test's question

// --- the connect-time lookup ------------------------------------------------

// Find the device `id` of `type`, now. Runs a browser on the CALLING thread with a nested event
// loop — which must therefore have a Qt event dispatcher, the requirement awaitSocketConnected()
// already records for the fetcher's worker QThread — for up to `timeoutMs`, checking `abandoned`
// between slices so a tab closing mid-lookup costs a quarter of a second.
//
// nullopt when it was not found; `why` then says why in a sentence, which is the wait reason the
// tab shows. A device found is remembered as well as returned.
std::optional<DiscoveredDevice> resolveDiscoveredDevice(const RemoteType &type, const QString &id,
                                                        int timeoutMs,
                                                        const std::function<bool()> &abandoned,
                                                        QString *why);

} // namespace loftail
