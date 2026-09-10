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

#include "PathTrouble.h"

#include <QCoreApplication>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and every string below is user-facing: they are the whole of what a
// waiting tab, its tooltip and the status bar say about a log on another machine.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::PathTrouble)
};

} // namespace

QString remoteParentFolderOf(const QString &path)
{
    const qsizetype cut = path.lastIndexOf(QLatin1Char('/'));
    if (cut < 0)
        return {};
    // The root's parent is the root, not the empty string, or the sentence reads
    // "there is no folder  either" and the stat asks about nothing.
    return cut == 0 ? QStringLiteral("/") : path.left(cut);
}

QString remotePathTroubleText(const RemotePathReport &report, const QString &path,
                              const QString &host)
{
    if (!report.known) {
        // QUOTED, NOT INTERPRETED. A status nobody recognised is exactly the case where
        // loftail has nothing of its own to add, and inventing a reason for it is how
        // "it is missing, or the account cannot read it" came to be said about files
        // that were neither.
        return report.detail.isEmpty()
            ? Tr::tr("Cannot open %1 on %2 — the server refused it without saying why.")
                  .arg(path, host)
            : Tr::tr("Cannot open %1 on %2 — the server said: %3.")
                  .arg(path, host, report.detail);
    }

    switch (report.presence) {
    case LogPresence::Absent:
        return Tr::tr("There is no %1 on %2.").arg(path, host);
    case LogPresence::NoDirectory: {
        const QString folder = remoteParentFolderOf(path);
        // Falls back to the plain absence when there is no parent to name — a bare
        // relative path, which the far end resolves against a home directory loftail
        // knows nothing about. Better the true smaller statement than a blank folder.
        if (folder.isEmpty())
            return Tr::tr("There is no %1 on %2.").arg(path, host);
        return Tr::tr("There is no folder %1 on %2, so %3 is not there.")
            .arg(folder, host, path);
    }
    case LogPresence::Unreadable:
        return Tr::tr("%1 on %2 is there, and this account cannot read it.").arg(path, host);
    case LogPresence::NotAFile:
        return Tr::tr("%1 on %2 is a folder, not a log file.").arg(path, host);
    case LogPresence::Present:
        break;
    }
    // Present, from a caller that only asks after something has already gone wrong: the
    // path is fine and whatever failed was about something else. Says so rather than
    // claiming one of the four above, which would be a sentence about the wrong subject.
    return Tr::tr("Cannot read %1 on %2, although it is there and readable.").arg(path, host);
}

} // namespace loftail
