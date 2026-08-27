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

#include <QList>
#include <QPair>
#include <QString>

#include <optional>

namespace loftail {

// One variable a restart script is given, as NAME and value (SPEC.md §4).
//
// The NAMES are constants of loftail's — never anything the user typed — which is what
// makes them safe to write straight onto a remote command line. Only the VALUES are
// paths, and only they go through shellQuote().
namespace RestartVars {
inline constexpr auto kLogFile = "LOGFILE";
inline constexpr auto kArchive = "ARCHIVE";
inline constexpr auto kMember  = "MEMBER";
} // namespace RestartVars

// Where and how to run the script that restarts the application writing a log.
//
// PURE STRING WORK, like ConfigLocation and the two location types beside it, so it
// answers identically for a log that has not turned up yet and can be shown in
// Preferences for a path somebody is still typing.
//
// THE SCRIPT RUNS WHERE THE LOG IS. A log opened over ssh:// is on somebody else's
// machine and so is the service that writes it, so that is where the script goes — over
// the exec channel the transport already has. An ARCHIVED log has no running service
// behind it at all, so the reduction peels to the container and the script runs on the
// outer system, with LOGFILE and ARCHIVE both naming the container and MEMBER naming the
// log inside it.
struct RestartTarget
{
    // THREE STATES, NOT A BOOL, the distinction ConfigAddress draws for the same reason.
    // Nothing configured is not an error: it is what makes the menu item explain itself.
    // Fold the two together and a refusal strip appears where an explanation belongs.
    enum class State {
        Unset,    // no restart script configured for this log
        Resolved, // `script` and `variables` are ready to run
        Refused,  // `reason` says why, in words meant for a person
    };

    State state = State::Unset;

    // Which machine. `host` is set only when `remote`, and never carries a password:
    // RemoteLocation::parse() discards one.
    bool                          remote = false;
    std::optional<RemoteLocation> host;

    // Verbatim, as the user typed it. NOT quoted and NOT escaped anywhere downstream:
    // this IS shell source, and running it as written is the whole feature.
    QString script;

    // LOGFILE, then ARCHIVE and MEMBER where the log is inside a container. Ordered
    // rather than a hash so the generated command is byte-stable and testable. Values are
    // paths on the machine the script will run on — never loftail's own composite
    // address, which no shell there could open.
    QList<QPair<QString, QString>> variables;

    // Why not. Empty unless Refused. Already translated, and any address inside it has
    // been through RemoteLocation::withoutPassword().
    QString reason;
};

// Resolve `script` (LogProfile::restartScript) against `logAddress`.
//
// Refused for an empty log address and for a remote-shaped one that does not parse —
// both decidable with no I/O, which is the only kind of refusal this layer makes. That a
// host is unreachable, or a shell missing, is the runner's answer and comes with a tab
// and a dialog behind it.
RestartTarget resolveRestartTarget(const QString &logAddress, const QString &script);

// Whether this build can run `target` at all.
//
// A remote restart needs libssh2, which is optional — so a build without it answers false
// with a reason, exactly as configAddressIsWritable() does for a remote config file.
bool restartTargetIsRunnable(const RestartTarget &target, QString *reason);

} // namespace loftail
