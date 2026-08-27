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

#include <QtTest>

#include "FakeSecretStore.h"
#include "RemoteLocation.h"
#include "SecretStore.h"

using namespace loftail;

// M14 — the seam a remembered password goes through, and the consent rule built on it
// (SPEC.md §3, ARCHITECTURE.md §6.3.2).
//
// UNGATED, and deliberately: not one line of this needs QtKeychain. What it pins is that
// the store's CONTRACT is the same in a build with a keychain and a build without —
// the ordering, the wording and, above all, where a secret is allowed to end up. A rule
// tested in only one build configuration is a rule that holds in only one, which is the
// same argument tst_sshexec and tst_socketdetach are ungated for.
class TestSecretStore : public QObject
{
    Q_OBJECT

private slots:
    void theStoreIsNeverNull();
    void withoutAKeychainTheDefaultIsUnavailable();
    void notFoundAndNoBackendAreDifferentAnswers();
    void theKeyIsTheCacheKey();
    void aKeychainThatAnswersTakesThePassword();
    void withNoKeychainTheCallerIsToldToUseItsFile();
    void aRefusedKeychainIsNeverDowngradedToAFile();
    void aRejectedPasswordIsErasedFromTheKeychain();
    void forgettingDoesNothingWithNoKeychain();
};

// Unlike sshPrompter(), which may be null because null IS a policy there ("never
// prompt"), secretStore() always answers. "No keychain" is a property the store states
// about itself, so no call site needs a null check for it.
void TestSecretStore::theStoreIsNeverNull()
{
    QVERIFY(secretStore() != nullptr);

    // Asked BEHAVIOURALLY rather than by comparing pointers, because since M17 the
    // returned object is deliberately not the installed one: secretStore() hands back a
    // view that runs every call on the application thread (ARCHITECTURE.md §6.3.3), so
    // that a keychain read reached from a fetcher's thread cannot be the thing that
    // remembers to marshal itself. What has to hold is that installing a store takes
    // effect and uninstalling restores the default — which is what this now asks.
    FakeSecretStore fake;
    fake.preload(QStringLiteral("ssh/deploy@web1:22"), QStringLiteral("hunter2"));
    {
        InstalledSecretStore installed(&fake);
        QString secret;
        QCOMPARE(secretStore()->read(QStringLiteral("ssh/deploy@web1:22"), &secret),
                 SecretStore::Result::Ok);
        QCOMPARE(secret, QStringLiteral("hunter2"));
    }

    // And that swapping stores actually re-routes: the first fake stops being consulted.
    // Checked against a SECOND fake rather than against the process default, because
    // this test has no QCoreApplication and a real keychain read bridges QtKeychain's
    // asynchronous job with a nested event loop whose timeout is a QTimer — which never
    // fires without an application, so that read would never return.
    const int readsBefore = fake.readCount();
    FakeSecretStore other;
    {
        InstalledSecretStore installed(&other);
        QString stale;
        QCOMPARE(secretStore()->read(QStringLiteral("ssh/deploy@web1:22"), &stale),
                 SecretStore::Result::NotFound);
    }
    QCOMPARE(fake.readCount(), readsBefore);

    // Uninstalling restores the process default rather than leaving a hole.
    QVERIFY(secretStore() != nullptr);
}

// This is what makes the CI leg that configures with -DLOFTAIL_WITH_KEYCHAIN=OFF mean
// something: without the dependency the default store must genuinely refuse, not merely
// fail later at the first read.
void TestSecretStore::withoutAKeychainTheDefaultIsUnavailable()
{
#if defined(LOFTAIL_HAVE_KEYCHAIN)
    QSKIP("this build has QtKeychain; whether a daemon answers is tst_keychainlive's question");
#else
    QVERIFY(!secretStore()->available());
    QVERIFY(secretStore()->backendName().isEmpty());

    QString secret;
    QCOMPARE(secretStore()->read(QStringLiteral("ssh/x@y:22"), &secret),
             SecretStore::Result::NoBackend);
    QCOMPARE(secretStore()->store(QStringLiteral("ssh/x@y:22"), QStringLiteral("p")),
             SecretStore::Result::NoBackend);
#endif
}

// The distinction the whole consent story rests on. "The keychain answered and has
// nothing for you" and "there is no keychain here" lead to different dialog text and
// different fallbacks; a store that returned one code for both would make it impossible
// to tell a user truthfully where their password went.
void TestSecretStore::notFoundAndNoBackendAreDifferentAnswers()
{
    FakeSecretStore fake;
    InstalledSecretStore installed(&fake);

    QString secret;
    fake.setAvailable(true);
    QCOMPARE(fake.read(QStringLiteral("ssh/nobody@nowhere:22"), &secret),
             SecretStore::Result::NotFound);

    fake.setAvailable(false);
    QCOMPARE(fake.read(QStringLiteral("ssh/nobody@nowhere:22"), &secret),
             SecretStore::Result::NoBackend);
}

// One host is one keychain entry for the same reason it is one prompt: both are keyed on
// RemoteLocation::target(). The prefix is what keeps a later kind of loftail secret from
// colliding with an SSH one.
void TestSecretStore::theKeyIsTheCacheKey()
{
    const auto location = RemoteLocation::parse(
        QStringLiteral("ssh://deploy@web1.example.com:22/var/log/app.log"));
    QVERIFY(location.has_value());

    const QString target = location->target();
    QCOMPARE(target, QStringLiteral("deploy@web1.example.com:22"));
    QCOMPARE(sshSecretKey(target), QStringLiteral("ssh/deploy@web1.example.com:22"));

    // Two logs on one host are one entry; two hosts are two.
    const auto second = RemoteLocation::parse(
        QStringLiteral("ssh://deploy@web1.example.com:22/var/log/other.log"));
    QVERIFY(second.has_value());
    QCOMPARE(sshSecretKey(second->target()), sshSecretKey(target));

    // A password spelled into the URL is dropped by target() and so cannot reach the key,
    // which is the same containment tst_hostbookmarks asserts for every path string.
    const auto withSecret = RemoteLocation::parse(
        QStringLiteral("ssh://deploy:hunter2@web1.example.com:22/var/log/app.log"));
    QVERIFY(withSecret.has_value());
    QVERIFY(!sshSecretKey(withSecret->target()).contains(QStringLiteral("hunter2")));
}

void TestSecretStore::aKeychainThatAnswersTakesThePassword()
{
    FakeSecretStore fake;
    fake.setAvailable(true);
    fake.setBackendName(QStringLiteral("KWallet"));
    InstalledSecretStore installed(&fake);

    QString message;
    QCOMPARE(rememberSshPassword(QStringLiteral("deploy@web1:22"),
                                 QStringLiteral("hunter2"), &message),
             RememberOutcome::StoredInKeychain);
    QCOMPARE(fake.contents().value(QStringLiteral("ssh/deploy@web1:22")),
             QStringLiteral("hunter2"));
}

void TestSecretStore::withNoKeychainTheCallerIsToldToUseItsFile()
{
    FakeSecretStore fake;
    fake.setAvailable(false);
    InstalledSecretStore installed(&fake);

    QString message;
    QCOMPARE(rememberSshPassword(QStringLiteral("deploy@web1:22"),
                                 QStringLiteral("hunter2"), &message),
             RememberOutcome::UseFileFallback);
    // Nothing was even attempted: the caller owns the fallback and was told about it by
    // name before the box could be ticked.
    QCOMPARE(fake.storeCount(), 0);
}

// THE consent rule, as an executable assertion.
//
// A keychain is present, the user was shown its name on the checkbox, and it refuses —
// a wallet they declined to unlock, a service that stopped answering. The answer must be
// Failed, never UseFileFallback: falling back here would write a secret into a file the
// user was never told about, from the one dialog whose whole job is to say where the
// secret goes.
void TestSecretStore::aRefusedKeychainIsNeverDowngradedToAFile()
{
    FakeSecretStore fake;
    fake.setAvailable(true);
    fake.setBackendName(QStringLiteral("KWallet"));
    InstalledSecretStore installed(&fake);

    for (const SecretStore::Result refusal : {SecretStore::Result::Denied,
                                              SecretStore::Result::Failed,
                                              SecretStore::Result::NoBackend}) {
        fake.failNext(refusal, QStringLiteral("the wallet is locked"));
        QString message;
        const RememberOutcome outcome = rememberSshPassword(
            QStringLiteral("deploy@web1:22"), QStringLiteral("hunter2"), &message);

        QCOMPARE(outcome, RememberOutcome::Failed);
        QVERIFY(outcome != RememberOutcome::UseFileFallback);
        // Reported, and in words that name the destination and the reason — otherwise the
        // caller has nothing honest to put in front of the user.
        QVERIFY(message.contains(QStringLiteral("KWallet")));
        QVERIFY(message.contains(QStringLiteral("the wallet is locked")));
        QVERIFY(!fake.holds(QStringLiteral("ssh/deploy@web1:22")));
    }
}

// A stored password the server rejects is dropped rather than kept to fail again. sshd
// counts failures against MaxAuthTries (6 by default), and the auth chain already spends
// an agent identity, several key files and up to three prompts.
void TestSecretStore::aRejectedPasswordIsErasedFromTheKeychain()
{
    FakeSecretStore fake;
    fake.setAvailable(true);
    fake.preload(QStringLiteral("ssh/deploy@web1:22"), QStringLiteral("stale"));
    InstalledSecretStore installed(&fake);

    forgetSshPassword(QStringLiteral("deploy@web1:22"));
    QVERIFY(!fake.holds(QStringLiteral("ssh/deploy@web1:22")));
    QCOMPARE(fake.eraseCount(), 1);
}

void TestSecretStore::forgettingDoesNothingWithNoKeychain()
{
    FakeSecretStore fake;
    fake.setAvailable(false);
    InstalledSecretStore installed(&fake);

    forgetSshPassword(QStringLiteral("deploy@web1:22"));
    QCOMPARE(fake.eraseCount(), 0);
}

QTEST_APPLESS_MAIN(TestSecretStore)
#include "tst_secretstore.moc"
