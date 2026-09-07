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

#include "KeychainSecretStore.h"
#include "SecretStore.h"

#include <thread>

using namespace loftail;

// M14 — the real OS keychain, and the ONLY place QtKeychain is exercised beyond linking.
//
// Shaped after tst_sshlive, and gated the same way and for the same reasons — but CI runs
// it now, through packaging/test-keychain/run-keychain-tests.sh, against a real GNOME
// Keyring on a session bus of its own. That harness sets LOFTAIL_TEST_KEYCHAIN itself and
// names the functions below that must have PASSED, because every one of them is gated on
// a backend answering and a QSKIP is a zero exit status.
//
// A bare `ctest` still skips: it would write to the developer's own keyring, and on a KDE
// box QtKeychain picks KWallet. Run it by hand against KWallet, the Credential Manager and
// the macOS Keychain when changing the backend — a Linux runner reaches none of the three
// (PLAN.md M14 risk).
//
// Everything ABOVE the backend — the auth chain's ordering, the checkbox's three
// destinations, the consent rule — is covered with no keychain at all by tst_secretstore,
// tst_hostbookmarks and tst_sshcredentials, using tests/FakeSecretStore.h.
class TestKeychainLive : public QObject
{
    Q_OBJECT

private:
    // Namespaced so a run cannot be mistaken for, or collide with, a real loftail entry
    // in kwalletmanager or seahorse.
    static QString key() { return QStringLiteral("loftail-test/deploy@example.invalid:22"); }

private slots:
    void init();
    void cleanup();
    void roundTripsASecret();
    void readingWhatIsNotThereIsNotFound();
    void erasingWhatIsNotThereSucceeds();
    void storingTwiceReplacesTheSecretRatherThanAddingOne();
    void aSecretSurvivesBeingLongAndNotAscii();
    void everyOperationRefusesToRunOffTheApplicationThread();
};

void TestKeychainLive::init()
{
    // Two gates, and both are needed for different reasons.
    //
    // The env var, because this test WRITES TO WHATEVER KEYRING ANSWERS — the developer's
    // own under a bare ctest, and the throwaway one the harness starts under CI, which is
    // the only reason CI may set it.
    if (!qEnvironmentVariableIsSet("LOFTAIL_TEST_KEYCHAIN")) {
        QSKIP("Set LOFTAIL_TEST_KEYCHAIN=1 to exercise the real keychain, or run "
              "packaging/test-keychain/run-keychain-tests.sh, which points it at a "
              "throwaway keyring of its own.");
    }

    // The round-trip probe, because "the library is here" is not the question. On a
    // headless machine libsecret dlopens perfectly well with no session bus behind it, so
    // QKeychain::isAvailable() says yes — upstream's own comment in keychain_unix.cpp
    // records the gap. SecretStore::available() asks for an answer instead.
    if (!secretStore()->available())
        QSKIP("no keychain backend answered on this machine");
}

void TestKeychainLive::cleanup()
{
    // Never leave a test secret in a real keyring, whatever the case did.
    secretStore()->erase(key());
}

void TestKeychainLive::roundTripsASecret()
{
    QString error;
    QCOMPARE(secretStore()->store(key(), QStringLiteral("hunter2"), &error),
             SecretStore::Result::Ok);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QString read;
    QCOMPARE(secretStore()->read(key(), &read, &error), SecretStore::Result::Ok);
    QCOMPARE(read, QStringLiteral("hunter2"));

    QCOMPARE(secretStore()->erase(key(), &error), SecretStore::Result::Ok);

    // And it is genuinely gone, which is what forgetSshPassword() depends on: a stale
    // password left behind would burn one of sshd's MaxAuthTries on every future connect.
    QCOMPARE(secretStore()->read(key(), &read, &error), SecretStore::Result::NotFound);

    // The name shown to the user is never empty on a machine that answers, because the
    // checkbox has to say where the password is going.
    QVERIFY(!secretStore()->backendName().isEmpty());
}

// The distinction the consent story rests on, against a real backend rather than a fake:
// "nothing stored under this key" must not read as "there is no keychain here".
void TestKeychainLive::readingWhatIsNotThereIsNotFound()
{
    QString read;
    QCOMPARE(secretStore()->read(key(), &read), SecretStore::Result::NotFound);
}

// forgetSshPassword() runs on every rejected stored password, including ones that were
// never there. A backend that called that an error would report one on the common path.
void TestKeychainLive::erasingWhatIsNotThereSucceeds()
{
    QCOMPARE(secretStore()->erase(key()), SecretStore::Result::Ok);
}

// A remembered password is REPLACED when the same host is saved again — the ordinary
// gesture of correcting a password that has changed. Nothing above this seam re-keys or
// erases first, so a backend that appended instead would leave two items under one key and
// hand back whichever it found first, i.e. the stale one, for ever. Only a real Secret
// Service can answer this; FakeSecretStore.h is a QHash and cannot get it wrong.
void TestKeychainLive::storingTwiceReplacesTheSecretRatherThanAddingOne()
{
    QCOMPARE(secretStore()->store(key(), QStringLiteral("first")), SecretStore::Result::Ok);
    QCOMPARE(secretStore()->store(key(), QStringLiteral("second")), SecretStore::Result::Ok);

    QString read;
    QCOMPARE(secretStore()->read(key(), &read), SecretStore::Result::Ok);
    QCOMPARE(read, QStringLiteral("second"));

    // And one erase is enough, which is the same claim from the other end: a second item
    // hiding under the key would surface here.
    QCOMPARE(secretStore()->erase(key()), SecretStore::Result::Ok);
    QCOMPARE(secretStore()->read(key(), &read), SecretStore::Result::NotFound);
}

// An SSH password is whatever the user typed, and the transport hands it to libssh2 as
// UTF-8 bytes. QtKeychain's Unix path carries a secret as a byte array with a content
// type, so a non-ASCII password is the one place a backend can silently re-encode — and
// length matters because libsecret and the Credential Manager have different item size
// limits, neither of them documented at the QtKeychain seam.
void TestKeychainLive::aSecretSurvivesBeingLongAndNotAscii()
{
    const QString secret = QStringLiteral("пароль-\u00E9\u00E8-\u4E2D\u6587-")
                           + QString(2048, QLatin1Char('x'));

    QString error;
    QCOMPARE(secretStore()->store(key(), secret, &error), SecretStore::Result::Ok);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QString read;
    QCOMPARE(secretStore()->read(key(), &read, &error), SecretStore::Result::Ok);
    QCOMPARE(read, secret);
}

// The thread rule, executed rather than merely asserted in a comment (ARCHITECTURE.md
// §6.3.2): a keychain is consulted ONLY on the thread that has a prompter, because a read
// can raise an unlock dialog and SshFetcher::reconnect() runs unattended. The guard is
// what makes that a runtime fact instead of a convention, and it is unreachable through
// secretStore(), which marshals every call to the application thread — so this drives a
// KeychainSecretStore directly, which is the only way the guard can be reached at all.
//
// std::thread and not QThread, deliberately: QThread::wait() joins through a
// QWaitCondition, whose TSan annotations live in a libQt6Core that is not built under TSan
// (CLAUDE.md, src/core's mutex ban), so a test whose own worker needs joining uses the
// standard one.
void TestKeychainLive::everyOperationRefusesToRunOffTheApplicationThread()
{
    KeychainSecretStore store;

    // Latch availability on the right thread first, so what the worker meets is a store
    // that WOULD have answered — otherwise every refusal below is satisfied by there
    // being no keychain, which is the one thing this case must not be able to pass on.
    QVERIFY(store.available());

    // A SECOND store, deliberately unprobed. available() answers its latch before it
    // asks anything, so a probed store says true from any thread — the rule is about the
    // LATCH, and the only way to reach the guard is with a store that has not taken one.
    KeychainSecretStore unprobed;

    SecretStore::Result readResult{}, storeResult{}, eraseResult{};
    QString readError, storeError, eraseError, secret;
    bool availableOffThread = true;

    std::thread worker([&] {
        availableOffThread = unprobed.available();
        readResult = store.read(key(), &secret, &readError);
        storeResult = store.store(key(), QStringLiteral("hunter2"), &storeError);
        eraseResult = store.erase(key(), &eraseError);
    });
    worker.join();

    // NoBackend and never Denied: a refusal is a thing a backend does, and no backend was
    // asked. The auth chain reads Denied as "the user said no" and stops asking.
    QCOMPARE(readResult, SecretStore::Result::NoBackend);
    QCOMPARE(storeResult, SecretStore::Result::NoBackend);
    QCOMPARE(eraseResult, SecretStore::Result::NoBackend);

    // available() answers false off-thread WITHOUT latching, which is the ordering the
    // .cpp calls the whole of it: a latch taken there turns the keychain off for the rest
    // of the process, and M14's remembered passwords stop working with every test above
    // this seam still green. The second half is the one that fails on the wrong order —
    // the same store, asked again from the right thread, still finds the keychain.
    QVERIFY(!availableOffThread);
    QVERIFY(unprobed.available());

    // Each refusal says why. These sentences reach the status bar through
    // Document::lastError(), so an empty one is a failure with nothing on screen.
    QVERIFY(!readError.isEmpty());
    QVERIFY(!storeError.isEmpty());
    QVERIFY(!eraseError.isEmpty());

    // And nothing was written, which is the claim that matters: a guard that returned
    // NoBackend after doing the work would satisfy every assertion above it.
    QCOMPARE(secretStore()->read(key(), &secret), SecretStore::Result::NotFound);
}

// GUILESS, not APPLESS: QtKeychain's jobs are asynchronous and KeychainSecretStore runs a
// nested QEventLoop over them, which needs a QCoreApplication to exist — and the store's
// own thread guard compares against that application's thread.
QTEST_GUILESS_MAIN(TestKeychainLive)
#include "tst_keychainlive.moc"
