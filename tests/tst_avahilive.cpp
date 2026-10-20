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

// Bonjour discovery against a REAL Avahi daemon (ARCHITECTURE.md §6.13).
//
// GATED TWICE, tst_keychainlive's shape and for its reason: not built without the Avahi backend,
// and skipped unless LOFTAIL_TEST_MDNS=1 — which packaging/test-mdns/run-mdns-tests.sh sets
// after starting a private system bus, an avahi-daemon on it, and avahi-publish advertising the
// two services below. Everything this asserts is about the AvahiPoll adapter and the browse ->
// resolve -> report chain, which no fake reaches: a scripted browser satisfies the seam whether
// or not the platform half works at all.
//
// The services the harness publishes:
//   loftail-cam-1   _loftailtest._tcp  2222  model=X100
//   loftail-printer _loftailtest._tcp  631   model=LaserJet

#include <QtTest>

#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QThread>

#include <memory>

#include "DiscoveryBrowser.h"
#include "RemoteType.h"

using namespace loftail;

class TestAvahiLive : public QObject
{
    Q_OBJECT

private:
    static RemoteType type()
    {
        RemoteType t;
        t.name = QStringLiteral("Live cameras");
        t.serviceType = QStringLiteral("_loftailtest._tcp");
        t.txtRe = QStringLiteral("^model=X");
        return t;
    }

private slots:
    void init()
    {
        if (qgetenv("LOFTAIL_TEST_MDNS") != "1")
            QSKIP("set by packaging/test-mdns/run-mdns-tests.sh, which starts an Avahi daemon");
        clearDiscoveredDevices();
    }

    void theBrowserFindsAPublishedServiceAndItsTxtRecords()
    {
        auto browser = makeDiscoveryBrowser(type());
        QList<DiscoveredDevice> found;
        QString failure;
        connect(browser.get(), &DiscoveryBrowser::found, this,
                [&](const DiscoveredDevice &d) { found.append(d); });
        connect(browser.get(), &DiscoveryBrowser::failed, this,
                [&](const QString &why) { failure = why; });
        browser->start(type());
        QTRY_VERIFY_WITH_TIMEOUT(!found.isEmpty() || !failure.isEmpty(), 10000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // Settle, then: the camera, and NOT the printer, whose TXT the type's rule excludes.
        QTest::qWait(1000);
        QStringList ids;
        for (const DiscoveredDevice &d : found)
            ids.append(d.id);
        QVERIFY2(ids.contains(QStringLiteral("loftail-cam-1")), qPrintable(ids.join(u',')));
        QVERIFY(!ids.contains(QStringLiteral("loftail-printer")));
        const DiscoveredDevice cam = found.at(int(ids.indexOf(QStringLiteral("loftail-cam-1"))));
        QCOMPARE(cam.port, 2222);
        QVERIFY(cam.details.contains(QStringLiteral("model=X100")));
        QVERIFY(!cam.host.isEmpty());
        QCOMPARE(cam.typeName, type().name);
    }

    void theConnectTimeLookupWorksOnAWorkerThread()
    {
        // Where a fetcher asks from: the AvahiPoll's notifiers and timers must belong to the
        // calling thread's loop, or this never answers.
        std::optional<DiscoveredDevice> found;
        QString why;
        QThread *worker = QThread::create([&] {
            found = resolveDiscoveredDevice(type(), QStringLiteral("loftail-cam-1"), 10000, {},
                                            &why);
        });
        worker->start();
        QVERIFY(worker->wait(20000));
        delete worker;
        QVERIFY2(found.has_value(), qPrintable(why));
        QCOMPARE(found->port, 2222);
    }

    void anAddressThatMovesUnderARegisteredServiceIsReportedAgain()
    {
        // THE RESOLVER IS KEPT ALIVE, and this is the one thing that executes it: the SERVICE
        // stays registered throughout and only its host's ADDRESS record moves, which is what
        // DHCP does to a device and what a resolver freed after its first answer never saw —
        // no remove, no add, so nothing would ever have been reported again (DiscoveryWatch.h).
        const QString publish = QStandardPaths::findExecutable(QStringLiteral("avahi-publish"));
        if (publish.isEmpty())
            QSKIP("avahi-publish is not installed");
        const QString host = QStringLiteral("loftail-moving.local");
        auto spawn = [&](const QStringList &args) {
            auto p = std::make_unique<QProcess>();
            p->setProcessChannelMode(QProcess::MergedChannels);
            p->start(publish, args);
            return p;
        };
        auto address = spawn({QStringLiteral("-a"), QStringLiteral("-R"), host,
                              QStringLiteral("192.0.2.10")});
        auto service = spawn({QStringLiteral("-s"), QStringLiteral("-H"), host,
                              QStringLiteral("loftail-moving"), QStringLiteral("_loftailtest._tcp"),
                              QStringLiteral("2223"), QStringLiteral("model=X200")});
        auto stopAll = qScopeGuard([&] {
            for (QProcess *p : {address.get(), service.get()}) {
                if (p) {
                    p->kill();
                    p->waitForFinished(2000);
                }
            }
        });

        auto browser = makeDiscoveryBrowser(type());
        QStringList hosts;
        int losses = 0;
        connect(browser.get(), &DiscoveryBrowser::found, this, [&](const DiscoveredDevice &d) {
            if (d.id == QStringLiteral("loftail-moving"))
                hosts.append(d.host);
        });
        connect(browser.get(), &DiscoveryBrowser::lost, this,
                [&](const QString &, const QString &id) {
                    if (id == QStringLiteral("loftail-moving"))
                        ++losses;
                });
        browser->start(type());
        QTRY_VERIFY_WITH_TIMEOUT(hosts.contains(QStringLiteral("192.0.2.10")), 15000);

        // The address moves; the service does not. And not at once: a device off its network
        // for a while has NO address record, long enough for Avahi to give the resolver up as
        // failed — the gap that a manual run against the real application found, where a
        // resolver freed on failure was never made again and the move was never reported.
        address->kill();
        address->waitForFinished(2000);
        QTest::qWait(12000);
        address = spawn({QStringLiteral("-a"), QStringLiteral("-R"), host,
                         QStringLiteral("192.0.2.11")});
        QTRY_VERIFY_WITH_TIMEOUT(hosts.contains(QStringLiteral("192.0.2.11")), 20000);
        QCOMPARE(hosts.last(), QStringLiteral("192.0.2.11"));
        // Reported AGAIN, by the resolver that was already running — not by the service being
        // withdrawn and re-registered, which is the only way it used to surface.
        QCOMPARE(losses, 0);
        // And what the standing watch reads is the new address.
        QCOMPARE(lastSeenDevice(type().name, QStringLiteral("loftail-moving"))->host,
                 QStringLiteral("192.0.2.11"));
        browser->stop();
    }

    void anInstanceNobodyPublishedIsNotFound()
    {
        QString why;
        QVERIFY(!resolveDiscoveredDevice(type(), QStringLiteral("no-such-device"), 2000, {}, &why)
                     .has_value());
        QVERIFY(why.contains(QStringLiteral("no-such-device")));
    }
};

QTEST_GUILESS_MAIN(TestAvahiLive)
#include "tst_avahilive.moc"
