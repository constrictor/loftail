#pragma once

#include <QString>

#include <optional>

namespace loftail {

// A log inside a compressed file or an archive (SPEC.md §3, M12).
//
// SPELLED AS A NESTED PATH, with no scheme of its own: the member simply continues
// the container's path, `/logs/bundle.tar.gz/var/log/app.log`. That is what makes it
// compose with a transport for free — an archive is a file TYPE and SSH is a way of
// reaching a file, so a remote archive is just the same nesting inside a remote
// address, `ssh://host/logs/bundle.tar.gz/app.log`, with no new spelling to invent
// and no combinatorial explosion of schemes.
//
// Like RemoteLocation, it travels through the rest of the application as an ordinary
// path STRING — `Document::path()`, the session's `documents[].path`, the recent-files
// list and the format-cache key all hold it unchanged — which is again why this needed
// no session schema bump.
//
// Everything here is free of libarchive and is ALWAYS compiled, including in a build
// configured without archive support: such a build must still recognise, persist and
// display an archived path identically, or the two builds would disagree about what a
// given settings file means (the reason RemoteLocation.h gives, for the same reason).
struct ArchiveLocation
{
    // The archive itself: a local path, or an `ssh://` URL for one on another machine.
    QString container;

    // The path inside it. EMPTY has two very different meanings, distinguished by
    // isSingleStream(): a `.gz` has exactly one member and never names it, while a
    // `.zip` with no member named is an address that is not yet openable.
    QString member;

    // --- Classification. Pure string work: no I/O, no content sniffing, so it gives
    // the same answer for a file that does not exist yet — which is what session
    // restore needs, and what lets these be called on every path in the application.

    // A container holding exactly one unnamed stream: .gz .bz2 .xz .zst .lzma .Z
    static bool isSingleStreamName(const QString &name);

    // A container that may hold several members: .zip .7z .tar and every compressed
    // tar. Tested BEFORE the single-stream table, so `.tar.gz` is a tar and not a gz.
    static bool isContainerName(const QString &name);

    // Whether `path` addresses something inside an archive, or an archive itself.
    static bool isArchivePath(const QString &path);

    // Split `path` into container and member, or nullopt when it addresses no archive.
    //
    // THE RESOLUTION RULE, in order:
    //   0. A local path that exists as a regular FILE is never split. This is what
    //      keeps a real directory named `bundle.zip` working: `bundle.zip/app.log`
    //      resolves to the file that is actually there.
    //   1. Otherwise split at the FIRST component carrying a container extension —
    //      that component and everything left of it is the container, the rest is the
    //      member (possibly empty, meaning "a member still has to be picked").
    //   2. Otherwise, if the last component carries a single-stream extension, the
    //      whole path is the container and its one member is implied.
    //   3. Otherwise this is an ordinary log.
    static std::optional<ArchiveLocation> split(const QString &path);

    // `path` in normal form, or `path` unchanged when it addresses no archive. Every
    // entry point normalizes before the string becomes a Document path — see
    // normalizeLogPath(), which is the function call sites actually use.
    static QString normalize(const QString &path);

    // Exactly one member, never named: the container is a bare compressed stream.
    bool isSingleStream() const { return !isContainerName(container); }

    // A container that could hold several members, with none picked yet. Not openable;
    // the caller has to ask (MainWindow::openFile does, exactly once).
    bool needsMember() const { return !isSingleStream() && member.isEmpty(); }

    // Enough of an address to open.
    bool isOpenable() const { return !container.isEmpty() && !needsMember(); }

    // THE NORMAL FORM, and one rule inside it does the real work:
    //
    // A SINGLE-STREAM CONTAINER COLLAPSES BACK TO ITS PLAIN PATH. `/logs/app.log.gz`
    // is the normal form of itself and never grows a member; only a multi-member
    // container carries one. Without that rule one log would have two spellings, and
    // so two tabs, two format-cache entries and two spools.
    //
    // The container is made absolute (local) or reduced to its own normal form
    // (remote), so `./b.zip/m` and `/abs/b.zip/m` are one key.
    QString toString() const;

    // The member as shown to a person: "app.log" — for a single stream, the container's
    // name with the compression suffix taken off.
    QString displayMember() const;
};

} // namespace loftail
