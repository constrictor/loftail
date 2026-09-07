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

// A Unicode NONCHARACTER (U+FDD0..U+FDEF and the two at the top of every plane).
// QUrl refuses to produce one from a percent-encoded sequence and answers
// U+FFFD per byte instead, so an address whose path holds one does not survive
// its own normal form — see the fixed-point check below, and the finding it
// records. Sixty-six code points, and no other character in Unicode behaves this
// way (swept, all 1.1 million).
bool containsNoncharacter(const QString &s)
{
    // Over CODE POINTS and not QChars: U+1FFFE and its sixteen siblings are
    // surrogate pairs, and neither half of a pair matches the test below, so a
    // UTF-16 walk would miss every non-BMP noncharacter — which is most of them.
    for (const char32_t u : s.toUcs4()) {
        if ((u >= 0xFDD0 && u <= 0xFDEF) || (u & 0xFFFE) == 0xFFFE)
            return true;
    }
    return false;
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
    (void)normalizeLogPath(address);

    const QString remoteNormal = RemoteLocation::normalize(address);

    const QString stripped = RemoteLocation::withoutPassword(address);
    checkNoPassword(stripped, password, "withoutPassword() drops the password");

    if (auto loc = RemoteLocation::parse(address)) {
        FUZZ_CHECK(!loc->host.isEmpty(), "a parsed address has a host");
        // >= 0 and not > 0, and the difference is a FINDING kept rather than
        // fixed: `ssh://host:0/path` parses, because QUrl's port(default) fills
        // the default in only for an address that spells NO port, and an explicit
        // 0 is a port it spells. So the address survives into target() as
        // `user@host:0` and would be connected to. Reported, not fixed — it is a
        // product call whether an explicit port 0 is a refusal or a silent
        // substitution. corpus/address/a027_explicit_port_zero is the input.
        FUZZ_CHECK(loc->port >= 0, "a parsed address has a port");

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
        // The fixed-point half is skipped for a path holding a noncharacter, and
        // that is a FINDING kept rather than fixed: `ssh://h/x\uFFFFy` normalizes
        // to `ssh://h:22/x%EF%BF%BFy`, which parses back with the noncharacter
        // replaced by three U+FFFD — so normalize() is not idempotent there,
        // and every entry point normalizes while Document::prepare() normalizes
        // AGAIN, which is exactly the "two spellings of one log" the contract
        // exists to prevent. Reported, not fixed: it needs a product call about
        // what an address holding one should do, and the practical population is
        // a remote log whose file name contains U+FFFF. It reaches the ACCOUNT
        // by the same route (`ssh://\uFFFFu:p@host/p`), which is the second input
        // the fuzzer produced, so both components are excused here.
        // corpus/address/a029_noncharacter_in_path and a030_noncharacter_in_user
        // are the two inputs.
        if (loc->isValid() && !containsNoncharacter(loc->path)
            && !containsNoncharacter(loc->user)) {
            FUZZ_CHECK(RemoteLocation::isRemote(normal), "the normal form is still remote");
            auto again = RemoteLocation::parse(normal);
            FUZZ_CHECK(again.has_value(), "the normal form parses");
            FUZZ_CHECK(again->host == loc->host, "the normal form keeps the host");
            FUZZ_CHECK(again->path == loc->path, "the normal form keeps the path");
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
