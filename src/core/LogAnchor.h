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

#include <optional>

namespace loftail {

// Where a log physically IS: a filesystem, and a path on it (ARCHITECTURE.md §6.8, §6.9).
//
// `remote` empty means the local machine. `path` is a path on whichever that is, so the
// two travel together — placing anything against a log means keeping the filesystem and
// replacing the path, which is exactly what keeps "the config is on the same device as
// the log" and "the restart script runs on the machine the log is on" true without any
// caller having to restate it.
//
// PURE STRING WORK, with one inherited exception: ArchiveLocation::split() stats a local
// path (its rule 0, which is what keeps a real directory named `bundle.zip` working), so
// a local answer can change when a file appears. The same weak purity logSettingsKey()
// already has.
struct LogAnchor
{
    std::optional<RemoteLocation> remote; // nullopt == local
    QString                       path;   // the CONTAINER's path for an archived log
    QString                       member; // the path inside it; empty when not archived

    // Whether `path` names an archive rather than the log itself. NOT derivable from
    // `member`: a bare compressed stream (`app.log.gz`) is an archive whose single member
    // is implied and therefore unnamed, so an empty member means "no member spelled out"
    // and never "not an archive".
    bool                          archived = false;
};

// The log's filesystem and its own path on it, or nullopt when the address is one nothing
// can be placed against — an empty string, or a remote-shaped address that does not parse.
//
// TWO CALLERS AND ONE RULE. ConfigLocation places a config path against `remote`/`path`
// and ignores `member`; RestartTarget hands all three to a shell as LOGFILE/ARCHIVE/MEMBER.
// The archive-peel loop below carries a trap that is easy to re-derive wrongly, which is
// the whole reason this is one function rather than two copies.
std::optional<LogAnchor> logAnchorOf(const QString &logAddress);

} // namespace loftail
