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

#include "LogSource.h"

#include "ArchiveLocation.h"
#include "BufferedLogSource.h"
#include "RemoteLocation.h"
#include "SourceSpool.h"
#include "SpooledLogSource.h"
// Unconditional: Q_DECLARE_TR_FUNCTIONS lives here, and the Tr shim below is compiled on
// every platform. Under the #else it expands to nothing on MSVC and the shim becomes a
// member function with no return type.
#include <QCoreApplication>
#if defined(Q_OS_WIN)
#else
#include "MappedLogSource.h"
#endif

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::LogSourceFactory)
};
} // namespace


namespace {

// A log that is not directly readable as a local file reads through a local spool that
// a fetcher fills (§6.3, §6.4). The spool is shared per log, keyed by the normalized
// address, so a second Document on the same log — or a rescan after a rotation — joins
// the live one instead of connecting or expanding a second time.
std::unique_ptr<LogSource> openSpooled(const QString &key, const QString &reuseError,
                                       OpenPolicy policy, QString *error)
{
    SourceSpoolRegistry &registry = SourceSpoolRegistry::instance();
    std::shared_ptr<SourceSpool> spool = registry.find(key);
    if (!spool) {
        if (policy == OpenPolicy::Reuse) {
            // A rotation mid-tail must never turn into a reconnect or a fresh
            // expansion: this runs from the watch tick, on the GUI thread.
            if (error)
                *error = reuseError;
            return nullptr;
        }
        spool = registry.acquire(key, error);
        if (!spool)
            return nullptr;
    }
    return SpooledLogSource::open(std::move(spool));
}

std::unique_ptr<LogSource> openRemote(const QString &path, OpenPolicy policy, QString *error)
{
    const auto location = RemoteLocation::parse(path);
    if (!location) {
        // withoutPassword(), because this is the one error that quotes an address that
        // did NOT parse: parse() is where a URL password is dropped, so an address it
        // refused still has one and it would be echoed into the refusal strip verbatim.
        if (error) {
            *error = Tr::tr("Not a valid remote log address: %1")
                         .arg(RemoteLocation::withoutPassword(path));
        }
        return nullptr;
    }
    // The registry keys on the normalized address string, not on the parsed value: it
    // holds spools for several kinds of source and understands none of them.
    return openSpooled(location->toString(),
                       Tr::tr("Not connected to %1.").arg(location->target()),
                       policy, error);
}

std::unique_ptr<LogSource> openArchive(const ArchiveLocation &location, OpenPolicy policy,
                                       QString *error)
{
    if (location.needsMember()) {
        // An address that names a multi-member container and no member cannot be
        // opened. The member is picked once, at the interactive entry point, so
        // reaching here means an address was persisted or typed without one.
        if (error) {
            *error = Tr::tr("%1 holds several logs; open it again and choose one.")
                         .arg(logSourceDisplayPath(location.container));
        }
        return nullptr;
    }
    // Namespaced, not the plain address: for a single-stream container the two are the
    // same string, and the expansion's own input would otherwise resolve to the
    // expansion (SourceSpool.h).
    const QString address = location.toString();
    return openSpooled(expandedSpoolKey(address),
                       Tr::tr("%1 is no longer expanded.").arg(address), policy,
                       error);
}

} // namespace

// Platform selection, not mode selection (invariant #5, §6): mmap on POSIX, buffered
// on Windows, falling back to buffered if the mapping fails (e.g. a special file that
// cannot be mmapped). A path that has to be fetched or expanded takes the spool route
// above instead.
std::unique_ptr<LogSource> openLogSource(const QString &path, OpenPolicy policy, QString *error)
{
    // Archive before transport, and the order is the point: a remote archive is an
    // archive whose container happens to live on another machine, so it resolves here
    // and the SSH fetcher is reached later, as the archive fetcher's own input (§6.4).
    if (const auto archive = ArchiveLocation::split(path))
        return openArchive(*archive, policy, error);

    return openContainerSource(path, policy, error);
}

std::unique_ptr<LogSource> openContainerSource(const QString &path, OpenPolicy policy,
                                               QString *error)
{
    if (RemoteLocation::isRemote(path))
        return openRemote(path, policy, error);

#if defined(Q_OS_WIN)
    return BufferedLogSource::open(path);
#else
    if (auto mapped = MappedLogSource::open(path))
        return mapped;
    return BufferedLogSource::open(path);
#endif
}

// pathIdentity() used to live here, next to the platform selection above, with its
// Windows half stubbed to 0. It is now implemented for both platforms in
// SharedReadFile.cpp: asking Windows for a file's identity means opening a handle, and
// opening a handle without disturbing the writer is that file's entire subject.

} // namespace loftail
