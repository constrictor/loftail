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

// Addresses: RemoteLocation over an `ssh://` URL and ArchiveLocation over a
// nested container path. Both are typed by a person — into the open dialog, onto
// the command line, into a host bookmark — and both are pure string work.
//
// The property that matters here is not "it did not crash". It is CLAUDE.md's
// standing rule that A PASSWORD NEVER LEAKS INTO ANY STRING THESE PRODUCE:
// parse() drops a URL password on the floor, and an address that does NOT parse
// (`ssh://u:pw@h`, which has no path) used to be echoed back verbatim into the
// refusal strip, credential and all — which is why withoutPassword() exists and
// why every name-shaped and path-shaped answer is checked here rather than only
// the one the bug was reported against.
//
// A SECOND PROPERTY JOINED IT: normalize() is idempotent. Every entry point runs
// a path through it before the string becomes a Document::path() and
// Document::prepare() runs it again, so "one log, one spelling" is exactly the
// claim that the second pass moves nothing. It held for every address but one
// class — the sixty-six Unicode noncharacters, which toString() percent-encodes
// and QUrl declines to give back — and those are refused at parse() now
// (bugs.md 44), so the property is asserted whole.
//
// The password is recovered independently, with QUrl, so the check does not read
// the answer off the very code under test. Two narrowings keep it a property
// rather than a coincidence generator, and the fuzzer produced the input for each
// within minutes: it is asserted only for a password of four characters or more
// that spells no other part of the address, and it looks for the password behind
// the COLON that spells it rather than for the bare bytes (see checkNoPassword).

#include "FuzzSupport.h"

#include "ArchiveLocation.h"
#include "RemoteLocation.h"

#include <QString>
#include <QUrl>

using namespace loftail;

namespace {

// A leaked password is always preceded by the colon that spells it — a URL has
// exactly one place to put one, `scheme://user:PASSWORD@host`, and the reported
// bug was an address ECHOED BACK VERBATIM, colon and all. So what is looked for
// is `:password` and not the bare bytes, and that is the difference between a
// property and a coincidence: the normal form of `ssh://:s:22@os/p` is
// `ssh://os:22/p`, whose host and default port spell the four characters `s:22`
// with no credential anywhere in it. The fuzzer found that one, and two more
// like it, in the first four minutes.
void checkNoPassword(const QString &produced, const QString &password, const char *what)
{
    if (password.size() < 4)
        return;
    FUZZ_CHECK(!produced.contains(QLatin1Char(':') + password), what);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    if (size > 4096)
        return 0;

    const QString address =
        QString::fromUtf8(reinterpret_cast<const char *>(data), qsizetype(size));

    // The password as an independent reader sees it. QUrl in the same strict mode
    // parse() uses, so this is the same string the address actually carries.
    // Only for a REMOTE-SHAPED address. `sshssh://deploy:hunter2@web1/p` looks
    // like userinfo to QUrl and is a plain local path to loftail — `ssh` and
    // `sftp` are the only two schemes isRemote() knows — so withoutPassword()
    // returns it unchanged BY DESIGN ("a local path has no userinfo"), and a
    // colon in a local file name is not a credential. Asking the question
    // outside the rule's own domain is how a property test manufactures its own
    // finding; the fuzzer produced exactly that input in the first minute.
    const QUrl url(address, QUrl::StrictMode);
    QString password = (RemoteLocation::isRemote(address) && url.isValid())
                           ? url.password(QUrl::FullyDecoded)
                           : QString();

    // A password that also spells part of the address ANYWHERE ELSE proves
    // nothing when it comes back out of a correctly stripped answer, and the
    // fuzzer produced three separate shapes of that within four minutes:
    // `sftp://h:BBBB…@BBBBBB…/p`, whose password is a substring of its own host;
    // `ssh://:%c2:s:@ossh/2:s:@…`, whose password is repeated inside the PATH in
    // percent-encoded form, so counting raw occurrences of the decoded string
    // does not see it; and one where the host is spelled in a different case,
    // QUrl having lowercased it. Hence both tests, and the case-insensitive one.
    if (!password.isEmpty()) {
        int seen = 0;
        for (qsizetype at = address.indexOf(password); at >= 0;
             at = address.indexOf(password, at + password.size()))
            ++seen;
        const auto ci = Qt::CaseInsensitive;
        if (seen > 1 || url.host().contains(password, ci)
            || url.path(QUrl::FullyDecoded).contains(password, ci)
            || url.userName(QUrl::FullyDecoded).contains(password, ci)) {
            password.clear();
        }
    }

    (void)RemoteLocation::isRemote(address);

    // NORMALIZING IS IDEMPOTENT, for every string and not only for one that
    // parses: every entry point runs a path through this before it becomes a
    // Document::path(), and Document::prepare() runs it AGAIN, so the whole
    // tree's "one log, one spelling" rests on the second pass moving nothing.
    // An address the parse refuses falls through unchanged, which satisfies it
    // as surely as one that normalizes does. bugs.md 44 is the finding that put
    // it here.
    const QString remoteNormal = RemoteLocation::normalize(address);
    FUZZ_CHECK(RemoteLocation::normalize(remoteNormal) == remoteNormal,
               "normalize is idempotent");

    // The same claim one level up, over the funnel the entry points actually
    // call — and NARROWED to a path that names no archive, which is a FINDING
    // kept rather than fixed. QDir::cleanPath() is not idempotent for a path
    // whose leading `/.` collapses INTO a `//`: `/.//a.zip` cleans to `//a.zip`,
    // which Qt keeps (a POSIX double-slash root) and cleans again to `/a.zip`.
    // ArchiveLocation::toString() cleans its container, so such an address has
    // two spellings for exactly bugs.md 44's reason and with exactly its cost —
    // reported separately because the ruling is a different one, cleaning twice
    // being one answer and refusing being another.
    // corpus/address/a033_dot_slash_before_a_container is the input.
    if (!ArchiveLocation::isArchivePath(address)) {
        const QString logNormal = normalizeLogPath(address);
        FUZZ_CHECK(normalizeLogPath(logNormal) == logNormal, "normalizeLogPath is idempotent");
    }

    const QString stripped = RemoteLocation::withoutPassword(address);
    checkNoPassword(stripped, password, "withoutPassword() drops the password");

    if (auto loc = RemoteLocation::parse(address)) {
        FUZZ_CHECK(!loc->host.isEmpty(), "a parsed address has a host");
        // THE WHOLE TCP RANGE SINCE bugs.md 45 WAS TAKEN, where this was `>= 0`
        // with the gap written out as a finding: `ssh://host:0/path` parsed,
        // because QUrl's port(default) fills the default in only for an address
        // that spells NO port, and an explicit 0 is a port it spells — so it
        // survived into target(), the session-cache key and the connect, and was
        // reported as though the host had refused. parse() refuses it now, which
        // is what lets the property be the range itself.
        // corpus/address/a027_explicit_port_zero is the input, and it passes by
        // being refused.
        FUZZ_CHECK(loc->port >= 1 && loc->port <= 65535, "a parsed address has a real port");

        checkNoPassword(remoteNormal, password, "the remote normal form carries no password");

        const QString normal = loc->toString();
        const QString shown = loc->toDisplayString();
        checkNoPassword(normal, password, "toString() carries no password");
        checkNoPassword(shown, password, "toDisplayString() carries no password");
        checkNoPassword(loc->target(), password, "target() carries no password");
        checkNoPassword(loc->effectiveUser(), password, "effectiveUser() is not a password");

        // The normal form is what a Document::path() holds, so it must itself be
        // an address: two spellings of one remote log have to compare equal in
        // viewOfPath(), the recent-files dedupe and the settings key.
        //
        // UNCONDITIONAL SINCE bugs.md 44 WAS TAKEN, and the unconditionality is
        // the point. This block used to be skipped for a path or an account
        // holding a Unicode noncharacter, which is the one class of character
        // that did not survive its own normal form — `ssh://h/x\uFFFFy`
        // normalized to `ssh://h:22/x%EF%BF%BFy` and parsed back with three
        // U+FFFD in it, so normalize() was not idempotent and one log had two
        // spellings. parse() now REFUSES such an address, which is why the
        // carve-out could go rather than merely being narrowed: this branch is
        // never entered for one. Keeping the property whole is what makes the
        // fuzzer, and not the sixty-six-code-point list in RemoteLocation.cpp,
        // the thing that would find a second class of character behaving that
        // way — the reason parse() checks a table instead of re-parsing its own
        // output, which would have made this assertion tautological.
        // corpus/address/a029_noncharacter_in_path and a030_noncharacter_in_user
        // are the two inputs, and both now pass by being refused.
        if (loc->isValid()) {
            FUZZ_CHECK(RemoteLocation::isRemote(normal), "the normal form is still remote");
            auto again = RemoteLocation::parse(normal);
            FUZZ_CHECK(again.has_value(), "the normal form parses");
            FUZZ_CHECK(again->host == loc->host, "the normal form keeps the host");
            FUZZ_CHECK(again->path == loc->path, "the normal form keeps the path");
            FUZZ_CHECK(again->user == loc->user, "the normal form keeps the user");
            FUZZ_CHECK(again->port == loc->port, "the normal form keeps the port");
            FUZZ_CHECK(again->toString() == normal, "the normal form is a fixed point");
        }
    }

    // The three display answers. RemoteLocation.h states of each of them, in
    // prose and unconditionally, that it never carries a password — and says why:
    // the addresses that reach them are exactly the ones parse() REFUSED and so
    // never cleaned. They are the strongest property in this file, because they
    // hold for every input rather than for the ones that parse.
    checkNoPassword(logSourceDisplayName(address), password, "a display name carries no password");
    checkNoPassword(logSourceBareName(address), password, "a bare name carries no password");
    checkNoPassword(logSourceDisplayPath(address), password, "a display path carries no password");

    // A name is a SEGMENT and never a path, and is never empty. Both are load-bearing
    // one level up: tabLabelsFor() GROUPS on the bare name, and a key with a
    // separator in it groups nothing with anything.
    const QString bare = logSourceBareName(address);
    FUZZ_CHECK(!bare.isEmpty(), "every address has a non-empty bare name");
    FUZZ_CHECK(!bare.contains(QLatin1Char('/')), "a bare name is a segment, not a path");
    FUZZ_CHECK(!logSourceDisplayName(address).isEmpty(), "every address has a display name");

    // ArchiveLocation. split() consults the filesystem exactly once — rule 0, "a
    // local path that exists as a regular FILE is never split", which is what
    // keeps a real directory named bundle.zip working — so nothing here may assert
    // anything that depends on which branch that stat took.
    (void)ArchiveLocation::isArchivePath(address);
    (void)ArchiveLocation::isContainerName(address);
    (void)ArchiveLocation::isSingleStreamName(address);
    if (auto arc = ArchiveLocation::split(address)) {
        // Only inside this branch: for an address that names no archive,
        // normalize() and split() hand the string back UNCHANGED by design, so a
        // password in it is the caller's own string and not something this layer
        // produced. The display answers above are what cover that case.
        FUZZ_CHECK(!arc->container.isEmpty(), "a split address has a container");
        checkNoPassword(arc->container, password, "an archive container carries no password");
        checkNoPassword(arc->member, password, "an archive member carries no password");
        checkNoPassword(arc->toString(), password, "an archive normal form carries no password");
        checkNoPassword(arc->displayMember(), password, "an archive member name is not a password");
        checkNoPassword(ArchiveLocation::normalize(address), password,
                        "an archive normal form carries no password");
    }
    return 0;
}
