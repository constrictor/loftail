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

#include <QString>
#include <QtGlobal>

namespace loftail {

// How long until loftail tries again, said in one place (SPEC.md §3).
//
// A tab that says "Cannot reach host — Connection refused" and then sits there is
// indistinguishable from one that has stopped trying, and those are the two states a
// reader most needs told apart. Both things that retry — a remote log's fetcher and a
// remote config file's read — say it through this pair, so a log tab and an editor tab
// cannot come to word the same sentence differently.

// A monotonic millisecond clock shared by whoever publishes a deadline and whoever
// renders it. Monotonic rather than wall clock deliberately: a countdown taken off
// QDateTime jumps by an hour at a daylight-saving change, and by whatever ntpd feels like
// the rest of the time, on a tab whose whole job at that moment is to look calm.
//
// Zeroed at its first call rather than at the epoch, so the number stays small and a test
// can reason about it. Its initialisation is thread-safe, which matters here: the first
// caller may be a fetcher worker or the GUI.
qint64 fetchMonotonicMs();

// "reason (12)" — the transport's own words, then how many seconds until the next
// attempt. `retryAtMs` is a DEADLINE on the clock above and not a remaining duration,
// because it is published from a worker and rendered on the GUI's own tick: a duration
// is stale by an unknown amount by the time anybody reads it.
//
// Rounded UP, so the last part-second reads "(1)" rather than "(0)": zero is what a
// countdown says when it has finished, and this one has not. A deadline of 0, one already
// passed, or an empty reason all give the reason back unchanged — a retry that is due, or
// an attempt already running, has nothing left to count.
QString retryCountdownText(const QString &reason, qint64 retryAtMs);

} // namespace loftail
