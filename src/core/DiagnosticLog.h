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

// loftail's own log about itself: what it tried to do about a log it could not reach,
// and how that turned out (`SPEC.md` §3 "Diagnostics"). It exists because the two things
// hardest to report from memory are also the two the user cannot watch — a connection
// that failed while they were doing something else, and a log that never turned up — and
// by the time either is noticed the status bar is showing only the latest of them.
//
// WHAT GOES IN IT, and the rule is narrow on purpose: an ATTEMPT and its OUTCOME, and a
// STATE that changed. Never a tick that changed nothing. A poll that finds the same size
// it found a second ago is not evidence of anything, and a log that records it buries the
// one line that is under a thousand that are not. Where an attempt genuinely does repeat
// — a host that has been down for an hour, retried every five seconds — diagLogEvery()
// collapses the repeats and says how many there were.
//
// WHAT NEVER GOES IN IT: a password, a passphrase, a key, or the contents of any log
// being read. The file records that a password was asked for and whether it was accepted,
// which is the diagnostic question; the password itself is not part of it. This file
// lands in bug reports.
//
// IT IS NOT TRANSLATED, and that is the same call `SshExecCommands`' shell text and the
// log4cplus conversion patterns get rather than an oversight (`ARCHITECTURE.md` §9.1).
// The audience for a line in here is whoever is diagnosing the fault — often not the
// person at the keyboard, and often reading it pasted into an issue — so the words have
// to mean one thing everywhere. The status bar is where the user is spoken to, and that
// prose is translated.
//
// ON BY DEFAULT AND CAPPED, which is a pair: a diagnostic log that has to be turned on
// before it is useful is never on when it is needed, and one with no ceiling is a bug of
// its own. It holds kMaxBytes and rolls over exactly once, so the cost is bounded at
// twice that whatever happens.
//
// THREAD-SAFE. Fetcher threads write to it, so it locks — a `std::mutex`, never a
// `QMutex`, because this is `src/core` (`ARCHITECTURE.md` §13.1).

// Where the file is. Resolves on first use to QStandardPaths::AppLocalDataLocation,
// which is why nothing needs to be initialised at startup.
QString diagLogPath();

// Point the log somewhere else. For tests, and for the one real caller — nothing in the
// application sets it, so the default location is what ships. Takes effect on the next
// write; anything already open is closed first.
void diagLogSetDirectory(const QString &dir);

// One line. `area` is a short untranslated tag naming the part of loftail speaking
// ("ssh", "wait", "app"); it is what makes the file greppable.
void diagLog(const char *area, const QString &message);

// One line at most every `everyMs` for `key`, for something that genuinely repeats — a
// reconnect to a host that is down retries every five seconds and would otherwise write
// seven hundred identical lines an hour. Suppressed repeats are COUNTED, not discarded,
// and the count is reported on the next line that gets through: "(+41 since the last)".
// A silent drop would make the log lie about how often loftail tried, which is exactly
// the question it is being read to answer.
void diagLogEvery(qint64 everyMs, const char *area, const QString &key,
                  const QString &message);

// Write the one line that identifies this run — the version and the build. First thing
// in the file after a rollover, so a pasted excerpt says which binary produced it.
void diagLogSessionStart();

// The cap, and the size the rolled-over copy is bounded by too.
constexpr qint64 kDiagLogMaxBytes = 1024LL * 1024;

} // namespace loftail
