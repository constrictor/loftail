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

#include "RemoteLocation.h"

#include <QString>

namespace loftail {

// WHY A LOG ON ANOTHER MACHINE IS NOT THERE, IN ONE SENTENCE WRITTEN IN ONE PLACE
// (SPEC.md §3, ARCHITECTURE.md §6.3.1, §6.5).
//
// PURE STRING WORK — no libssh2, no sockets — and therefore ALWAYS COMPILED, like
// SshExecCommands and ExecSizeProbe, so the wording is tested in every configuration
// rather than in the one that links the transport.
//
// It exists because four surfaces used to word this for themselves and every one of them
// folded two answers into one: the exec transport said "it is missing, or the account
// cannot read it" (both, about a file it had just been told which), the SFTP transport
// printed a bare status number, and the poll loop said a file was "not readable right
// now" about one that was simply absent. A reader acts on that sentence — it is the whole
// of what they have — so a wrong one sends them to look in the wrong place.

// What the far end said about a path, in the same vocabulary a local path answers in
// (LogPresence), so that what loftail can say about a log over there and what it can say
// about a log over here cannot drift apart.
struct RemotePathReport
{
    // Meaningful only while `known` is true.
    LogPresence presence = LogPresence::Present;

    // False when the server refused with something loftail did not ask about. `detail`
    // then carries its own words, which is the only honest thing left to say: a guess
    // about a status nobody recognises is worse than quoting it.
    bool known = false;

    // The far end's own words, for the `known == false` case: an SFTP status with its
    // name where libssh2 has one, or what the shell complained.
    QString detail;
};

// The folder that would hold `path`, on the far end. Empty when the path names no
// folder at all — a bare relative path, which the server resolves against a home
// directory loftail knows nothing about.
//
// NEVER QFileInfo, and that is the whole reason this is a function rather than a line at
// each call site. QFileInfo is a question about the LOCAL filesystem: on Windows it reads
// a leading `C:` as a drive and a backslash as a separator, so an ordinary remote path
// would be cut in a place the far end has never heard of. Three callers — the sentence
// below, the exec transport's `test -d` and the SFTP stat of the parent — and they have
// to agree, or loftail says one folder is missing and looked at another.
QString remoteParentFolderOf(const QString &path);

// THE ONE PLACE A REMOTE "not there" SENTENCE IS WORDED. Every caller in SshSession and
// SshFetcher goes through it — a second wording is how the surfaces came to disagree in
// the first place, and each of them is reached from a different failure so nothing on
// screen would ever show the two side by side.
//
// `path` is the remote path as the user gave it (never a display name: the reader is
// being told where to go and look, and on the far end that is a path); `host` is the
// machine. Neither may carry a password — every caller reaches this from a parsed
// RemoteLocation, which dropped one.
QString remotePathTroubleText(const RemotePathReport &report, const QString &path,
                              const QString &host);

} // namespace loftail
