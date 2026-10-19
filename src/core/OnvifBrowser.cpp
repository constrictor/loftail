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

#include "DiscoveryBackends.h"

#include "DiagnosticLog.h"

#include <QCoreApplication>
#include <QHash>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::OnvifBrowser)
};

const QHostAddress kWsDiscoveryGroup(QStringLiteral("239.255.255.250"));
constexpr quint16 kWsDiscoveryPort = 3702;
// How often the probe is repeated while the browser runs. A camera that was powering up when
// the first probe went out answers the next; WS-Discovery is UDP, so one lost datagram is an
// ordinary event rather than a failure.
constexpr int kReprobeMs = 4000;

// ONVIF discovery: loftail's own WS-Discovery probe, because no platform offers one.
//
// The probe goes out on EVERY multicast-capable IPv4 interface rather than on the default
// route's: a camera network is very often a second NIC that the default route does not touch,
// and a probe sent only where the default route goes finds nothing on exactly that machine.
// The answers come back unicast to the socket's own port, so nothing joins a group.
class OnvifBrowser final : public DiscoveryBrowser
{
public:
    explicit OnvifBrowser(QObject *parent) : DiscoveryBrowser(parent)
    {
        connect(&m_socket, &QUdpSocket::readyRead, this, &OnvifBrowser::readAnswers);
        connect(&m_timer, &QTimer::timeout, this, &OnvifBrowser::probe);
    }

    void start(const RemoteType &type) override
    {
        stop();
        m_type = type;
        if (!m_socket.bind(QHostAddress::AnyIPv4, 0)) {
            const QString reason = Tr::tr("Cannot listen for ONVIF answers — %1")
                                       .arg(m_socket.errorString());
            QTimer::singleShot(0, this, [this, reason] { emit failed(reason); });
            return;
        }
        probe();
        m_timer.start(kReprobeMs);
    }

    void stop() override
    {
        m_timer.stop();
        if (m_socket.state() != QAbstractSocket::UnconnectedState)
            m_socket.close();
        m_ours.clear();
    }

private:
    void probe()
    {
        const QString id = QStringLiteral("urn:uuid:")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_ours.insert(id);
        const QByteArray message = wsDiscoveryProbe(id);
        int sent = 0;
        const auto interfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface &iface : interfaces) {
            const auto flags = iface.flags();
            if (!flags.testFlag(QNetworkInterface::IsUp)
                || !flags.testFlag(QNetworkInterface::IsRunning)
                || !flags.testFlag(QNetworkInterface::CanMulticast)) {
                continue;
            }
            bool hasIpv4 = false;
            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                    hasIpv4 = true;
            }
            if (!hasIpv4)
                continue;
            m_socket.setMulticastInterface(iface);
            if (m_socket.writeDatagram(message, kWsDiscoveryGroup, kWsDiscoveryPort) > 0)
                ++sent;
        }
        // No interface took it — a machine with no network, or none that multicasts. Fall back
        // to the default route once, so the case is at least tried rather than silently empty.
        if (sent == 0)
            m_socket.writeDatagram(message, kWsDiscoveryGroup, kWsDiscoveryPort);
    }

    void readAnswers()
    {
        while (m_socket.hasPendingDatagrams()) {
            const QNetworkDatagram datagram = m_socket.receiveDatagram(64 * 1024);
            const QList<WsProbeMatch> matches = parseWsProbeMatches(datagram.data());
            for (const WsProbeMatch &match : matches) {
                DiscoveredDevice device = deviceFromProbeMatch(m_type.name, match);
                // An XAddr naming no host, or a link-local one the device advertises on the
                // wrong interface: where the answer CAME FROM is where the device is.
                if (device.host.isEmpty() && !datagram.senderAddress().isNull())
                    device.host = datagram.senderAddress().toString();
                report(m_type, device);
            }
        }
    }

    RemoteType    m_type;
    QUdpSocket    m_socket;
    QTimer        m_timer;
    QSet<QString> m_ours;
};

} // namespace

std::unique_ptr<DiscoveryBrowser> makeOnvifBrowser(QObject *parent)
{
    return std::make_unique<OnvifBrowser>(parent);
}

} // namespace loftail
