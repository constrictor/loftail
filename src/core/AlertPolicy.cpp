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

#include "AlertPolicy.h"

namespace loftail {

AlertPolicy::Decision AlertPolicy::recordBatch(qint64 nowMs, int matchCount)
{
    if (matchCount <= 0)
        return {};

    m_pending += matchCount;

    // The first notification for a log is never delayed: waiting out an interval that
    // has not started yet would make the feature look broken on the one match a user is
    // most likely to be testing it with.
    if (m_everNotified && nowMs - m_lastNotifyMs < m_intervalMs)
        return {}; // suppressed; m_pending carries it to the next admitted decision

    Decision d;
    d.notify = true;
    d.count = m_pending;
    m_pending = 0;
    m_lastNotifyMs = nowMs;
    m_everNotified = true;
    return d;
}

AlertPolicy::Decision AlertPolicy::poll(qint64 nowMs)
{
    if (m_pending <= 0)
        return {};
    if (m_everNotified && nowMs - m_lastNotifyMs < m_intervalMs)
        return {};

    Decision d;
    d.notify = true;
    d.count = m_pending;
    m_pending = 0;
    m_lastNotifyMs = nowMs;
    m_everNotified = true;
    return d;
}

void AlertPolicy::reset()
{
    m_pending = 0;
    m_lastNotifyMs = 0;
    m_everNotified = false;
}

} // namespace loftail
