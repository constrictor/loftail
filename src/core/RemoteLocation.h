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

    // "user@host:port" — the connection-pool key, so every file on one host shares a
    // single SSH connection and, at restore, a single password prompt.
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

// Whether this path is read through a spool rather than directly, so what grows on
// disk is the local cache and not the path itself. Such a path is never handed to
// QFileSystemWatcher: it is either not a local path at all, or — an archive — a
// container whose changing would not change what has already been expanded.
bool logPathIsSpooled(const QString &s);

// The file name for a title bar, tab or status line: "app.log" locally,
// "app.log (prod-web)" for a remote file — the host is what disambiguates two tabs
// showing the same-named log from different machines — and "app.log (bundle.tar.gz)"
// for one inside an archive.
QString logSourceDisplayName(const QString &path);

// The full location for a tooltip or an error message: the local path, or the URL.
QString logSourceDisplayPath(const QString &path);

// Whether opening `path` is worth attempting. NON-BLOCKING, and deliberately
// optimistic for a remote path: answering it truthfully would mean a network round
// trip, and this is called during session restore where a stall would be a hang. For
// an archived path this asks about the CONTAINER — opening the archive to confirm the
// member is there would cost as much as expanding it.
bool logSourceAvailable(const QString &path);

} // namespace loftail
