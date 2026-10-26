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

// Device discovery, everywhere it can be reached without a platform responder
// (ARCHITECTURE.md §6.13): the match rules every backend shares, the WS-Discovery messages
// ONVIF is found with, the connect-time lookup through the browser seam, and — where the
// machine allows multicast on loopback — the ONVIF browser against a real UDP responder.

#include <QtTest>

#include <QNetworkInterface>
#include <QThread>
#include <QUdpSocket>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "DiscoveryBrowser.h"
#include "DiscoveryMatch.h"
#include "DiscoveryWatch.h"
#include "RemoteType.h"

using namespace loftail;

namespace {

// A ProbeMatches reply as a real camera words it: SOAP 1.2, the 2005/04 discovery namespace,
// prefixes of its own choosing, and several scopes on one line.
QByteArray probeMatches(const QString &relatesTo, const QString &endpoint, const QString &xaddr,
                        const QString &scopes)
{
    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
               "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
               "xmlns:wsdd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
               "xmlns:tdn=\"http://www.onvif.org/ver10/network/wsdl\">"
               "<SOAP-ENV:Header><wsa:RelatesTo>%1</wsa:RelatesTo></SOAP-ENV:Header>"
               "<SOAP-ENV:Body><wsdd:ProbeMatches><wsdd:ProbeMatch>"
               "<wsa:EndpointReference><wsa:Address>%2</wsa:Address></wsa:EndpointReference>"
               "<wsdd:Types>tdn:NetworkVideoTransmitter</wsdd:Types>"
               "<wsdd:Scopes>%4</wsdd:Scopes>"
               "<wsdd:XAddrs>%3</wsdd:XAddrs>"
               "<wsdd:MetadataVersion>1</wsdd:MetadataVersion>"
               "</wsdd:ProbeMatch></wsdd:ProbeMatches></SOAP-ENV:Body></SOAP-ENV:Envelope>")
        .arg(relatesTo, endpoint, xaddr, scopes)
        .toUtf8();
}

// A browser whose devices the test supplies — the seam the platform backends sit behind.
class ScriptedBrowser final : public DiscoveryBrowser
{
public:
    ScriptedBrowser(QList<DiscoveredDevice> devices, int delayMs, QObject *parent)
        : DiscoveryBrowser(parent), m_devices(std::move(devices)), m_delayMs(delayMs)
    {
    }
    void start(const RemoteType &type) override
    {
        m_type = type;
        QTimer::singleShot(m_delayMs, this, [this] {
            for (const DiscoveredDevice &d : m_devices)
                report(m_type, d);
        });
    }
    void stop() override {}

private:
    QList<DiscoveredDevice> m_devices;
    int m_delayMs;
    RemoteType m_type;
};

DiscoveredDevice bonjourDevice(const QString &id, const QString &host, const QStringList &txt)
{
    DiscoveredDevice d;
    d.id = id;
    d.host = host;
    d.port = 22;
    d.details = txt;
    return d;
}

RemoteType camerasOverBonjour()
{
    RemoteType t;
    t.name = QStringLiteral("Cameras");
    t.serviceType = QStringLiteral("_ssh._tcp");
    t.txtRe = QStringLiteral("^model=X");
    return t;
}

} // namespace

class TestDiscovery : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();

    void anEmptyPatternMatchesAnythingAndAnInvalidOneNothing();
    void aBonjourDeviceMustMatchItsInstanceAndOneTxtRecord();
    void anOnvifDeviceMustMatchOneScope();
    void theProbeAsksForAVideoTransmitterUnderItsOwnMessageId();
    void aProbeMatchIsReadWhateverItsPrefixes();
    void aReplyToSomebodyElsesProbeIsNotAnAnswer();
    void aMalformedDatagramIsAnEmptyAnswer();
    void aProbeMatchBecomesADeviceNamedByItsNameScope();
    void theScopesAreShownHardwareFirstWithoutTheStandardPrefix();

    void theConnectTimeLookupFindsTheDeviceByItsId();
    void aDeviceThatDoesNotAnswerIsNotFoundAndSaysSo();
    void aDeviceTheRulesExcludeIsNeverReported();
    void aBuildWithoutABackendSaysWhyRatherThanFindingNothing();

    void everyXAddrIsAnAddressOfTheDevice();
    void aReportedDeviceAlwaysListsItsHostAmongItsAddresses();
    void theWatchRunsOneBrowserPerTypeAndRestartsOneThatChanged();
    void aDeviceThatLeavesStaysInTheCacheAndIsAnsweredOnlyWhileWatched();

    void theOnvifBrowserFindsACameraOverRealMulticast();
};

void TestDiscovery::cleanup()
{
    setDiscoveryBrowserFactory(nullptr);
    clearDiscoveredDevices();
}

void TestDiscovery::anEmptyPatternMatchesAnythingAndAnInvalidOneNothing()
{
    QVERIFY(discoveryPatternMatches(QString(), QStringLiteral("anything")));
    QVERIFY(discoveryPatternMatchesAny(QString(), {}));
    QVERIFY(discoveryPatternMatches(QStringLiteral("^cam-\\d+$"), QStringLiteral("cam-12")));
    QVERIFY(!discoveryPatternMatches(QStringLiteral("^cam-\\d+$"), QStringLiteral("printer")));
    // INVALID MATCHES NOTHING, never everything — or a typo offers every printer on the
    // network as one of the user's cameras.
    QVERIFY(!discoveryPatternMatches(QStringLiteral("cam-("), QStringLiteral("cam-(")));
    QVERIFY(!discoveryPatternMatchesAny(QStringLiteral("[unclosed"), {QStringLiteral("x")}));
    // A pattern with no subject to match is no match.
    QVERIFY(!discoveryPatternMatchesAny(QStringLiteral("x"), {}));
}

void TestDiscovery::aBonjourDeviceMustMatchItsInstanceAndOneTxtRecord()
{
    RemoteType t = camerasOverBonjour();
    t.instanceRe = QStringLiteral("^cam");
    const auto good = bonjourDevice(QStringLiteral("cam-1"), QStringLiteral("10.0.0.5"),
                                    {QStringLiteral("fw=2.1"), QStringLiteral("model=X100")});
    QVERIFY(deviceMatchesType(t, good));
    auto wrongName = good;
    wrongName.id = QStringLiteral("printer");
    QVERIFY(!deviceMatchesType(t, wrongName));
    auto wrongModel = good;
    wrongModel.details = {QStringLiteral("model=Y")};
    QVERIFY(!deviceMatchesType(t, wrongModel));
}

void TestDiscovery::anOnvifDeviceMustMatchOneScope()
{
    RemoteType t;
    t.discovery = RemoteType::Discovery::Onvif;
    t.scopeRe = QStringLiteral("/hardware/Acme");
    DiscoveredDevice d;
    d.id = QStringLiteral("urn:uuid:1");
    d.details = {QStringLiteral("onvif://www.onvif.org/type/video_encoder"),
                 QStringLiteral("onvif://www.onvif.org/hardware/Acme-X")};
    QVERIFY(deviceMatchesType(t, d));
    d.details.removeLast();
    QVERIFY(!deviceMatchesType(t, d));
}

void TestDiscovery::theProbeAsksForAVideoTransmitterUnderItsOwnMessageId()
{
    const QByteArray probe = wsDiscoveryProbe(QStringLiteral("urn:uuid:abc"));
    QVERIFY(probe.contains("<w:MessageID>urn:uuid:abc</w:MessageID>"));
    QVERIFY(probe.contains("dn:NetworkVideoTransmitter"));
    QVERIFY(probe.contains("http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe"));
    QXmlStreamReader xml(probe);
    while (!xml.atEnd())
        xml.readNext();
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
}

void TestDiscovery::aProbeMatchIsReadWhateverItsPrefixes()
{
    const QByteArray reply = probeMatches(
        QStringLiteral("urn:uuid:ours"), QStringLiteral("urn:uuid:cam-7"),
        QStringLiteral("http://10.1.2.3:8080/onvif/device_service http://[fe80::1]/onvif"),
        QStringLiteral("onvif://www.onvif.org/name/Lobby%20Cam  onvif://www.onvif.org/location/hall"));
    const auto matches = parseWsProbeMatches(reply, QStringLiteral("urn:uuid:ours"));
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.at(0).endpoint, QStringLiteral("urn:uuid:cam-7"));
    QCOMPARE(matches.at(0).xaddrs.size(), 2);
    QCOMPARE(matches.at(0).scopes.size(), 2);
    QCOMPARE(matches.at(0).types, QStringList{QStringLiteral("tdn:NetworkVideoTransmitter")});
}

void TestDiscovery::aReplyToSomebodyElsesProbeIsNotAnAnswer()
{
    const QByteArray reply = probeMatches(QStringLiteral("urn:uuid:theirs"),
                                          QStringLiteral("urn:uuid:cam-7"),
                                          QStringLiteral("http://10.1.2.3/"), QString());
    QVERIFY(parseWsProbeMatches(reply, QStringLiteral("urn:uuid:ours")).isEmpty());
    QCOMPARE(parseWsProbeMatches(reply).size(), 1);
}

void TestDiscovery::aMalformedDatagramIsAnEmptyAnswer()
{
    QVERIFY(parseWsProbeMatches(QByteArrayLiteral("<a><b></a>")).isEmpty());
    QVERIFY(parseWsProbeMatches(QByteArrayLiteral("")).isEmpty());
    QVERIFY(parseWsProbeMatches(QByteArrayLiteral("\xff\xfe garbage")).isEmpty());
    // A match with no endpoint has no identity and is dropped.
    const QByteArray noEndpoint = probeMatches(QString(), QString(),
                                               QStringLiteral("http://10.1.2.3/"), QString());
    QVERIFY(parseWsProbeMatches(noEndpoint).isEmpty());
}

void TestDiscovery::aProbeMatchBecomesADeviceNamedByItsNameScope()
{
    WsProbeMatch m;
    m.endpoint = QStringLiteral("urn:uuid:cam-7");
    m.xaddrs = {QStringLiteral("http://10.1.2.3:8080/onvif/device_service")};
    m.scopes = {QStringLiteral("onvif://www.onvif.org/name/Lobby%20Cam")};
    const DiscoveredDevice d = deviceFromProbeMatch(QStringLiteral("Cameras"), m);
    QCOMPARE(d.typeName, QStringLiteral("Cameras"));
    QCOMPARE(d.id, QStringLiteral("urn:uuid:cam-7"));
    QCOMPARE(d.host, QStringLiteral("10.1.2.3"));
    QCOMPARE(d.port, 8080);
    QCOMPARE(d.displayName, QStringLiteral("Lobby Cam"));

    m.scopes.clear();
    QCOMPARE(deviceFromProbeMatch(QStringLiteral("Cameras"), m).displayName,
             QStringLiteral("10.1.2.3"));
}

void TestDiscovery::theScopesAreShownHardwareFirstWithoutTheStandardPrefix()
{
    // Advertised in the order a real camera sends them: the hardware buried in the middle,
    // a vendor scope of its own, and a name that is percent-encoded.
    const QStringList advertised = {
        QStringLiteral("onvif://www.onvif.org/type/video_encoder"),
        QStringLiteral("onvif://www.onvif.org/Profile/Streaming"),
        QStringLiteral("onvif://www.onvif.org/hardware/DS-2CD2143G0-I"),
        QStringLiteral("http://www.example.com/vendor/scope"),
        QStringLiteral("ONVIF://WWW.ONVIF.ORG/name/Lobby%20Cam"),
        QStringLiteral("onvif://www.onvif.org/"),
        QStringLiteral("onvif://www.onvif.org/location/country/ua")};
    QCOMPARE(onvifScopesForDisplay(advertised),
             (QStringList{QStringLiteral("hardware/DS-2CD2143G0-I"),
                          QStringLiteral("type/video_encoder"),
                          QStringLiteral("Profile/Streaming"),
                          QStringLiteral("http://www.example.com/vendor/scope"),
                          QStringLiteral("name/Lobby Cam"),
                          QStringLiteral("location/country/ua")}));
    // With no hardware scope nothing is reordered; with none at all there is nothing to show.
    QCOMPARE(onvifScopesForDisplay({QStringLiteral("onvif://www.onvif.org/type/a"),
                                    QStringLiteral("onvif://www.onvif.org/name/b")}),
             (QStringList{QStringLiteral("type/a"), QStringLiteral("name/b")}));
    QVERIFY(onvifScopesForDisplay({}).isEmpty());

    // DISPLAY ONLY: the type's pattern still sees the scopes as advertised.
    RemoteType t;
    t.discovery = RemoteType::Discovery::Onvif;
    t.scopeRe = QStringLiteral("^onvif://www\\.onvif\\.org/hardware/DS-");
    DiscoveredDevice d;
    d.details = advertised;
    QVERIFY(deviceMatchesType(t, d));
}

void TestDiscovery::everyXAddrIsAnAddressOfTheDevice()
{
    // A camera on two networks answers with an XAddr on each, and a connection to either is a
    // connection to the camera — so both are its addresses, and the first is where it is.
    WsProbeMatch m;
    m.endpoint = QStringLiteral("urn:uuid:cam-8");
    m.xaddrs = {QStringLiteral("http://10.1.2.3/onvif/device_service"),
                QStringLiteral("http://192.168.7.3/onvif/device_service"),
                QStringLiteral("not a url"),
                QStringLiteral("http://10.1.2.3:8080/again")};
    const DiscoveredDevice d = deviceFromProbeMatch(QStringLiteral("Cameras"), m);
    QCOMPARE(d.host, QStringLiteral("10.1.2.3"));
    QCOMPARE(d.addresses,
             (QStringList{QStringLiteral("10.1.2.3"), QStringLiteral("192.168.7.3")}));
}

void TestDiscovery::aReportedDeviceAlwaysListsItsHostAmongItsAddresses()
{
    // report() is the one place every backend passes through, so it is where `host` and
    // `addresses` are made to agree: the move rule asks the set, the connect asks the host.
    DiscoveredDevice onlyHost = bonjourDevice(QStringLiteral("cam-1"), QStringLiteral("10.0.0.5"),
                                              {QStringLiteral("model=X1")});
    DiscoveredDevice onlySet = bonjourDevice(QStringLiteral("cam-2"), QString(),
                                             {QStringLiteral("model=X1")});
    onlySet.addresses = {QStringLiteral("10.0.0.6"), QStringLiteral("10.0.0.7")};
    setDiscoveryBrowserFactory([=](const RemoteType &, QObject *parent) {
        return std::make_unique<ScriptedBrowser>(QList<DiscoveredDevice>{onlyHost, onlySet}, 0,
                                                 parent);
    });
    auto browser = makeDiscoveryBrowser(camerasOverBonjour());
    QList<DiscoveredDevice> found;
    connect(browser.get(), &DiscoveryBrowser::found, this,
            [&](const DiscoveredDevice &d) { found.append(d); });
    browser->start(camerasOverBonjour());
    QTRY_COMPARE(found.size(), 2);
    QCOMPARE(found.at(0).addresses, QStringList{QStringLiteral("10.0.0.5")});
    QCOMPARE(found.at(1).host, QStringLiteral("10.0.0.6"));
    QCOMPARE(found.at(1).addresses,
             (QStringList{QStringLiteral("10.0.0.6"), QStringLiteral("10.0.0.7")}));
}

namespace {

// Counts how many browsers the watch makes and how many are alive, per type name.
struct BrowserCensus
{
    QHash<QString, int> made;
    QHash<QString, int> alive;
};

class CountedBrowser final : public DiscoveryBrowser
{
public:
    CountedBrowser(std::shared_ptr<BrowserCensus> census, QString name, QObject *parent)
        : DiscoveryBrowser(parent), m_census(std::move(census)), m_name(std::move(name))
    {
        ++m_census->made[m_name];
        ++m_census->alive[m_name];
    }
    ~CountedBrowser() override { --m_census->alive[m_name]; }
    void start(const RemoteType &) override {}
    void stop() override {}

private:
    std::shared_ptr<BrowserCensus> m_census;
    QString                        m_name;
};

} // namespace

void TestDiscovery::theWatchRunsOneBrowserPerTypeAndRestartsOneThatChanged()
{
    auto census = std::make_shared<BrowserCensus>();
    setDiscoveryBrowserFactory([census](const RemoteType &type, QObject *parent) {
        return std::make_unique<CountedBrowser>(census, type.name, parent);
    });
    RemoteType cams = camerasOverBonjour();
    RemoteType boards;
    boards.name = QStringLiteral("Boards");

    {
        DiscoveryWatch watch;
        QVERIFY(!discoveryWatchIsRunning(cams.name));

        watch.setWatchedTypes({cams});
        QCOMPARE(census->made.value(cams.name), 1);
        QVERIFY(discoveryWatchIsRunning(QStringLiteral("CAMERAS")));   // names fold

        // The same definition again — what every relabel does — makes nothing new.
        watch.setWatchedTypes({cams});
        QCOMPARE(census->made.value(cams.name), 1);

        // A changed definition is browsed again, with the old browser gone.
        cams.txtRe = QStringLiteral("^model=Y");
        watch.setWatchedTypes({cams, boards});
        QCOMPARE(census->made.value(cams.name), 2);
        QCOMPARE(census->alive.value(cams.name), 1);
        QCOMPARE(census->alive.value(boards.name), 1);
        QCOMPARE(watch.watchedTypeNames(), (QStringList{boards.name, cams.name}));

        // A type no open log names any more is not browsed.
        watch.setWatchedTypes({boards});
        QCOMPARE(census->alive.value(cams.name), 0);
        QVERIFY(!discoveryWatchIsRunning(cams.name));
        QVERIFY(discoveryWatchIsRunning(boards.name));
    }
    // And the watch takes its browsers with it.
    QCOMPARE(census->alive.value(boards.name), 0);
    QVERIFY(!discoveryWatchIsRunning(boards.name));
}

namespace {

// Reports one device and then says it has gone, which is what Bonjour does spuriously.
class LeavingBrowser final : public DiscoveryBrowser
{
public:
    using DiscoveryBrowser::DiscoveryBrowser;
    void start(const RemoteType &type) override
    {
        m_type = type;
        QTimer::singleShot(0, this, [this] {
            report(m_type, bonjourDevice(QStringLiteral("cam-1"), QStringLiteral("10.0.0.5"),
                                         {QStringLiteral("model=X1")}));
            emit lost(m_type.name, QStringLiteral("cam-1"));
            m_done = true;
        });
    }
    void stop() override {}
    bool m_done = false;

private:
    RemoteType m_type;
};

} // namespace

void TestDiscovery::aDeviceThatLeavesStaysInTheCacheAndIsAnsweredOnlyWhileWatched()
{
    LeavingBrowser *made = nullptr;
    setDiscoveryBrowserFactory([&made](const RemoteType &, QObject *parent) {
        auto b = std::make_unique<LeavingBrowser>(parent);
        made = b.get();
        return b;
    });
    const RemoteType cams = camerasOverBonjour();
    DiscoveryWatch watch;
    watch.setWatchedTypes({cams});
    QVERIFY(made);
    QTRY_VERIFY(made->m_done);

    // `lost` IS NOT AN EVENT: the cache keeps the device, and the watch answers for it.
    QVERIFY(lastSeenDevice(cams.name, QStringLiteral("cam-1")).has_value());
    const auto watched = watchedDevice(cams.name, QStringLiteral("cam-1"));
    QVERIFY(watched.has_value());
    QCOMPARE(watched->host, QStringLiteral("10.0.0.5"));

    // With no watch the cache is a guess, and a "has it moved?" question gets no answer.
    watch.setWatchedTypes({});
    QVERIFY(!watchedDevice(cams.name, QStringLiteral("cam-1")).has_value());
    QVERIFY(lastSeenDevice(cams.name, QStringLiteral("cam-1")).has_value());
}

void TestDiscovery::theConnectTimeLookupFindsTheDeviceByItsId()
{
    const QList<DiscoveredDevice> devices = {
        bonjourDevice(QStringLiteral("cam-1"), QStringLiteral("10.0.0.5"),
                      {QStringLiteral("model=X1")}),
        bonjourDevice(QStringLiteral("cam-2"), QStringLiteral("10.0.0.9"),
                      {QStringLiteral("model=X2")}),
    };
    setDiscoveryBrowserFactory([devices](const RemoteType &, QObject *parent) {
        return std::make_unique<ScriptedBrowser>(devices, 50, parent);
    });

    // ON A WORKER QTHREAD, which is where a fetcher asks: the browser and its nested event loop
    // belong to the thread that calls.
    std::optional<DiscoveredDevice> found;
    QString why;
    QThread *worker = QThread::create([&] {
        found = resolveDiscoveredDevice(camerasOverBonjour(), QStringLiteral("cam-2"), 3000, {},
                                        &why);
    });
    worker->start();
    QVERIFY(worker->wait(10000));
    delete worker;
    QVERIFY2(found.has_value(), qPrintable(why));
    QCOMPARE(found->host, QStringLiteral("10.0.0.9"));
    QCOMPARE(found->typeName, QStringLiteral("Cameras"));
    // And it is remembered, so the next lookup has somewhere to start.
    const auto cached = lastSeenDevice(QStringLiteral("cameras"), QStringLiteral("cam-2"));
    QVERIFY(cached.has_value());
    QCOMPARE(cached->host, QStringLiteral("10.0.0.9"));
}

void TestDiscovery::aDeviceThatDoesNotAnswerIsNotFoundAndSaysSo()
{
    setDiscoveryBrowserFactory([](const RemoteType &, QObject *parent) {
        return std::make_unique<ScriptedBrowser>(QList<DiscoveredDevice>{}, 0, parent);
    });
    QString why;
    QElapsedTimer clock;
    clock.start();
    const auto found =
        resolveDiscoveredDevice(camerasOverBonjour(), QStringLiteral("cam-9"), 600, {}, &why);
    QVERIFY(!found.has_value());
    QVERIFY(clock.elapsed() >= 500);
    QVERIFY2(why.contains(QStringLiteral("cam-9")) && why.contains(QStringLiteral("Cameras")),
             qPrintable(why));

    // An ABANDONED lookup returns at the next slice rather than at the timeout.
    clock.restart();
    resolveDiscoveredDevice(camerasOverBonjour(), QStringLiteral("cam-9"), 20000,
                            [] { return true; }, &why);
    QVERIFY(clock.elapsed() < 2000);
}

void TestDiscovery::aDeviceTheRulesExcludeIsNeverReported()
{
    // The backend saw it; the type's TXT rule says it is not one of ours.
    setDiscoveryBrowserFactory([](const RemoteType &, QObject *parent) {
        return std::make_unique<ScriptedBrowser>(
            QList<DiscoveredDevice>{bonjourDevice(QStringLiteral("printer"),
                                                  QStringLiteral("10.0.0.1"),
                                                  {QStringLiteral("model=LaserJet")})},
            0, parent);
    });
    QString why;
    QVERIFY(!resolveDiscoveredDevice(camerasOverBonjour(), QStringLiteral("printer"), 400, {},
                                     &why)
                 .has_value());
    QVERIFY(!lastSeenDevice(QStringLiteral("Cameras"), QStringLiteral("printer")).has_value());
}

void TestDiscovery::aBuildWithoutABackendSaysWhyRatherThanFindingNothing()
{
    QString why;
    if (bonjourDiscoveryAvailable(&why)) {
        QVERIFY(!bonjourBackendName().isEmpty());
        QSKIP("this build has a Bonjour backend; the refusal is the other configuration's");
    }
    QVERIFY(!why.isEmpty());
    QString reason;
    const auto found =
        resolveDiscoveredDevice(camerasOverBonjour(), QStringLiteral("x"), 5000, {}, &reason);
    QVERIFY(!found.has_value());
    QCOMPARE(reason, why);
}

void TestDiscovery::theOnvifBrowserFindsACameraOverRealMulticast()
{
    // A camera, played by a std::thread: it joins the WS-Discovery group on loopback, waits for
    // a Probe and answers it UNICAST to wherever it came from, exactly as a device does. The
    // thread is a std::thread and never a QThread, tst_sshconnect's rule: the code under test
    // has to turn its own event loop for this to answer, which is what is being tested.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    QVERIFY(fd >= 0);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(3702);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ip_mreq group{};
    group.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        ::close(fd);
        QSKIP("cannot bind the WS-Discovery port here (3702 is taken)");
    }
    // JOINED ON EVERY IPv4 INTERFACE, because the browser probes on every multicast-capable one
    // and the kernel loops each probe back to a local member of the group on THAT interface.
    // Joining loopback alone hears nothing: loopback is rarely flagged as able to multicast.
    int joined = 0;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            group.imr_interface.s_addr = htonl(entry.ip().toIPv4Address());
            if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof group) == 0)
                ++joined;
        }
    }
    if (joined == 0) {
        ::close(fd);
        QSKIP("cannot join the WS-Discovery group on any interface here");
    }

    std::atomic<bool> running{true};
    std::atomic<int> probes{0};
    std::thread camera([&] {
        while (running) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0)
                continue;
            char buf[8192];
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0,
                                         reinterpret_cast<sockaddr *>(&from), &len);
            if (n <= 0)
                continue;
            const QByteArray probe(buf, int(n));
            const int at = int(probe.indexOf("<w:MessageID>"));
            if (at < 0)
                continue;
            const int end = int(probe.indexOf("</w:MessageID>", at));
            const QString id = QString::fromUtf8(probe.mid(at + 13, end - at - 13));
            ++probes;
            const QByteArray reply =
                probeMatches(id, QStringLiteral("urn:uuid:lobby-cam"),
                             QStringLiteral("http://127.0.0.1:8080/onvif/device_service"),
                             QStringLiteral("onvif://www.onvif.org/name/Lobby "
                                            "onvif://www.onvif.org/hardware/Acme"));
            ::sendto(fd, reply.constData(), size_t(reply.size()), 0,
                     reinterpret_cast<sockaddr *>(&from), len);
        }
    });

    RemoteType t;
    t.name = QStringLiteral("Cameras");
    t.discovery = RemoteType::Discovery::Onvif;
    t.scopeRe = QStringLiteral("/hardware/Acme");
    QString why;
    const auto found = resolveDiscoveredDevice(t, QStringLiteral("urn:uuid:lobby-cam"), 5000, {},
                                               &why);
    running = false;
    camera.join();
    ::close(fd);

    if (probes == 0)
        QSKIP("no probe reached loopback — this machine routes no multicast there");
    QVERIFY2(found.has_value(), qPrintable(why));
    QCOMPARE(found->host, QStringLiteral("127.0.0.1"));   // the XAddr's, not the sender's
    QCOMPARE(found->displayName, QStringLiteral("Lobby"));
}

QTEST_GUILESS_MAIN(TestDiscovery)
#include "tst_discovery.moc"
