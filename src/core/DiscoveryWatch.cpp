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

#include "DiscoveryWatch.h"

#include "DiagnosticLog.h"
#include "DiscoveryBrowser.h"

#include <QSet>

#include <mutex>

namespace loftail {
namespace {

std::mutex &runningMutex()
{
    static std::mutex m;
    return m;
}

// Folded type names with a browser running. A SET of counts rather than a flag per watch, so
// two windows watching one type do not switch it off for each other.
QHash<QString, int> &running()
{
    static QHash<QString, int> r;
    return r;
}

QString fold(const QString &name)
{
    return name.trimmed().toCaseFolded();
}

void markRunning(const QString &typeName, int delta)
{
    const std::scoped_lock lock(runningMutex());
    const QString key = fold(typeName);
    const int now = running().value(key) + delta;
    if (now > 0)
        running().insert(key, now);
    else
        running().remove(key);
}

} // namespace

DiscoveryWatch::DiscoveryWatch(QObject *parent) : QObject(parent) {}

DiscoveryWatch::~DiscoveryWatch()
{
    setWatchedTypes({});
}

void DiscoveryWatch::startEntry(Entry &entry)
{
    entry.browser = makeDiscoveryBrowser(entry.type, this);
    const QString name = entry.type.name;
    // `found` needs no handler: report() has already put the device in the cache, which is
    // the whole of what the watch is for.
    connect(entry.browser.get(), &DiscoveryBrowser::lost, this,
            [name](const QString &, const QString &id) {
                // NOT AN EVENT. Logged, throttled per device, and otherwise ignored.
                diagLogEvery(60000, "discovery", name + QLatin1Char('/') + id,
                             QStringLiteral("%1/%2 left the network — ignored; an open log on it "
                                            "goes on as it was")
                                 .arg(name, id));
            });
    connect(entry.browser.get(), &DiscoveryBrowser::failed, this, [name](const QString &why) {
        diagLog("discovery", QStringLiteral("watch over %1 cannot browse: %2").arg(name, why));
    });
    entry.browser->start(entry.type);
}

void DiscoveryWatch::setWatchedTypes(const QVector<RemoteType> &types)
{
    QHash<QString, RemoteType> wanted;
    for (const RemoteType &t : types) {
        if (!t.name.trimmed().isEmpty())
            wanted.insert(fold(t.name), t);
    }
    // Stop what is no longer wanted, or wanted differently.
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        const auto w = wanted.constFind(it.key());
        if (w != wanted.constEnd() && *w == (*it)->type) {
            ++it;
            continue;
        }
        (*it)->browser->stop();
        markRunning((*it)->type.name, -1);
        diagLog("discovery", QStringLiteral("watch over %1 stopped").arg((*it)->type.name));
        it = m_entries.erase(it);
    }
    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
        if (m_entries.contains(it.key()))
            continue;
        auto entry = std::make_shared<Entry>();
        entry->type = *it;
        markRunning(entry->type.name, +1);
        startEntry(*entry);
        diagLog("discovery", QStringLiteral("watch over %1 started").arg(entry->type.name));
        m_entries.insert(it.key(), entry);
    }
}

QStringList DiscoveryWatch::watchedTypeNames() const
{
    QStringList out;
    for (const auto &e : m_entries)
        out.append(e->type.name);
    out.sort();
    return out;
}

bool discoveryWatchIsRunning(const QString &typeName)
{
    const std::scoped_lock lock(runningMutex());
    return running().contains(fold(typeName));
}

std::optional<DiscoveredDevice> watchedDevice(const QString &typeName, const QString &id)
{
    if (!discoveryWatchIsRunning(typeName))
        return std::nullopt;
    return lastSeenDevice(typeName, id);
}

} // namespace loftail
