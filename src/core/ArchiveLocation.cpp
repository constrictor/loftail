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

#include "ArchiveLocation.h"

#include "RemoteLocation.h"

#include <QDir>
#include <QFileInfo>
#include <QLatin1String>

namespace loftail {

namespace {

// A container may hold several members and so needs one named. Checked BEFORE the
// single-stream table below, which is the whole reason `.tar.gz` is a tar rather than
// a gz: the longest match has to win, and these are the longer suffixes.
constexpr const char *kContainerSuffixes[] = {
    ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.z",
    ".tgz", ".tbz", ".tbz2", ".txz", ".tzst", ".taz",
    ".tar", ".zip", ".7z",
};

// A bare compressed stream: exactly one member, never named.
constexpr const char *kSingleStreamSuffixes[] = {
    ".gz", ".bz2", ".xz", ".zst", ".lzma", ".z",
};

// The last path component of a plain path or a URL. Both tables are about a file
// NAME, so every classifier reduces to this first and callers may pass either.
QStringView lastComponent(const QString &path)
{
    const auto cut = path.lastIndexOf(u'/');
    return cut < 0 ? QStringView(path) : QStringView(path).mid(cut + 1);
}

bool endsWithAny(QStringView name, const char *const *suffixes, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const auto suffix = QLatin1String(suffixes[i]);
        if (name.size() > suffix.size() && name.endsWith(suffix, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

// Where the container ends inside `path`: the offset of the separator that follows the
// first component carrying an archive extension of EITHER kind, or path.size() when it
// is the last one. -1 when no component is an archive.
//
// Both tables cut, not just the multi-member one, so that the collapse rule below is
// total: a typed `/logs/app.log.gz/app.log` reduces to `/logs/app.log.gz` rather than
// becoming a second spelling of one log. Which table matched decides what the address
// MEANS — isSingleStream() — but not where it splits.
qsizetype archiveCut(QStringView path)
{
    qsizetype start = 0;
    while (start <= path.size()) {
        qsizetype end = path.indexOf(u'/', start);
        if (end < 0)
            end = path.size();
        const QStringView comp = path.mid(start, end - start);
        if (endsWithAny(comp, kContainerSuffixes, std::size(kContainerSuffixes))
            || endsWithAny(comp, kSingleStreamSuffixes, std::size(kSingleStreamSuffixes))) {
            return end;
        }
        if (end == path.size())
            return -1;
        start = end + 1;
    }
    return -1;
}

// Where the path begins inside a remote address: the offset of the '/' that closes the
// authority, or -1 when the address spells no path at all. Hand-cut rather than asked of
// QUrl because the answer has to be an offset into the STRING THE USER TYPED — see
// split(), which takes the member out of it verbatim.
qsizetype remotePathStart(const QString &s)
{
    const qsizetype scheme = s.indexOf(QLatin1String("://"));
    if (scheme < 0)
        return -1;
    return s.indexOf(u'/', scheme + 3);
}

} // namespace

bool ArchiveLocation::isSingleStreamName(const QString &name)
{
    const QStringView comp = lastComponent(name);
    if (endsWithAny(comp, kContainerSuffixes, std::size(kContainerSuffixes)))
        return false; // .tar.gz is a tar, not a gz
    return endsWithAny(comp, kSingleStreamSuffixes, std::size(kSingleStreamSuffixes));
}

bool ArchiveLocation::isContainerName(const QString &name)
{
    return endsWithAny(lastComponent(name), kContainerSuffixes, std::size(kContainerSuffixes));
}

bool ArchiveLocation::isArchivePath(const QString &path)
{
    return split(path).has_value();
}

std::optional<ArchiveLocation> ArchiveLocation::split(const QString &path)
{
    if (path.isEmpty())
        return std::nullopt;

    const bool remote = RemoteLocation::isRemote(path);

    // Rule 0. A local path that already names a regular file is never split — which is
    // what keeps a real directory called `bundle.zip` working: the file inside it wins
    // over the reading where the directory is an archive. Cannot apply remotely, where
    // the answer would cost a round trip on a path that is normalized constantly.
    if (!remote) {
        const QString native = QDir::fromNativeSeparators(path);
        if (QFileInfo(native).isFile()) {
            ArchiveLocation loc;
            if (isContainerName(native) || isSingleStreamName(native)) {
                loc.container = native;
                return loc;
            }
            return std::nullopt;
        }
    }

    // Rules 1 and 2, over whichever path string the address actually contains. Both
    // rules are one cut: which table matched decides the meaning, not the boundary.
    if (remote) {
        // THE MEMBER IS OPAQUE AND THE CONTAINER IS A URL, which is the whole of the
        // remote branch and the one thing it used to get wrong. It cut the URL's
        // DECODED path and appended the member back verbatim, so a member holding a
        // percent sign lost one layer of encoding per normalize —
        // `ssh://h/u.tar/b%2520c` answered `ssh://h:22/u.tar/b%20c` and then
        // `ssh://h:22/u.tar/b c`, three spellings of one log against a funnel every
        // entry point runs and Document::prepare() runs AGAIN (bugs.md 49). The member
        // is now taken off the RAW string, exactly as the local branch below already
        // takes it, so it is never encoded and never decoded in either direction and
        // the two branches agree about what a member is.
        //
        // The whole address is still parsed first, and only to be JUDGED: a
        // noncharacter anywhere in it, a port out of range, a missing host are
        // refusals belonging to the address rather than to its container half
        // (bugs.md 44, 45), and taking the member raw must not smuggle one past them.
        const auto url = RemoteLocation::parse(path);
        if (!url)
            return std::nullopt;
        // THE CUT IS TAKEN IN THE STRING THE MEMBER COMES OUT OF, never transplanted
        // from the decoded path, whose lengths are a different arithmetic: the raw path
        // is cut, and the container is re-parsed from the raw address to its left, so
        // one offset serves both halves. The only shapes that answer differently are
        // those spelling a container suffix in percent-encoded form (`u%2Etar`), which
        // are no longer read as archives — RemoteLocation::normalize() decodes them on
        // the way past, so such an address reaches the same normal form regardless.
        const qsizetype pathStart = remotePathStart(path);
        if (pathStart < 0)
            return std::nullopt;
        const QStringView rawPath = QStringView(path).mid(pathStart);
        const qsizetype cut = archiveCut(rawPath);
        if (cut < 0)
            return std::nullopt;
        const auto containerUrl = RemoteLocation::parse(path.left(pathStart + cut));
        if (!containerUrl)
            return std::nullopt;
        // The cut was found in the RAW path and the container is what the URL makes of
        // it, so the two are asked to agree before the split stands: a `#` or a `%2F`
        // inside the component that carried the extension leaves a container the URL no
        // longer reads as an archive, and returning one would make `ssh://h/u#a.tar/m` a
        // bare compressed stream called `u`. Where they disagree this is no archive
        // address at all, which is the answer the decoded cut used to give for free.
        if (!isContainerName(containerUrl->path) && !isSingleStreamName(containerUrl->path))
            return std::nullopt;
        ArchiveLocation loc;
        loc.container = containerUrl->toString();
        loc.member = rawPath.mid(cut + 1).toString();
        return loc;
    }

    const QString native = QDir::fromNativeSeparators(path);
    const qsizetype cut = archiveCut(native);
    if (cut < 0)
        return std::nullopt;
    ArchiveLocation loc;
    loc.container = native.left(cut);
    loc.member = native.mid(cut + 1);
    return loc;
}

QString ArchiveLocation::normalize(const QString &path)
{
    if (const auto loc = split(path))
        return loc->toString();
    return path;
}

QString ArchiveLocation::toString() const
{
    if (container.isEmpty())
        return {};

    QString base = RemoteLocation::isRemote(container)
        ? RemoteLocation::normalize(container)
        // Absolute and cleaned to a FIXED POINT, through the one helper both funnels
        // ask: absoluteFilePath() cleans once and QDir::cleanPath() is not idempotent,
        // so `/.//a.zip` would answer `//a.zip` here and `/a.zip` the next time this
        // address was normalized — one log with two spellings, which is what every entry
        // point normalizing and Document::prepare() normalizing AGAIN cannot survive
        // (bugs.md 47). The helper also declines to touch a container holding a NUL,
        // whose absolute path is not absolute at all (bugs.md 48).
        : absoluteLocalPath(QDir::fromNativeSeparators(container));

    // The collapse rule: a bare compressed stream keeps its plain path and never grows
    // a member, so `/logs/app.log.gz` has exactly one spelling.
    if (isSingleStream() || member.isEmpty())
        return base;
    return base + u'/' + member;
}

QString ArchiveLocation::displayMember() const
{
    if (!isSingleStream())
        return QFileInfo(member).fileName();

    // One unnamed member: it is the container's own name with the compression suffix
    // taken off, which is what the writer called the log before it was compressed.
    QString name = QFileInfo(lastComponent(container).toString()).fileName();
    for (const char *suffix : kSingleStreamSuffixes) {
        const auto s = QLatin1String(suffix);
        if (name.size() > s.size() && name.endsWith(s, Qt::CaseInsensitive))
            return name.left(name.size() - s.size());
    }
    return name;
}

} // namespace loftail
