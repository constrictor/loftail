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

#include "DiscoveryMatch.h"

#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>

namespace loftail {

bool discoveryPatternMatches(const QString &pattern, const QString &subject)
{
    if (pattern.isEmpty())
        return true;
    const QRegularExpression re(pattern);
    if (!re.isValid())
        return false;   // matches NOTHING, never everything
    return re.match(subject).hasMatch();
}

bool discoveryPatternMatchesAny(const QString &pattern, const QStringList &subjects)
{
    if (pattern.isEmpty())
        return true;
    const QRegularExpression re(pattern);
    if (!re.isValid())
        return false;
    for (const QString &s : subjects) {
        if (re.match(s).hasMatch())
            return true;
    }
    return false;
}

bool deviceMatchesType(const RemoteType &type, const DiscoveredDevice &device)
{
    switch (type.discovery) {
    case RemoteType::Discovery::Bonjour:
        return discoveryPatternMatches(type.instanceRe, device.id)
            && discoveryPatternMatchesAny(type.txtRe, device.details);
    case RemoteType::Discovery::Onvif:
        return discoveryPatternMatchesAny(type.scopeRe, device.details);
    }
    return false;
}

QString onvifNameFromScopes(const QStringList &scopes)
{
    static const QString prefix = QStringLiteral("onvif://www.onvif.org/name/");
    for (const QString &s : scopes) {
        if (s.startsWith(prefix, Qt::CaseInsensitive))
            return QUrl::fromPercentEncoding(s.mid(prefix.size()).toUtf8());
    }
    return {};
}

QByteArray wsDiscoveryProbe(const QString &messageId)
{
    // The minimal Probe every ONVIF device answers: the WS-Discovery 2005/04 namespace, which is
    // the one ONVIF's Core Specification names, and the device-type filter for a video
    // transmitter. Built as text rather than through a writer because it never varies but for
    // the id, which is a uuid and needs no escaping.
    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
               "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
               "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
               "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
               "<e:Header>"
               "<w:MessageID>%1</w:MessageID>"
               "<w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
               "<w:Action e:mustUnderstand=\"true\">"
               "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
               "</e:Header>"
               "<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body>"
               "</e:Envelope>")
        .arg(messageId)
        .toUtf8();
}

namespace {

QStringList splitList(const QString &text)
{
    return text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

} // namespace

QList<WsProbeMatch> parseWsProbeMatches(const QByteArray &datagram, const QString &relatesTo)
{
    // NAMESPACE-AGNOSTIC BY LOCAL NAME, deliberately. Devices disagree about prefixes, about
    // the addressing namespace (2004/08 against 2005/08), and some answer in a SOAP 1.1
    // envelope; matching on local names is what every interoperable ONVIF client ends up doing.
    QList<WsProbeMatch> out;
    QXmlStreamReader xml(datagram);
    QString relates;
    WsProbeMatch current;
    bool inMatch = false;
    bool inEndpointRef = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QStringView name = xml.name();
            if (name == u"RelatesTo") {
                relates = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            } else if (name == u"ProbeMatch") {
                inMatch = true;
                current = WsProbeMatch{};
            } else if (inMatch && name == u"EndpointReference") {
                inEndpointRef = true;
            } else if (inMatch && inEndpointRef && name == u"Address") {
                current.endpoint =
                    xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            } else if (inMatch && name == u"Types") {
                current.types =
                    splitList(xml.readElementText(QXmlStreamReader::SkipChildElements));
            } else if (inMatch && name == u"Scopes") {
                current.scopes =
                    splitList(xml.readElementText(QXmlStreamReader::SkipChildElements));
            } else if (inMatch && name == u"XAddrs") {
                current.xaddrs =
                    splitList(xml.readElementText(QXmlStreamReader::SkipChildElements));
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QStringView name = xml.name();
            if (name == u"EndpointReference") {
                inEndpointRef = false;
            } else if (name == u"ProbeMatch" && inMatch) {
                inMatch = false;
                // A match with no endpoint has no identity, so it is nothing a saved address
                // could ever name again.
                if (!current.endpoint.isEmpty())
                    out.append(current);
            }
        }
    }
    if (xml.hasError())
        return {};
    if (!relatesTo.isEmpty() && relates != relatesTo)
        return {};
    return out;
}

DiscoveredDevice deviceFromProbeMatch(const QString &typeName, const WsProbeMatch &match)
{
    DiscoveredDevice d;
    d.typeName = typeName;
    d.id = match.endpoint;
    d.details = match.scopes;
    for (const QString &xaddr : match.xaddrs) {
        const QUrl url(xaddr);
        if (!url.isValid() || url.host().isEmpty())
            continue;
        if (d.host.isEmpty()) {
            d.host = url.host();
            d.port = url.port(0);
        }
        if (!d.addresses.contains(url.host()))
            d.addresses.append(url.host());
    }
    d.displayName = onvifNameFromScopes(match.scopes);
    if (d.displayName.isEmpty())
        d.displayName = d.host.isEmpty() ? d.id : d.host;
    return d;
}

} // namespace loftail
