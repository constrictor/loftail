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

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "FakeSecretStore.h"
#include "HostBookmarkStore.h"
#include "RemoteLocation.h"
#include "SecretStore.h"
#include "SshPrompter.h"

using namespace loftail;

// M11 — saved hosts, and the containment rules around the one genuinely dangerous
// thing in this feature: a password the user asked to keep in clear text (SPEC.md §3).
//
// The store itself is ordinary (PresetStore's shape and guarantees). What is worth
// testing hard is where a secret may and may not end up: on disk only when asked for,
// in a file only its owner can read, and never in any of the several places a remote
// path is written — the session, the recent-files list, the format-cache key, a
// window title.
class TestHostBookmarks : public QObject
{
    Q_OBJECT

private:
    static HostBookmark sample()
    {
        HostBookmark b;
        b.label = QStringLiteral("prod-web");
        b.user = QStringLiteral("deploy");
        b.host = QStringLiteral("web1.example.com");
        b.port = 22;
        b.auth = HostBookmark::Auth::Password;
        b.paths = QStringList{QStringLiteral("/var/log/app.log")};
        b.pollMs = 2000;
        return b;
    }

private slots:
    void roundTripsABookmark();
    void replacesTheBookmarkOfTheSameName();
    void matchesNamesIgnoringCaseAndSpace();
    void replacingKeepsTheListOrder();
    void collapsesDuplicateNamesOnRead();
    void removesAHost();
    void withoutSavePasswordNoPasswordKeyIsWritten();
    void turningSavePasswordOffErasesTheStoredSecret();
    void aFileHoldingAPasswordIsOwnerOnly();
    void rejectsAnUnknownSchemaVersion();
    void findMatchesOnTheConnectionIdentity();
    void bookmarkBuildsItsLocationAndOptions();
    void credentialCacheAsksOncePerHost();
    void aPasswordNeverLeaksIntoAPathString();
    // M14 — the keychain, and what it does and does not change about the file above.
    void indexOfTargetMatchesTheCacheKey();
    void aKeychainKeepsThePasswordOutOfTheFile();
    void aRefusedKeychainNeverFallsBackToPlainText();
};

void TestHostBookmarks::roundTripsABookmark()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());

    HostBookmark b = sample();
    b.tailStartBytes = 64 * 1024 * 1024;
    QVERIFY(store.save(b));

    const QVector<HostBookmark> all = store.all();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0).label, QStringLiteral("prod-web"));
    QCOMPARE(all.at(0).user, QStringLiteral("deploy"));
    QCOMPARE(all.at(0).host, QStringLiteral("web1.example.com"));
    QCOMPARE(all.at(0).port, 22);
    QCOMPARE(all.at(0).auth, HostBookmark::Auth::Password);
    QCOMPARE(all.at(0).pollMs, 2000);
    QCOMPARE(all.at(0).tailStartBytes, 64LL * 1024 * 1024);
    QCOMPARE(all.at(0).paths, QStringList{QStringLiteral("/var/log/app.log")});
    QCOMPARE(all.at(0).displayName(), QStringLiteral("prod-web"));
}

void TestHostBookmarks::replacesTheBookmarkOfTheSameName()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));

    // The name is the identity: same name, one entry, whatever changed underneath —
    // here a different machine entirely.
    HostBookmark updated = sample();
    updated.host = QStringLiteral("web2.example.com");
    updated.pollMs = 5000;
    QVERIFY(store.save(updated));

    QVector<HostBookmark> all = store.all();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0).host, QStringLiteral("web2.example.com"));
    QCOMPARE(all.at(0).pollMs, 5000);

    // A different name on the very same connection is a different entry — the two are
    // distinguishable in the list, which is the whole test.
    HostBookmark other = sample();
    other.label = QStringLiteral("prod-web (root)");
    QVERIFY(store.save(other));
    QCOMPARE(store.all().size(), 2);

    // An unnamed bookmark is identified by its host, which is what the list shows.
    HostBookmark unnamed = sample();
    unnamed.label.clear();
    QVERIFY(store.save(unnamed));
    QCOMPARE(store.all().size(), 3);
    unnamed.pollMs = 250;
    QVERIFY(store.save(unnamed));
    all = store.all();
    QCOMPARE(all.size(), 3);
    QCOMPARE(all.at(2).pollMs, 250);
}

void TestHostBookmarks::matchesNamesIgnoringCaseAndSpace()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));

    // Two rows a person cannot tell apart in the list are exactly the duplication the
    // rule exists to prevent, so the comparison is not literal.
    HostBookmark shouty = sample();
    shouty.label = QStringLiteral("  PROD-Web ");
    shouty.pollMs = 3000;
    QVERIFY(store.save(shouty));

    const QVector<HostBookmark> all = store.all();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0).pollMs, 3000);
}

void TestHostBookmarks::replacingKeepsTheListOrder()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());
    for (const QString &name : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}) {
        HostBookmark b = sample();
        b.label = name;
        QVERIFY(store.save(b));
    }

    HostBookmark b = sample();
    b.label = QStringLiteral("a");
    b.pollMs = 4000;
    QVERIFY(store.save(b));

    const QVector<HostBookmark> all = store.all();
    QCOMPARE(all.size(), 3);
    // Overwriting must not move the row the user is looking at to the bottom.
    QCOMPARE(all.at(0).label, QStringLiteral("a"));
    QCOMPARE(all.at(0).pollMs, 4000);
    QCOMPARE(all.at(2).label, QStringLiteral("c"));
}

void TestHostBookmarks::collapsesDuplicateNamesOnRead()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());

    // A file written before names were the identity, or edited by hand. The first wins:
    // showing both would put rows in the list that cannot be told apart or removed
    // individually — the state this whole rule exists to keep out.
    QVector<HostBookmark> written;
    written.append(sample());
    HostBookmark twin = sample();
    twin.host = QStringLiteral("web9.example.com");
    written.append(twin);
    QVERIFY(store.replaceAll(written));

    const QVector<HostBookmark> all = store.all();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0).host, QStringLiteral("web1.example.com"));
}

void TestHostBookmarks::removesAHost()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));
    QVERIFY(store.remove(QStringLiteral("prod-web")));
    QVERIFY(store.all().isEmpty());
    // Removing something absent is a no-op, not a failure.
    QVERIFY(store.remove(QStringLiteral("nobody")));
}

void TestHostBookmarks::withoutSavePasswordNoPasswordKeyIsWritten()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());

    HostBookmark b = sample();
    b.savePassword = false;
    b.password = QStringLiteral("hunter2"); // set, but not opted into
    QVERIFY(store.save(b));

    QFile f(store.filePath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    f.close();

    // Not merely absent from the parsed struct — absent from the BYTES. A secret that
    // is in the file but ignored on read is still a secret in the file.
    QVERIFY(!raw.contains("hunter2"));

    // And the key itself is not emitted. (Checked on the parsed object, not the raw
    // text: "auth": "password" names the auth METHOD and legitimately appears.)
    const QJsonObject host = QJsonDocument::fromJson(raw)
                                 .object()
                                 .value(QStringLiteral("hosts"))
                                 .toArray()
                                 .at(0)
                                 .toObject();
    QVERIFY(!host.contains(QStringLiteral("password")));
    QCOMPARE(host.value(QStringLiteral("savePassword")).toBool(), false);
    QVERIFY(store.all().at(0).password.isEmpty());
}

void TestHostBookmarks::turningSavePasswordOffErasesTheStoredSecret()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());

    HostBookmark b = sample();
    b.savePassword = true;
    b.password = QStringLiteral("hunter2");
    QVERIFY(store.save(b));
    QVERIFY(store.all().at(0).password == QStringLiteral("hunter2"));

    // Changing one's mind has to actually remove it, not just stop reading it.
    b.savePassword = false;
    QVERIFY(store.save(b));

    QFile f(store.filePath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    f.close();
    QVERIFY(!raw.contains("hunter2"));
}

void TestHostBookmarks::aFileHoldingAPasswordIsOwnerOnly()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());

    HostBookmark b = sample();
    b.savePassword = true;
    b.password = QStringLiteral("hunter2");
    QVERIFY(store.save(b));

    const QFile::Permissions perms = QFile::permissions(store.filePath());
    QVERIFY(perms.testFlag(QFile::ReadOwner));
#ifdef Q_OS_UNIX
    // The warning shown next to the checkbox promises this. Anyone else on the
    // machine must not simply be able to read the file.
    QVERIFY(!perms.testFlag(QFile::ReadGroup));
    QVERIFY(!perms.testFlag(QFile::ReadOther));
#endif
}

void TestHostBookmarks::rejectsAnUnknownSchemaVersion()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));

    QFile f(store.filePath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    root.insert(QStringLiteral("schemaVersion"), HostBookmarkStore::kSchemaVersion + 1);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(root).toJson());
    f.close();

    // Exact-version gating with no migration, as PresetStore and SessionStore do:
    // a half-understood file is worse than none.
    QVERIFY(store.all().isEmpty());
}

void TestHostBookmarks::findMatchesOnTheConnectionIdentity()
{
    QVector<HostBookmark> bookmarks;
    bookmarks.append(sample());

    const auto location = RemoteLocation::parse(
        QStringLiteral("ssh://deploy@web1.example.com/var/log/anything.log"));
    QVERIFY(location.has_value());

    bool found = false;
    const HostBookmark hit = HostBookmarkStore::find(bookmarks, *location, &found);
    QVERIFY(found);
    QCOMPARE(hit.label, QStringLiteral("prod-web"));

    // The path is not part of the match: a bookmark describes a CONNECTION, and one
    // connection serves every file on the host.
    const auto other = RemoteLocation::parse(
        QStringLiteral("ssh://root@web1.example.com/var/log/app.log"));
    QVERIFY(other.has_value());
    HostBookmarkStore::find(bookmarks, *other, &found);
    QVERIFY(!found);
}

void TestHostBookmarks::bookmarkBuildsItsLocationAndOptions()
{
    HostBookmark b = sample();
    b.tailStartBytes = 8 * 1024 * 1024;

    const RemoteLocation location = b.locationFor(QStringLiteral("/var/log/app.log"));
    QCOMPARE(location.toString(),
             QStringLiteral("ssh://deploy@web1.example.com:22/var/log/app.log"));
    QCOMPARE(location.target(), QStringLiteral("deploy@web1.example.com:22"));

    const SshFetchOptions options = b.fetchOptions();
    QCOMPARE(options.pollMs, 2000);
    QCOMPARE(options.tailStartBytes, 8LL * 1024 * 1024);
}

void TestHostBookmarks::credentialCacheAsksOncePerHost()
{
    SshCredentialCache::clear();
    const QString target = QStringLiteral("deploy@web1:22");
    QVERIFY(!SshCredentialCache::has(target));

    SshCredentialCache::remember(target, QStringLiteral("hunter2"));
    // This is what makes N files on one host — and a whole session restore — cost one
    // prompt rather than one each.
    QVERIFY(SshCredentialCache::has(target));
    QCOMPARE(SshCredentialCache::password(target), QStringLiteral("hunter2"));
    QVERIFY(!SshCredentialCache::has(QStringLiteral("deploy@web2:22")));

    SshCredentialCache::forget(target);
    QVERIFY(!SshCredentialCache::has(target));

    SshCredentialCache::remember(target, QStringLiteral("x"));
    SshCredentialCache::clear();
    QVERIFY(!SshCredentialCache::has(target));
}

void TestHostBookmarks::aPasswordNeverLeaksIntoAPathString()
{
    // A remote path is written to the session file, the recent-files menu, the format
    // cache key and the window title. None of those may ever carry a credential, so
    // the URL form drops one outright rather than carrying it.
    const QString withSecret =
        QStringLiteral("ssh://deploy:hunter2@web1.example.com/var/log/app.log");
    const auto location = RemoteLocation::parse(withSecret);
    QVERIFY(location.has_value());

    QVERIFY(!location->toString().contains(QStringLiteral("hunter2")));
    QVERIFY(!location->target().contains(QStringLiteral("hunter2")));
    QVERIFY(!RemoteLocation::normalize(withSecret).contains(QStringLiteral("hunter2")));
    QVERIFY(!logSourceDisplayName(withSecret).contains(QStringLiteral("hunter2")));
    QVERIFY(!logSourceDisplayPath(withSecret).contains(QStringLiteral("hunter2")));

    // And the case this used to miss. Everything above is downstream of a SUCCESSFUL
    // parse(), which is where the password is dropped — so an address parse() REFUSES
    // never met the rule at all, and was echoed back verbatim into the refusal strip.
    // `ssh://u:pw@h` has no path; RemoteLocation::isValid() wants a host and a path.
    const QString unparseable = QStringLiteral("ssh://deploy:hunter2@web1.example.com");
    QVERIFY(!RemoteLocation::parse(unparseable).has_value());
    QVERIFY(!logSourceDisplayName(unparseable).contains(QStringLiteral("hunter2")));
    QVERIFY(!logSourceDisplayPath(unparseable).contains(QStringLiteral("hunter2")));
    QVERIFY(!RemoteLocation::withoutPassword(unparseable).contains(QStringLiteral("hunter2")));
}

// passwordAccepted() is handed a target and nothing else, so the store must be reachable
// by the same string the credential cache and the keychain are keyed on. Comparing
// forwards is what makes the two spellings of target() — with and without a user — both
// work, where parsing "user@host:port" apart would have to guess.
void TestHostBookmarks::indexOfTargetMatchesTheCacheKey()
{
    HostBookmark withUser = sample();

    HostBookmark noUser;
    noUser.label = QStringLiteral("bare");
    noUser.host = QStringLiteral("logs.internal");
    noUser.port = 2222;

    HostBookmark sixSix;
    sixSix.label = QStringLiteral("v6");
    sixSix.user = QStringLiteral("root");
    sixSix.host = QStringLiteral("fd00::1");
    sixSix.port = 22;

    const QVector<HostBookmark> all{withUser, noUser, sixSix};

    QCOMPARE(HostBookmarkStore::indexOfTarget(all, QStringLiteral("deploy@web1.example.com:22")), 0);
    // No '@' at all — the spelling target() uses when there is no user.
    QCOMPARE(noUser.locationFor(QString()).target(), QStringLiteral("logs.internal:2222"));
    QCOMPARE(HostBookmarkStore::indexOfTarget(all, QStringLiteral("logs.internal:2222")), 1);
    // Colons of the address's own, which is why nothing here splits on one.
    QCOMPARE(HostBookmarkStore::indexOfTarget(all, sixSix.locationFor(QString()).target()), 2);

    QCOMPARE(HostBookmarkStore::indexOfTarget(all, QStringLiteral("nobody@nowhere:22")), -1);
    // The port is part of the identity: the same host on another port is another host.
    QCOMPARE(HostBookmarkStore::indexOfTarget(all, QStringLiteral("logs.internal:22")), -1);
}

// With a keychain present the file is not the destination at all, so the secret must not
// be in it — the same assertion withoutSavePasswordNoPasswordKeyIsWritten() makes about
// the bytes, for the other reason a password can be absent from them.
void TestHostBookmarks::aKeychainKeepsThePasswordOutOfTheFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));

    FakeSecretStore keychain;
    keychain.setAvailable(true);
    keychain.setBackendName(QStringLiteral("KWallet"));
    InstalledSecretStore installed(&keychain);

    const QString target = sample().locationFor(QString()).target();
    QString message;
    QCOMPARE(rememberSshPassword(target, QStringLiteral("hunter2"), &message),
             RememberOutcome::StoredInKeychain);

    QCOMPARE(keychain.contents().value(sshSecretKey(target)), QStringLiteral("hunter2"));

    // The file was never touched, and could not have been: the outcome above is the only
    // thing that authorises writing to it.
    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    QVERIFY(!bytes.contains("hunter2"));
    // The key, not the word: "auth":"password" is a legitimate VALUE in this file and
    // says nothing about a secret being in it.
    QVERIFY(!bytes.contains("\"password\":"));
}

// THE consent rule, in the file that owns credential containment.
//
// A keychain is there, the user was shown its name on the checkbox, and it refuses. The
// answer must be Failed and the file must stay clean — a fallback here would put a secret
// somewhere the user was never told about, which is worse than not saving it at all.
void TestHostBookmarks::aRefusedKeychainNeverFallsBackToPlainText()
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

    const QString target = sample().locationFor(QString()).target();
    QString message;
    QCOMPARE(rememberSshPassword(target, QStringLiteral("hunter2"), &message),
             RememberOutcome::Failed);

    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(!file.readAll().contains("hunter2"));

    // And the bookmark on disk still says the password was not kept, so a later open does
    // not prime the cache with something that was never stored.
    const QVector<HostBookmark> all = store.all();
    QCOMPARE(all.size(), 1);
    QVERIFY(!all.first().savePassword);
    QVERIFY(all.first().password.isEmpty());
}

QTEST_APPLESS_MAIN(TestHostBookmarks)
#include "tst_hostbookmarks.moc"
