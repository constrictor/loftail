#include <QtTest>

#include "TabLabels.h"

using namespace loftail;

// What each open log is CALLED on its tab (SPEC.md §3, ARCHITECTURE.md §12.4).
//
// The rule is a statement about a SET — a log's label depends on which other logs are
// open — so it is a free function over a list of addresses, and this is the test that
// drives it directly. What a real window then does with the labels, and that closing a
// log shortens the survivor again, is tst_multidoc's.
//
// Every kind of address the application accepts appears here, because "the nearest
// differing parent segment" means something different for each: a local path has
// directories, a remote log has a host that is already in its name, and an archived one
// has a container that is too, plus directories on both sides of it.
class TestTabLabels : public QObject
{
    Q_OBJECT

private slots:
    void logsWithDifferentNamesKeepTheirOwnNames();
    void logsWithDifferentNamesKeepTheirOwnNames_data();

    void twoLogsWithOneNameEachGrowTheNearestDifferingDirectory();
    void twoLogsWithOneNameEachGrowTheNearestDifferingDirectory_data();

    void aDeeperCollisionGrowsAsManySegmentsAsItTakesAndNoMore();
    void aDeeperCollisionGrowsAsManySegmentsAsItTakesAndNoMore_data();

    void aRemoteLogIsToldApartByItsHostBeforeAnyDirectory();
    void aRemoteLogIsToldApartByItsHostBeforeAnyDirectory_data();

    void anArchivedLogIsToldApartByItsContainerBeforeAnyDirectory();
    void anArchivedLogIsToldApartByItsContainerBeforeAnyDirectory_data();

    void anEnormousDirectoryIsElidedInTheMiddleAndTheLogsOwnNameIsKeptWhole();
    void addressesThatNoSegmentCanSeparateKeepTheirSharedName();
};

// One row per case: the addresses as they would be open, and the label each should
// wear, in the same order.
static void run(const QStringList &addresses, const QStringList &expected)
{
    QCOMPARE(tabLabelsFor(addresses), expected);
}

void TestTabLabels::logsWithDifferentNamesKeepTheirOwnNames_data()
{
    QTest::addColumn<QStringList>("addresses");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("one log")
        << QStringList{QStringLiteral("/var/log/app.log")}
        << QStringList{QStringLiteral("app.log")};

    QTest::newRow("two names, one directory")
        << QStringList{QStringLiteral("/var/log/app.log"), QStringLiteral("/var/log/db.log")}
        << QStringList{QStringLiteral("app.log"), QStringLiteral("db.log")};

    // Nothing to disambiguate, so nothing is added — even though the directories differ.
    QTest::newRow("two names, two directories")
        << QStringList{QStringLiteral("/srv/a/app.log"), QStringLiteral("/srv/b/db.log")}
        << QStringList{QStringLiteral("app.log"), QStringLiteral("db.log")};
}

void TestTabLabels::logsWithDifferentNamesKeepTheirOwnNames()
{
    QFETCH(QStringList, addresses);
    QFETCH(QStringList, expected);
    run(addresses, expected);
}

void TestTabLabels::twoLogsWithOneNameEachGrowTheNearestDifferingDirectory_data()
{
    QTest::addColumn<QStringList>("addresses");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("one service per directory")
        << QStringList{QStringLiteral("/var/log/svc-a/app.log"),
                       QStringLiteral("/var/log/svc-b/app.log")}
        << QStringList{QStringLiteral("svc-a/app.log"), QStringLiteral("svc-b/app.log")};

    // The order out is the order in: MainWindow maps these onto its own contexts.
    QTest::newRow("order is preserved")
        << QStringList{QStringLiteral("/var/log/svc-b/app.log"),
                       QStringLiteral("/etc/other.log"),
                       QStringLiteral("/var/log/svc-a/app.log")}
        << QStringList{QStringLiteral("svc-b/app.log"), QStringLiteral("other.log"),
                       QStringLiteral("svc-a/app.log")};

    // A log at the root of the filesystem has no parent to grow; it keeps its name and
    // the one that can grow does.
    QTest::newRow("one side has no parent")
        << QStringList{QStringLiteral("/app.log"), QStringLiteral("/svc-b/app.log")}
        << QStringList{QStringLiteral("app.log"), QStringLiteral("svc-b/app.log")};
}

void TestTabLabels::twoLogsWithOneNameEachGrowTheNearestDifferingDirectory()
{
    QFETCH(QStringList, addresses);
    QFETCH(QStringList, expected);
    run(addresses, expected);
}

void TestTabLabels::aDeeperCollisionGrowsAsManySegmentsAsItTakesAndNoMore_data()
{
    QTest::addColumn<QStringList>("addresses");
    QTest::addColumn<QStringList>("expected");

    // The nearest parent is `current` for both, so one segment settles nothing.
    QTest::newRow("two logs sharing two segments")
        << QStringList{QStringLiteral("/srv/prod/current/app.log"),
                       QStringLiteral("/srv/test/current/app.log")}
        << QStringList{QStringLiteral("prod/current/app.log"),
                       QStringLiteral("test/current/app.log")};

    // Three logs, and one of them is already unique at one segment — but the group is
    // cut at ONE depth, so all three read the same way rather than two of them being
    // shorter than the third.
    QTest::newRow("three logs, two of them deeper")
        << QStringList{QStringLiteral("/srv/a/current/app.log"),
                       QStringLiteral("/srv/b/current/app.log"),
                       QStringLiteral("/srv/c/old/app.log")}
        << QStringList{QStringLiteral("a/current/app.log"), QStringLiteral("b/current/app.log"),
                       QStringLiteral("c/old/app.log")};
}

void TestTabLabels::aDeeperCollisionGrowsAsManySegmentsAsItTakesAndNoMore()
{
    QFETCH(QStringList, addresses);
    QFETCH(QStringList, expected);
    run(addresses, expected);
}

void TestTabLabels::aRemoteLogIsToldApartByItsHostBeforeAnyDirectory_data()
{
    QTest::addColumn<QStringList>("addresses");
    QTest::addColumn<QStringList>("expected");

    // The host is part of the log's own name already, so one path on two machines needs
    // no directory at all — which is the common case for anyone tailing one service
    // across hosts.
    QTest::newRow("one path, two hosts")
        << QStringList{QStringLiteral("ssh://host-a/var/log/app.log"),
                       QStringLiteral("ssh://host-b/var/log/app.log")}
        << QStringList{QStringLiteral("app.log (host-a)"), QStringLiteral("app.log (host-b)")};

    // Same host: now the directories are what differ, and the host stays bracketed on.
    QTest::newRow("one host, two directories")
        << QStringList{QStringLiteral("ssh://host-a/var/log/svc-a/app.log"),
                       QStringLiteral("ssh://host-a/var/log/svc-b/app.log")}
        << QStringList{QStringLiteral("svc-a/app.log (host-a)"),
                       QStringLiteral("svc-b/app.log (host-a)")};

    // A remote log and a local one with the same file name are already distinct.
    QTest::newRow("remote against local")
        << QStringList{QStringLiteral("ssh://host-a/var/log/app.log"),
                       QStringLiteral("/var/log/app.log")}
        << QStringList{QStringLiteral("app.log (host-a)"), QStringLiteral("app.log")};
}

void TestTabLabels::aRemoteLogIsToldApartByItsHostBeforeAnyDirectory()
{
    QFETCH(QStringList, addresses);
    QFETCH(QStringList, expected);
    run(addresses, expected);
}

void TestTabLabels::anArchivedLogIsToldApartByItsContainerBeforeAnyDirectory_data()
{
    QTest::addColumn<QStringList>("addresses");
    QTest::addColumn<QStringList>("expected");

    // The container is bracketed onto the name exactly as a host is.
    QTest::newRow("two containers")
        << QStringList{QStringLiteral("/srv/a.tar.gz/var/log/app.log"),
                       QStringLiteral("/srv/b.tar.gz/var/log/app.log")}
        << QStringList{QStringLiteral("app.log (a.tar.gz)"), QStringLiteral("app.log (b.tar.gz)")};

    // One container, two members with the same name: the directories INSIDE it are the
    // nearest thing that differs.
    QTest::newRow("one container, two members")
        << QStringList{QStringLiteral("/srv/bundle.tar.gz/svc-a/app.log"),
                       QStringLiteral("/srv/bundle.tar.gz/svc-b/app.log")}
        << QStringList{QStringLiteral("svc-a/app.log (bundle.tar.gz)"),
                       QStringLiteral("svc-b/app.log (bundle.tar.gz)")};

    // Same member path in two identically-named containers: the segments run out inside
    // the archive and continue ABOVE it.
    QTest::newRow("two containers with one name")
        << QStringList{QStringLiteral("/srv/a/bundle.tar.gz/log/app.log"),
                       QStringLiteral("/srv/b/bundle.tar.gz/log/app.log")}
        << QStringList{QStringLiteral("a/log/app.log (bundle.tar.gz)"),
                       QStringLiteral("b/log/app.log (bundle.tar.gz)")};

    // A bare compressed stream is shown as the log the writer meant, so two of them
    // collide on that name and are told apart by the directories holding them.
    QTest::newRow("two single streams")
        << QStringList{QStringLiteral("/srv/svc-a/app.log.gz"),
                       QStringLiteral("/srv/svc-b/app.log.gz")}
        << QStringList{QStringLiteral("svc-a/app.log"), QStringLiteral("svc-b/app.log")};
}

void TestTabLabels::anArchivedLogIsToldApartByItsContainerBeforeAnyDirectory()
{
    QFETCH(QStringList, addresses);
    QFETCH(QStringList, expected);
    run(addresses, expected);
}

void TestTabLabels::anEnormousDirectoryIsElidedInTheMiddleAndTheLogsOwnNameIsKeptWhole()
{
    // The two directories differ in their last character, so an elision that kept only
    // the head would take away the very thing the segment was added for.
    const QString a =
        QStringLiteral("/srv/logs-for-the-alpha-deployment-eu-west-1/app.log");
    const QString b =
        QStringLiteral("/srv/logs-for-the-alpha-deployment-eu-west-2/app.log");
    const QStringList labels = tabLabelsFor({a, b});

    QCOMPARE(labels.size(), 2);
    QVERIFY2(labels.at(0) != labels.at(1), qPrintable(labels.join(u' ')));
    for (const QString &l : labels) {
        QVERIFY(l.endsWith(QStringLiteral("/app.log"))); // the name itself, never cut
        QVERIFY(l.contains(QChar(0x2026)));              // and something was cut
        QVERIFY(l.startsWith(QStringLiteral("logs-for"))); // head kept
        QVERIFY(l.size() <= kMaxTabPrefixChars + int(qstrlen("app.log")));
    }
}

void TestTabLabels::addressesThatNoSegmentCanSeparateKeepTheirSharedName()
{
    // Two accounts on one host reading one path. Nothing in the address's segments can
    // separate them, and inventing a marker would say something the address does not —
    // so both keep the shared name and the tooltip carries the full address.
    const QStringList labels = tabLabelsFor({QStringLiteral("ssh://alice@host-a/var/log/app.log"),
                                             QStringLiteral("ssh://bob@host-a/var/log/app.log")});
    QCOMPARE(labels, QStringList({QStringLiteral("app.log (host-a)"),
                                  QStringLiteral("app.log (host-a)")}));
}

QTEST_MAIN(TestTabLabels)
#include "tst_tablabels.moc"
