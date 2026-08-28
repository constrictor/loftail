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

#include <optional>

namespace loftail {

// A log that lives on another machine, reached over SSH (SPEC.md §3, M11). It is
// spelled as an `ssh://user@host:port/path` URL and travels through the rest of the
// application as an ordinary path STRING — `Document::path()`, the session's
// `documents[].path`, the recent-files list and the format-cache key all hold it
// unchanged. That is what let this feature land without a session schema bump.
//
// Everything here is free of libssh2 and is ALWAYS compiled, including in a build
// configured without SSH support: a build that cannot open a remote file must still
// recognise, persist and display one identically, or the two builds would disagree
// about what a given settings file means.
struct RemoteLocation
{
    static constexpr int kDefaultPort = 22;

    QString user; // empty means "unspecified" — resolved at connect time, not here
    QString host;
    QString path; // the remote path, decoded; "~/x.log" style paths keep their tilde
    int     port = kDefaultPort;

    // Cheap scheme test, safe to call on every path in the application. Accepts
    // `sftp://` as well: dragging a file out of a GNOME Files SSH mount produces one.
    static bool isRemote(const QString &s);

    // Parse an ssh:// or sftp:// URL. Returns nullopt for a local path, a malformed
    // URL, or one with no host. A password embedded in the URL (`ssh://u:pw@h/p`) is
    // DELIBERATELY DISCARDED rather than honored — see toString().
    static std::optional<RemoteLocation> parse(const QString &s);

    // `s` in normal form, or `s` unchanged when it is not a remote URL. Every entry
    // point (open, drop, command line, recent files, session restore) normalizes
    // before the string becomes a Document path, so that two spellings of one remote
    // file compare equal in viewOfPath(), the recent-files dedupe and the format cache.
    static QString normalize(const QString &s);

    bool isValid() const { return !host.isEmpty() && !path.isEmpty(); }

    // The normal form: scheme always `ssh`, port always spelled out (so `ssh://h/p`
    // and `ssh://h:22/p` are one string), path percent-encoded. The user is emitted
    // only when it was given — synthesizing the local account name would be wrong for
    // anyone whose ~/.ssh/config sets a different User for the host.
    //
    // NEVER contains a password. This string is written to the session file, the
    // recent-files list and the window title; a credential must not ride along.
    QString toString() const;

    // `s` with any URL password taken out, for a string that is about to be SHOWN.
    //
    // parse() drops a password on the floor, so everything downstream of a SUCCESSFUL
    // parse is already clean. An address that does NOT parse never goes through it —
    // `ssh://u:pw@h` has no path and `ssh://u:pw@` has no host — and used to be echoed
    // back verbatim into the refusal strip, credential and all. This is the one place
    // that is answered, so the refusal's name half and its reason half can both ask.
    // A non-remote string is returned unchanged: a local path has no userinfo.
    static QString withoutPassword(const QString &s);

    // The account this address will actually sign in as: `user` where the address
    // spells one, the local account name otherwise — which is what ssh does absent a
    // User directive, and what SshSession::authenticate() has always filled in.
    //
    // It exists because that fill-in used to happen INSIDE the connect, after target()
    // had already been asked, so an address with no user was keyed two different ways at
    // once: `host:22` by everything holding the parsed location, `me@host:22` by
    // everything downstream of the connect. Two consequences, both silent — a remembered
    // password primed into SshCredentialCache by MainWindow::primeRemoteCredentials()
    // was filed under a key the auth ladder never looked up, and
    // HostBookmarkStore::indexOfTarget() could not find a saved host with no user, which
    // is what DISABLED the password dialog's "Remember this password" box and made its
    // note say there was no saved host for one that was saved.
    //
    // NOT used by toString(): the address the session file and the recent-files menu
    // hold must stay the one the user typed, and synthesizing a user into it would be
    // wrong for anyone whose ~/.ssh/config sets a different User for the host. This is
    // about the key a connect is filed under, which is the connect's own answer.
    QString effectiveUser() const;

    // "user@host:port" — the connection-pool key, so every file on one host shares a
    // single SSH connection and, at restore, a single password prompt. The user half is
    // effectiveUser(), so the key is the same string before and after a connect has
    // filled the account in.
    QString target() const;

    // The host as shown to a person: the bare host name, no user or port.
    QString displayHost() const { return host; }
};

// The helpers below are the ONLY things the rest of the application needs in order to
// treat a path it cannot simply fopen() — remote, archived, or both — like a local
// one. Each falls through to the plain local behavior for a plain path, so call sites
// need no branch of their own. They live here, next to the first kind of path that
// needed them, and are archive-aware through ArchiveLocation (implemented in the .cpp
// so this header stays the small value type it was).

// Reduce a raw path to the ONE spelling that may become a Document::path(): a remote
// URL to its normal form, an archived path to its container's normal form plus the
// member, a plain path unchanged. Every entry point (open, drop, command line, recent
// files, session restore) calls this before the string becomes a Document path, and
// Document::prepare() calls it again, so that two spellings of one log compare equal
// in viewOfPath(), the recent-files dedupe and the format cache no matter who opened it.
QString normalizeLogPath(const QString &s);

// The key one log is remembered under in the settings tree (LogSettings.h): its
// normalized address for anything read through a spool, its canonical path otherwise.
//
// The spooled branch is load-bearing. canonicalFilePath() returns EMPTY for a remote
// URL and for an archive member — neither is a file on this filesystem — and the
// absoluteFilePath() fallback would then mangle one into "<cwd>/ssh:/user@host/a.log",
// a key that changes with the working directory and silently loses the log's settings.
QString logSettingsKey(const QString &path);

// The string a file pattern is matched against (LogSettings.h).
//
//   fullPath == false  the log's own file name: the member's name inside an archive,
//                      the compressed name with its suffix taken off for a bare .gz,
//                      the last component of the remote path for an ssh:// address.
//                      An archive is a file TYPE and SSH is a TRANSPORT; neither is
//                      part of what the log is called.
//   fullPath == true   the normalized address verbatim, scheme and port included, so
//                      what the pattern sees is exactly what the dialog shows.
QString logMatchTarget(const QString &path, bool fullPath);

// Whether this path is read through a spool rather than directly, so what grows on
// disk is the local cache and not the path itself. Such a path is never handed to
// QFileSystemWatcher: it is either not a local path at all, or — an archive — a
// container whose changing would not change what has already been expanded.
bool logPathIsSpooled(const QString &s);

// The file name for a title bar, tab or status line: "app.log" locally,
// "app.log (prod-web)" for a remote file — the host is what disambiguates two tabs
// showing the same-named log from different machines — and "app.log (bundle.tar.gz)"
// for one inside an archive.
//
// TWO PROPERTIES HOLD FOR EVERY ADDRESS, AND BOTH ARE LOAD-BEARING.
//
// It is NEVER EMPTY. An address with no file-name part — `ssh://`, `/var/log/`, `/`,
// the empty string — used to answer "", and every consumer showed the gap rather than
// the address: the refusal strip read "Cannot open : …" with nothing before the colon,
// a waiting tab wore its marker and nothing else, the window title trailed off after
// the em dash and a recent-files entry was a blank clickable row. The fallback is the
// deepest non-empty segment of the address ("log" for `/var/log/`), then the scheme
// word for a remote-shaped address that has nothing else ("ssh" for `ssh://`), then a
// placeholder for the truly nameless (`/`, "").
//
// It NEVER CONTAINS A SEPARATOR. prefixedLabelsFor() (TabLabels.h) builds a recent-files
// entry as parent directories plus this string, which only stays unambiguous while the
// name is the tail of its own label — so the fallback is a SEGMENT and never the raw
// address. The raw address is also unbounded in width, which is the recent-files menu's
// own reason for not using one.
//
// And it never carries a password: the fallback goes through
// RemoteLocation::withoutPassword() first, because the addresses that reach it are
// exactly the ones parse() refused and so never cleaned.
QString logSourceDisplayName(const QString &path);

// The first half of that name alone: "app.log" for every one of `/var/log/app.log`,
// `ssh://web1/var/log/app.log` and `/srv/bundle.tar.gz/var/log/app.log` — the log's own
// name with nothing bracketed onto it to say where it is.
//
// It carries the same three guarantees as the display name above, and for the same
// reasons: never empty, never a separator, never a password. The separator half is
// load-bearing in a second place here — tabLabelsFor() (TabLabels.h) GROUPS on this
// string to find the logs that would otherwise wear one name, and a key with a path in
// it groups nothing with anything.
QString logSourceBareName(const QString &path);

// The full location for a tooltip or an error message: the local path, or the URL.
// Password-free for the same reason and by the same route as the name above.
QString logSourceDisplayPath(const QString &path);

// What is at an address, as far as can be told WITHOUT I/O beyond an attribute query.
//
// Absent and Unreadable are not the same thing to a user, and folding them together is
// what made a log the user can see in their file manager report that it "has not
// appeared yet". Both WAIT rather than fail — a permission is granted as readily as a
// file is written — but each says its own sentence (SPEC.md §3), and one of them is a
// reason to stop retrying a container that will never open (§6.4).
enum class LogPresence {
    Present,    // it is there, and this process may read it
    Absent,     // there is nothing at the address
    Unreadable, // something is there and this process may not read it
};

// Which of the three `path` is. NON-BLOCKING, and deliberately optimistic for a remote
// path — Present, always: answering it truthfully would mean a network round trip, and
// this is called during session restore where a stall would be a hang. For an archived
// path this asks about the CONTAINER, because opening the archive to confirm the member
// is there would cost as much as expanding it.
LogPresence logSourcePresence(const QString &path);

// Whether opening `path` is worth attempting: logSourcePresence() == Present, and the
// same non-blocking, remote-optimistic answer. Kept as its own name because most
// callers only ever want the one bit.
bool logSourceAvailable(const QString &path);

// Whether `path` NAMES A LOG AT ALL — pure string work, no I/O, and a much weaker
// question than logSourceAvailable() above. `/var/log/nope.log` is well-formed and
// unavailable; `ssh://` is neither.
//
// This is the line between waiting and failing (M13, §6.5). A well-formed address that
// is not available is a log that has not turned up yet, and loftail waits for it; an
// address that names no log will not start naming one however long it waits, so that
// stays a refusal. Without the distinction a typo like "ssh://" opens a tab that waits
// forever for something that cannot exist.
bool logPathIsWellFormed(const QString &path);

} // namespace loftail
