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

#include "SshRetryPolicy.h"

namespace loftail {

void ReconnectGrace::signedIn()
{
    m_signedInOnce = true;
    // The window closes on success rather than merely being ignored, so that a fetcher
    // which reconnects late in one outage still gets the full grace in the next.
    m_waiting = false;
    m_firstFailureMs = 0;
}

bool ReconnectGrace::keepTrying(qint64 nowMs)
{
    if (!m_signedInOnce)
        return false;

    if (!m_waiting) {
        m_waiting = true;
        m_firstFailureMs = nowMs;
        // The first failure of an outage is inside the window by definition: the clock
        // starts here, so measuring from it would give this attempt zero and give up
        // before anything had been retried at all.
        return true;
    }
    return nowMs - m_firstFailureMs < m_graceMs;
}

} // namespace loftail
