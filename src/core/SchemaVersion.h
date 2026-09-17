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

#include <QJsonObject>
#include <QString>

// What a `schemaVersion` stamp says about a file this build has just opened, and the
// one place the rule is written down (ARCHITECTURE.md §8.4).
//
// EVERY STORE ASKS THE SAME THREE-WAY QUESTION, and for six milestones two of them
// asked a two-way one: `version != kSchemaVersion` folds "written by a build that is
// newer than this one" together with "not a file we wrote at all", and the two want
// OPPOSITE answers. A file from the future must be left EXACTLY as it is, because
// there is no migration downwards and whatever is in it is the user's current
// configuration; a file with no stamp is not anybody's configuration and standing off
// it only means the store can never write again. HostBookmarkStore and PresetStore
// took the folded reading, so the first bump of either would have discarded every
// bookmark, every remembered password and every preset on every installation — and
// then written the discard back over the file on the next save.
namespace loftail::Schema {

// The only three things a stamp can say. `Usable` covers every version from 1 up to
// and including the store's own: an OLDER file is read and migrated forward, which is
// the whole of what upgrade safety means here.
enum class Verdict {
    Usable,     // read it, migrating it forward from `version` if it is behind
    Unstamped,  // no stamp, or one that is not a positive integer: not a file we wrote
    FromFuture, // a later version: READ NOTHING, AND WRITE NOTHING OVER IT
};

// The stamp on a store's root object. 0 for an absent key, a non-integer or a
// negative — all of which judge() reads as Unstamped.
int versionOf(const QJsonObject &root);

// `current` is the store's own kSchemaVersion. Kept separate from versionOf() so that
// a store which keeps its stamp somewhere other than the root object — QSettings, a
// nested record — asks the same question about the number it has in hand.
Verdict judge(int version, int current);

// Both at once, for the ordinary caller. `*version` (when given) receives the stamp,
// which is what a migration needs in order to know where to start.
Verdict judge(const QJsonObject &root, int current, int *version = nullptr);

// THE WRITE HALF, and the half that is easy to leave out. A store that merely declines
// to READ a file from the future still destroys it on the next save — which for a
// stateless store (HostBookmarkStore, PresetStore) is the very next gesture, since
// each of those re-reads the file, folds the change in and writes the whole thing
// back. Every write funnel asks this before it commits.
//
// Answers false for a missing or unparseable file: there is nothing there to protect,
// and refusing to write over a damaged file would leave the store unable to recover.
bool fileIsFromFuture(const QString &path, int current);

// Copy `path` aside as "<path>.v<from>.bak" before this build rewrites it at a higher
// version, so that a migration which turns out to be wrong is recoverable by hand
// rather than only in principle. Called at LOAD, the moment a file stamped below
// `current` is read, rather than at the write it is protecting against: a load knows
// the version it read and a write does not, and a backup that is made and never needed
// costs one small file.
//
// Does nothing — and answers true — when a backup for that version is already there,
// so it happens once per file per version it was migrated FROM rather than once per
// launch. Failure is not an error any caller acts on: a read-only configuration
// directory must not stop the file being read.
bool backupOnce(const QString &path, int from);

// The name backupOnce() writes to, exposed for the tests and for anything that has to
// tell a backup from a live file when it walks a directory.
QString backupPathFor(const QString &path, int from);

} // namespace loftail::Schema
