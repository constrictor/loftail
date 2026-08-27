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

#include <QStringList>

namespace loftail {

// What an open log is CALLED, decided for the WHOLE set at once (SPEC.md §5a,
// ARCHITECTURE.md §12.4).
//
// TWO RULES LIVE HERE, and they are deliberately different. A tab shows the log's own
// name and brackets on whatever tells it from the tabs beside it; the recent-files menu
// keeps the older spelling, which grows parent directories in front of the name. They
// share the address arithmetic and nothing else.
//
// Why a free function over the whole set rather than a name computed per log as it
// opens: a label is a statement about a log's NEIGHBOURS, so it changes when they do.
// Closing one of two `app.log`s has to shorten the survivor back to `app.log`, and that
// only falls out if the set is relabelled on every open and close. It is also the whole
// reason this is a pure function of a list of addresses — that is the shape the rule
// actually has, and the shape a table-driven test can drive directly.
//
// Not on the ingest path. MainWindow caches what this returns per file (see
// DocumentContext::tabLabel); rewriting a QTabBar entry relays the whole bar out, so
// the labels are recomputed when the set of open logs CHANGES and never per tick.

// Longest the path component of a tab's bracket grows before its middle is elided. Only
// that component is bounded: it is the one with nothing to bound it, where a host and a
// container name are each a single segment. The log's own name is not elided here
// either — the tab bar's own ElideMiddle handles a long one, and a label that is short
// when unambiguous must stay byte-identical when it grows a bracket. The elision keeps
// the run's head and tail, which is where the telling-apart is.
inline constexpr int kMaxTabQualifierChars = 24;

// The same budget for a recent-files entry, which is wider because a menu is laid out to
// its widest item and has the room, where a tab bar divides one fixed width among every
// open log.
inline constexpr int kMaxRecentPrefixChars = 40;

// One label per address, in the same order. Handles every address the application
// accepts — a local path, an `ssh://` URL and a path inside an archive — by asking
// RemoteLocation/ArchiveLocation what the parts are rather than cutting up the string.
//
// A log whose name no other open log answers to wears that name and nothing else. Where
// two or more DO share a name — the ordinary case for anyone tailing one service across
// hosts or deployments — each grows a bracket holding the most prominent thing that
// tells it from the others, in this order:
//
//   1. the DEVICE it is on: the host of an `ssh://` address, nothing for a local log.
//   2. the archive CONTAINER it is in.
//   3. the PATH RUN: its parent directories with the ones EVERY member of the group
//      carries stripped from both ends — the common root and the common tail — and all
//      of what is left, outermost first, joined by `/`.
//
// An axis is spent only where it BUYS a distinction, so a group whose logs are all on
// one host is told apart by its directories alone and never says the host. The
// components that are spent read in that order inside one parenthesis, separated by
// ", ": `app.log (host-a, svc-a)`.
//
// Addresses that stay ambiguous after all three — two users on one host naming one path,
// two ports on one host — keep their duplicate labels rather than growing something
// invented. The tooltip carries the full address in every case.
QStringList tabLabelsFor(const QStringList &addresses,
                         int maxQualifierChars = kMaxTabQualifierChars);

// The older rule, kept for File ▸ Open Recent: the log's own name as
// logSourceDisplayName() spells it — with a remote host or an archive container already
// bracketed on — grown by the nearest parent directories that differ wherever two
// entries would otherwise read alike, as many as it takes and no more.
//
// A menu entry is not a tab: it is read against the other nine entries rather than
// against the logs open beside it, it is as wide as the widest item rather than a share
// of a fixed bar, and a path prefix is what a person recognises a remembered file by.
QStringList prefixedLabelsFor(const QStringList &addresses,
                              int maxPrefixChars = kMaxRecentPrefixChars);

} // namespace loftail
