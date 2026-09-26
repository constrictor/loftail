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

#include "ConfigChangeWatcher.h"

#include "ConfigFileIO.h"
#include "ConfigView.h"
#include "LiveController.h"
#include "LogSource.h"

#include <QDateTime>
#include <QFileInfo>
#include <QTimer>

namespace loftail {

ConfigChangeWatcher::ConfigChangeWatcher(ConfigView *view)
    : QObject(view), m_view(view), m_address(view->address()),
      m_remote(configAddressIsRemote(view->address()))
{
    if (m_remote) {
        m_poll = new QTimer(this);
        m_poll->setObjectName(QStringLiteral("configChangePoll")); // findChild, for tests
        m_poll->setInterval(kRemotePollMs);
        connect(m_poll, &QTimer::timeout, this, &ConfigChangeWatcher::probeRemote);
        m_poll->start();
        return;
    }
    // The stat the page's own read was taken against — near enough: anything that moved
    // between that read and this line is caught by the content comparison the first
    // event after it runs, because a stat that moved is what triggers one.
    m_lastStat = statNow();
    m_watcher = new LiveWatcher(this);
    connect(m_watcher, &LiveWatcher::maybeChanged, this, &ConfigChangeWatcher::checkLocal);
    m_watcher->watch(m_address);
}

ConfigChangeWatcher::~ConfigChangeWatcher() = default;

void ConfigChangeWatcher::checkNow()
{
    if (m_remote)
        probeRemote();
    else
        checkLocal();
}

ConfigChangeWatcher::Stat ConfigChangeWatcher::statNow() const
{
    const QFileInfo info(m_address);
    Stat s;
    s.exists = info.exists();
    if (s.exists) {
        s.size = info.size();
        s.mtimeMs = info.lastModified().toMSecsSinceEpoch();
        // The identity as well as the size and time: an editor that saves through a
        // rename puts a new file at the path, and on a coarse-grained filesystem one the
        // same size written in the same second would otherwise look like no change.
        s.identity = pathIdentity(m_address);
    }
    return s;
}

void ConfigChangeWatcher::checkLocal()
{
    // Our own write is synchronous on this thread and followed at once by
    // markWrittenAs(), so by the time any event from it arrives here the baseline
    // already holds those bytes — the content comparison, not this gate, is what makes
    // our own save silent. The gate is only a cost bound.
    if (m_view->isBusy())
        return;
    const Stat now = statNow();
    if (now == m_lastStat)
        return;
    m_lastStat = now;
    const ConfigReadResult read = readConfigFile(m_address);
    // A file that cannot be read right now (mid-write permissions, a directory put at the
    // path) is not a change worth announcing; the next event that moves the stat asks
    // again.
    if (!read.ok)
        return;
    m_view->externalChange(read.bytes, read.existed);
}

void ConfigChangeWatcher::probeRemote()
{
    // One at a time, and never across the page's own read or write: both are busy, and
    // the page's own reply is what sets the baseline in any case.
    if (m_probing || m_view->isBusy())
        return;
    m_probing = true;
    const quint64 generation = m_view->diskGeneration();
    auto *transfer = new ConfigTransfer(this); // closing the tab abandons it
    connect(transfer, &ConfigTransfer::probeFinished, this,
            [this, transfer, generation](const ConfigReadResult &result) {
                m_probing = false;
                transfer->deleteLater();
                if (!result.ok)
                    return;
                // Dropped if the page read, wrote or was reconciled while this was in
                // flight: the bytes describe a file from before that, and a remote save
                // is in place, so they may even describe one half written.
                if (m_view->isBusy() || m_view->diskGeneration() != generation)
                    return;
                m_view->externalChange(result.bytes, result.existed);
            });
    transfer->startProbe(m_address);
}

} // namespace loftail
