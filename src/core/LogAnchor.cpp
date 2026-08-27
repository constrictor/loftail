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

#include "LogAnchor.h"

#include "ArchiveLocation.h"

namespace loftail {

std::optional<LogAnchor> logAnchorOf(const QString &logAddress)
{
    if (logAddress.isEmpty())
        return std::nullopt;

    // Peel containers until the address names something that sits on a filesystem
    // rather than inside a file. `ssh://h/srv/b.tar.gz/app.log` peels once, to
    // `ssh://h:22/srv/b.tar.gz`, whose directory is then the base.
    //
    // split() is reused verbatim rather than the rule being re-derived, which is what
    // keeps a real directory named `bundle.zip` working (its own rule 0).
    //
    // TERMINATION IS ON INEQUALITY AND NOTHING ELSE. split() applied to a BARE container
    // answers with that same container and an empty member — a `.zip` with nothing
    // picked, a `.gz` whose member is implied — so peeling unconditionally spins on an
    // unchanged string and overflows the stack. It must NOT also test that the container
    // got shorter: for a remote address split() returns the container in NORMAL FORM,
    // with the port spelled out, so `ssh://host/srv/b.zip/m` peels to the strictly
    // LONGER `ssh://host:22/srv/b.zip`, and a length test would refuse the one peel that
    // address needs. The loop bound is a backstop against a cycle no known input
    // produces, not the argument for why this ends.
    QString address = logAddress;
    QString member;
    bool    archived = false;
    for (int peel = 0; peel < 8; ++peel) {
        const auto archive = ArchiveLocation::split(address);
        if (!archive || archive->container == address)
            break;
        // THE FIRST PEEL OWNS THE MEMBER. Later ones exist only to reduce a container
        // that is itself inside something, and they answer about the wrong level — so
        // this is set once and never overwritten, or `bundle.zip/var/log/app.log` would
        // end up reporting the member of its own container rather than of the log.
        if (!archived) {
            member = archive->member;
            archived = true;
        }
        address = archive->container;
    }

    // A BARE single-stream container is archived too, and the loop above never fires for
    // one: `app.log.gz`'s container IS itself, so it terminates on the first comparison.
    // So the flag is asked of split() at the SETTLED address rather than inferred from
    // the loop or from `member` — split() answers nullopt for an ordinary log and a value
    // for anything archive-shaped, which is exactly the question.
    archived = archived || ArchiveLocation::split(address).has_value();

    if (RemoteLocation::isRemote(address)) {
        const auto loc = RemoteLocation::parse(address);
        if (!loc || !loc->isValid())
            return std::nullopt; // refused by the caller, which words it
        LogAnchor a;
        a.remote = loc;
        a.path = loc->path;
        a.member = member;
        a.archived = archived;
        return a;
    }

    // The peeled address, NOT the one we were handed: for an archived log this is the
    // container, and anchoring on the original would put a config inside the archive and
    // point a restart script at a path that is not a file.
    LogAnchor a;
    a.path = address;
    a.member = member;
    a.archived = archived;
    return a;
}

} // namespace loftail
