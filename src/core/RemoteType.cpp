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

#include "RemoteType.h"

namespace loftail {

int RemoteType::effectivePort(int advertised) const
{
    if (port > 0)
        return port;
    // The ADVERTISED port is used only where the advertisement is FOR this connection — which
    // for Bonjour is when the browsed service is the connection's own (`_ssh._tcp` for SSH,
    // `_telnet._tcp` for telnet). An `_http._tcp` record's port is a web server's, and
    // connecting SSH to it would be a refusal that reads as the device's fault.
    if (discovery == Discovery::Bonjour && advertised > 0) {
        const QString own = connection == Connection::Telnet ? QStringLiteral("_telnet._tcp")
                                                             : QStringLiteral("_ssh._tcp");
        if (serviceType.trimmed().compare(own, Qt::CaseInsensitive) == 0)
            return advertised;
    }
    return RemoteLocation::defaultPortFor(transport());
}

QString RemoteType::discoveryToString(Discovery d)
{
    return d == Discovery::Onvif ? QStringLiteral("onvif") : QStringLiteral("bonjour");
}

RemoteType::Discovery RemoteType::discoveryFromString(const QString &s)
{
    return s == QLatin1String("onvif") ? Discovery::Onvif : Discovery::Bonjour;
}

QString RemoteType::connectionToString(Connection c)
{
    return c == Connection::Telnet ? QStringLiteral("telnet") : QStringLiteral("ssh");
}

RemoteType::Connection RemoteType::connectionFromString(const QString &s)
{
    return s == QLatin1String("telnet") ? Connection::Telnet : Connection::Ssh;
}

QJsonObject remoteTypeToJson(const RemoteType &t)
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), t.name);
    o.insert(QStringLiteral("discovery"), RemoteType::discoveryToString(t.discovery));
    o.insert(QStringLiteral("serviceType"), t.serviceType);
    o.insert(QStringLiteral("domain"), t.domain);
    o.insert(QStringLiteral("instanceMatch"), t.instanceRe);
    o.insert(QStringLiteral("txtMatch"), t.txtRe);
    o.insert(QStringLiteral("scopeMatch"), t.scopeRe);
    o.insert(QStringLiteral("connection"), RemoteType::connectionToString(t.connection));
    o.insert(QStringLiteral("port"), t.port);
    o.insert(QStringLiteral("user"), t.user);
    o.insert(QStringLiteral("savePassword"), t.savePassword);
    if (t.savePassword && !t.password.isEmpty())
        o.insert(QStringLiteral("password"), t.password);
    o.insert(QStringLiteral("defaultLog"), t.defaultLogPath);
    o.insert(QStringLiteral("pollMs"), t.pollMs);
    o.insert(QStringLiteral("tailStartBytes"), static_cast<double>(t.tailStartBytes));
    o.insert(QStringLiteral("compress"), t.compress);
    o.insert(QStringLiteral("consolePreset"), t.consolePreset);
    return o;
}

RemoteType remoteTypeFromJson(const QJsonObject &o)
{
    RemoteType t;
    const auto str = [&o](const char *key, QString *into) {
        if (o.contains(QLatin1String(key)))
            *into = o.value(QLatin1String(key)).toString();
    };
    str("name", &t.name);
    t.discovery = RemoteType::discoveryFromString(o.value(QStringLiteral("discovery")).toString());
    str("serviceType", &t.serviceType);
    str("domain", &t.domain);
    str("instanceMatch", &t.instanceRe);
    str("txtMatch", &t.txtRe);
    str("scopeMatch", &t.scopeRe);
    t.connection =
        RemoteType::connectionFromString(o.value(QStringLiteral("connection")).toString());
    t.port = qBound(0, o.value(QStringLiteral("port")).toInt(0), 65535);
    str("user", &t.user);
    t.savePassword = o.value(QStringLiteral("savePassword")).toBool();
    str("password", &t.password);
    str("defaultLog", &t.defaultLogPath);
    t.pollMs = o.value(QStringLiteral("pollMs")).toInt(1000);
    t.tailStartBytes = static_cast<qint64>(o.value(QStringLiteral("tailStartBytes")).toDouble());
    t.compress = o.value(QStringLiteral("compress")).toBool();
    str("consolePreset", &t.consolePreset);
    return t;
}

} // namespace loftail
