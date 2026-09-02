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
#include "SshFetcher.h"
#include "SshSession.h"

#include <QtTest>

using namespace loftail;

// Per-host fetch tuning, and the one rule that decides which connects may compress.
//
// UNGATED, for the argument tst_sshretry, tst_sshsessioncache and tst_sshexec beside it
// already make: what actually opens a compressed connection needs libssh2, a server and a
// network, so a decision compiled only there is a decision tested only there. What is here
// is the whole of the decision — the option's default, its journey to the fetcher, and
// `compressionFor()`, which is constexpr and lives in the always-compiled header for
// exactly this reason.
class TestSshOptions : public QObject
{
    Q_OBJECT

private slots:
    void compressionIsOffUntilItIsAskedFor();
    void anErrandNeverCompressesHoweverTheHostIsConfigured();
    void theOptionTravelsToTheFetcherThatIsAboutToBeBuilt();
};

void TestSshOptions::compressionIsOffUntilItIsAskedFor()
{
    // The default matters on its own: the deflating is done by the machine holding the
    // log, so a build that started compressing without being asked would be spending
    // somebody else's processor on every remote tab loftail has ever opened.
    QVERIFY2(!SshFetchOptions{}.compress, "compression is opt-in");

    QCOMPARE(SshSession::compressionFor(SshSession::Purpose::Fetch, false),
             SshSession::Compression::None);
    QCOMPARE(SshSession::compressionFor(SshSession::Purpose::Fetch, true),
             SshSession::Compression::Zlib);
}

// The rule requirement 2 of the feature exists for: a config read, a config write and a
// restart script move kilobytes over a link whose far end pays for the compression, so
// they do not compress even on a host whose FETCHES do.
//
// The errand's own call site passes `userAsked = true` deliberately (SshWorkerPool.inl),
// so what this states is the same thing the running code states, rather than an option
// that happens to be off. It is also what keeps SshSessionCache's target+role key honest:
// no compressed session is ever created on the errand path, so none can be checked in.
void TestSshOptions::anErrandNeverCompressesHoweverTheHostIsConfigured()
{
    QCOMPARE(SshSession::compressionFor(SshSession::Purpose::Errand, true),
             SshSession::Compression::None);
    QCOMPARE(SshSession::compressionFor(SshSession::Purpose::Errand, false),
             SshSession::Compression::None);

    // constexpr, so a regression is a build failure as well as a red test — and so the
    // rule is available to a build with no libssh2 at all, which is every build CI runs.
    static_assert(SshSession::compressionFor(SshSession::Purpose::Errand, true)
                      == SshSession::Compression::None,
                  "an errand connect must never ask for compression");
    static_assert(SshSession::compressionFor(SshSession::Purpose::Fetch, true)
                      == SshSession::Compression::Zlib,
                  "a fetch connect must honour the host's choice");
}

// The dialog and the Remote Hosts menu both hand the choice over by location, and the
// fetcher reads it back when it is built. A field added to SshFetchOptions and not carried
// here is a tick box that does nothing.
void TestSshOptions::theOptionTravelsToTheFetcherThatIsAboutToBeBuilt()
{
    const auto parsed = RemoteLocation::parse(QStringLiteral("ssh://me@web1/var/log/app.log"));
    QVERIFY(parsed.has_value());

    QVERIFY2(!sshFetchOptions(*parsed).compress, "nothing said about this host yet");

    SshFetchOptions options;
    options.compress = true;
    options.pollMs = 2500;
    setSshFetchOptions(*parsed, options);

    const SshFetchOptions back = sshFetchOptions(*parsed);
    QVERIFY(back.compress);
    QCOMPARE(back.pollMs, 2500);

    // Per LOCATION, so a second log on the same host that was opened without the box
    // ticked is not quietly compressed by the first one's choice.
    const auto other = RemoteLocation::parse(QStringLiteral("ssh://me@web1/var/log/b.log"));
    QVERIFY(other.has_value());
    QVERIFY(!sshFetchOptions(*other).compress);
}

QTEST_MAIN(TestSshOptions)
#include "tst_sshoptions.moc"
