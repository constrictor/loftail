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
#include "FakeTelnetd.h"
#include "RemoteLocation.h"
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
    std::mutex mutex;
    QString    host;
    int        port = 0;
    bool       present = true;
};

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
        QTimer::singleShot(20, this, [this] {
            DiscoveredDevice d;
            {
                const std::scoped_lock lock(m_net->mutex);
                if (!m_net->present)
                    return;
                d.host = m_net->host;
                d.port = m_net->port;
            }
            d.id = QStringLiteral("board-1");
            d.displayName = QStringLiteral("Board one");
            d.details = {QStringLiteral("model=X")};
            report(m_type, d);
        });
    }
    void stop() override {}

private:
    std::shared_ptr<Network> m_net;
    RemoteType m_type;
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
