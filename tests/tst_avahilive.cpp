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

#include <QThread>

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
