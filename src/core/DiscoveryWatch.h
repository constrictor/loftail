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

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>

namespace loftail {

class DiscoveryBrowser;

// THE STANDING WATCH over the devices whose logs are open (ARCHITECTURE.md §6.13).
//
// A `discover://` log is looked up before every connect, which finds a device that moved only
// once its old link has FAILED — and a link to an address DHCP has since handed to another
// machine does not fail, it shows that machine's log. So while such a log is open its type is
// browsed continuously, and what the browser sees lands in the last-seen cache, where every
// fetcher on a device of that type asks on each poll whether the address it is connected to is
// still the device's (discoveryEndpointSource()'s `stillCurrent`).
//
// Three rules, each easy to undo:
//
// - `found` is the ONLY event that does anything, and all it does is feed the cache.
//   `lost` NEVER evicts, never reconnects and never marks a tab — Bonjour responders and
//   daemons report spurious removals, and a log that blinked out every time one did would be
//   worse than one that never followed a move. A device that has really gone says so through
//   its connection failing, which the ordinary wait and stale states already report.
// - It is refreshed from the window's two existing refresh points and nowhere else — when the
//   set of open logs changes and when hosts.json changes — so no timer re-browses and nothing
//   polls the type list.
// - It lives on the APPLICATION thread, whose event loop drives every backend; the fetchers on
//   their workers read the cache and the running set below, both of which are guarded.
class DiscoveryWatch : public QObject
{
    Q_OBJECT

public:
    explicit DiscoveryWatch(QObject *parent = nullptr);
    ~DiscoveryWatch() override;

    // Browse exactly these types: start one newly named, restart one whose definition moved,
    // stop the rest.
    void setWatchedTypes(const QVector<RemoteType> &types);

    QStringList watchedTypeNames() const;

private:
    struct Entry
    {
        RemoteType                        type;
        std::unique_ptr<DiscoveryBrowser> browser;
    };
    void startEntry(Entry &entry);

    QHash<QString, std::shared_ptr<Entry>> m_entries;   // keyed on the folded type name
};

// Whether a standing watch is browsing `typeName` right now. Answered from any thread.
bool discoveryWatchIsRunning(const QString &typeName);

// The cached device, but ONLY while a watch keeps that cache current — anything older is a
// guess, which the connect path may try but a "has it moved?" question must not act on.
std::optional<DiscoveredDevice> watchedDevice(const QString &typeName, const QString &id);

} // namespace loftail
