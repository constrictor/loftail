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
#include "LogSource.h"
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
    void anAddressHoldingANoncharacterIsRefusedRatherThanRespelled();
    void normalizingAnAddressTwiceIsNormalizingItOnce();
    void normalizingAnAddressTwiceIsNormalizingItOnce_data();
    void aPortOutsideTheTcpRangeIsARefusedAddressAndNotAFailedConnect();
    void anAddressHoldingANulIsRefusedRatherThanRekeyed();
    void availabilityIsOptimisticForRemote();
    void presenceTellsAnAbsentLogFromAnUnreadableOne();
    void presenceTellsAMissingFolderAndAFolderFromBothOfThem();
    void settingsKeyIsWorkingDirectoryIndependent();
    void theSettingsTreeRoundTripsARemotePath();
    void aWholePathPatternSeesTheAddressAsAPersonWouldTypeIt();

    // M27 — the second transport, asserted in THIS file rather than one of its own,
    // because every claim here is a claim about both schemes and the one time this file
    // had a gap it was exactly that: everyAddressGetsANonEmptyNameAndNoNameIsAPath had no
    // remote row, so a display name with two separators in it contradicted its own rule
    // for as long as the rule had existed.
    void aSerialAddressIsRemoteAndSaysWhichTransport();
    void aSerialAddressSpellsNoPortAndAnExplicitOneIsRefused();
    void aByIdDeviceKeepsItsCaseAndItsUnderscores();
    void anInvalidPercentEscapeOnASerialAddressIsRefused();
    void aDeviceNameOutsideItsCharacterSetIsRefused();
    void aSerialTargetCannotBeConfusedWithAHost();
    void aSerialAddressNamesNoDefaultUser();
    void aSerialAddressNeverEmitsItsPassword();
    void aSerialArchiveAddressSplits();
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

void TestRemoteLocation::aSerialAddressIsRemoteAndSaysWhichTransport()
{
    // isRemote() means "read through a spool", not "on the network" — which is what makes
    // logPathIsSpooled() and every path-shaped helper work for a device with no further
    // edits, and what makes this the highest-blast-radius line in the milestone.
    QVERIFY(RemoteLocation::isRemote(QStringLiteral("serial://ttyUSB0/var/log/app.log")));
    QVERIFY(RemoteLocation::isRemote(QStringLiteral("SERIAL://ttyUSB0/var/log/app.log")));
    QVERIFY(logPathIsSpooled(QStringLiteral("serial://ttyUSB0/var/log/app.log")));

    // And transportOf() is what a caller that genuinely means SSH asks instead, because
    // otherwise it would take a device for a host name.
    QCOMPARE(RemoteLocation::transportOf(QStringLiteral("serial://ttyUSB0/a.log")),
             RemoteLocation::Transport::Serial);
    QCOMPARE(RemoteLocation::transportOf(QStringLiteral("ssh://h/a.log")),
             RemoteLocation::Transport::Ssh);
    QCOMPARE(RemoteLocation::transportOf(QStringLiteral("sftp://h/a.log")),
             RemoteLocation::Transport::Ssh);
    QVERIFY(!RemoteLocation::transportOf(QStringLiteral("/var/log/a.log")).has_value());

    const auto loc = RemoteLocation::parse(QStringLiteral("serial://root@ttyUSB0/var/log/app.log"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->transport, RemoteLocation::Transport::Serial);
    QCOMPARE(loc->user, QStringLiteral("root"));
    QCOMPARE(loc->host, QStringLiteral("ttyUSB0"));
    QCOMPARE(loc->path, QStringLiteral("/var/log/app.log"));

    // The display name brackets the DEVICE exactly as it brackets a host, and it is still
    // a segment rather than a path — the two properties this file is about.
    QCOMPARE(logSourceDisplayName(QStringLiteral("serial://ttyUSB0/var/log/app.log")),
             QStringLiteral("app.log (ttyUSB0)"));
    QCOMPARE(logSourceBareName(QStringLiteral("serial://ttyUSB0/var/log/app.log")),
             QStringLiteral("app.log"));
    QVERIFY(!logSourceBareName(QStringLiteral("serial://ttyUSB0/var/log/app.log"))
                 .contains(u'/'));

    // An address with no path names a device and no log on it, which is not a log address.
    QVERIFY(!RemoteLocation::parse(QStringLiteral("serial://ttyUSB0")).has_value());
    QVERIFY(!logPathIsWellFormed(QStringLiteral("serial://ttyUSB0")));
    // And one with no device at all.
    QVERIFY(!RemoteLocation::parse(QStringLiteral("serial:///var/log/app.log")).has_value());
    // Yet the name is STILL never empty and still a segment, which is the fallback rule —
    // the scheme word is what a bare `serial://` is called.
    QVERIFY(!logSourceDisplayName(QStringLiteral("serial://")).isEmpty());
    QVERIFY(!logSourceDisplayName(QStringLiteral("serial://")).contains(u'/'));
}

void TestRemoteLocation::aSerialAddressSpellsNoPortAndAnExplicitOneIsRefused()
{
    // A serial line has no port, so the normal form must not grow one — otherwise
    // normalize() produces an address its own parse refuses, and "one log, one spelling"
    // is gone.
    const auto loc = RemoteLocation::parse(QStringLiteral("serial://ttyUSB0/a.log"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->toString(), QStringLiteral("serial://ttyUSB0/a.log"));
    QVERIFY(!loc->toString().contains(QStringLiteral(":22")));
    QCOMPARE(RemoteLocation::normalize(loc->toString()), loc->toString());

    // The port FIELD keeps its default all the same, so every existing range assertion
    // about a parsed location — the address fuzzer's included — holds for a serial address
    // unaltered rather than needing a carve-out.
    QCOMPARE(loc->port, RemoteLocation::kDefaultPort);

    // An explicit one is refused with no I/O, which is an M17 refusal naming the address.
    // `115200` is the case worth naming: somebody means a baud rate, and the baud belongs
    // to the preset, which by design is not bound to a device.
    QVERIFY(!RemoteLocation::parse(QStringLiteral("serial://ttyUSB0:22/a.log")).has_value());
    QVERIFY(!RemoteLocation::parse(QStringLiteral("serial://ttyUSB0:115200/a.log")).has_value());
    QVERIFY(!logPathIsWellFormed(QStringLiteral("serial://ttyUSB0:115200/a.log")));
}

void TestRemoteLocation::aByIdDeviceKeepsItsCaseAndItsUnderscores()
{
    // THE FINDING THE SERIAL BRANCH OF parse() EXISTS FOR. QUrl normalises a host and part
    // of that is lowercasing it, so `serial://ttyUSB0/...` through QUrl comes back as
    // `ttyusb0` — and `/dev` is case-sensitive. Such an address would parse, would
    // normalize to a fixed point, would satisfy every property the fuzzer asserts, and
    // would name a device node that does not exist.
    const QString byId = QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0");
    const QString address = QStringLiteral("serial://root@%1/var/log/app.log").arg(byId);

    const auto loc = RemoteLocation::parse(address);
    QVERIFY(loc.has_value());
    QCOMPARE(loc->host, byId);                       // every capital, every underscore
    QVERIFY(loc->host != loc->host.toLower());
    QCOMPARE(loc->toString(), address);
    QCOMPARE(RemoteLocation::normalize(address), address);
    // And the plain port name, which is the commoner shape and folds just as badly.
    const auto plain = RemoteLocation::parse(QStringLiteral("serial://ttyUSB0/a.log"));
    QVERIFY(plain.has_value());
    QCOMPARE(plain->host, QStringLiteral("ttyUSB0"));
    // A Windows COM port, which is upper-case throughout.
    const auto com = RemoteLocation::parse(QStringLiteral("serial://COM3/C:/logs/app.log"));
    QVERIFY(com.has_value());
    QCOMPARE(com->host, QStringLiteral("COM3"));
}

void TestRemoteLocation::anInvalidPercentEscapeOnASerialAddressIsRefused()
{
    // THE STRICTNESS THE HAND-CUT BRANCH DOES NOT GET FOR FREE, found by the fuzzer within a
    // minute of the scheme existing. `QUrl(s, QUrl::StrictMode)` REFUSES a URL holding an
    // invalid escape, so the ssh branch answers nullopt and normalize() hands the string back
    // unchanged. `QUrl::fromPercentEncoding()` is lenient instead — it decodes `%U:` to `z` —
    // so the serial branch produced a path holding bytes the address never spelled, whose own
    // normal form then moved AGAIN on the next pass: one log, two spellings, which is a second
    // slot out of the pool of 500 and settings written under one name and read under the other.
    //
    // Refused rather than repaired, and at parse() because it is decidable with no I/O.
    QVERIFY(!RemoteLocation::parse(QStringLiteral("serial://t/~a.%U:/../a.zzst")).has_value());
    QVERIFY(!RemoteLocation::parse(QStringLiteral("serial://h/a%2")).has_value());   // truncated
    QVERIFY(!RemoteLocation::parse(QStringLiteral("serial://h/a%")).has_value());    // bare
    QVERIFY(!RemoteLocation::parse(QStringLiteral("serial://h/a%GG")).has_value());  // not hex
    QVERIFY(!RemoteLocation::parse(QStringLiteral("serial://%Uh/a.log")).has_value()); // authority too

    // The property the fuzzer asserts, stated here over the input that violated it: a refused
    // address falls through normalize() unchanged, so it is trivially a fixed point.
    const QString bad = QStringLiteral("serial://t/~a.%U:/../a.zzst");
    QCOMPARE(normalizeLogPath(bad), bad);
    QCOMPARE(normalizeLogPath(normalizeLogPath(bad)), normalizeLogPath(bad));

    // And a WELL-FORMED escape still round-trips, or the refusal has taken the encoding with it.
    const auto good = RemoteLocation::parse(QStringLiteral("serial://ttyUSB0/var/log/a%20b.log"));
    QVERIFY(good.has_value());
    QCOMPARE(good->path, QStringLiteral("/var/log/a b.log"));
    QCOMPARE(RemoteLocation::normalize(good->toString()), good->toString());
}

void TestRemoteLocation::aDeviceNameOutsideItsCharacterSetIsRefused()
{
    // Each of these would need encoding, and the narrow charset is what makes the branch
    // idempotent with no encoding to get wrong. Refused with no I/O, so it fails the open
    // and names the address rather than opening a tab that waits for a device that cannot
    // exist.
    const QStringList refused{
        QStringLiteral("serial://tty USB0/a.log"),
        QStringLiteral("serial://tty%20USB0/a.log"),
        QStringLiteral("serial://tty?USB0/a.log"),
        QStringLiteral("serial://[::1]/a.log"),
    };
    for (const QString &address : refused) {
        QVERIFY2(!RemoteLocation::parse(address).has_value(), qUtf8Printable(address));
        QVERIFY2(!logPathIsWellFormed(address), qUtf8Printable(address));
        // AND IT IS STILL IDEMPOTENT, which is the property that must hold for every
        // string and not only for the ones that parse: a refused address falls through
        // normalize() unchanged.
        QCOMPARE(RemoteLocation::normalize(RemoteLocation::normalize(address)),
                 RemoteLocation::normalize(address));
        QCOMPARE(normalizeLogPath(normalizeLogPath(address)), normalizeLogPath(address));
    }
}

void TestRemoteLocation::aSerialTargetCannotBeConfusedWithAHost()
{
    // A SECURITY DECISION RATHER THAN TIDINESS. target() keys the credential cache, the
    // keychain, the connect hold and the session cache — so a host genuinely NAMED
    // `ttyUSB0` must not share a keychain entry with the device of that name, because that
    // collision sends one machine's password to another.
    const auto device = RemoteLocation::parse(QStringLiteral("serial://root@ttyUSB0/a.log"));
    const auto host = RemoteLocation::parse(QStringLiteral("ssh://root@ttyUSB0/a.log"));
    QVERIFY(device.has_value());
    QVERIFY(host.has_value());
    QVERIFY2(device->target() != host->target(),
             qUtf8Printable(device->target() + QStringLiteral(" == ") + host->target()));
    QCOMPARE(device->target(), QStringLiteral("serial:root@ttyUSB0"));
    // The ssh target is LOWERCASE, and that is the very fold the serial branch had to be
    // written around: QUrl normalises a host, which is right for a host name (DNS is
    // case-insensitive) and fatal for a device node (`/dev` is not). The two spellings
    // differing is incidental here; what matters is the `serial:` prefix, which is what
    // keeps them apart even for a device and a host whose names are identical in case too.
    QCOMPARE(host->target(), QStringLiteral("root@ttyusb0:22"));

    // A device with no account named is still keyed distinctly, and still says which
    // transport it is.
    const auto anon = RemoteLocation::parse(QStringLiteral("serial://ttyUSB0/a.log"));
    QVERIFY(anon.has_value());
    QCOMPARE(anon->target(), QStringLiteral("serial:ttyUSB0"));
}

void TestRemoteLocation::aSerialAddressNamesNoDefaultUser()
{
    // ssh fills the local account name in absent a User directive. A DEVICE HAS NO SUCH
    // CONVENTION — its accounts have nothing to do with this machine's — so filling one in
    // would key the connect under an account nobody named and would send this machine's
    // login name to somebody else's hardware.
    const auto anon = RemoteLocation::parse(QStringLiteral("serial://ttyUSB0/a.log"));
    QVERIFY(anon.has_value());
    QVERIFY(anon->effectiveUser().isEmpty());

    // Whereas an ssh address with no user still resolves to one, which is the behaviour
    // this must not have disturbed.
    const auto ssh = RemoteLocation::parse(QStringLiteral("ssh://h/a.log"));
    QVERIFY(ssh.has_value());
    QVERIFY(!ssh->effectiveUser().isEmpty());
}

void TestRemoteLocation::aSerialAddressNeverEmitsItsPassword()
{
    // The rule that binds every scheme: a password in the address is dropped on the floor
    // by parse(), and an address parse REFUSES is filtered by withoutPassword() before it
    // is shown. Both halves, for the new scheme.
    const auto loc = RemoteLocation::parse(QStringLiteral("serial://root:hunter2@ttyUSB0/a.log"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->user, QStringLiteral("root"));
    QVERIFY(!loc->toString().contains(QStringLiteral("hunter2")));
    QVERIFY(!loc->toDisplayString().contains(QStringLiteral("hunter2")));
    QVERIFY(!loc->target().contains(QStringLiteral("hunter2")));

    // An address with no path does not parse, so it is shown verbatim — and this is the
    // filter that keeps the credential out of it. withoutPassword() needed no change for
    // the new scheme, which is worth asserting rather than assuming.
    const QString unparseable = QStringLiteral("serial://root:hunter2@ttyUSB0");
    QVERIFY(!RemoteLocation::parse(unparseable).has_value());
    QVERIFY(!RemoteLocation::withoutPassword(unparseable).contains(QStringLiteral("hunter2")));
    QVERIFY(RemoteLocation::withoutPassword(unparseable).contains(QStringLiteral("root")));
    QVERIFY(!logSourceDisplayName(unparseable).contains(QStringLiteral("hunter2")));
    QVERIFY(!logSourceDisplayPath(unparseable).contains(QStringLiteral("hunter2")));
}

void TestRemoteLocation::aSerialArchiveAddressSplits()
{
    // A transport and a file TYPE are orthogonal, and the archive address's whole design is
    // that it has no scheme of its own — the member continues the container's path. So a
    // rolled log inside a bundle on a device composes for free, and this is what says it
    // did rather than leaving it to be assumed.
    const QString address =
        QStringLiteral("serial://ttyUSB0/srv/bundle.tar.gz/var/log/app.log");
    QCOMPARE(logSourceBareName(address), QStringLiteral("app.log"));
    QVERIFY(logPathIsSpooled(address));
    QCOMPARE(normalizeLogPath(normalizeLogPath(address)), normalizeLogPath(address));

    // A single compressed log on a device keeps its plain name and grows no member.
    const QString gz = QStringLiteral("serial://ttyUSB0/var/log/app.log.1.gz");
    QCOMPARE(normalizeLogPath(normalizeLogPath(gz)), normalizeLogPath(gz));
    QCOMPARE(logSourceBareName(gz), QStringLiteral("app.log.1"));
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

// bugs.md 44. THE SIXTY-SIX UNICODE NONCHARACTERS ARE THE ONE CLASS OF CHARACTER THAT
// DOES NOT SURVIVE ITS OWN NORMAL FORM. toString() percent-encodes one and QUrl
// declines to give it back, answering U+FFFD per byte — so `ssh://h/x<U+FFFF>y`
// normalizes to `ssh://h:22/x%EF%BF%BFy` and re-parses to a path holding three
// replacement characters. Every entry point normalizes and Document::prepare()
// normalizes AGAIN, so such a log had two spellings: two settings slots out of the
// pool of 500, settings written under one name and read under the other.
//
// The ruling is a REFUSAL at parse() rather than an agreement between toString() and
// parse() about a character neither of them wants. It is decided with no I/O, so per
// M17 it fails the open outright and names the reason instead of putting up a tab.
void TestRemoteLocation::anAddressHoldingANoncharacterIsRefusedRatherThanRespelled()
{
    // In the path (corpus/address/a029_noncharacter_in_path), in the account
    // (a030_noncharacter_in_user), in a plane above the BMP — sixteen of the sixty-six
    // are surrogate pairs, and neither half of a pair is a noncharacter on its own —
    // and in the U+FDD0..U+FDEF block, which is the half that is not `& 0xFFFE`.
    const QStringList refused = {
        QStringLiteral("ssh://h/x￿y"),
        QStringLiteral("sftp://J/~]￿"),
        QStringLiteral("ssh://￿u@os/togs/app.log"),
        QStringLiteral("ssh://h/x￾y"),
        QStringLiteral("ssh://h/x﷐y"),
        QStringLiteral("ssh://h/x﷯y"),
        QString::fromUcs4(U"ssh://h/x\U0001FFFEy"),
        QString::fromUcs4(U"ssh://h/x\U0010FFFFy"),
        QString::fromUcs4(U"ssh://u\U0001FFFFa@h/p"),
    };
    for (const QString &address : refused) {
        QVERIFY2(!RemoteLocation::parse(address).has_value(), qPrintable(address));
        // And therefore it names no log at all, which is what keeps it a refusal
        // rather than a tab waiting for something that cannot turn up.
        QVERIFY2(!logPathIsWellFormed(address), qPrintable(address));
    }

    // A noncharacter in the HOST is refused too, though QUrl's own hostname rules get
    // there first — the check covers the component for the shape's sake, not because
    // anything today reaches it.
    QVERIFY(!RemoteLocation::parse(QStringLiteral("ssh://h￿x/p")).has_value());

    // Nothing else moved. Percent-encoding, spaces, non-ASCII that is not a
    // noncharacter, tildes and the replacement character ITSELF — which is an ordinary
    // character and survives its own normal form perfectly — all still parse.
    for (const QString &kept : {QStringLiteral("ssh://web1/var/log/app.log"),
                                QStringLiteral("ssh://deploy@web1:2222/var/log/app.log"),
                                QStringLiteral("ssh://h/logs/журнал.log"),
                                QStringLiteral("ssh://h/my%20app.log"),
                                QStringLiteral("ssh://h/~/app.log"),
                                QStringLiteral("ssh://h/x�y"),
                                QString::fromUcs4(U"ssh://h/x\U0001F600y")}) {
        QVERIFY2(RemoteLocation::parse(kept).has_value(), qPrintable(kept));
        QVERIFY2(logPathIsWellFormed(kept), qPrintable(kept));
    }

    // What the user is told. It is the refusal every unparseable address already gets,
    // so nothing new reaches the screen — and it goes through withoutPassword(), which
    // is what keeps this from becoming a fresh route for a credential: the address is
    // quoted VERBATIM because parse() refused it and so never dropped its password.
    QString error;
    const QString withSecret = QStringLiteral("ssh://deploy:hunter2@web1/var/log/x￿y");
    QVERIFY(!openLogSource(withSecret, OpenPolicy::Interactive, &error));
    QVERIFY2(!error.isEmpty(), qPrintable(error));
    QVERIFY2(!error.contains(QStringLiteral("hunter2")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("web1")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("deploy")), qPrintable(error)); // which login
}

// bugs.md 45. A PORT IS RANGE-CHECKED WHERE EVERY OTHER PART OF AN ADDRESS IS — at
// parse(), with no I/O, so the open fails and names the address (M17). QUrl::port(22)
// substitutes the default only where the address spells NO port, so an explicit `:0`
// used to be taken at face value: target() answered `user@host:0`, SshSessionCache
// keyed on it, and connectTo() handed it to a socket, which reported it as though the
// host had refused the connection.
//
// WHICH LAYER REFUSES WHAT: the lower bound is loftail's, the upper bound is Qt's as
// well as loftail's. `ssh://h:65536/p` and `ssh://h:-1/p` are already invalid URLs
// (measured on Qt 6.10), so the upper half of the check is unreachable today — it is
// written out all the same, so that the set of addresses loftail accepts is not a
// function of the Qt build. Either way, this case asserts the refusal and not who
// performed it.
void TestRemoteLocation::aPortOutsideTheTcpRangeIsARefusedAddressAndNotAFailedConnect()
{
    // Refused. `:0` is the fuzzer's input (corpus/address/a027_explicit_port_zero);
    // `:00` is the same value spelled so that QUrl still reads a port; the last two are
    // out of range at the top and below zero, which Qt refuses first.
    for (const QString &bad : {QStringLiteral("ssh://h:0/p"),
                               QStringLiteral("ssh://h:00/p"),
                               QStringLiteral("ssh://u@h:0/var/log/app.log"),
                               QStringLiteral("sftp://h:0/p"),
                               QStringLiteral("ssh://h:65536/p"),
                               QStringLiteral("ssh://h:-1/p")}) {
        QVERIFY2(!RemoteLocation::parse(bad).has_value(), qPrintable(bad));
        // And so it names no log: a refusal that fails the open, never a tab waiting
        // for a machine that cannot be reached on a port that is not one.
        QVERIFY2(!logPathIsWellFormed(bad), qPrintable(bad));
        // A refused address normalizes to itself, so it is still a fixed point.
        QCOMPARE(RemoteLocation::normalize(bad), bad);
    }

    // The boundary sits at 1 and at 65535, and both ends are ACCEPTED.
    const auto lowest = RemoteLocation::parse(QStringLiteral("ssh://h:1/p"));
    QVERIFY(lowest.has_value());
    QCOMPARE(lowest->port, 1);
    const auto highest = RemoteLocation::parse(QStringLiteral("ssh://h:65535/p"));
    QVERIFY(highest.has_value());
    QCOMPARE(highest->port, 65535);
    QCOMPARE(highest->toString(), QStringLiteral("ssh://h:65535/p"));

    // An address that spells no port at all is untouched: that is the one case
    // QUrl::port(default) was always answering, and it still defaults to 22 — as does
    // an empty `:`, which spells no port either.
    const auto none = RemoteLocation::parse(QStringLiteral("ssh://h/p"));
    QVERIFY(none.has_value());
    QCOMPARE(none->port, RemoteLocation::kDefaultPort);
    const auto colon = RemoteLocation::parse(QStringLiteral("ssh://h:/p"));
    QVERIFY(colon.has_value());
    QCOMPARE(colon->port, RemoteLocation::kDefaultPort);

    // What the user is told, and the rule the refusal path carries: the address is
    // quoted VERBATIM, because parse() refused it and so never dropped its password —
    // so the reason goes through withoutPassword() like every other unparseable one.
    QString error;
    const QString withSecret = QStringLiteral("ssh://deploy:hunter2@web1:0/var/log/app.log");
    QVERIFY(!openLogSource(withSecret, OpenPolicy::Interactive, &error));
    QVERIFY2(!error.isEmpty(), qPrintable(error));
    QVERIFY2(!error.contains(QStringLiteral("hunter2")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("web1")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("deploy")), qPrintable(error)); // which login
}

// bugs.md 48. A NUL IS THE OTHER CHARACTER THAT DOES NOT SURVIVE AN ADDRESS, and it
// breaks the LOCAL half where the noncharacter broke the remote one. QFileInfo treats a
// path holding one as a broken filename and answers absoluteFilePath() with a string
// that is still RELATIVE — `a\0/../b` answers `a\0/../b`, which the clean reduces to
// `b` — so logSettingsKey(), whose whole job is a spelling that does not move with the
// working directory, handed back one that does and resolved it against that directory
// the second time it was applied. One log, two spellings, at the usual cost: a second
// slot out of the pool of 500 and settings written under one name and read under the
// other.
//
// The ruling is entry 44's: a REFUSAL, decided with no I/O, rather than teaching the key
// to re-absolutize its own answer — which repairs a symptom and leaves the address. It
// lives in logPathIsWellFormed() rather than at parse(), because a NUL is not only a
// remote question: the plain key and the archive funnel both carried it, and that one
// function is asked about all three.
void TestRemoteLocation::anAddressHoldingANulIsRefusedRatherThanRekeyed()
{
    const QChar nul(u'\0');
    // The fuzzer's two inputs (corpus/address/a034_nul_and_dotdot_in_a_relative_path and
    // a035_nul_and_dotdot_before_a_container), then the same character in each of the
    // other places an address can hold one: an absolute local path, a remote path, the
    // account, an archive container and a member inside one.
    const QStringList refused = {
        QStringLiteral("a") + nul + QStringLiteral("/../b"),
        QStringLiteral("a") + nul + QStringLiteral("/../b.tar.gz/m"),
        QDir::rootPath() + QStringLiteral("var/log/a") + nul + QStringLiteral(".log"),
        QStringLiteral("ssh://h/x") + nul + QStringLiteral("/../y"),
        QStringLiteral("ssh://u") + nul + QStringLiteral("v@h/p"),
        QDir::rootPath() + QStringLiteral("srv/b") + nul + QStringLiteral(".zip/app.log"),
        QDir::rootPath() + QStringLiteral("srv/b.zip/app") + nul + QStringLiteral(".log"),
        QString(nul),
    };
    for (const QString &address : refused) {
        // Printed with the NUL spelled out: qPrintable() stops at one, so a failure would
        // otherwise name half an address.
        const QString shown = QString(address).replace(nul, QStringLiteral("<NUL>"));
        // It names no log at all, so the open FAILS and says so — it is not a log that
        // has not turned up yet, and no amount of waiting will put a NUL in a filename.
        QVERIFY2(!logPathIsWellFormed(address), qPrintable(shown));
        // And it is left alone by both funnels, so it is a fixed point on its way to
        // being refused: whatever is compared, keyed or reported, is compared, keyed and
        // reported once.
        QCOMPARE(normalizeLogPath(address), address);
        QCOMPARE(logSettingsKey(address), address);
    }

    // The remote shapes are refused by QUrl first — a NUL is an invalid path character
    // there — so parse() answered nullopt for them before this existed. Asserted for the
    // shape's sake: which layer says no is not the contract, and the local and archive
    // halves have no such layer under them at all.
    QVERIFY(!RemoteLocation::parse(QStringLiteral("ssh://h/x") + nul + QStringLiteral("/y"))
                 .has_value());

    // Nothing else moved. A control character that is not a NUL is an ordinary (if odd)
    // character in a local filename and still names a log, as do the ordinary addresses
    // beside it — a relative path among them, which is the shape this refusal must not
    // be mistaken for.
    for (const QString &kept : {QStringLiteral("ssh://web1/var/log/app.log"),
                                QDir::rootPath() + QStringLiteral("var/log/app.log"),
                                QDir::rootPath() + QStringLiteral("srv/b.zip/app.log"),
                                QDir::rootPath() + QStringLiteral("var/log/a\tb.log"),
                                QStringLiteral("a/../b")}) {
        QVERIFY2(logPathIsWellFormed(kept), qPrintable(kept));
    }

    // What the user is told, and the rule every refusal path carries: the address is
    // quoted VERBATIM, parse() having refused it and so never dropped its password, so
    // the reason goes through withoutPassword() like every other unparseable one.
    QString error;
    const QString withSecret =
        QStringLiteral("ssh://deploy:hunter2@web1/var/log/x") + nul + QStringLiteral("y");
    QVERIFY(!openLogSource(withSecret, OpenPolicy::Interactive, &error));
    QVERIFY2(!error.isEmpty(), qPrintable(error));
    QVERIFY2(!error.contains(QStringLiteral("hunter2")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("web1")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("deploy")), qPrintable(error)); // which login
}

void TestRemoteLocation::normalizingAnAddressTwiceIsNormalizingItOnce_data()
{
    QTest::addColumn<QString>("address");

    QTest::newRow("plain remote") << QStringLiteral("ssh://web1/var/log/app.log");
    QTest::newRow("user and port") << QStringLiteral("ssh://deploy@web1:2222/a.log");
    QTest::newRow("sftp") << QStringLiteral("sftp://web1/a.log");
    QTest::newRow("already normal") << QStringLiteral("ssh://web1:22/a.log");
    QTest::newRow("space") << QStringLiteral("ssh://h/my app.log");
    QTest::newRow("encoded space") << QStringLiteral("ssh://h/my%20app.log");
    QTest::newRow("cyrillic") << QStringLiteral("ssh://h/logs/журнал.log");
    QTest::newRow("astral") << QString::fromUcs4(U"ssh://h/x\U0001F600y");
    QTest::newRow("replacement char") << QStringLiteral("ssh://h/x�y");
    QTest::newRow("tilde") << QStringLiteral("ssh://h/~/a.log");
    QTest::newRow("password") << QStringLiteral("ssh://u:pw@h/a.log");
    QTest::newRow("local") << QStringLiteral("/var/log/app.log");
    QTest::newRow("not an address") << QStringLiteral("ssh://");
    QTest::newRow("empty") << QString();
    // The two the fuzzer produced, which is what this case exists for.
    QTest::newRow("a029 noncharacter in path") << QStringLiteral("sftp://J/~]￿");
    QTest::newRow("a030 noncharacter in user") << QStringLiteral("ssh://￿u:p@os/togs/sbn.zip/");
    QTest::newRow("noncharacter FDD0") << QStringLiteral("ssh://h/x﷐y");
    QTest::newRow("noncharacter astral") << QString::fromUcs4(U"ssh://h/x\U0001FFFEy");
    QTest::newRow("a027 explicit port zero") << QStringLiteral("sftp://hoh:0/");
    QTest::newRow("port out of range") << QStringLiteral("ssh://h:65536/p");
    // And the NUL, whose absolute path was not absolute (bugs.md 48). a034 is the
    // fuzzer's plain-key shape and a035 its archive shape; the remote ones are the same
    // character in the two components an address can spell it in.
    QTest::newRow("a034 nul and dotdot in a relative path")
        << (QStringLiteral("a") + QChar(u'\0') + QStringLiteral("/../b"));
    QTest::newRow("a035 nul and dotdot before a container")
        << (QStringLiteral("a") + QChar(u'\0') + QStringLiteral("/../b.tar.gz/m"));
    QTest::newRow("nul in an absolute path")
        << (QDir::rootPath() + QStringLiteral("var/log/a") + QChar(u'\0')
            + QStringLiteral("/../b.log"));
    QTest::newRow("nul in a remote path")
        << (QStringLiteral("ssh://h/x") + QChar(u'\0') + QStringLiteral("/../y"));
    QTest::newRow("nul in the account")
        << (QStringLiteral("ssh://u") + QChar(u'\0') + QStringLiteral("v@h/p"));
}

// THE INVARIANT ITSELF, which is stronger than asserting the refusal: normalize() is
// what every entry point runs a path through before it becomes a Document::path(), and
// Document::prepare() runs it a SECOND time — so the whole tree's "one log, one
// spelling" rests on the result being a fixed point. A refused address falls through
// normalize() unchanged, which is itself a fixed point, so the property holds for the
// characters this cannot spell as well as for the ones it can.
void TestRemoteLocation::normalizingAnAddressTwiceIsNormalizingItOnce()
{
    QFETCH(QString, address);

    const QString once = RemoteLocation::normalize(address);
    QCOMPARE(RemoteLocation::normalize(once), once);

    // And the same for the funnel every entry point actually calls, which adds the
    // archive branch on top — a030 is an archive-shaped address as well as a remote one.
    // Unconditional over the local archive rows as well since bugs.md 47 was taken, and
    // over the NUL rows since 48 was: both funnels clean to a fixed point, and neither
    // respells an address that names no log.
    const QString onceLog = normalizeLogPath(address);
    QCOMPARE(normalizeLogPath(onceLog), onceLog);

    // The settings key is the thing that would be spelled two ways, so it is asserted
    // where the damage would be rather than only where the cause is — as the property
    // itself, LogFileStore::save() re-keying an address its caller has already keyed.
    const QString key = logSettingsKey(address);
    QCOMPARE(logSettingsKey(key), key);
    QCOMPARE(logSettingsKey(onceLog), logSettingsKey(normalizeLogPath(onceLog)));
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

    // A MISSING FOLDER IS ITS OWN ANSWER, and was Absent until it became one: "app.log
    // has not appeared yet" about a mistyped directory sends the reader to look inside a
    // tree that is not there to look in.
    QCOMPARE(logSourcePresence(dir.filePath(QStringLiteral("nosuch/deeper/app.log"))),
             LogPresence::NoDirectory);
    // A DIRECTORY IS READABLE, so without an answer of its own it reports Present, the
    // open then fails, and the sentence is whatever the file layer made of being handed
    // a folder. It is the one answer here that never resolves on its own, which is what
    // logPresenceMayAppear() is for: everything that waits must be able to stop.
    QCOMPARE(logSourcePresence(dir.path()), LogPresence::NotAFile);

    QVERIFY(logPresenceMayAppear(LogPresence::Absent));
    QVERIFY(logPresenceMayAppear(LogPresence::NoDirectory));
    QVERIFY(!logPresenceMayAppear(LogPresence::Present));
    QVERIFY(!logPresenceMayAppear(LogPresence::NotAFile));
    // Unreadable waits too, but for a DIFFERENT reason — it is there, so nothing has to
    // appear — and folding it in here would make an unreadable archive container get
    // polled eighty times a minute for the life of the tab (ArchiveFetcher).
    QVERIFY(!logPresenceMayAppear(LogPresence::Unreadable));

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

void TestRemoteLocation::presenceTellsAMissingFolderAndAFolderFromBothOfThem()
{
    // The remote half of the same rule: still OPTIMISTIC, so neither of the two new
    // answers may leak into a remote address. Answering either truthfully costs a round
    // trip, and this runs during session restore where a stall would be a hang — the
    // transport asks the real question on its own failure path (SshSession::classifyPath).
    for (const QString &address : {QStringLiteral("ssh://web1/no/such/dir/app.log"),
                                   QStringLiteral("ssh://web1/"),
                                   QStringLiteral("ssh://web1/var/log")}) {
        QCOMPARE(logSourcePresence(address), LogPresence::Present);
    }
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
