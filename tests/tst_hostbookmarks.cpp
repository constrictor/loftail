#include <QtTest>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "HostBookmarkStore.h"
#include "RemoteLocation.h"
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
    void replacesRatherThanDuplicatingAHost();
    void removesAHost();
    void withoutSavePasswordNoPasswordKeyIsWritten();
    void turningSavePasswordOffErasesTheStoredSecret();
    void aFileHoldingAPasswordIsOwnerOnly();
    void rejectsAnUnknownSchemaVersion();
    void findMatchesOnTheConnectionIdentity();
    void bookmarkBuildsItsLocationAndOptions();
    void credentialCacheAsksOncePerHost();
    void aPasswordNeverLeaksIntoAPathString();
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

void TestHostBookmarks::replacesRatherThanDuplicatingAHost()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));

    HostBookmark updated = sample();
    updated.label = QStringLiteral("production web");
    updated.paths.append(QStringLiteral("/var/log/other.log"));
    QVERIFY(store.save(updated));

    const QVector<HostBookmark> all = store.all();
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.at(0).label, QStringLiteral("production web"));
    QCOMPARE(all.at(0).paths.size(), 2);

    // A different port is a different connection, so a different bookmark.
    HostBookmark other = sample();
    other.port = 2222;
    QVERIFY(store.save(other));
    QCOMPARE(store.all().size(), 2);
}

void TestHostBookmarks::removesAHost()
{
    QTemporaryDir dir;
    HostBookmarkStore store(dir.path());
    QVERIFY(store.save(sample()));
    QVERIFY(store.remove(QStringLiteral("deploy"), QStringLiteral("web1.example.com"), 22));
    QVERIFY(store.all().isEmpty());
    // Removing something absent is a no-op, not a failure.
    QVERIFY(store.remove(QStringLiteral("nobody"), QStringLiteral("nowhere"), 22));
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
}

QTEST_APPLESS_MAIN(TestHostBookmarks)
#include "tst_hostbookmarks.moc"
