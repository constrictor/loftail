#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "ConfigLocation.h"
#include "RemoteLocation.h"

using namespace loftail;

// Where a log's config file is (SPEC.md §4). One address derived from another: the
// configured path placed on the filesystem the log is on, resolved against the log's own
// directory.
//
// THE HIGHEST-VALUE TEST IN THE FEATURE, because a wrong answer here does not show a
// wrong file — it WRITES one, at a path nobody asked for, possibly on somebody else's
// server. Every other part of the editor fails visibly; this one fails by succeeding
// somewhere else.
//
// UNGATED and core-only, for the reason ArchiveLocation's and RemoteLocation's own tests
// are: resolving, refusing and displaying a config path must be identical in a build with
// SSH or archive support and one without, or the two disagree about what a settings file
// means.
class TestConfigLocation : public QObject
{
    Q_OBJECT

private slots:
    void nothingConfiguredIsNotARefusal();
    void aRelativePathResolvesAgainstTheLogsOwnDirectory();
    void anAbsolutePathIsTakenAsItStands();
    void aRemoteLogsConfigStaysOnItsHost();
    void anArchivedLogsConfigSitsBesideTheContainer();
    void aPathLandingInsideAContainerIsRefused();
    void aConfigPathMayNotBeARemoteAddress();
    void aLogAddressThatDoesNotParseIsRefused();
    void everyResolvedAddressIsNamedAndIsNotAPath();
    void neitherHalfOfAnyAnswerEverCarriesAPassword();

private:
    // "/srv/x" is NOT absolute on Windows — it has no drive — so an expectation spelled
    // that way misses by a drive letter. Absolute paths are built from QDir::rootPath(),
    // which is already absolute on both, exactly as tst_archivelocation::abs() does.
    static QString root(const QString &tail) { return QDir::rootPath() + tail; }

    QTemporaryDir m_dir;
};

void TestConfigLocation::nothingConfiguredIsNotARefusal()
{
    // Unset is a THIRD state and not a refusal: it is what makes the menu item open a
    // file dialog. Folding it into Refused puts an error strip where a picker belongs.
    for (const QString &empty : {QString(), QStringLiteral("   ")}) {
        const auto a = resolveConfigAddress(root(QStringLiteral("var/log/app.log")), empty);
        QCOMPARE(a.state, ConfigAddress::State::Unset);
        QVERIFY(a.address.isEmpty());
        QVERIFY(a.reason.isEmpty());
    }
}

void TestConfigLocation::aRelativePathResolvesAgainstTheLogsOwnDirectory()
{
    // The whole reason the setting is worth having at the PATTERN level: one entry
    // resolves per log, each against its own directory.
    const auto a = resolveConfigAddress(root(QStringLiteral("srv/prod/logs/app.log")),
                                        QStringLiteral("../conf/log4cplus.properties"));
    QCOMPARE(a.state, ConfigAddress::State::Resolved);
    QCOMPARE(a.address, root(QStringLiteral("srv/prod/conf/log4cplus.properties")));
    QCOMPARE(a.baseDir, root(QStringLiteral("srv/prod/logs")));

    // The same setting, a different log: a different file. That is the feature, and it
    // is why Preferences has to show the resolved address for the log it is previewing.
    const auto b = resolveConfigAddress(root(QStringLiteral("srv/test/logs/app.log")),
                                        QStringLiteral("../conf/log4cplus.properties"));
    QCOMPARE(b.address, root(QStringLiteral("srv/test/conf/log4cplus.properties")));
    QVERIFY(a.address != b.address);

    // A bare name is the sibling case, and "./a/../b" cleans.
    QCOMPARE(resolveConfigAddress(root(QStringLiteral("var/log/app.log")),
                                  QStringLiteral("log4cplus.properties"))
                 .address,
             root(QStringLiteral("var/log/log4cplus.properties")));
    QCOMPARE(resolveConfigAddress(root(QStringLiteral("var/log/app.log")),
                                  QStringLiteral("./a/../b/x.properties"))
                 .address,
             root(QStringLiteral("var/log/b/x.properties")));
}

void TestConfigLocation::anAbsolutePathIsTakenAsItStands()
{
    const QString abs = root(QStringLiteral("etc/log4cplus.properties"));
    const auto a = resolveConfigAddress(root(QStringLiteral("var/log/app.log")), abs);
    QCOMPARE(a.state, ConfigAddress::State::Resolved);
    QCOMPARE(a.address, abs);
}

void TestConfigLocation::aRemoteLogsConfigStaysOnItsHost()
{
    // "The same device as the log" — the config for a log on another machine is on that
    // machine, and the transport is DERIVED rather than spelled again.
    const auto rel = resolveConfigAddress(QStringLiteral("ssh://host/var/log/app.log"),
                                          QStringLiteral("../conf/x.properties"));
    QCOMPARE(rel.state, ConfigAddress::State::Resolved);
    QCOMPARE(rel.address, QStringLiteral("ssh://host:22/var/conf/x.properties"));

    const auto abs = resolveConfigAddress(QStringLiteral("ssh://host/var/log/app.log"),
                                          QStringLiteral("/etc/x.properties"));
    QCOMPARE(abs.address, QStringLiteral("ssh://host:22/etc/x.properties"));

    // The user rides along, because it is part of which account's files these are.
    const auto user = resolveConfigAddress(QStringLiteral("ssh://deploy@host/var/log/app.log"),
                                           QStringLiteral("x.properties"));
    QCOMPARE(user.address, QStringLiteral("ssh://deploy@host:22/var/log/x.properties"));
}

void TestConfigLocation::anArchivedLogsConfigSitsBesideTheContainer()
{
    // The ruling: the base is the CONTAINER's own directory on the real filesystem.
    const QString log = root(QStringLiteral("srv/bundle.zip/var/log/app.log"));
    const auto a = resolveConfigAddress(log, QStringLiteral("conf/x.properties"));
    QCOMPARE(a.state, ConfigAddress::State::Resolved);
    QCOMPARE(a.address, root(QStringLiteral("srv/conf/x.properties")));

    // A remote container reduces once more: the host is kept, the container's directory
    // is the base. Getting this wrong by returning the container string would produce an
    // address with a `.zip` in the middle of it.
    const auto remote = resolveConfigAddress(
        QStringLiteral("ssh://host/srv/bundle.tar.gz/app.log"), QStringLiteral("conf/x.properties"));
    QCOMPARE(remote.state, ConfigAddress::State::Resolved);
    QCOMPARE(remote.address, QStringLiteral("ssh://host:22/srv/conf/x.properties"));

    // A SHORT remote member, which is its own case and not a duplicate of the one above.
    // ArchiveLocation::split() hands back the container in NORMAL FORM, port spelled
    // out — so `ssh://host/srv/b.zip/m` peels to the strictly LONGER
    // `ssh://host:22/srv/b.zip`. A peel guarded on "the container got shorter" refuses
    // the one peel this address needs and silently anchors inside the archive. Caught
    // exactly this way; the long-member row above passes with that bug in place.
    const auto shortMember = resolveConfigAddress(QStringLiteral("ssh://host/srv/b.zip/m"),
                                                  QStringLiteral("x.properties"));
    QCOMPARE(shortMember.state, ConfigAddress::State::Resolved);
    QCOMPARE(shortMember.address, QStringLiteral("ssh://host:22/srv/x.properties"));

    // A bare single-stream log is a container whose member is implied, so its own
    // directory is the base and nothing surprising happens.
    QCOMPARE(resolveConfigAddress(root(QStringLiteral("var/log/app.log.gz")),
                                  QStringLiteral("x.properties"))
                 .address,
             root(QStringLiteral("var/log/x.properties")));
}

void TestConfigLocation::aPathLandingInsideAContainerIsRefused()
{
    // Refused in words rather than opened read-only: a member cannot be written without
    // rebuilding the container, and a second kind of editor tab is a second thing to
    // explain. The reason must NAME the container, or the user cannot act on it.
    const QString log = root(QStringLiteral("srv/logs/app.log"));
    const auto a = resolveConfigAddress(log, QStringLiteral("../bundle.zip/etc/x.properties"));
    QCOMPARE(a.state, ConfigAddress::State::Refused);
    QVERIFY2(a.reason.contains(QStringLiteral("bundle.zip")), qPrintable(a.reason));
    QVERIFY(a.address.isEmpty());

    // The remote twin is as much inside a container as the local one. Skipping the check
    // for remote opens an editor onto a member the save path cannot write.
    const auto remote = resolveConfigAddress(QStringLiteral("ssh://host/srv/logs/app.log"),
                                             QStringLiteral("../bundle.zip/etc/x.properties"));
    QCOMPARE(remote.state, ConfigAddress::State::Refused);
}

void TestConfigLocation::aConfigPathMayNotBeARemoteAddress()
{
    // A second URL would name a second host: a second credential prompt, and at the
    // pattern level one host named for every log the pattern matches.
    for (const QString &url : {QStringLiteral("ssh://other/etc/x.properties"),
                               QStringLiteral("sftp://other/etc/x.properties")}) {
        const auto a = resolveConfigAddress(root(QStringLiteral("var/log/app.log")), url);
        QCOMPARE(a.state, ConfigAddress::State::Refused);
        QVERIFY(a.address.isEmpty());
        QVERIFY(!a.reason.isEmpty());
    }
}

void TestConfigLocation::aLogAddressThatDoesNotParseIsRefused()
{
    // No host, so parse() fails. This is the branch that never went through parse()'s
    // own password-dropping, which is what the next case is about.
    const auto a = resolveConfigAddress(QStringLiteral("ssh://"), QStringLiteral("x.properties"));
    QCOMPARE(a.state, ConfigAddress::State::Refused);

    const auto empty = resolveConfigAddress(QString(), QStringLiteral("x.properties"));
    QCOMPARE(empty.state, ConfigAddress::State::Refused);
    QVERIFY(!empty.reason.isEmpty());
}

void TestConfigLocation::everyResolvedAddressIsNamedAndIsNotAPath()
{
    // A resolved address is shown — on a tab, in a title, in a refusal — so it owes the
    // same two guarantees every log address does. In the shape of
    // tst_remotelocation::everyAddressGetsANonEmptyNameAndNoNameIsAPath, whose own table
    // had no remote row for a milestone, which is exactly how that gap survived.
    const QStringList logs = {
        root(QStringLiteral("var/log/app.log")),
        root(QStringLiteral("var/log/app.log.gz")),
        root(QStringLiteral("srv/bundle.zip/var/log/app.log")),
        QStringLiteral("ssh://host/var/log/app.log"),
        QStringLiteral("ssh://deploy@host:2222/var/log/app.log"),
        QStringLiteral("ssh://host/srv/bundle.tar.gz/app.log"),
    };
    for (const QString &log : logs) {
        for (const QString &configured :
             {QStringLiteral("x.properties"), QStringLiteral("../conf/x.properties")}) {
            const auto a = resolveConfigAddress(log, configured);
            QVERIFY2(a.state == ConfigAddress::State::Resolved,
                     qPrintable(log + QStringLiteral(" + ") + configured));
            const QString name = logSourceDisplayName(a.address);
            QVERIFY2(!name.isEmpty(), qPrintable(a.address));
            // A name is a SEGMENT, never a path — the guarantee the tab label and the
            // recent-files rule are both built on.
            QVERIFY2(!name.contains(u'/'), qPrintable(name));
        }
    }
}

void TestConfigLocation::neitherHalfOfAnyAnswerEverCarriesAPassword()
{
    // The rule that broke twice before: a password stayed hidden only because the
    // userinfo happened to be the part that got dropped. Assert it over BOTH halves —
    // the address and the reason — and over an address that does NOT parse, which is
    // the case that never reaches parse()'s own dropping.
    const QStringList logs = {
        QStringLiteral("ssh://deploy:hunter2@web1/var/log/app.log"),
        QStringLiteral("ssh://deploy:hunter2@web1"),   // no path: does not parse
        QStringLiteral("ssh://deploy:hunter2@"),       // no host: does not parse
    };
    for (const QString &log : logs) {
        for (const QString &configured : {QStringLiteral("x.properties"),
                                          QStringLiteral("ssh://deploy:hunter2@w/x")}) {
            const auto a = resolveConfigAddress(log, configured);
            QVERIFY2(!a.address.contains(QStringLiteral("hunter2")), qPrintable(a.address));
            QVERIFY2(!a.reason.contains(QStringLiteral("hunter2")), qPrintable(a.reason));
            QVERIFY2(!a.baseDir.contains(QStringLiteral("hunter2")), qPrintable(a.baseDir));
        }
    }
}

QTEST_APPLESS_MAIN(TestConfigLocation)
#include "tst_configlocation.moc"
