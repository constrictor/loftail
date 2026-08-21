#include <QtTest>

#include "TabLabels.h"

using namespace loftail;

// What each open log is CALLED on its tab (SPEC.md §5a, ARCHITECTURE.md §12.4), and what
// a recent-files entry is called, which is a different rule kept in the same file.
//
// The rule is a statement about a SET — a log's label depends on which other logs are
// open — so it is a free function over a list of addresses, and this is the test that
// drives it directly. What a real window then does with the labels, and that closing a
// log shortens the survivor again, is tst_multidoc's.
//
// Every kind of address the application accepts appears here, because each offers the
// three axes differently: a local path has only directories, a remote log has a host, an
// archived one has a container plus directories on both sides of it.
class TestTabLabels : public QObject
{
    Q_OBJECT

private slots:
    void aLogWhoseNameNoOneSharesCarriesNoQualifierAtAll();
    void aLogWhoseNameNoOneSharesCarriesNoQualifierAtAll_data();

    void twoLogsWithOneNameAreToldApartByTheirDeviceFirst();
    void twoLogsWithOneNameAreToldApartByTheirDeviceFirst_data();

    void aDeviceEveryLogSharesIsNotSpelledOut();
    void aThirdLogOnAnotherHostBringsTheDeviceBack();

    void anArchiveContainerQualifiesOnlyWhenItIsWhatDiffers();
    void anArchiveContainerQualifiesOnlyWhenItIsWhatDiffers_data();

    void aContainerWithNoMemberPickedIsNotNamedTwice();
    void twoCompressedStreamsOfOneLogAreToldApartByTheirSuffix();

    void thePathRunDropsWhatEveryMemberSharesAtBothEnds();
    void thePathRunDropsWhatEveryMemberSharesAtBothEnds_data();

    void aMemberWhosePathIsWhollyCommonShowsNoRunAtAll();
    void membersOfDifferentDepthsStripAgainstWhatTheyActuallyShare();
    void aRunAboveAnArchiveContinuesIntoIt();

    void addressesThatNothingCanSeparateKeepTheirSharedName();
    void anEnormousPathRunIsElidedInTheMiddleAndTheLogsOwnNameIsKeptWhole();
    void aQualifierThatWouldElideToOneStringBuysNothingAndIsNotSpent();
    void aNameThatCarriesItsOwnParenthesesIsNotSpecialCased();
    void anAddressThatDoesNotParseGetsALabelAndNeverAPassword();
    void theDegenerateAddressesStillGetANonEmptyLabel();
    void theOrderOutIsTheOrderIn();

    void theRecentMenuKeepsTheOlderPrefixRule();
    void theRecentMenuKeepsTheOlderPrefixRule_data();
    void aRecentEntryWithAnEnormousDirectoryIsElidedInItsMiddle();
};

// One row per case: the addresses as they would be open, and the label each should
// wear, in the same order.
static void run(const QStringList &addresses, const QStringList &expected)
{
    QCOMPARE(tabLabelsFor(addresses), expected);
}

// A bracket is what a log grows to be told from the tabs beside it, so a log with
// nothing beside it answering to its name never grows one — a remote or archived log
// included, which is the visible change from the rule this replaced.
void TestTabLabels::aLogWhoseNameNoOneSharesCarriesNoQualifierAtAll_data()
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

    // The host is a way of telling two logs apart, not a decoration on every remote one.
    QTest::newRow("a remote log on its own")
        << QStringList{QStringLiteral("ssh://host-a/var/log/app.log")}
        << QStringList{QStringLiteral("app.log")};

    QTest::newRow("an archived log on its own")
        << QStringList{QStringLiteral("/srv/bundle.zip/var/log/db.log")}
        << QStringList{QStringLiteral("db.log")};
}

void TestTabLabels::aLogWhoseNameNoOneSharesCarriesNoQualifierAtAll()
{
    QFETCH(QStringList, addresses);
    QFETCH(QStringList, expected);
    run(addresses, expected);
}

void TestTabLabels::twoLogsWithOneNameAreToldApartByTheirDeviceFirst_data()
{
    QTest::addColumn<QStringList>("addresses");
    QTest::addColumn<QStringList>("expected");

    // Which machine a log is on is the most prominent thing about it, and it is the
    // common case for anyone tailing one service across hosts.
    QTest::newRow("one path, two hosts")
        << QStringList{QStringLiteral("ssh://host-a/var/log/app.log"),
                       QStringLiteral("ssh://host-b/var/log/app.log")}
        << QStringList{QStringLiteral("app.log (host-a)"), QStringLiteral("app.log (host-b)")};

    // The directories differ too, and are not spent: the device already settled it.
    QTest::newRow("the device is enough even where the path differs as well")
        << QStringList{QStringLiteral("ssh://host-a/var/log/svc-a/app.log"),
                       QStringLiteral("ssh://host-b/srv/other/svc-b/app.log")}
        << QStringList{QStringLiteral("app.log (host-a)"), QStringLiteral("app.log (host-b)")};

    // A local log has no device, and an absent component is dropped rather than shown
    // as an empty bracket.
    QTest::newRow("remote against local")
        << QStringList{QStringLiteral("ssh://host-a/var/log/app.log"),
                       QStringLiteral("/var/log/app.log")}
        << QStringList{QStringLiteral("app.log (host-a)"), QStringLiteral("app.log")};
}

void TestTabLabels::twoLogsWithOneNameAreToldApartByTheirDeviceFirst()
{
    QFETCH(QStringList, addresses);
    QFETCH(QStringList, expected);
    run(addresses, expected);
}

// An axis is spent only where it BUYS a distinction. A host every log in the group is on
// tells none of them apart, so it is left out and the directories do the work — which is
// the whole of "if both files are on the same device, find the path element".
void TestTabLabels::aDeviceEveryLogSharesIsNotSpelledOut()
{
    run({QStringLiteral("ssh://host-a/var/log/svc-a/app.log"),
         QStringLiteral("ssh://host-a/var/log/svc-b/app.log")},
        {QStringLiteral("app.log (svc-a)"), QStringLiteral("app.log (svc-b)")});
}

// ...and it comes back the moment it starts telling something apart. The two components
// then read in priority order inside one parenthesis: device, then path.
void TestTabLabels::aThirdLogOnAnotherHostBringsTheDeviceBack()
{
    run({QStringLiteral("ssh://host-a/var/log/svc-a/app.log"),
         QStringLiteral("ssh://host-a/var/log/svc-b/app.log"),
         QStringLiteral("ssh://host-b/var/log/app.log")},
        {QStringLiteral("app.log (host-a, svc-a)"), QStringLiteral("app.log (host-a, svc-b)"),
         QStringLiteral("app.log (host-b)")});
}

void TestTabLabels::anArchiveContainerQualifiesOnlyWhenItIsWhatDiffers_data()
{
    QTest::addColumn<QStringList>("addresses");
    QTest::addColumn<QStringList>("expected");

    // The container is the second axis, offered after the device and before any
    // directory — it says WHERE the log is in the same way a host does.
    QTest::newRow("two containers")
        << QStringList{QStringLiteral("/srv/a.tar.gz/var/log/app.log"),
                       QStringLiteral("/srv/b.tar.gz/var/log/app.log")}
        << QStringList{QStringLiteral("app.log (a.tar.gz)"), QStringLiteral("app.log (b.tar.gz)")};

    // One container, two members of one name: the container is common, so it says
    // nothing and the directories inside it do.
    QTest::newRow("one container, two members")
        << QStringList{QStringLiteral("/srv/bundle.tar.gz/svc-a/app.log"),
                       QStringLiteral("/srv/bundle.tar.gz/svc-b/app.log")}
        << QStringList{QStringLiteral("app.log (svc-a)"), QStringLiteral("app.log (svc-b)")};

    // Two identically-named containers holding one member path: the directories inside
    // are common as well, and what is left is where the containers sit.
    QTest::newRow("two containers with one name")
        << QStringList{QStringLiteral("/srv/a/bundle.tar.gz/log/app.log"),
                       QStringLiteral("/srv/b/bundle.tar.gz/log/app.log")}
        << QStringList{QStringLiteral("app.log (a)"), QStringLiteral("app.log (b)")};
}

void TestTabLabels::anArchiveContainerQualifiesOnlyWhenItIsWhatDiffers()
{
    QFETCH(QStringList, addresses);
    QFETCH(QStringList, expected);
    run(addresses, expected);
}

// A container with no member picked is named for the container itself, so putting the
// container on the bracket as well would read "bundle.zip (bundle.zip)".
void TestTabLabels::aContainerWithNoMemberPickedIsNotNamedTwice()
{
    run({QStringLiteral("/srv/a/bundle.zip"), QStringLiteral("/srv/b/bundle.zip")},
        {QStringLiteral("bundle.zip (a)"), QStringLiteral("bundle.zip (b)")});
}

// The one case where naming the container beside the log's own name is right: a bare
// compressed stream is shown as the log the writer meant, so two compressions of one log
// answer to one name and only the suffix can separate them.
void TestTabLabels::twoCompressedStreamsOfOneLogAreToldApartByTheirSuffix()
{
    run({QStringLiteral("/srv/app.log.gz"), QStringLiteral("/srv/app.log.bz2")},
        {QStringLiteral("app.log (app.log.gz)"), QStringLiteral("app.log (app.log.bz2)")});

    // And where the suffix is shared it says nothing, exactly as any other container.
    run({QStringLiteral("/srv/svc-a/app.log.gz"), QStringLiteral("/srv/svc-b/app.log.gz")},
        {QStringLiteral("app.log (svc-a)"), QStringLiteral("app.log (svc-b)")});
}

void TestTabLabels::thePathRunDropsWhatEveryMemberSharesAtBothEnds_data()
{
    QTest::addColumn<QStringList>("addresses");
    QTest::addColumn<QStringList>("expected");

    // The old rule grew from the file outwards and carried `current` on both labels,
    // where it tells nothing apart. Both ends are stripped now.
    QTest::newRow("a common tail as well as a common root")
        << QStringList{QStringLiteral("/srv/prod/current/app.log"),
                       QStringLiteral("/srv/test/current/app.log")}
        << QStringList{QStringLiteral("app.log (prod)"), QStringLiteral("app.log (test)")};

    // What is left may be more than one segment, and all of it is shown.
    QTest::newRow("two segments differ between the common ends")
        << QStringList{QStringLiteral("/srv/prod/nginx/logs/app.log"),
                       QStringLiteral("/srv/test/apache/logs/app.log")}
        << QStringList{QStringLiteral("app.log (prod/nginx)"),
                       QStringLiteral("app.log (test/apache)")};

    // Three logs, cut against what all THREE share rather than pairwise.
    QTest::newRow("three logs")
        << QStringList{QStringLiteral("/srv/a/current/app.log"),
                       QStringLiteral("/srv/b/current/app.log"),
                       QStringLiteral("/srv/c/old/app.log")}
        << QStringList{QStringLiteral("app.log (a/current)"), QStringLiteral("app.log (b/current)"),
                       QStringLiteral("app.log (c/old)")};
}

void TestTabLabels::thePathRunDropsWhatEveryMemberSharesAtBothEnds()
{
    QFETCH(QStringList, addresses);
    QFETCH(QStringList, expected);
    run(addresses, expected);
}

// A log at the root has no directory of its own to show, and shows none rather than an
// empty bracket. The one beside it shows what it has.
void TestTabLabels::aMemberWhosePathIsWhollyCommonShowsNoRunAtAll()
{
    run({QStringLiteral("/app.log"), QStringLiteral("/svc-b/app.log")},
        {QStringLiteral("app.log"), QStringLiteral("app.log (svc-b)")});
}

// Members of different depths are aligned from the root for the head and from the file
// for the tail, and the two are clamped against each other so a member consumed by one
// end cannot be re-cut by the other.
void TestTabLabels::membersOfDifferentDepthsStripAgainstWhatTheyActuallyShare()
{
    run({QStringLiteral("/srv/prod/app.log"), QStringLiteral("/srv/prod/deep/nested/app.log")},
        {QStringLiteral("app.log"), QStringLiteral("app.log (deep/nested)")});

    run({QStringLiteral("/srv/prod/logs/app.log"), QStringLiteral("/srv/logs/app.log")},
        {QStringLiteral("app.log (prod)"), QStringLiteral("app.log")});
}

// An archived log has two directory spaces — above the container and inside it — and
// they are stripped SEPARATELY before being joined. Concatenating first would hide the
// boundary from the stripper, and a run could splice a directory outside one container
// onto one inside another.
void TestTabLabels::aRunAboveAnArchiveContinuesIntoIt()
{
    run({QStringLiteral("/srv/a/bundle.tar.gz/svc-a/app.log"),
         QStringLiteral("/srv/b/bundle.tar.gz/svc-b/app.log")},
        {QStringLiteral("app.log (a/svc-a)"), QStringLiteral("app.log (b/svc-b)")});
}

// Two accounts on one host reading one path, and two ports on one host: nothing in the
// address's axes can separate them — displayHost() carries neither user nor port — and
// inventing a marker would say something the address does not. Both keep the shared name
// and the tooltip carries the full address.
void TestTabLabels::addressesThatNothingCanSeparateKeepTheirSharedName()
{
    run({QStringLiteral("ssh://alice@host-a/var/log/app.log"),
         QStringLiteral("ssh://bob@host-a/var/log/app.log")},
        {QStringLiteral("app.log"), QStringLiteral("app.log")});

    run({QStringLiteral("ssh://host-a:2222/var/log/app.log"),
         QStringLiteral("ssh://host-a/var/log/app.log")},
        {QStringLiteral("app.log"), QStringLiteral("app.log")});
}

void TestTabLabels::anEnormousPathRunIsElidedInTheMiddleAndTheLogsOwnNameIsKeptWhole()
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
    QVERIFY(labels.at(0).endsWith(QStringLiteral("1)"))); // the tail, where the telling-apart is
    QVERIFY(labels.at(1).endsWith(QStringLiteral("2)")));
    for (const QString &l : labels) {
        QVERIFY(l.startsWith(QStringLiteral("app.log (logs-for"))); // the name whole, the head kept
        QVERIFY(l.contains(QChar(0x2026)));                         // and something was cut
        // The name itself is never elided here; only the run is bounded, plus " (" and ")".
        QVERIFY(l.size() <= int(qstrlen("app.log")) + 3 + kMaxTabQualifierChars);
    }
}

// The distinctness test is on the label AS SHOWN. Two runs that differ only in the middle
// elide to one string, so the path axis buys nothing and is not spent — both logs keep
// the shared name rather than wearing one identical bracket that explains nothing.
void TestTabLabels::aQualifierThatWouldElideToOneStringBuysNothingAndIsNotSpent()
{
    const QString head = QStringLiteral("aaaaaaaaaaaa");
    const QString tail = QStringLiteral("zzzzzzzzzzzz");
    const QString a = QStringLiteral("/srv/") + head + QStringLiteral("AAAAAAAAAAAAAAAAAAAAAAAAAA")
        + tail + QStringLiteral("/app.log");
    const QString b = QStringLiteral("/srv/") + head + QStringLiteral("BBBBBBBBBBBBBBBBBBBBBBBBBB")
        + tail + QStringLiteral("/app.log");

    run({a, b}, {QStringLiteral("app.log"), QStringLiteral("app.log")});
}

// A log whose own name carries brackets is not special-cased, and the price is recorded
// here rather than discovered: a bracket is a SUFFIX now, so unlike the prefix rule this
// replaced there is no structural guarantee that a qualified label cannot equal some
// other log's plain name. It takes a file literally named "app.log (host-a)" beside a log
// on a host called host-a; the tooltip is what tells those two apart, exactly as it does
// for the two accounts above.
void TestTabLabels::aNameThatCarriesItsOwnParenthesesIsNotSpecialCased()
{
    const QStringList labels = tabLabelsFor({QStringLiteral("/var/log/app.log (host-a)"),
                                             QStringLiteral("ssh://host-a/var/log/app.log"),
                                             QStringLiteral("ssh://host-b/var/log/app.log")});
    QCOMPARE(labels, QStringList({QStringLiteral("app.log (host-a)"),
                                  QStringLiteral("app.log (host-a)"),
                                  QStringLiteral("app.log (host-b)")}));
}

// The old rule spent a BOUNDED number of parent segments, and an unparseable remote
// address kept its password out of sight only because the userinfo happened to be the
// leaf that was dropped. This rule shows the whole of what is left, so the password is
// taken out deliberately instead.
void TestTabLabels::anAddressThatDoesNotParseGetsALabelAndNeverAPassword()
{
    const QStringList addresses = {
        QStringLiteral("ssh://deploy:hunter2@web1"),                 // no path: does not parse
        QStringLiteral("ssh://deploy:hunter2@web1"),                 // ...twice, so it is a group
        QStringLiteral("ssh://deploy:hunter2@web1/var/log/app.log"), // parses; parse() drops it
        QStringLiteral("ssh://deploy:hunter2@/x"),                   // no host either
    };
    const QStringList labels = tabLabelsFor(addresses);
    QCOMPARE(labels.size(), addresses.size());
    for (const QString &l : labels) {
        QVERIFY2(!l.contains(QStringLiteral("hunter2")), qPrintable(l));
        QVERIFY2(!l.isEmpty(), qPrintable(l));
    }
    // The user is kept: it is not a secret, and it is what says which login this is.
    QVERIFY(labels.at(0).contains(QStringLiteral("deploy")));
}

void TestTabLabels::theDegenerateAddressesStillGetANonEmptyLabel()
{
    for (const QString &odd : {QStringLiteral("/"), QString(), QStringLiteral("ssh://")}) {
        const QStringList one = tabLabelsFor({odd});
        QCOMPARE(one.size(), 1);
        QVERIFY2(!one.first().isEmpty(), qPrintable(odd));
    }
    // Together, where two of them share a name and have no axis at all to spend.
    const QStringList all = tabLabelsFor({QStringLiteral("/"), QString(), QStringLiteral("ssh://")});
    QCOMPARE(all.size(), 3);
    for (const QString &l : all)
        QVERIFY(!l.isEmpty());
}

// The order out is the order in: MainWindow maps these onto its own contexts by position.
void TestTabLabels::theOrderOutIsTheOrderIn()
{
    run({QStringLiteral("/var/log/svc-b/app.log"), QStringLiteral("/etc/other.log"),
         QStringLiteral("/var/log/svc-a/app.log")},
        {QStringLiteral("app.log (svc-b)"), QStringLiteral("other.log"),
         QStringLiteral("app.log (svc-a)")});
}

// --- The other rule in the file ---------------------------------------------

// File > Open Recent keeps the older spelling: the log's own name as
// logSourceDisplayName() writes it — host or container already bracketed on — grown by
// the nearest parent directories that differ. An entry is read against the other nine
// rather than against the logs open beside it, and a path is what a person recognises a
// remembered file by.
void TestTabLabels::theRecentMenuKeepsTheOlderPrefixRule_data()
{
    QTest::addColumn<QStringList>("addresses");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("one entry")
        << QStringList{QStringLiteral("/var/log/app.log")}
        << QStringList{QStringLiteral("app.log")};

    QTest::newRow("one service per directory")
        << QStringList{QStringLiteral("/var/log/svc-a/app.log"),
                       QStringLiteral("/var/log/svc-b/app.log")}
        << QStringList{QStringLiteral("svc-a/app.log"), QStringLiteral("svc-b/app.log")};

    // The nearest parent is `current` for both, so one segment settles nothing — and
    // unlike the tab rule, the common segment is carried rather than stripped.
    QTest::newRow("two entries sharing two segments")
        << QStringList{QStringLiteral("/srv/prod/current/app.log"),
                       QStringLiteral("/srv/test/current/app.log")}
        << QStringList{QStringLiteral("prod/current/app.log"),
                       QStringLiteral("test/current/app.log")};

    // The host is in the name here, always, so two hosts never collide and grow nothing.
    QTest::newRow("one path, two hosts")
        << QStringList{QStringLiteral("ssh://host-a/var/log/app.log"),
                       QStringLiteral("ssh://host-b/var/log/app.log")}
        << QStringList{QStringLiteral("app.log (host-a)"), QStringLiteral("app.log (host-b)")};

    QTest::newRow("one host, two directories")
        << QStringList{QStringLiteral("ssh://host-a/var/log/svc-a/app.log"),
                       QStringLiteral("ssh://host-a/var/log/svc-b/app.log")}
        << QStringList{QStringLiteral("svc-a/app.log (host-a)"),
                       QStringLiteral("svc-b/app.log (host-a)")};

    // The container is bracketed onto the name exactly as a host is.
    QTest::newRow("two containers")
        << QStringList{QStringLiteral("/srv/a.tar.gz/var/log/app.log"),
                       QStringLiteral("/srv/b.tar.gz/var/log/app.log")}
        << QStringList{QStringLiteral("app.log (a.tar.gz)"), QStringLiteral("app.log (b.tar.gz)")};

    QTest::newRow("one container, two members")
        << QStringList{QStringLiteral("/srv/bundle.tar.gz/svc-a/app.log"),
                       QStringLiteral("/srv/bundle.tar.gz/svc-b/app.log")}
        << QStringList{QStringLiteral("svc-a/app.log (bundle.tar.gz)"),
                       QStringLiteral("svc-b/app.log (bundle.tar.gz)")};

    // An address with no file-name part gets its deepest segment as a name rather than
    // "", which is what makes these two GROUP and grow `a/`/`b/` instead of each
    // carrying a whole path. Every base stays free of separators, which is the property
    // this rule's prefix arithmetic rests on.
    QTest::newRow("no file-name part")
        << QStringList{QStringLiteral("/srv/a/logs/"), QStringLiteral("/srv/b/logs/")}
        << QStringList{QStringLiteral("a/logs"), QStringLiteral("b/logs")};

    // Two accounts on one host: no depth separates them, so neither grows anything.
    QTest::newRow("nothing can separate them")
        << QStringList{QStringLiteral("ssh://alice@host-a/var/log/app.log"),
                       QStringLiteral("ssh://bob@host-a/var/log/app.log")}
        << QStringList{QStringLiteral("app.log (host-a)"), QStringLiteral("app.log (host-a)")};
}

void TestTabLabels::theRecentMenuKeepsTheOlderPrefixRule()
{
    QFETCH(QStringList, addresses);
    QFETCH(QStringList, expected);
    QCOMPARE(prefixedLabelsFor(addresses), expected);
}

void TestTabLabels::aRecentEntryWithAnEnormousDirectoryIsElidedInItsMiddle()
{
    // The two directories differ in their last character, so an elision that kept only
    // the head would take away the very thing the segment was added for. Driven at the
    // tab budget rather than the menu's, which this directory would just fit inside.
    const QString a = QStringLiteral("/srv/logs-for-the-alpha-deployment-eu-west-1/app.log");
    const QString b = QStringLiteral("/srv/logs-for-the-alpha-deployment-eu-west-2/app.log");
    const QStringList labels = prefixedLabelsFor({a, b}, kMaxTabQualifierChars);

    QCOMPARE(labels.size(), 2);
    QVERIFY2(labels.at(0) != labels.at(1), qPrintable(labels.join(u' ')));
    for (const QString &l : labels) {
        QVERIFY(l.endsWith(QStringLiteral("/app.log"))); // the name itself, never cut
        QVERIFY(l.contains(QChar(0x2026)));              // and something was cut
        QVERIFY(l.startsWith(QStringLiteral("logs-for"))); // head kept
        QVERIFY(l.size() <= kMaxTabQualifierChars + int(qstrlen("app.log")));
    }
}

QTEST_MAIN(TestTabLabels)

#include "tst_tablabels.moc"
