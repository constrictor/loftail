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

#include "SshSessionCache.h"

#include <chrono>
#include <cstddef>
#include <utility>

namespace loftail {

qint64 sshMonotonicMs()
{
    // std::chrono and not QElapsedTimer, for the reason src/core uses std::mutex
    // (ARCHITECTURE.md §13.1): this is read from every worker thread and there is nothing
    // to be gained from a Qt type here. Monotonic, so a clock the user drags backwards
    // cannot make an idle session look fresh for an hour.
    using namespace std::chrono;
    return qint64(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

SshSessionCache::SshSessionCache(qint64 idleMs, int capacity)
    : m_idleMs(idleMs), m_capacity(qMax(0, capacity))
{
    // Floored rather than trusted, because check-in compares against it as a size. Zero is
    // a legitimate setting and means "hold nothing" — every session is let go the moment
    // it comes back, which is the behaviour of the build that had no cache at all.
}

SshSessionCache::~SshSessionCache()
{
    // Same path as the shutdown drain's, so a cache that was never closed by hand — a
    // test's own, and nothing else, since the process's one is closed by
    // drainSshWorkers() — still lets its connections go through cutLoose() rather than
    // blocking somebody's exit on a farewell packet.
    close();
}

void SshSessionCache::discard(std::vector<Entry> going)
{
    // NOT under m_mutex, and the parameter is by value to make that structural rather
    // than remembered: destroying a session is a socket write, and holding the lock
    // across one would let a single wedged connection block every other errand in the
    // process for as long as the session timeout.
    for (auto &entry : going) {
        if (!entry.session)
            continue;
        entry.session->cutLoose();
        entry.session.reset();
    }
}

void SshSessionCache::takeExpiredLocked(qint64 nowMs, std::vector<Entry> *going)
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        // Two ways out and both are wanted here. Past the deadline is the one this sweep
        // exists for. Gone unhealthy while nobody was looking at it is the other: the
        // latch can be set by the very errand that handed the session back — a read that
        // delivered its bytes and then timed out reports success (SshSessionHealth.h) —
        // so a session can arrive healthy and be found dead the next time anything asks.
        const bool expired = nowMs - it->idleSinceMs >= m_idleMs;
        if (expired || !it->session || !it->session->healthy()) {
            going->push_back(std::move(*it));
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

SshSessionCache::Checkout SshSessionCache::checkOut(const QString &target, SshSessionRole need,
                                                    qint64 nowMs)
{
    Checkout           out;
    std::vector<Entry> going;
    {
        std::scoped_lock lock(m_mutex);
        takeExpiredLocked(nowMs, &going);
        if (!m_closed && !target.isEmpty()) {
            auto pick = m_entries.end();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->target != target)
                    continue;
                if (it->role == need) {
                    pick = it; // an exact match always wins
                    break;
                }
                // The one-way borrow (SshSessionCache.h): an ExecOnly errand may take a
                // Transport session, never the reverse. Remembered rather than taken, so
                // that an exact match later in the list is still preferred — leaving the
                // Transport session here is what keeps a config read that follows free.
                if (need == SshSessionRole::ExecOnly && it->role == SshSessionRole::Transport
                    && pick == m_entries.end()) {
                    pick = it;
                }
            }
            if (pick != m_entries.end()) {
                if (pick->session && pick->session->healthy()) {
                    out.session = std::move(pick->session);
                    out.role = pick->role;
                } else {
                    going.push_back(std::move(*pick));
                }
                // ERASED WHETHER OR NOT IT WAS TAKEN, which is the mutual exclusion: two
                // workers on one host must never come away holding the same session, and
                // nothing else in the process would stop them — SshConnectHold guards a
                // connect, and a hit performs none.
                m_entries.erase(pick);
            }
        }
    }
    discard(std::move(going));
    return out;
}

void SshSessionCache::checkIn(const QString &target, SshSessionRole role,
                              std::unique_ptr<CachedSshSession> session, qint64 nowMs)
{
    if (!session)
        return;

    // Before the lock, and unconditionally: dropping the errand's abandon check releases
    // that worker's shared block, whose destructor is somebody else's code. Running it
    // under m_mutex would put an unbounded amount of it inside this class's critical
    // section for no reason.
    session->detachOwner();

    std::vector<Entry> going;
    {
        std::scoped_lock lock(m_mutex);
        takeExpiredLocked(nowMs, &going);

        Entry entry;
        entry.target = target;
        entry.role = role;
        entry.session = std::move(session);
        entry.idleSinceMs = nowMs;

        if (m_closed || target.isEmpty() || !entry.session->healthy()) {
            // Taken and destroyed rather than handed back: a caller that has finished with
            // a session must not be left holding one it now has to decide about, and the
            // shutdown latch in particular has to be able to answer "give me that" without
            // the straggler needing to know why.
            going.push_back(std::move(entry));
        } else {
            m_entries.push_back(std::move(entry));
            // The front is the oldest return, check-ins appending and checkouts erasing
            // in place. Over the cap, the oldest goes — the one whose deadline is nearest
            // anyway. Compared as a size rather than as an int: the constructor floors the
            // cap at zero, so the conversion is the safe direction and the mixed-sign
            // comparison a signed one would be does not arise.
            const auto cap = std::size_t(m_capacity);
            while (m_entries.size() > cap) {
                going.push_back(std::move(m_entries.front()));
                m_entries.erase(m_entries.begin());
            }
        }
    }
    discard(std::move(going));
}

void SshSessionCache::close()
{
    std::vector<Entry> going;
    {
        std::scoped_lock lock(m_mutex);
        m_closed = true;
        going.swap(m_entries);
    }
    discard(std::move(going));
}

int SshSessionCache::size() const
{
    std::scoped_lock lock(m_mutex);
    return int(m_entries.size());
}

bool SshSessionCache::closed() const
{
    std::scoped_lock lock(m_mutex);
    return m_closed;
}

SshSessionCache &sshSessionCache()
{
    static SshSessionCache cache;
    return cache;
}

} // namespace loftail
