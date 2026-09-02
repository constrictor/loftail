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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "LogFileStore.h"
#include "LogSettings.h"
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
    void aUserlessAddressIsKeyedOnTheAccountItWillConnectAs();
    void displayHelpersFallBackToLocalBehavior();
    void everyAddressGetsANonEmptyNameAndNoNameIsAPath();
    void everyAddressGetsANonEmptyNameAndNoNameIsAPath_data();
    void anAddressThatDoesNotParseStillLosesItsPassword();
    void availabilityIsOptimisticForRemote();
    void presenceTellsAnAbsentLogFromAnUnreadableOne();
    void settingsKeyIsWorkingDirectoryIndependent();
    void theSettingsTreeRoundTripsARemotePath();
    void aWholePathPatternSeesTheAddressAsAPersonWouldTypeIt();
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

// An address with no user is keyed on the account the connect will actually use, and it
// is the SAME key before and after that connect has filled the account in.
//
// It was not. SshSession::authenticate() filled `user` from the local account and only
// then asked for target(), so an `ssh://host/path` was `host:22` to everything holding
// the parsed location and `me@host:22` to everything downstream of the connect. Both of
// the things that carry a remembered password across a restart look it up from the first
// side — MainWindow::primeRemoteCredentials() and, through indexOfTarget(), the password
// dialog's "Remember this password" box — so a password remembered for such a host was
// never used again, and the next launch asked for it.
void TestRemoteLocation::aUserlessAddressIsKeyedOnTheAccountItWillConnectAs()
{
    const auto bare = RemoteLocation::parse(QStringLiteral("ssh://web1/var/log/a.log"));
    QVERIFY(bare.has_value());

    QVERIFY(!bare->effectiveUser().isEmpty());
    QCOMPARE(bare->target(),
             QStringLiteral("%1@web1:22").arg(bare->effectiveUser()));

    // Spelling the same account out changes nothing, which is what makes the two sides
    // of a connect agree.
    const auto spelled =
        RemoteLocation::parse(QStringLiteral("ssh://%1@web1/var/log/a.log").arg(bare->effectiveUser()));
    QVERIFY(spelled.has_value());
    QCOMPARE(bare->target(), spelled->target());

    // The ADDRESS is untouched: the session file, the recent-files menu and the window
    // title keep what the user typed, because a synthesized user would be wrong for
    // anyone whose ~/.ssh/config names a different one.
    QCOMPARE(bare->toString(), QStringLiteral("ssh://web1:22/var/log/a.log"));
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

// An address with no file-name part still has to be CALLED something. It used to be
// called "" — QFileInfo("/var/log/").fileName() — and every consumer showed the gap
// instead of the log: "Cannot open : …" in the refusal strip with nothing before the
// colon, a waiting tab wearing its marker and nothing else, "loftail — " in the title
// bar, a blank clickable row in the recent-files menu.
//
// The second half of the claim is the one that is easy to undo: the fallback is a
// SEGMENT, never the raw address. prefixedLabelsFor() builds a recent-files entry as
// parent directories plus this string, which only stays unambiguous while the name is
// the tail of its own label — and a menu is as wide as its widest item.
//
// It holds for logSourceBareName() too, and there it is load-bearing in a second way:
// tabLabelsFor() GROUPS on the bare name to find the logs that would otherwise wear one
// name, and a key with a path in it groups nothing with anything. That is what the three
// REMOTE rows below are for — `ssh://h/var/log/` used to answer "/var/log/ (h)", because
// the remote branch fell back to the whole remote path where the local one falls back to
// its deepest segment, and no row here had ever asked.
void TestRemoteLocation::everyAddressGetsANonEmptyNameAndNoNameIsAPath_data()
{
    QTest::addColumn<QString>("address");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<QString>("bare");

    QTest::newRow("ordinary local") << "/var/log/app.log" << "app.log" << "app.log";
    // The deepest thing in the address that could be a name: the directory itself.
    QTest::newRow("local directory") << "/var/log/" << "log" << "log";
    QTest::newRow("relative directory") << "logs/" << "logs" << "logs";
    // Nothing left but the scheme, which at least says ssh from sftp.
    QTest::newRow("scheme only") << "ssh://" << "ssh" << "ssh";
    QTest::newRow("sftp scheme only") << "sftp://" << "sftp" << "sftp";
    QTest::newRow("scheme and slash") << "ssh:///" << "ssh" << "ssh";
    // A host but no path — RemoteLocation::isValid() wants both, so this does not parse.
    QTest::newRow("host but no path") << "ssh://web1" << "web1" << "web1";
    QTest::newRow("user and host, no path") << "ssh://deploy@web1" << "deploy@web1"
                                            << "deploy@web1";
    // Nothing in the address that could be a name at all.
    QTest::newRow("root") << "/" << "(unnamed)" << "(unnamed)";
    QTest::newRow("empty") << "" << "(unnamed)" << "(unnamed)";

    // A remote log, and the same fallbacks on the far side of the host.
    QTest::newRow("remote log") << "ssh://web1/var/log/app.log" << "app.log (web1)"
                                << "app.log";
    QTest::newRow("remote directory") << "ssh://web1/var/log/" << "log (web1)" << "log";
    QTest::newRow("remote root") << "ssh://web1/" << "(unnamed) (web1)" << "(unnamed)";

    // An archive brackets its container on exactly as a host does, and the bare name is
    // the log inside it either way.
    QTest::newRow("archive member") << "/srv/bundle.tar.gz/var/log/app.log"
                                    << "app.log (bundle.tar.gz)" << "app.log";
    // A bare compressed stream is the log the writer meant, named once and not twice.
    QTest::newRow("single stream") << "/srv/app.log.gz" << "app.log" << "app.log";
    // No member picked: the container IS what is being named, so the bare is its name.
    QTest::newRow("container, no member") << "/srv/bundle.zip" << "bundle.zip"
                                          << "bundle.zip";
}

void TestRemoteLocation::everyAddressGetsANonEmptyNameAndNoNameIsAPath()
{
    QFETCH(QString, address);
    QFETCH(QString, expected);
    QFETCH(QString, bare);

    const QString name = logSourceDisplayName(address);
    QCOMPARE(name, expected);
    QVERIFY(!name.isEmpty());
    QVERIFY(!name.contains(u'/'));

    const QString plain = logSourceBareName(address);
    QCOMPARE(plain, bare);
    QVERIFY(!plain.isEmpty());
    QVERIFY(!plain.contains(u'/'));
    // The two are one decision taken apart, never two: the display name is the bare name
    // with whatever says WHERE the log is bracketed onto it, so the bare is always what
    // the display name starts with.
    QVERIFY2(name.startsWith(plain), qPrintable(name + QLatin1String(" / ") + plain));
}

// The password rule was written for addresses that PARSE — parse() is where a URL
// password is dropped on the floor — and an address that does not parse never goes
// through it. `ssh://u:pw@h` has no path and `ssh://u:pw@` has no host, so both were
// shown back verbatim: once as the refusal's name half and once inside its reason.
void TestRemoteLocation::anAddressThatDoesNotParseStillLosesItsPassword()
{
    const QStringList unparseable = {
        QStringLiteral("ssh://deploy:hunter2@web1.example.com"), // no path
        QStringLiteral("ssh://deploy:hunter2@"),                 // no host either
        QStringLiteral("sftp://deploy:hunter2@web1"),
    };
    for (const QString &address : unparseable) {
        QVERIFY(!RemoteLocation::parse(address).has_value()); // the precondition
        QVERIFY2(!RemoteLocation::withoutPassword(address).contains(QStringLiteral("hunter2")),
                 qPrintable(address));
        QVERIFY2(!logSourceDisplayName(address).contains(QStringLiteral("hunter2")),
                 qPrintable(address));
        QVERIFY2(!logSourceBareName(address).contains(QStringLiteral("hunter2")),
                 qPrintable(address));
        QVERIFY2(!logSourceDisplayPath(address).contains(QStringLiteral("hunter2")),
                 qPrintable(address));
        // The user is kept: it is what says WHICH login was refused, and it is not a
        // secret. Only the password goes.
        QVERIFY2(RemoteLocation::withoutPassword(address).contains(QStringLiteral("deploy")),
                 qPrintable(address));
    }

    // And a string with nothing to take out comes back byte-identical, so nothing that
    // merely passes through this is rewritten.
    for (const QString &plain : {QStringLiteral("/var/log/app.log"),
                                 QStringLiteral("ssh://web1/var/log/app.log"),
                                 QStringLiteral("ssh://deploy@web1/var/log/app.log"),
                                 QStringLiteral("ssh://")}) {
        QCOMPARE(RemoteLocation::withoutPassword(plain), plain);
    }
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

void TestRemoteLocation::presenceTellsAnAbsentLogFromAnUnreadableOne()
{
    // exists() and isReadable() were one answer, and the conflation was user-visible: a
    // file whose mode is 000 was reported as one that "has not appeared yet", sending
    // the reader looking for a file they can see in their file manager. Both still WAIT
    // — a permission is granted as readily as a file is written — but each has to say
    // its own sentence (SPEC.md §3), and the archive layer needs the distinction for a
    // second reason: an absent container is worth retrying and an unreadable one is not.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString present = dir.filePath(QStringLiteral("there.log"));
    QFile f(present);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    QCOMPARE(logSourcePresence(present), LogPresence::Present);
    QCOMPARE(logSourcePresence(dir.filePath(QStringLiteral("gone.log"))), LogPresence::Absent);

    // Optimistic for remote, exactly as availability is, and therefore NEVER Unreadable:
    // that answer would cost a round trip, and this runs during session restore.
    QCOMPARE(logSourcePresence(QStringLiteral("ssh://web1/var/log/app.log")),
             LogPresence::Present);
    QCOMPARE(logSourcePresence(QStringLiteral("ssh://")), LogPresence::Absent);

#if !defined(Q_OS_WIN)
    QVERIFY(QFile::setPermissions(present, QFileDevice::WriteOwner));
    if (QFileInfo(present).isReadable()) {
        QVERIFY(QFile::setPermissions(present,
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QSKIP("running as root: a mode-000 file is still readable");
    }
    QCOMPARE(logSourcePresence(present), LogPresence::Unreadable);
    QVERIFY(!logSourceAvailable(present)); // still not openable, so still a wait
    QVERIFY(QFile::setPermissions(present,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner));
#endif
}

void TestRemoteLocation::settingsKeyIsWorkingDirectoryIndependent()
{
    // Regression: the key fell through to QFileInfo::absoluteFilePath() for a remote
    // URL, which prepended the working directory and collapsed the "//" — so a log's
    // remembered settings were lost whenever loftail was launched from a different
    // directory.
    const QString url = QStringLiteral("ssh://deploy@web1/var/log/app.log");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString before = QDir::currentPath();
    const QString keyHere = logSettingsKey(url);
    QVERIFY(QDir::setCurrent(dir.path()));
    const QString keyThere = logSettingsKey(url);
    QVERIFY(QDir::setCurrent(before));

    QCOMPARE(keyHere, keyThere);
    QCOMPARE(keyHere, RemoteLocation::normalize(url));
    QVERIFY(!keyHere.contains(QStringLiteral("ssh:/var")));
    QVERIFY(keyHere.startsWith(QStringLiteral("ssh://")));

    // Equivalent spellings share one node.
    QCOMPARE(logSettingsKey(QStringLiteral("sftp://deploy@web1:22/var/log/app.log")),
             keyHere);
}

void TestRemoteLocation::theSettingsTreeRoundTripsARemotePath()
{
    // Through the per-log pool (M21), which is where the file level lives now.
    QTemporaryDir configDir;
    QVERIFY(configDir.isValid());
    LogFileStore store(configDir.path());
    store.load();

    LogFileSettings s;
    s.address = QStringLiteral("ssh://deploy@web1/var/log/app.log");
    s.profile = LogProfile::builtIn();
    s.profile->format.pattern = QStringLiteral("%d{ISO8601} [%t] %-5p %c - %m%n");
    QVERIFY(store.save(s, LogProfile::builtIn()));

    // Reopened by a different spelling of the same file: still one record, found.
    const LogFileSettings hit = store.read(QStringLiteral("ssh://deploy@web1:22/var/log/app.log"));
    QVERIFY(hit.profile.has_value());
    QCOMPARE(hit.profile->format.pattern, s.profile->format.pattern);

    // A different remote file is not confused with it.
    QVERIFY(!store.read(QStringLiteral("ssh://deploy@web2/var/log/app.log")).saysSomething());
}

// REGRESSION. A remote address is a URL, so RemoteLocation::toString() percent-encodes
// everything that is not URL-safe — and logMatchTarget(fullPath) handed that encoded
// string to a file pattern while the file-NAME branch one line down read the decoded
// path. So a log on another machine whose path holds a space (or any non-ASCII
// character) could be claimed by a pattern naming its bare file name and by NOTHING
// else: `*/1/my app.log` matched nothing, `*my app.log` with "Match the whole path" on
// matched nothing, and only `*/1/my%20app.log` worked — which nothing on screen said,
// logSourceDisplayPath() having shown the encoded form too.
//
// The two now answer one string, which is the claim worth stating: what the pattern
// sees is what the dialog shows. The KEY is deliberately NOT in it — logSettingsKey()
// still answers the encoded normal form, or every record already stored would be
// re-keyed by this fix.
void TestRemoteLocation::aWholePathPatternSeesTheAddressAsAPersonWouldTypeIt()
{
    RemoteLocation loc;
    loc.user = QStringLiteral("deploy");
    loc.host = QStringLiteral("web1");
    loc.port = 22;
    loc.path = QStringLiteral("/1/df_log_vmsapp (1).txt");
    const QString address = loc.toString(); // exactly what OpenRemoteDialog hands over

    QVERIFY(address.contains(QStringLiteral("%20")));  // the address IS encoded
    QCOMPARE(logSettingsKey(address), address);        // and the key stays that way

    const QString typed = QStringLiteral("ssh://deploy@web1:22/1/df_log_vmsapp (1).txt");
    QCOMPARE(logMatchTarget(address, true), typed);
    QCOMPARE(logSourceDisplayPath(address), typed);    // the pattern sees what is shown

    LogSettingsTree tree;
    LogProfile root;
    root.format.pattern = QStringLiteral("ROOT");
    tree.setDefaults(root);

    LogPatternNode n;
    n.match = QStringLiteral("*/1/df_log_vmsapp (1).txt");
    n.matchFullPath = true;
    n.profile.format.pattern = QStringLiteral("BY-PATH");
    tree.addPattern(n);
    QCOMPARE(tree.inherited(logSettingsKey(address)).format.pattern,
             QStringLiteral("BY-PATH"));

    // Non-ASCII is the same defect and is fixed by the same line.
    loc.path = QStringLiteral("/logs/журнал.log");
    const QString cyrillic = loc.toString();
    QVERIFY(cyrillic.contains(QStringLiteral("%D0")));
    QCOMPARE(logMatchTarget(cyrillic, true),
             QStringLiteral("ssh://deploy@web1:22/logs/журнал.log"));

    // Nothing moved for an address that needed no encoding: toDisplayString() is
    // toString() term for term, so every pattern that works today goes on working.
    loc.path = QStringLiteral("/var/log/app.log");
    QCOMPARE(loc.toDisplayString(), loc.toString());
    QCOMPARE(logMatchTarget(loc.toString(), true), loc.toString());

    // An IPv6 literal keeps its brackets, or the answer is not an address either half
    // could parse back.
    RemoteLocation six;
    six.host = QStringLiteral("::1");
    six.port = 22;
    six.path = QStringLiteral("/var/log/my app.log");
    QCOMPARE(six.toDisplayString(), QStringLiteral("ssh://[::1]:22/var/log/my app.log"));

    // The file-NAME branch is untouched, and is what worked all along.
    QCOMPARE(logMatchTarget(address, false), QStringLiteral("df_log_vmsapp (1).txt"));
}

QTEST_APPLESS_MAIN(TestRemoteLocation)
#include "tst_remotelocation.moc"
