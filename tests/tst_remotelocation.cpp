#include <QtTest>

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include "FormatCache.h"
#include "RemoteLocation.h"

using namespace loftail;

// M11 — the ssh:// URL value type and the path-shaped helpers built on it
// (SPEC.md §3, ARCHITECTURE.md §6.3). A remote log travels through the whole
// application as a path STRING, so the contract under test is that one remote file
// has exactly ONE spelling: every entry point normalizes, and everything that
// compares paths (viewOfPath, the recent-files dedupe, the format-cache key, the
// session) therefore agrees. Core-only, no QApplication, no network.
class TestRemoteLocation : public QObject
{
    Q_OBJECT

private slots:
    void recognisesRemoteSchemes();
    void parsesFullUrl();
    void defaultsThePortAndOmitsAnUnspecifiedUser();
    void normalizesEquivalentSpellingsToOneString();
    void roundTripsThroughToString();
    void decodesPercentEncodedPaths();
    void keepsTildeRelativePaths();
    void rejectsMalformedUrls();
    void neverEmitsAPasswordFromTheUrl();
    void targetGroupsFilesOnOneHost();
    void displayHelpersFallBackToLocalBehavior();
    void availabilityIsOptimisticForRemote();
    void formatCacheKeyIsWorkingDirectoryIndependent();
    void formatCacheRoundTripsARemotePath();
};

void TestRemoteLocation::recognisesRemoteSchemes()
{
    QVERIFY(RemoteLocation::isRemote(QStringLiteral("ssh://h/a.log")));
    QVERIFY(RemoteLocation::isRemote(QStringLiteral("SSH://h/a.log"))); // case-insensitive
    // A file manager's SSH mount drags out as sftp://; it means the same thing.
    QVERIFY(RemoteLocation::isRemote(QStringLiteral("sftp://h/a.log")));

    QVERIFY(!RemoteLocation::isRemote(QStringLiteral("/var/log/a.log")));
    QVERIFY(!RemoteLocation::isRemote(QStringLiteral("a.log")));
    QVERIFY(!RemoteLocation::isRemote(QString()));
    // A Windows drive letter must not read as a scheme.
    QVERIFY(!RemoteLocation::isRemote(QStringLiteral("C:/logs/a.log")));
    // Neither must a local file:// URL, which has its own handling.
    QVERIFY(!RemoteLocation::isRemote(QStringLiteral("file:///var/log/a.log")));
}

void TestRemoteLocation::parsesFullUrl()
{
    const auto loc = RemoteLocation::parse(QStringLiteral("ssh://deploy@web1:2222/var/log/app.log"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->user, QStringLiteral("deploy"));
    QCOMPARE(loc->host, QStringLiteral("web1"));
    QCOMPARE(loc->port, 2222);
    QCOMPARE(loc->path, QStringLiteral("/var/log/app.log"));
    QVERIFY(loc->isValid());
}

void TestRemoteLocation::defaultsThePortAndOmitsAnUnspecifiedUser()
{
    const auto loc = RemoteLocation::parse(QStringLiteral("ssh://web1/var/log/app.log"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->port, RemoteLocation::kDefaultPort);
    // The user stays EMPTY rather than being filled in with the local account name:
    // ~/.ssh/config may set a different User for this host, and inventing one here
    // would connect as the wrong identity.
    QVERIFY(loc->user.isEmpty());
    QCOMPARE(loc->toString(), QStringLiteral("ssh://web1:22/var/log/app.log"));
}

void TestRemoteLocation::normalizesEquivalentSpellingsToOneString()
{
    // Every spelling of one remote file must collapse to a single string, or the
    // application opens two tabs on it and remembers its format twice.
    const QString canonical = QStringLiteral("ssh://deploy@web1:22/var/log/app.log");
    const QStringList spellings = {
        QStringLiteral("ssh://deploy@web1/var/log/app.log"),    // implicit port
        QStringLiteral("ssh://deploy@web1:22/var/log/app.log"), // explicit port
        QStringLiteral("sftp://deploy@web1/var/log/app.log"),   // the other scheme
        QStringLiteral("SSH://deploy@web1/var/log/app.log"),    // upper-case scheme
    };
    for (const QString &s : spellings)
        QCOMPARE(RemoteLocation::normalize(s), canonical);

    // A local path is returned untouched — call sites need no branch of their own.
    QCOMPARE(RemoteLocation::normalize(QStringLiteral("/var/log/app.log")),
             QStringLiteral("/var/log/app.log"));
    QCOMPARE(RemoteLocation::normalize(QString()), QString());
}

void TestRemoteLocation::roundTripsThroughToString()
{
    const QString url = QStringLiteral("ssh://deploy@web1:2222/var/log/app.log");
    const auto first = RemoteLocation::parse(url);
    QVERIFY(first.has_value());
    const auto second = RemoteLocation::parse(first->toString());
    QVERIFY(second.has_value());
    QCOMPARE(second->user, first->user);
    QCOMPARE(second->host, first->host);
    QCOMPARE(second->port, first->port);
    QCOMPARE(second->path, first->path);
    // Normalization is idempotent — a stored path re-read and re-stored is stable.
    QCOMPARE(RemoteLocation::normalize(first->toString()), first->toString());
}

void TestRemoteLocation::decodesPercentEncodedPaths()
{
    const auto loc = RemoteLocation::parse(QStringLiteral("ssh://h/var/log/my%20app.log"));
    QVERIFY(loc.has_value());
    // The struct holds the DECODED path, because that is what SFTP is handed.
    QCOMPARE(loc->path, QStringLiteral("/var/log/my app.log"));
    // The URL form re-encodes it, so the round trip through a settings file is safe.
    QCOMPARE(loc->toString(), QStringLiteral("ssh://h:22/var/log/my%20app.log"));
    QCOMPARE(RemoteLocation::normalize(loc->toString()), loc->toString());
}

void TestRemoteLocation::keepsTildeRelativePaths()
{
    // A URL path always starts at '/', but a remote path may be relative to the
    // login directory. The two forms convert on the way in and out.
    const auto loc = RemoteLocation::parse(QStringLiteral("ssh://h/~/app.log"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->path, QStringLiteral("~/app.log"));
    QCOMPARE(loc->toString(), QStringLiteral("ssh://h:22/~/app.log"));
}

void TestRemoteLocation::rejectsMalformedUrls()
{
    QVERIFY(!RemoteLocation::parse(QStringLiteral("/var/log/app.log")).has_value());
    QVERIFY(!RemoteLocation::parse(QStringLiteral("ssh://")).has_value());   // no host
    QVERIFY(!RemoteLocation::parse(QStringLiteral("ssh:///a.log")).has_value()); // no host
    QVERIFY(!RemoteLocation::parse(QStringLiteral("ssh://host")).has_value());   // no path
    QVERIFY(!RemoteLocation::parse(QString()).has_value());
    // A rejected URL normalizes to itself rather than to something invented, so a
    // bad string fails at open with the text the user actually typed.
    QCOMPARE(RemoteLocation::normalize(QStringLiteral("ssh://host")),
             QStringLiteral("ssh://host"));
}

void TestRemoteLocation::neverEmitsAPasswordFromTheUrl()
{
    // toString() is written to the session file, the recent-files menu and the window
    // title. A credential must not ride along, so a password in the URL is dropped
    // outright rather than carried.
    const auto loc = RemoteLocation::parse(QStringLiteral("ssh://deploy:hunter2@web1/var/log/a.log"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->user, QStringLiteral("deploy"));
    const QString emitted = loc->toString();
    QVERIFY(!emitted.contains(QStringLiteral("hunter2")));
    QCOMPARE(emitted, QStringLiteral("ssh://deploy@web1:22/var/log/a.log"));
    QVERIFY(!loc->target().contains(QStringLiteral("hunter2")));
    QVERIFY(!logSourceDisplayPath(QStringLiteral("ssh://deploy:hunter2@web1/var/log/a.log"))
                 .contains(QStringLiteral("hunter2")));
}

void TestRemoteLocation::targetGroupsFilesOnOneHost()
{
    // The pool key must ignore the path, so every file on one host shares a single
    // connection — and, at session restore, a single password prompt.
    const auto a = RemoteLocation::parse(QStringLiteral("ssh://deploy@web1/var/log/a.log"));
    const auto b = RemoteLocation::parse(QStringLiteral("ssh://deploy@web1/var/log/b.log"));
    QVERIFY(a.has_value() && b.has_value());
    QCOMPARE(a->target(), b->target());
    QCOMPARE(a->target(), QStringLiteral("deploy@web1:22"));

    // A different user or port is a different connection.
    const auto c = RemoteLocation::parse(QStringLiteral("ssh://root@web1/var/log/a.log"));
    const auto d = RemoteLocation::parse(QStringLiteral("ssh://deploy@web1:2222/var/log/a.log"));
    QVERIFY(c.has_value() && d.has_value());
    QVERIFY(c->target() != a->target());
    QVERIFY(d->target() != a->target());
}

void TestRemoteLocation::displayHelpersFallBackToLocalBehavior()
{
    // Local: exactly what QFileInfo::fileName() gave before this existed.
    QCOMPARE(logSourceDisplayName(QStringLiteral("/var/log/app.log")),
             QStringLiteral("app.log"));
    QCOMPARE(logSourceDisplayPath(QStringLiteral("/var/log/app.log")),
             QStringLiteral("/var/log/app.log"));

    // Remote: the host is what tells two same-named logs from different machines
    // apart in the tab bar.
    QCOMPARE(logSourceDisplayName(QStringLiteral("ssh://deploy@web1/var/log/app.log")),
             QStringLiteral("app.log (web1)"));
    QCOMPARE(logSourceDisplayPath(QStringLiteral("ssh://deploy@web1/var/log/app.log")),
             QStringLiteral("ssh://deploy@web1:22/var/log/app.log"));
}

void TestRemoteLocation::availabilityIsOptimisticForRemote()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString present = dir.filePath(QStringLiteral("there.log"));
    QFile f(present);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    QVERIFY(logSourceAvailable(present));
    QVERIFY(!logSourceAvailable(dir.filePath(QStringLiteral("gone.log"))));

    // A well-formed remote path is always "available": answering honestly would cost
    // a network round trip, and this runs during session restore where a stall is a
    // hang. An unreachable host surfaces as an open failure instead.
    QVERIFY(logSourceAvailable(QStringLiteral("ssh://web1/var/log/app.log")));
    // A malformed one is not — there is nothing to try.
    QVERIFY(!logSourceAvailable(QStringLiteral("ssh://")));
}

void TestRemoteLocation::formatCacheKeyIsWorkingDirectoryIndependent()
{
    // Regression: canonicalKey() fell through to QFileInfo::absoluteFilePath() for a
    // remote URL, which prepended the working directory and collapsed the "//" —
    // so a file's remembered format was lost whenever loftail was launched from a
    // different directory.
    const QString url = QStringLiteral("ssh://deploy@web1/var/log/app.log");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString before = QDir::currentPath();
    const QString keyHere = FormatCache::canonicalKey(url);
    QVERIFY(QDir::setCurrent(dir.path()));
    const QString keyThere = FormatCache::canonicalKey(url);
    QVERIFY(QDir::setCurrent(before));

    QCOMPARE(keyHere, keyThere);
    QCOMPARE(keyHere, RemoteLocation::normalize(url));
    QVERIFY(!keyHere.contains(QStringLiteral("ssh:/var")));
    QVERIFY(keyHere.startsWith(QStringLiteral("ssh://")));

    // Equivalent spellings share one cache entry.
    QCOMPARE(FormatCache::canonicalKey(QStringLiteral("sftp://deploy@web1:22/var/log/app.log")),
             keyHere);
}

void TestRemoteLocation::formatCacheRoundTripsARemotePath()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings store(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);

    FormatSettings s;
    s.pattern = QStringLiteral("%d{ISO8601} [%t] %-5p %c - %m%n");
    FormatCache::save(store, QStringLiteral("ssh://deploy@web1/var/log/app.log"), s);

    // Reopened by a different spelling of the same file: still one entry, found.
    const auto loaded =
        FormatCache::load(store, QStringLiteral("ssh://deploy@web1:22/var/log/app.log"));
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->pattern, s.pattern);

    // A different remote file is not confused with it.
    QVERIFY(!FormatCache::load(store, QStringLiteral("ssh://deploy@web2/var/log/app.log"))
                 .has_value());
}

QTEST_APPLESS_MAIN(TestRemoteLocation)
#include "tst_remotelocation.moc"
