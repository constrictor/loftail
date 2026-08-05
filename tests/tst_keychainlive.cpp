#include <QtTest>

#include "SecretStore.h"

using namespace loftail;

// M14 — the real OS keychain, and the ONLY place QtKeychain is exercised beyond linking.
//
// Shaped after tst_sshlive, and gated the same way and for the same reasons. CI never runs
// it: the GitHub runners are headless with no session bus, so nothing there can answer,
// and a green pipeline says nothing whatsoever about whether KWallet, GNOME Keyring, the
// Credential Manager or the macOS Keychain actually work. Run it by hand when changing the
// backend (PLAN.md M14 risk).
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
};

void TestKeychainLive::init()
{
    // Two gates, and both are needed for different reasons.
    //
    // The env var, because this test WRITES TO THE DEVELOPER'S REAL KEYRING — the same
    // device tst_sshlive uses for the same kind of reason, and CI never sets it.
    if (!qEnvironmentVariableIsSet("LOFTAIL_TEST_KEYCHAIN")) {
        QSKIP("Set LOFTAIL_TEST_KEYCHAIN=1 to exercise the real keychain. This test writes "
              "to your keyring and is never run in CI.");
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

// GUILESS, not APPLESS: QtKeychain's jobs are asynchronous and KeychainSecretStore runs a
// nested QEventLoop over them, which needs a QCoreApplication to exist — and the store's
// own thread guard compares against that application's thread.
QTEST_GUILESS_MAIN(TestKeychainLive)
#include "tst_keychainlive.moc"
