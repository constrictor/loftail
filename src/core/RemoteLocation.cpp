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

#include "RemoteLocation.h"

#include "ArchiveLocation.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLatin1String>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and the one string below is user-facing all the same: it reaches a
// tab, a window title and the refusal strip. Q_DECLARE_TR_FUNCTIONS is what lets
// lupdate file it under a name that means something rather than under the file it
// happens to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::RemoteLocation)
};
} // namespace

namespace {

// A remote path may be written relative to the login directory ("~/app.log"). A URL
// has no way to spell that — its path always starts at '/' — so the two forms are
// converted on the way in and out, and RemoteLocation::path holds the form SFTP wants.
QString pathFromUrl(const QString &urlPath)
{
    if (urlPath.startsWith(QLatin1String("/~")))
        return urlPath.mid(1);
    return urlPath;
}

QString pathToUrl(const QString &remotePath)
{
    if (remotePath.startsWith(u'/'))
        return remotePath;
    return u'/' + remotePath;
}

} // namespace

bool RemoteLocation::isRemote(const QString &s)
{
    return s.startsWith(QLatin1String("ssh://"), Qt::CaseInsensitive)
        || s.startsWith(QLatin1String("sftp://"), Qt::CaseInsensitive);
}

std::optional<RemoteLocation> RemoteLocation::parse(const QString &s)
{
    if (!isRemote(s))
        return std::nullopt;

    const QUrl url(s, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty())
        return std::nullopt;

    RemoteLocation loc;
    loc.user = url.userName(QUrl::FullyDecoded);
    loc.host = url.host();
    loc.port = url.port(kDefaultPort);
    loc.path = pathFromUrl(url.path(QUrl::FullyDecoded));
    // A password in the URL is dropped on the floor, not stored and not used. Keeping
    // it would put a credential into the session file and the recent-files menu the
    // moment the URL became a Document path; the user is prompted instead.
    if (!loc.isValid())
        return std::nullopt;
    return loc;
}

QString RemoteLocation::normalize(const QString &s)
{
    if (const auto loc = parse(s))
        return loc->toString();
    return s;
}

QString RemoteLocation::withoutPassword(const QString &s)
{
    if (!isRemote(s))
        return s;

    // Deliberately hand-cut rather than routed through QUrl. Every address that reaches
    // here is one QUrl or parse() has already REFUSED — that is the whole reason it is
    // being shown as a string instead of as a parsed location — so asking QUrl to
    // re-serialize it would hand back either nothing or a tidied-up address that is no
    // longer the one the user typed. The authority is the span between "://" and the
    // next '/', the userinfo is what precedes its last '@', and a password is what
    // follows the first ':' inside that.
    const int schemeEnd = int(s.indexOf(QLatin1String("://")));
    if (schemeEnd < 0)
        return s;
    const int authorityStart = schemeEnd + 3;
    int authorityEnd = int(s.indexOf(u'/', authorityStart));
    if (authorityEnd < 0)
        authorityEnd = int(s.size());
    if (authorityEnd <= authorityStart)
        return s;

    const int at = int(s.lastIndexOf(u'@', authorityEnd - 1));
    if (at < authorityStart)
        return s;
    const int colon = int(s.indexOf(u':', authorityStart));
    if (colon < 0 || colon > at)
        return s;
    return s.left(colon) + s.mid(at); // the user is kept; only the secret goes
}

QString RemoteLocation::toString() const
{
    QUrl url;
    url.setScheme(QStringLiteral("ssh"));
    if (!user.isEmpty())
        url.setUserName(user);
    url.setHost(host);
    url.setPort(port); // always spelled, so ssh://h/p and ssh://h:22/p are one string
    url.setPath(pathToUrl(path));
    return url.toString(QUrl::FullyEncoded);
}

QString RemoteLocation::toDisplayString() const
{
    // Built by hand rather than through QUrl, which has no "serialize decoded" mode:
    // QUrl::toString() rejects FullyDecoded outright, and decoding component by
    // component and re-joining is what this already is. The shape is toString()'s,
    // term for term, so the two agree byte for byte whenever nothing needs encoding.
    QString out = QStringLiteral("ssh://");
    if (!user.isEmpty())
        out += user + u'@';
    // An IPv6 literal is bracketed, as QUrl brackets it in toString(): RemoteLocation
    // stores the host unbracketed (parse() takes QUrl::host()), so an address that
    // came back through here unbracketed would not be one either half could re-parse.
    out += host.contains(u':') ? u'[' + host + u']' : host;
    out += u':' + QString::number(port);
    out += pathToUrl(path);
    return out;
}

QString RemoteLocation::effectiveUser() const
{
    if (!user.isEmpty())
        return user;
    // What ssh does with no User directive, and what the connect fills in — reached from
    // here so that both spell it the same way. HomeLocation rather than the environment's
    // USER: it is what Qt already answers on every platform, and it is the same value
    // tryDefaultKeys() derives ~/.ssh from a few lines later.
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation).section(u'/', -1);
}

QString RemoteLocation::target() const
{
    const QString account = effectiveUser();
    // Still guarded, because effectiveUser() can come back empty on a machine with no
    // home directory at all — and "@host:22" would be a key that reads as a bug.
    if (account.isEmpty())
        return QStringLiteral("%1:%2").arg(host).arg(port);
    return QStringLiteral("%1@%2:%3").arg(account, host).arg(port);
}

// --- Path-shaped helpers shared by core and UI -----------------------------

namespace {

// The display name of a path that is NOT an archive address. Split out so the archive
// branch below can label its container with it without recursing back into itself —
// a container path is an archive address, so calling the public function would not
// terminate.
// The name an address with no file-name part still gets. Never empty, never a
// separator and never a credential — the three properties logSourceDisplayName()
// promises (RemoteLocation.h), and this is where the last two are actually kept, since
// the addresses that reach here are exactly the ones RemoteLocation::parse() refused
// and so never cleaned.
QString tailName(const QString &address)
{
    QString rest = RemoteLocation::withoutPassword(address);
    QString scheme;
    if (RemoteLocation::isRemote(address)) {
        const int mark = int(rest.indexOf(QLatin1String("://")));
        scheme = rest.left(mark); // "ssh" / "sftp" — the last thing an `ssh://` has
        rest = rest.mid(mark + 3);
    }
#ifdef Q_OS_WIN
    // Native separators, and only here: a backslash is an ordinary character in a POSIX
    // file name, so folding it into a separator everywhere would split one segment into
    // two on the platform where it is not one. TabLabels.cpp cuts the same way.
    rest.replace(u'\\', u'/');
#endif
    const QStringList segments = rest.split(u'/', Qt::SkipEmptyParts);
    if (!segments.isEmpty())
        return segments.last(); // "/var/log/" is the log directory, and reads as one

    if (!scheme.isEmpty())
        return scheme;
    // "/" and "" — an address with nothing in it that could be a name at all. Saying so
    // beats the empty string every consumer used to be handed, and the reason half of a
    // refusal carries the address itself.
    return Tr::tr("(unnamed)");
}

// A name computed from `whole`, or what `whole` can offer when there was none.
QString orTailOf(const QString &name, const QString &whole)
{
    return name.isEmpty() ? tailName(whole) : name;
}

// A display name taken apart: the log's own name, and what is bracketed onto it to say
// WHERE that log is — a host, or an archive container. logSourceDisplayName() is the two
// put back together, and is what almost everything asks for; logSourceBareName() is the
// first half alone, which is what a tab groups on (TabLabels.h) before deciding which of
// several ranked things actually tells two same-named logs apart.
struct NameParts
{
    QString bare;      // never empty, never a separator, never a credential
    QString qualifier; // empty when the address says nothing about where the log is
};

QString composeName(const NameParts &parts)
{
    // The two-argument arg(), never .arg(bare).arg(qualifier): a log literally named
    // "%2" would otherwise eat the second substitution.
    return parts.qualifier.isEmpty()
        ? parts.bare
        : QStringLiteral("%1 (%2)").arg(parts.bare, parts.qualifier);
}

NameParts plainNameParts(const QString &path)
{
    if (const auto loc = RemoteLocation::parse(path)) {
        // A remote address with no file-name part — `ssh://h/var/log/` — falls back to
        // its deepest segment exactly as a local one does. It used to fall back to the
        // whole remote path, which put a SEPARATOR into a name this file promises has
        // none: "/var/log/ (h)". The property test below never caught it because its
        // table had no remote-directory row, and nothing else looked.
        return {orTailOf(QFileInfo(loc->path).fileName(), loc->path), loc->displayHost()};
    }
    // A remote-shaped address that did NOT parse never goes near QFileInfo: its last
    // path component is the authority — `ssh://deploy:hunter2@web1` has no path at all
    // and fileName() hands back the whole userinfo, password included.
    if (RemoteLocation::isRemote(path))
        return {tailName(path), QString()};
    return {orTailOf(QFileInfo(path).fileName(), path), QString()};
}

QString plainDisplayName(const QString &path)
{
    return composeName(plainNameParts(path));
}

// Likewise: a container path is itself an archive address, so the archive branch must
// ask this rather than the public function.
LogPresence plainPresence(const QString &path)
{
    if (RemoteLocation::isRemote(path)) {
        // Optimistic by design: the honest answer costs a connection, and this is
        // called from session restore, where blocking would be a hang. A host that
        // turns out to be unreachable surfaces as an open failure instead. An address
        // that does not parse names no file, so nothing is at it and nothing will be —
        // logPathIsWellFormed() is what keeps that from becoming an endless wait.
        return RemoteLocation::parse(path) ? LogPresence::Present : LogPresence::Absent;
    }
    const QFileInfo info(path);
    if (!info.exists())
        return LogPresence::Absent;
    // exists() and isReadable() were one answer until M-archive-wait, and the conflation
    // was visible: a file whose mode is 000 was reported as one that "has not appeared
    // yet", sending the reader looking for a file they can see. isReadable() is an
    // access(2) question, so it costs an attribute query and no open.
    return info.isReadable() ? LogPresence::Present : LogPresence::Unreadable;
}

} // namespace

QString normalizeLogPath(const QString &s)
{
    // Archive first: its normal form contains a remote address when the container is
    // remote, and ArchiveLocation::toString() normalizes that part itself.
    if (const auto loc = ArchiveLocation::split(s))
        return loc->toString();
    return RemoteLocation::normalize(s);
}

bool logPathIsSpooled(const QString &s)
{
    return RemoteLocation::isRemote(s) || ArchiveLocation::isArchivePath(s);
}

QString logSettingsKey(const QString &path)
{
    if (logPathIsSpooled(path))
        return normalizeLogPath(path);

    // THE NAME AS OPENED, made absolute and cleaned — NEVER canonicalFilePath(), which is
    // what this was and what put one log's address into the store under two spellings.
    // A pattern is tested against logMatchTarget(), which resolves no symbolic link, so a
    // key that did put the log under `2026-08-29.log` while its pattern claimed
    // `latest.log` split the two questions the settings tree asks about one log: which
    // node it inherits from, and where its own record lives. The visible cost was a
    // symlinked log getting a per-log record for merely being opened — the redundancy
    // rule reduces against the pattern the raw name matched, and the record is filed
    // under a name that matches nothing — so a daily-rotated `latest.log` burned a fresh
    // slot out of the pool of 500 every day, evicting records somebody did configure, and
    // shadowed its own pattern for good afterwards. The mirror case is a spelling that is
    // merely non-canonical (`loftail log/messages` from /var, a `..`, a symlinked
    // directory): the log opened on the built-in defaults because the raw string missed
    // the pattern, and that wrong format was then pinned under the canonical key.
    //
    // TWO ACCEPTED COSTS, both of them the price of the name being authoritative.
    // (1) Two symlinks to one file are now two logs here, with a record each; they are
    //     two names, and a pattern, a tab label and the recent-files menu already treat
    //     them as two. (2) A record written by a build that canonicalised is keyed on a
    //     spelling nothing asks for any more — LogFileStore::read() looks that spelling
    //     up as a fallback and COPIES the record under the name asked for, which is the
    //     only place the old form is allowed to appear. A copy and never a move, because
    //     the old spelling is NOT a dead one: it is a real file's real name, and this
    //     function answers that name unchanged, so re-keying in place would hand a
    //     configured file's settings to a symlink of it for nothing more than the link
    //     being opened once.
    //
    // absoluteFilePath() cleans `.` and `..` and resolves nothing, so it is stable
    // against the working directory (which is what the key is for) without being stable
    // against the file's own identity (which it deliberately no longer claims).
    return QFileInfo(path).absoluteFilePath();
}

QString legacyLogSettingsKey(const QString &path)
{
    // The spooled branch never canonicalised, so nothing about it moved.
    if (logPathIsSpooled(path))
        return QString();

    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    // Empty for a log that is not there — the old key fell back to the absolute path in
    // exactly that case, so there is nothing to look up that logSettingsKey() has not
    // already answered. Same string, same answer: also nothing to look up.
    if (canonical.isEmpty() || canonical == info.absoluteFilePath())
        return QString();
    return canonical;
}

namespace {

// A normal-form address with its percent-encoding taken back off, for the two things a
// PERSON reads: the address a file pattern is matched against and the one shown on a
// tooltip or in a refusal. They ask one function because they have to agree — the
// pattern is typed by somebody reading the path — and because the disagreement is
// silent both ways: a whole-path pattern spelled `*/1/my app.log` matched nothing at
// all against `ssh://u@h:22/1/my%20app.log`, and nothing on screen said to type `%20`,
// so a remote log whose path holds a space (or any non-ASCII character) could be
// claimed by its bare file name and by nothing else.
//
// The argument is already normalized: this only ever undoes the encoding, never the
// absolutizing or the port. A container that did not PARSE keeps its own spelling and
// goes through withoutPassword(), which is the rule everywhere an address is shown.
QString decodedAddress(const QString &normalized)
{
    if (const auto loc = ArchiveLocation::split(normalized)) {
        const auto url = RemoteLocation::parse(loc->container);
        if (!url)
            return RemoteLocation::withoutPassword(normalized); // local, or unparseable
        // The member is stored verbatim and was never encoded, so only the container
        // half moves — and the collapse rule stays ArchiveLocation::toString()'s, which
        // cannot be reused here: it re-normalizes a remote container and would put the
        // encoding straight back.
        const QString base = url->toDisplayString();
        if (loc->isSingleStream() || loc->member.isEmpty())
            return base;
        return base + u'/' + loc->member;
    }
    if (const auto url = RemoteLocation::parse(normalized))
        return url->toDisplayString();
    return RemoteLocation::withoutPassword(normalized);
}

} // namespace

QString logMatchTarget(const QString &path, bool fullPath)
{
    QString normalized = normalizeLogPath(path);
    if (fullPath)
        return decodedAddress(normalized);

    if (const auto loc = ArchiveLocation::split(normalized))
        return loc->displayMember();
    if (const auto url = RemoteLocation::parse(normalized))
        return QFileInfo(url->path).fileName();
    return QFileInfo(normalized).fileName();
}

namespace {

NameParts nameParts(const QString &path)
{
    if (const auto loc = ArchiveLocation::split(path)) {
        // The member name gets the same guarantee as everything else: a member written
        // with a trailing slash has no file-name part either, and it would arrive at a
        // tab as "(bundle.tar.gz)" with nothing in front of the bracket.
        const QString member =
            orTailOf(loc->displayMember(),
                     loc->member.isEmpty() ? loc->container : loc->member);
        // A bare compressed stream is shown as the log the writer meant — "app.log",
        // not "app.log (app.log.gz)", which would name the same thing twice.
        if (loc->isSingleStream()) {
            if (RemoteLocation::isRemote(loc->container)) {
                if (const auto url = RemoteLocation::parse(loc->container))
                    return {member, url->displayHost()};
            }
            return {member, QString()};
        }
        if (loc->member.isEmpty())
            return plainNameParts(loc->container);
        // The qualifier is one opaque string, which is what keeps a member inside a
        // REMOTE container reading "app.log (bundle.tar.gz (h))" exactly as it always
        // has. A tab renders that same address flat — "app.log (h, bundle.tar.gz)" —
        // which is why TabLabels.cpp builds its ranked components from
        // RemoteLocation/ArchiveLocation itself rather than taking this string apart.
        return {member, plainDisplayName(loc->container)};
    }
    return plainNameParts(path);
}

} // namespace

QString logSourceDisplayName(const QString &path)
{
    return composeName(nameParts(path));
}

QString logSourceBareName(const QString &path)
{
    return nameParts(path).bare;
}

QString logSourceDisplayPath(const QString &path)
{
    // The normal form with its percent-encoding taken off — the SAME string a whole-path
    // file pattern is tested against, which is the point of both going through
    // decodedAddress(): what the pattern sees is exactly what the dialog shows, and it
    // stopped being true the moment a remote path held a space.
    //
    // Password-free in every branch. A parsed address is built from a parse() that
    // dropped it; the fall-through is the raw string precisely because nothing could
    // parse it, and an archive's normal form keeps a container it could not normalize
    // verbatim for the same reason, so both go through the one filter.
    return decodedAddress(normalizeLogPath(path));
}

LogPresence logSourcePresence(const QString &path)
{
    // An archived log is present exactly when its container is: whether the member
    // is really in there costs an expansion to answer, and a wrong member surfaces as
    // an open failure instead.
    if (const auto loc = ArchiveLocation::split(path))
        return plainPresence(loc->container);
    return plainPresence(path);
}

bool logSourceAvailable(const QString &path)
{
    return logSourcePresence(path) == LogPresence::Present;
}

bool logPathIsWellFormed(const QString &path)
{
    // An archive is well-formed when its container address is; whether the member is
    // really inside is the same unanswerable-without-expanding question as above, and a
    // missing one surfaces as an open failure rather than as an endless wait.
    const auto archive = ArchiveLocation::split(path);
    // WITH ONE EXCEPTION, and it is pure string work: a multi-member container with no
    // member spelled out names no log at all, and no amount of waiting will put one in
    // the address. Answering "well-formed" there turned M17's no-I/O refusal into an
    // endless wait for any such container that did not happen to exist yet — the tab
    // could not open even once the file arrived, because the address it holds is still
    // not openable (openArchive, LogSourceFactory.cpp).
    if (archive && archive->needsMember())
        return false;
    const QString address = archive ? archive->container : path;

    // A remote address either parses into a host and a path or it does not. "ssh://"
    // does not, and no amount of waiting will give it a host.
    if (RemoteLocation::isRemote(address))
        return RemoteLocation::parse(address).has_value();

    // Any non-empty local path names a file that could exist. Deliberately not
    // isAbsolute(): a relative path is resolved against the working directory and is a
    // perfectly ordinary thing to pass on the command line.
    return !address.isEmpty();
}

} // namespace loftail
