#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTimer>

#include "FakeFetcher.h"
#include "FakeSecretStore.h"
#include "HostBookmarkStore.h"
#include "MainWindow.h"
#include "SecretStore.h"
#include "SshPromptDialogs.h"
#include "SshPrompter.h"

using namespace loftail;

// M14 — the password prompt's three destinations, and where a remembered password
// actually lands (SPEC.md §3, ARCHITECTURE.md §6.3.2).
//
// UNGATED, and nothing here needs libssh2 or QtKeychain: the backend is FakeSecretStore
// and the prompter is driven directly rather than through a connect. What is under test is
// the CONSENT contract — that the label names the destination before the box can be
// ticked, and that what the label said is where the secret goes — which must read the
// same in every build configuration or it is not a contract.
class TestSshCredentials : public QObject
{
    Q_OBJECT

private:
    // Drive the modal prompt from outside, the convention tst_openflow uses: a timer that
    // waits for the dialog, inspects it, and dismisses it. `seen` is read afterwards so an
    // assertion cannot pass vacuously because the dialog never appeared.
    struct Prompt
    {
        bool    seen = false;
        bool    rememberEnabled = false;
        QString checkboxText;
        QString noteText;
        QTimer  timer;
    };

    static void driveDialog(Prompt &p, bool tickRemember, const QString &type)
    {
        p.timer.setInterval(5);
        QObject::connect(&p.timer, &QTimer::timeout, [&p, tickRemember, type]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            auto *field = dialog->findChild<QLineEdit *>(QStringLiteral("sshPasswordField"));
            auto *save = dialog->findChild<QCheckBox *>(QStringLiteral("sshRememberPassword"));
            auto *note = dialog->findChild<QLabel *>(QStringLiteral("sshRememberNote"));
            if (!field || !save || !note)
                return;

            p.seen = true;
            p.rememberEnabled = save->isEnabled();
            p.checkboxText = save->text();
            p.noteText = note->text();
            p.timer.stop();

            field->setText(type);
            if (tickRemember && save->isEnabled())
                save->setChecked(true);

            // Click the real OK button rather than calling accept(): the dialog reads
            // acceptance off the button box's clicked() role, so a bare accept() would
            // close it and report a cancel.
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            if (buttons)
                buttons->button(QDialogButtonBox::Ok)->click();
        });
        p.timer.start();
    }

    static HostBookmark sample()
    {
        HostBookmark b;
        b.label = QStringLiteral("prod-web");
        b.user = QStringLiteral("deploy");
        b.host = QStringLiteral("web1.example.com");
        b.port = 22;
        b.auth = HostBookmark::Auth::Password;
        return b;
    }

    static QString target() { return QStringLiteral("deploy@web1.example.com:22"); }

    // A pattern and matching content, so opening settles without raising the Log Format
    // dialog — this test is about the credential cache, not about format detection, and a
    // modal dialog nobody dismisses is a hang.
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
    static QByteArray sampleLog()
    {
        return "2026-07-21 00:00:01,000 [t0] INFO  logger.a - first\n";
    }

private slots:
    void aKeychainNamesItselfOnTheCheckbox();
    void withoutAKeychainTheFileIsNamedAndWarnedAbout();
    void withNeitherAKeychainNorAHostTheBoxIsDisabled();
    void anUntickedBoxStoresNothingAnywhere();
    void aTickedBoxWithAKeychainLeavesTheFileAlone();
    void aTickedBoxWithoutAKeychainWritesTheFile();
    void aRefusedKeychainWritesNothing();
    void openingPrimesTheCacheFromASavedHost();
    void primingNeverOverwritesAnAcceptedPassword();
};

// State 1: a keychain will answer. The checkbox names it, and the note carries no ⚠ —
// this is the case where loftail is not writing a secret to a file it owns.
void TestSshCredentials::aKeychainNamesItselfOnTheCheckbox()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    FakeSecretStore keychain;
    keychain.setAvailable(true);
    keychain.setBackendName(QStringLiteral("KWallet"));
    InstalledSecretStore installed(&keychain);

    GuiSshPrompter prompter;
    prompter.setBookmarkDir(dir.path());

    Prompt prompt;
    driveDialog(prompt, false, QStringLiteral("hunter2"));

    QString password;
    bool remember = false;
    QVERIFY(prompter.askPassword(target(), QStringLiteral("Password:"), &password, &remember));

    QVERIFY(prompt.seen);
    QVERIFY(prompt.rememberEnabled);
    QVERIFY(prompt.checkboxText.contains(QStringLiteral("KWallet")));
    QVERIFY(prompt.noteText.contains(QStringLiteral("KWallet")));
    QVERIFY(!prompt.noteText.contains(QStringLiteral("⚠")));
    QVERIFY(!prompt.noteText.contains(QStringLiteral("plain text")));
    QCOMPARE(password, QStringLiteral("hunter2"));
}

// State 2: no keychain, but a saved host to keep it in. Today's wording, unchanged — and
// it must still name the actual file, because a secret whose location is unstated is worse
// than one you can find.
void TestSshCredentials::withoutAKeychainTheFileIsNamedAndWarnedAbout()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));

    FakeSecretStore keychain;
    keychain.setAvailable(false);
    InstalledSecretStore installed(&keychain);

    GuiSshPrompter prompter;
    prompter.setBookmarkDir(dir.path());

    Prompt prompt;
    driveDialog(prompt, false, QStringLiteral("hunter2"));

    QString password;
    bool remember = false;
    QVERIFY(prompter.askPassword(target(), QStringLiteral("Password:"), &password, &remember));

    QVERIFY(prompt.seen);
    QVERIFY(prompt.rememberEnabled);
    QVERIFY(prompt.noteText.contains(QStringLiteral("⚠")));
    QVERIFY(prompt.noteText.contains(QStringLiteral("plain text")));
    QVERIFY(prompt.noteText.contains(store.filePath().toHtmlEscaped()));
}

// State 3: nowhere to put it. Before M14 the box was offered and did nothing at all; this
// is that same fact, said out loud.
void TestSshCredentials::withNeitherAKeychainNorAHostTheBoxIsDisabled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    FakeSecretStore keychain;
    keychain.setAvailable(false);
    InstalledSecretStore installed(&keychain);

    GuiSshPrompter prompter;
    prompter.setBookmarkDir(dir.path());

    Prompt prompt;
    driveDialog(prompt, true, QStringLiteral("hunter2"));

    QString password;
    bool remember = false;
    QVERIFY(prompter.askPassword(target(), QStringLiteral("Password:"), &password, &remember));

    QVERIFY(prompt.seen);
    QVERIFY(!prompt.rememberEnabled);
    QVERIFY(prompt.noteText.contains(QStringLiteral("Open Remote")));
    // A disabled box cannot be ticked, so the answer that reaches core is false and
    // passwordAccepted() has nothing to do.
    QVERIFY(!remember);
}

void TestSshCredentials::anUntickedBoxStoresNothingAnywhere()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));

    FakeSecretStore keychain;
    keychain.setAvailable(true);
    InstalledSecretStore installed(&keychain);

    GuiSshPrompter prompter;
    prompter.setBookmarkDir(dir.path());
    prompter.passwordAccepted(target(), QStringLiteral("hunter2"), false);

    QCOMPARE(keychain.storeCount(), 0);
    QVERIFY(store.all().first().password.isEmpty());
    QVERIFY(!store.all().first().savePassword);
}

void TestSshCredentials::aTickedBoxWithAKeychainLeavesTheFileAlone()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));

    FakeSecretStore keychain;
    keychain.setAvailable(true);
    keychain.setBackendName(QStringLiteral("KWallet"));
    InstalledSecretStore installed(&keychain);

    GuiSshPrompter prompter;
    prompter.setBookmarkDir(dir.path());
    prompter.passwordAccepted(target(), QStringLiteral("hunter2"), true);

    QCOMPARE(keychain.contents().value(sshSecretKey(target())), QStringLiteral("hunter2"));

    // A bookmark EXISTS here, so the file was a possible destination and was not used.
    QVERIFY(!store.all().first().savePassword);
    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(!file.readAll().contains("hunter2"));
}

void TestSshCredentials::aTickedBoxWithoutAKeychainWritesTheFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));

    FakeSecretStore keychain;
    keychain.setAvailable(false);
    InstalledSecretStore installed(&keychain);

    GuiSshPrompter prompter;
    prompter.setBookmarkDir(dir.path());
    prompter.passwordAccepted(target(), QStringLiteral("hunter2"), true);

    // This is the wire that was dangling before M14: ticking the box in the ad-hoc prompt
    // had never persisted anything, because SshSession dropped `remember` on the floor.
    const QVector<HostBookmark> all = store.all();
    QCOMPARE(all.size(), 1);
    QVERIFY(all.first().savePassword);
    QCOMPARE(all.first().password, QStringLiteral("hunter2"));

#ifdef Q_OS_UNIX
    // And the file carries a secret now, so it must have become owner-only.
    const QFile::Permissions perms = QFile::permissions(store.filePath());
    QVERIFY(!perms.testFlag(QFile::ReadGroup));
    QVERIFY(!perms.testFlag(QFile::ReadOther));
#endif
}

// The consent rule at the UI boundary: a keychain that refuses does not silently become
// the file, even though a bookmark is sitting right there ready to take it.
void TestSshCredentials::aRefusedKeychainWritesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));

    FakeSecretStore keychain;
    keychain.setAvailable(true);
    keychain.setBackendName(QStringLiteral("KWallet"));
    keychain.failNext(SecretStore::Result::Denied, QStringLiteral("the wallet is locked"));
    InstalledSecretStore installed(&keychain);

    GuiSshPrompter prompter;
    prompter.setBookmarkDir(dir.path());

    // The failure is reported with a modal box; dismiss it so the test does not block.
    QTimer dismiss;
    dismiss.setInterval(5);
    connect(&dismiss, &QTimer::timeout, []() {
        if (auto *box = QApplication::activeModalWidget())
            box->close();
    });
    dismiss.start();

    prompter.passwordAccepted(target(), QStringLiteral("hunter2"), true);
    dismiss.stop();

    QVERIFY(!store.all().first().savePassword);
    QVERIFY(store.all().first().password.isEmpty());
    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(!file.readAll().contains("hunter2"));
}

// The OTHER dangling wire M14 connects: HostBookmarkStore::find() had no call site in
// src/ at all, so a bookmark's remembered password was written and never read back.
//
// Driven through the real MainWindow::openFile(), because that is the single funnel every
// entry point goes through and the placement is the point — the transport is faked, the
// path from an open to the credential cache is not.
void TestSshCredentials::openingPrimesTheCacheFromASavedHost()
{
    // FakeRemoteFarm turns on QStandardPaths test mode, so defaultDir() — and with it the
    // bookmark this writes — lands in a test location, not the developer's real config.
    FakeRemoteFarm farm;
    SshCredentialCache::clear();

    HostBookmark bookmark = sample();
    bookmark.savePassword = true;
    bookmark.password = QStringLiteral("hunter2");
    HostBookmarkStore store(HostBookmarkStore::defaultDir());
    QVERIFY(store.save(bookmark));

    const QString url = QStringLiteral("ssh://deploy@web1.example.com/var/log/app.log");
    farm.at(url)->setInitialContent(sampleLog());

    MainWindow window;
    window.openFile(url, QString::fromLatin1(kPattern));

    QVERIFY(SshCredentialCache::has(target()));
    QCOMPARE(SshCredentialCache::password(target()), QStringLiteral("hunter2"));

    SshCredentialCache::clear();
    store.remove(bookmark.displayName());
}

// A password the server has already accepted this session outranks whatever is on disk —
// otherwise a stale saved password would displace a working one on every subsequent open
// of the same host, and the chain would spend an attempt failing with it.
void TestSshCredentials::primingNeverOverwritesAnAcceptedPassword()
{
    FakeRemoteFarm farm;
    SshCredentialCache::clear();
    SshCredentialCache::remember(target(), QStringLiteral("the-one-that-works"));

    HostBookmark bookmark = sample();
    bookmark.savePassword = true;
    bookmark.password = QStringLiteral("stale");
    HostBookmarkStore store(HostBookmarkStore::defaultDir());
    QVERIFY(store.save(bookmark));

    const QString url = QStringLiteral("ssh://deploy@web1.example.com/var/log/app.log");
    farm.at(url)->setInitialContent(sampleLog());

    MainWindow window;
    window.openFile(url, QString::fromLatin1(kPattern));

    QCOMPARE(SshCredentialCache::password(target()), QStringLiteral("the-one-that-works"));

    SshCredentialCache::clear();
    store.remove(bookmark.displayName());
}

QTEST_MAIN(TestSshCredentials)
#include "tst_sshcredentials.moc"
