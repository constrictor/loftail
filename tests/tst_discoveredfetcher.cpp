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

// A log on a DISCOVERED device, end to end below the window (ARCHITECTURE.md §6.13): a
// `discover://Type/id/path` address, the fetcher makeDiscoveredFetcher() builds for it, a
// scripted browser standing in for the network, and a real telnet server with a real /bin/sh
// behind it standing in for the device.
//
// What this exists to prove is the one thing the feature is FOR: the device moves to a new
// address and the open log follows it, with nothing about the address — and so nothing about
// the log's settings, spool or session entry — changing. A fake fetcher (tests/FakeFetcher.h)
// cannot prove it, because it replaces exactly the half that does the finding.

#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <memory>
#include <mutex>

#include "DiscoveredFetcher.h"
#include "DiscoveryBrowser.h"
#include "DiscoveryWatch.h"
#include "FakeTelnetd.h"
#include "RemoteLocation.h"
#include "RestartRunner.h"
#include "RestartTarget.h"
#include "SerialFetchOptions.h"
#include "SerialProfile.h"
#include "SshPrompter.h"
#include "TelnetSession.h"

using namespace loftail;

namespace {

// Where the scripted network says the device is. Written by the test, read by browsers made
// on the fetcher's worker thread — hence the mutex.
struct Network
{
    std::mutex  mutex;
    QString     host;
    int         port = 0;
    bool        present = true;
    // Every address the device answers on, `host` among them; empty means `host` alone.
    QStringList addresses;
};

// A LIVE backend, as Avahi with kept resolvers is one: it looks at the scripted network every
// few milliseconds and reports the device again whenever what it would say has CHANGED, and
// says `lost` when the device leaves — which is what a standing watch (DiscoveryWatch.h) is
// built on, and what a backend that answered once and fell silent could not stand in for.
class ScriptedBrowser final : public DiscoveryBrowser
{
public:
    ScriptedBrowser(std::shared_ptr<Network> net, QObject *parent)
        : DiscoveryBrowser(parent), m_net(std::move(net))
    {
    }
    void start(const RemoteType &type) override
    {
        m_type = type;
        m_wasPresent = false;
        m_last = DiscoveredDevice{};
        if (!m_tick) {
            m_tick = new QTimer(this);
            m_tick->setInterval(20);
            connect(m_tick, &QTimer::timeout, this, [this] { look(); });
        }
        m_tick->start();
    }
    void stop() override
    {
        if (m_tick)
            m_tick->stop();
    }

private:
    void look()
    {
        DiscoveredDevice d;
        bool present = false;
        {
            const std::scoped_lock lock(m_net->mutex);
            present = m_net->present;
            d.host = m_net->host;
            d.port = m_net->port;
            d.addresses = m_net->addresses;
        }
        if (!present) {
            if (m_wasPresent)
                emit lost(m_type.name, QStringLiteral("board-1"));
            m_wasPresent = false;
            return;
        }
        d.id = QStringLiteral("board-1");
        d.displayName = QStringLiteral("Board one");
        d.details = {QStringLiteral("model=X")};
        if (m_wasPresent && d == m_last)
            return;
        m_wasPresent = true;
        m_last = d;
        report(m_type, d);
    }

    std::shared_ptr<Network> m_net;
    RemoteType               m_type;
    QTimer                  *m_tick = nullptr;
    bool                     m_wasPresent = false;
    DiscoveredDevice         m_last;
};

QString readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

class TestDiscoveredFetcher : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir            m_dir;
    std::shared_ptr<Network> m_net;

    QString toolsDir()
    {
        const QString dir = m_dir.filePath(QStringLiteral("tools"));
        if (QFileInfo::exists(dir))
            return dir;
        QDir().mkpath(dir);
        for (const QString &tool : {QStringLiteral("stat"), QStringLiteral("tail"),
                                    QStringLiteral("head"), QStringLiteral("base64"),
                                    QStringLiteral("printf"), QStringLiteral("test"),
                                    QStringLiteral("ls"), QStringLiteral("wc")}) {
            for (const QString &from : {QStringLiteral("/bin/"), QStringLiteral("/usr/bin/")}) {
                if (QFileInfo::exists(from + tool)) {
                    QFile::link(from + tool, dir + u'/' + tool);
                    break;
                }
            }
        }
        QFile shim(dir + QStringLiteral("/stty"));
        if (shim.open(QIODevice::WriteOnly)) {
            shim.write("#!/bin/sh\nexit 0\n");
            shim.close();
            shim.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        return dir;
    }

    static RemoteType boardsType()
    {
        RemoteType t;
        t.name = QStringLiteral("Boards");
        t.serviceType = QStringLiteral("_telnet._tcp");
        t.txtRe = QStringLiteral("^model=X$");
        t.connection = RemoteType::Connection::Telnet;
        t.user = QStringLiteral("root");
        return t;
    }

    RemoteLocation addressFor(const QString &log) const
    {
        return RemoteLocation::parse(QStringLiteral("discover://Boards/board-1%1").arg(log))
            .value_or(RemoteLocation{});
    }

    static void primeOptions(const RemoteLocation &address)
    {
        SerialFetchOptions o;
        o.profile = SerialProfile::builtIn();
        o.profile.promptTimeoutMs = 700;
        o.profile.attempts = 4;
        o.profile.pollMs = 200;
        setTelnetFetchOptions(address, o);
    }

    // Until `done` holds or the budget runs out, pumping events so queued work lands.
    template <typename F>
    static bool waitFor(F done, int ms)
    {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms) {
            if (done())
                return true;
            QTest::qWait(50);
        }
        return done();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
            QSKIP("no /bin/sh");
    }

    void init()
    {
        m_net = std::make_shared<Network>();
        auto net = m_net;
        setDiscoveryBrowserFactory([net](const RemoteType &, QObject *parent) {
            return std::make_unique<ScriptedBrowser>(net, parent);
        });
        setKnownRemoteTypes({boardsType()});
        // The TYPE's password, filed under the type's key — what MainWindow primes from a
        // saved type, and what the resolved far end asks for.
        SshCredentialCache::remember(boardsType().credentialTarget(), QStringLiteral("hunter2"));
    }

    void cleanup()
    {
        setDiscoveryBrowserFactory(nullptr);
        setKnownRemoteTypes({});
        clearDiscoveredDevices();
        clearSerialFetchOptions();
        TelnetSessionRegistry::instance().clear();
        SshCredentialCache::clear();
    }

    void theAddressNamesTheTypeAndTheDeviceAndNeverAnAddress()
    {
        const auto loc = RemoteLocation::parse(
            QStringLiteral("discover://Lab%20Cams/urn%3Auuid%3A1b2c3d4e-0000/var/log/app.log"));
        QVERIFY(loc.has_value());
        QCOMPARE(loc->transport, RemoteLocation::Transport::Discovered);
        QCOMPARE(loc->discoveryType, QStringLiteral("Lab Cams"));
        QCOMPARE(loc->host, QStringLiteral("urn:uuid:1b2c3d4e-0000"));
        QCOMPARE(loc->path, QStringLiteral("/var/log/app.log"));
        QCOMPARE(loc->target(), QStringLiteral("discover:Lab Cams/urn:uuid:1b2c3d4e-0000"));
        QVERIFY(loc->effectiveUser().isEmpty());
        // Shortened for a person, and never a separator in a display name.
        QCOMPARE(loc->displayHost(), QStringLiteral("Lab Cams 1b2c3d4e"));
        QVERIFY(!logSourceDisplayName(loc->toString()).contains(u'/'));
    }

    void theDeviceIsFoundAndItsLogRead()
    {
        const QByteArray content("first line\nsecond line\n");
        const QString log = m_dir.filePath(QStringLiteral("found.log"));
        {
            QFile f(log);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(content);
        }
        FakeTelnetd server(toolsDir(), m_dir.path());
        server.start();
        {
            const std::scoped_lock lock(m_net->mutex);
            m_net->host = QStringLiteral("127.0.0.1");
            m_net->port = server.port();
        }
        const RemoteLocation address = addressFor(log);
        primeOptions(address);

        QString error;
        auto fetcher = makeDiscoveredFetcher(address, &error);
        QVERIFY2(fetcher, qPrintable(error));
        const QString spoolDir = m_dir.filePath(QStringLiteral("spool-found"));
        QDir().mkpath(spoolDir);
        QVERIFY(fetcher->start(spoolDir, &error));
        QVERIFY2(waitFor([&] { return fetcher->status().state == FetchStatus::State::Live
                                      && fetcher->status().committedSize == content.size(); },
                         20000),
                 qPrintable(fetcherState(*fetcher)));
        QCOMPARE(readAll(fetcher->spoolPath(fetcher->status().generation)),
                 QString::fromUtf8(content));
        // The device answered discovery, so it is remembered by its own name.
        QCOMPARE(lastSeenDevice(QStringLiteral("Boards"), QStringLiteral("board-1"))->displayName,
                 QStringLiteral("Board one"));

        fetcher->requestStop();
        QVERIFY(waitFor([&] { return fetcher->isStopped(); }, 10000));
        fetcher.reset();
        server.stop();
    }

    void aDeviceThatMovesIsFollowedToItsNewAddress()
    {
        // THE WHOLE POINT: the device's address changes under an open log, and the log goes on
        // being read from the new one with nothing about its own address changing.
        const QString log = m_dir.filePath(QStringLiteral("moving.log"));
        {
            QFile f(log);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("before the move\n");
        }
        auto first = std::make_unique<FakeTelnetd>(toolsDir(), m_dir.path());
        first->start();
        {
            const std::scoped_lock lock(m_net->mutex);
            m_net->host = QStringLiteral("127.0.0.1");
            m_net->port = first->port();
        }
        const RemoteLocation address = addressFor(log);
        const QString keyBefore = logSettingsKey(address.toString());
        primeOptions(address);

        QString error;
        auto fetcher = makeDiscoveredFetcher(address, &error);
        QVERIFY2(fetcher, qPrintable(error));
        const QString spoolDir = m_dir.filePath(QStringLiteral("spool-moving"));
        QDir().mkpath(spoolDir);
        QVERIFY(fetcher->start(spoolDir, &error));
        QVERIFY2(waitFor([&] { return fetcher->status().state == FetchStatus::State::Live; },
                         20000),
                 qPrintable(fetcherState(*fetcher)));

        // The device goes away from its old address and turns up at a new one.
        first->stop();
        first.reset();
        FakeTelnetd second(toolsDir(), m_dir.path());
        second.start();
        {
            const std::scoped_lock lock(m_net->mutex);
            m_net->port = second.port();
        }
        {
            QFile f(log);
            QVERIFY(f.open(QIODevice::Append));
            f.write("after the move\n");
        }

        QVERIFY2(waitFor([&] {
                     return second.connections() > 0
                         && fetcher->status().state == FetchStatus::State::Live
                         && readAll(fetcher->spoolPath(fetcher->status().generation))
                                .contains(QStringLiteral("after the move"));
                 },
                         30000),
                 qPrintable(fetcherState(*fetcher)));
        // And the log is still the same log.
        QCOMPARE(logSettingsKey(address.toString()), keyBefore);

        fetcher->requestStop();
        QVERIFY(waitFor([&] { return fetcher->isStopped(); }, 10000));
        fetcher.reset();
        second.stop();
    }

    // --- the standing watch (DiscoveryWatch.h) -------------------------------------------

    void theMoveRuleIsTheAddressSetHeldForAWholeGracePeriod()
    {
        {
            const std::scoped_lock lock(m_net->mutex);
            m_net->present = false;   // the browser says nothing; the cache is set by hand
        }
        const RemoteLocation address = addressFor(QStringLiteral("/x.log"));
        const EndpointSource source = discoveryEndpointSource(address);
        QVERIFY(source.resolve);
        QVERIFY(source.stillCurrent);

        DiscoveredDevice at;
        at.typeName = QStringLiteral("Boards");
        at.id = QStringLiteral("board-1");
        at.host = QStringLiteral("10.0.0.1");
        at.port = 2323;
        at.addresses = {QStringLiteral("10.0.0.1"), QStringLiteral("10.0.0.2")};
        const RemoteLocation connected = endpointFor(boardsType(), at, address);
        QString where;

        // No watch: not knowing is never a move, whatever the cache says.
        DiscoveredDevice elsewhere = at;
        elsewhere.host = QStringLiteral("10.0.0.9");
        elsewhere.addresses = {QStringLiteral("10.0.0.9")};
        rememberDiscoveredDevice(elsewhere);
        QVERIFY(source.stillCurrent(connected, &where));
        QTest::qWait(int(kMoveGraceMs) + 200);
        QVERIFY(source.stillCurrent(connected, &where));

        DiscoveryWatch watch;
        watch.setWatchedTypes({boardsType()});
        QVERIFY(discoveryWatchIsRunning(QStringLiteral("boards")));

        // Where it was connected, and where the backend now NAMES the other interface: both
        // are the device, so neither is a move.
        rememberDiscoveredDevice(at);
        QVERIFY(source.stillCurrent(connected, &where));
        DiscoveredDevice otherInterface = at;
        otherInterface.host = QStringLiteral("10.0.0.2");
        rememberDiscoveredDevice(otherInterface);
        QVERIFY(source.stillCurrent(connected, &where));

        // Elsewhere: not on the first look, and not within the grace period...
        rememberDiscoveredDevice(elsewhere);
        QVERIFY(source.stillCurrent(connected, &where));
        QTest::qWait(int(kMoveGraceMs) / 3);
        QVERIFY(source.stillCurrent(connected, &where));
        // ...a blip back inside it starts the clock again...
        rememberDiscoveredDevice(at);
        QVERIFY(source.stillCurrent(connected, &where));
        rememberDiscoveredDevice(elsewhere);
        QVERIFY(source.stillCurrent(connected, &where));
        QTest::qWait(int(kMoveGraceMs) * 2 / 3);
        QVERIFY(source.stillCurrent(connected, &where));
        // ...and once it has held for the whole of it, the device has moved.
        QTest::qWait(int(kMoveGraceMs) / 3 + 200);
        QVERIFY(!source.stillCurrent(connected, &where));
        QCOMPARE(where, QStringLiteral("10.0.0.9:2323"));

        // A different PORT at the same address is a move as well.
        DiscoveredDevice newPort = at;
        newPort.port = 2424;
        rememberDiscoveredDevice(newPort);
        QVERIFY(source.stillCurrent(connected, &where));
        QTest::qWait(int(kMoveGraceMs) + 200);
        QVERIFY(!source.stillCurrent(connected, &where));

        watch.setWatchedTypes({});
        QVERIFY(!discoveryWatchIsRunning(QStringLiteral("Boards")));
    }

    void aDeviceThatMovesIsFollowedEvenWhileTheOldAddressStillAnswers()
    {
        // THE CASE THE WATCH EXISTS FOR: the old address goes on answering — DHCP gave it to
        // another machine, or a replacement unit took it — so the old link never fails, and
        // only the watch can say the device is somewhere else.
        const QString log = m_dir.filePath(QStringLiteral("followed.log"));
        {
            QFile f(log);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("one\n");
        }
        FakeTelnetd first(toolsDir(), m_dir.path());
        first.start();
        setNetwork(first.port());

        DiscoveryWatch watch;
        watch.setWatchedTypes({boardsType()});

        const RemoteLocation address = addressFor(log);
        auto fetcher = openLive(address, QStringLiteral("spool-followed"));
        QVERIFY(fetcher);
        const auto generationBefore = fetcher->status().generation;

        FakeTelnetd second(toolsDir(), m_dir.path());
        second.start();
        setNetwork(second.port());

        QVERIFY2(waitFor([&] {
                     return second.connections() > 0
                         && fetcher->status().state == FetchStatus::State::Live
                         && fetcher->status().generation != generationBefore;
                 },
                         30000),
                 qPrintable(fetcherState(*fetcher)));
        // The old address was never refused: it was LEFT, which is the difference.
        QVERIFY(first.isRunning());
        QCOMPARE(readAll(fetcher->spoolPath(fetcher->status().generation)),
                 QStringLiteral("one\n"));

        closeFetcher(fetcher);
        second.stop();
        first.stop();
    }

    void aServiceThatDisappearsLeavesTheOpenLogAlone()
    {
        const QString log = m_dir.filePath(QStringLiteral("blink.log"));
        {
            QFile f(log);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("one\n");
        }
        FakeTelnetd server(toolsDir(), m_dir.path());
        server.start();
        setNetwork(server.port());

        DiscoveryWatch watch;
        watch.setWatchedTypes({boardsType()});
        auto fetcher = openLive(addressFor(log), QStringLiteral("spool-blink"));
        QVERIFY(fetcher);
        const int connections = server.connections();
        const auto generation = fetcher->status().generation;

        // Bonjour says the device has gone. It has not.
        {
            const std::scoped_lock lock(m_net->mutex);
            m_net->present = false;
        }
        QTest::qWait(int(kMoveGraceMs) + 1500);
        // NOT AN EVENT: the cache is untouched, the session is untouched, the tab is untouched.
        QVERIFY(lastSeenDevice(QStringLiteral("Boards"), QStringLiteral("board-1")).has_value());
        QCOMPARE(fetcher->status().state, FetchStatus::State::Live);
        QCOMPARE(fetcher->status().generation, generation);
        QCOMPARE(server.connections(), connections);
        // And the log goes on being tailed through it.
        {
            QFile f(log);
            QVERIFY(f.open(QIODevice::Append));
            f.write("two\n");
        }
        QVERIFY2(waitFor([&] {
                     return readAll(fetcher->spoolPath(fetcher->status().generation))
                         == QStringLiteral("one\ntwo\n");
                 },
                         15000),
                 qPrintable(fetcherState(*fetcher)));

        closeFetcher(fetcher);
        server.stop();
    }

    void aServiceThatFlapsAtTheSameAddressIsNotReconnected()
    {
        const QString log = m_dir.filePath(QStringLiteral("flap.log"));
        {
            QFile f(log);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("one\n");
        }
        FakeTelnetd server(toolsDir(), m_dir.path());
        server.start();
        setNetwork(server.port());

        DiscoveryWatch watch;
        watch.setWatchedTypes({boardsType()});
        auto fetcher = openLive(addressFor(log), QStringLiteral("spool-flap"));
        QVERIFY(fetcher);
        const int connections = server.connections();
        const auto generation = fetcher->status().generation;

        for (int i = 0; i < 6; ++i) {
            {
                const std::scoped_lock lock(m_net->mutex);
                m_net->present = !m_net->present;
            }
            QTest::qWait(700);
        }
        QTest::qWait(int(kMoveGraceMs));
        QCOMPARE(fetcher->status().state, FetchStatus::State::Live);
        QCOMPARE(fetcher->status().generation, generation);
        QCOMPARE(server.connections(), connections);

        closeFetcher(fetcher);
        server.stop();
    }

    void aDeviceReachedOverTwoAddressesDoesNotFlap()
    {
        const QString log = m_dir.filePath(QStringLiteral("twohomes.log"));
        {
            QFile f(log);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("one\n");
        }
        FakeTelnetd server(toolsDir(), m_dir.path());
        server.start();
        {
            const std::scoped_lock lock(m_net->mutex);
            m_net->host = QStringLiteral("127.0.0.1");
            m_net->port = server.port();
            m_net->addresses = {QStringLiteral("127.0.0.1"), QStringLiteral("192.0.2.1")};
        }

        DiscoveryWatch watch;
        watch.setWatchedTypes({boardsType()});
        auto fetcher = openLive(addressFor(log), QStringLiteral("spool-twohomes"));
        QVERIFY(fetcher);
        const int connections = server.connections();
        const auto generation = fetcher->status().generation;

        // The backend names one interface and then the other, as one per link does — and then
        // goes on naming the OTHER one for longer than the grace period, which is what tells
        // the address-set rule from a comparison of the preferred host: inside the grace either
        // would hold, and past it only the set keeps a connection that is still to the device.
        for (int i = 0; i < 4; ++i) {
            {
                const std::scoped_lock lock(m_net->mutex);
                m_net->host = i % 2 ? QStringLiteral("127.0.0.1") : QStringLiteral("192.0.2.1");
            }
            QTest::qWait(700);
        }
        {
            const std::scoped_lock lock(m_net->mutex);
            m_net->host = QStringLiteral("192.0.2.1");
        }
        QTest::qWait(int(kMoveGraceMs) + 2000);
        QCOMPARE(fetcher->status().state, FetchStatus::State::Live);
        QCOMPARE(fetcher->status().generation, generation);
        QCOMPARE(server.connections(), connections);

        closeFetcher(fetcher);
        server.stop();
    }

    void restartAppRunsOnTheDeviceWhereItIsNow()
    {
        // File ▸ Restart App on a discovered device (SPEC.md §4): it used to be refused by name,
        // the errand reading an address the `discover://` one does not carry. Over telnet it
        // runs in the console session registered under the DEVICE — the one its log signed in,
        // which after a move is at the device's NEW address.
        const QString log = m_dir.filePath(QStringLiteral("restart.log"));
        {
            QFile f(log);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("one\n");
        }
        FakeTelnetd first(toolsDir(), m_dir.path());
        first.start();
        setNetwork(first.port());
        DiscoveryWatch watch;
        watch.setWatchedTypes({boardsType()});
        const RemoteLocation address = addressFor(log);
        auto fetcher = openLive(address, QStringLiteral("spool-restart"));
        QVERIFY(fetcher);
        const auto generation = fetcher->status().generation;

        FakeTelnetd second(toolsDir(), m_dir.path());
        second.start();
        setNetwork(second.port());
        QVERIFY2(waitFor([&] {
                     return second.connections() > 0
                         && fetcher->status().state == FetchStatus::State::Live
                         && fetcher->status().generation != generation;
                 },
                         30000),
                 qPrintable(fetcherState(*fetcher)));

        const QString marker = m_dir.filePath(QStringLiteral("restarted"));
        const RestartTarget target = resolveRestartTarget(
            address.toString(), QStringLiteral("echo \"$LOGFILE\" > %1").arg(marker));
        QCOMPARE(target.state, RestartTarget::State::Resolved);
        QString why;
        QVERIFY2(restartTargetIsRunnable(target, &why), qPrintable(why));

        RestartRunner runner;
        std::optional<RestartResult> result;
        connect(&runner, &RestartRunner::finished, this,
                [&](const RestartResult &r) { result = r; });
        runner.start(target);
        QVERIFY(waitFor([&] { return result.has_value(); }, 30000));
        QVERIFY2(result->ok, qPrintable(result->error));
        QCOMPARE(result->exitCode, 0);
        QCOMPARE(readAll(marker).trimmed(), log);

        closeFetcher(fetcher);
        second.stop();
        first.stop();
    }

    void aDeviceThatIsNotThereIsWaitedForAndSaysSo()
    {
        {
            const std::scoped_lock lock(m_net->mutex);
            m_net->present = false;
        }
        const RemoteLocation address = addressFor(QStringLiteral("/var/log/absent.log"));
        primeOptions(address);
        QString error;
        auto fetcher = makeDiscoveredFetcher(address, &error);
        QVERIFY(fetcher);
        const QString spoolDir = m_dir.filePath(QStringLiteral("spool-absent"));
        QDir().mkpath(spoolDir);
        QVERIFY(fetcher->start(spoolDir, &error));
        QVERIFY2(waitFor([&] { return fetcher->status().state == FetchStatus::State::Waiting; },
                         20000),
                 qPrintable(fetcherState(*fetcher)));
        QVERIFY2(fetcher->status().error.contains(QStringLiteral("board-1")),
                 qPrintable(fetcher->status().error));
        fetcher->requestStop();
        QVERIFY(waitFor([&] { return fetcher->isStopped(); }, 10000));
    }

    void aTypeThatIsNotConfiguredIsRefusedWithNoIo()
    {
        setKnownRemoteTypes({});
        QString error;
        auto fetcher = makeDiscoveredFetcher(addressFor(QStringLiteral("/a.log")), &error);
        QVERIFY(!fetcher);
        QVERIFY2(error.contains(QStringLiteral("Boards")), qPrintable(error));
    }

    void theResolvedFarEndSignsInWithTheTypesAccountAndKey()
    {
        RemoteType t = boardsType();
        t.port = 0;
        t.serviceType = QStringLiteral("_telnet._tcp");
        DiscoveredDevice d;
        d.id = QStringLiteral("board-1");
        d.host = QStringLiteral("10.0.0.7");
        d.port = 2323;
        const RemoteLocation e = endpointFor(t, d, addressFor(QStringLiteral("/x.log")));
        QCOMPARE(e.transport, RemoteLocation::Transport::Telnet);
        QCOMPARE(e.user, QStringLiteral("root"));
        QCOMPARE(e.host, QStringLiteral("10.0.0.7"));
        QCOMPARE(e.port, 2323);
        QCOMPARE(e.path, QStringLiteral("/x.log"));
        // Filed per TYPE, not per address the device has today.
        QCOMPARE(e.credentialTarget(), t.credentialTarget());
        QVERIFY(e.target() != e.credentialTarget());
    }

private:
    void setNetwork(int port)
    {
        const std::scoped_lock lock(m_net->mutex);
        m_net->host = QStringLiteral("127.0.0.1");
        m_net->port = port;
        m_net->present = true;
    }

    // A fetcher for `address`, primed and Live, or null with the reason logged.
    std::unique_ptr<SourceFetcher> openLive(const RemoteLocation &address, const QString &spool)
    {
        primeOptions(address);
        QString error;
        auto fetcher = makeDiscoveredFetcher(address, &error);
        if (!fetcher) {
            qWarning() << error;
            return nullptr;
        }
        const QString spoolDir = m_dir.filePath(spool);
        QDir().mkpath(spoolDir);
        if (!fetcher->start(spoolDir, &error)) {
            qWarning() << error;
            return nullptr;
        }
        if (!waitFor([&] { return fetcher->status().state == FetchStatus::State::Live; },
                     20000)) {
            qWarning() << fetcherState(*fetcher);
            return nullptr;
        }
        return fetcher;
    }

    static void closeFetcher(std::unique_ptr<SourceFetcher> &fetcher)
    {
        fetcher->requestStop();
        waitFor([&] { return fetcher->isStopped(); }, 10000);
        fetcher.reset();
    }

    static QString fetcherState(const SourceFetcher &f)
    {
        const FetchStatus s = f.status();
        return QStringLiteral("state %1, committed %2, error \"%3\"")
            .arg(QString::fromLatin1(fetchStateName(s.state)))
            .arg(s.committedSize)
            .arg(s.error);
    }
};

QTEST_GUILESS_MAIN(TestDiscoveredFetcher)
#include "tst_discoveredfetcher.moc"
