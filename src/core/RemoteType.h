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

#pragma once

#include "RemoteLocation.h"

#include <QJsonObject>
#include <QString>

namespace loftail {

// A NAMED KIND OF REMOTE: how to find machines of this kind on the network, and how to sign in
// to one once found (SPEC.md §3, §4, ARCHITECTURE.md §6.13).
//
// A fleet of cameras or boards gets its addresses from DHCP, so a saved host's IP goes stale
// and a log opened by address stops opening. A remote type names the fleet rather than a
// machine: a discovery recipe (a Bonjour service type and a TXT-record match, or an ONVIF scope
// match), a connection (SSH or telnet), the account, and the log a device of this kind is
// usually opened on. A log opened through discovery is then remembered as a reference to its
// TYPE and the device's own stable id — `discover://Type/id/path` — so the next launch finds the
// device again at whatever address it has now.
//
// THE TYPE IS LOOKED UP AT CONNECT TIME, NEVER CAPTURED INTO THE ADDRESS. That is what lets an
// edit to the type — a new account, telnet instead of SSH, a different port — reach every log
// already opened through it, and it is why the address carries no user and no host at all.
struct RemoteType
{
    enum class Discovery {
        Bonjour,   // mDNS/DNS-SD: a service type browsed in a domain
        Onvif,     // WS-Discovery: the ONVIF NetworkVideoTransmitter probe
    };
    enum class Connection {
        Ssh,
        Telnet,
    };

    // The identity: trimmed and case-insensitive, HostBookmarkStore's rule for a host's name.
    // It is also what a `discover://` address names, so renaming a type orphans the logs opened
    // through it — the dialog says so before it lets that happen.
    QString name;

    Discovery discovery = Discovery::Bonjour;
    // Bonjour: which service to browse for, and in which domain.
    QString serviceType = QStringLiteral("_ssh._tcp");
    QString domain = QStringLiteral("local");
    // Bonjour: a regular expression the service INSTANCE name must match, and one at least one
    // TXT record (as `key=value`) must match. Empty means "anything". An invalid expression
    // matches NOTHING, never everything (LogPatternNode::matches()'s rule).
    QString instanceRe;
    QString txtRe;
    // ONVIF: a regular expression at least one advertised scope URI must match.
    QString scopeRe;

    Connection connection = Connection::Ssh;
    // 0 means "the advertised port where the advertisement is for this connection, else the
    // connection's default": a Bonjour `_ssh._tcp` record carries the SSH port, an ONVIF probe
    // match carries a web-service address that says nothing about SSH or telnet.
    int port = 0;

    // The account. Optional: SSH falls back to its own convention (effectiveUser()), and a
    // console asks the login prompt with nothing typed.
    QString user;
    // Whether `password` is stored ON DISK IN CLEAR TEXT — HostBookmark::savePassword's own
    // contract and consent, word for word. Only meaningful with a user; the dialog will not
    // take one without.
    bool    savePassword = false;
    QString password;

    // The log a device of this kind is usually opened on, pre-filled in the browser. Optional.
    QString defaultLogPath;

    // Per-connection advanced settings: SSH's cadence, tail-start and compression, and the
    // console preset (serial.json) a telnet login is held to.
    int     pollMs = 1000;
    qint64  tailStartBytes = 0;
    bool    compress = false;
    QString consolePreset;

    // EVERY FIELD. A field this does not compare is one an edit to cannot be noticed by any
    // change guard that asks whether anything moved — the LogProfile::operator== trap.
    bool operator==(const RemoteType &) const = default;

    RemoteLocation::Transport transport() const
    {
        return connection == Connection::Telnet ? RemoteLocation::Transport::Telnet
                                                : RemoteLocation::Transport::Ssh;
    }

    // Which port to connect to for a device that advertised `advertised` (0 for none).
    int effectivePort(int advertised) const;

    // The key a type's own password is remembered under, in SshCredentialCache and the
    // keychain: one per TYPE, so a password typed once serves every device of the kind, and
    // prefixed so it cannot collide with any host's or device's target().
    QString credentialTarget() const { return QStringLiteral("remotetype:") + name.trimmed(); }

    static QString discoveryToString(Discovery d);
    static Discovery discoveryFromString(const QString &s);
    static QString connectionToString(Connection c);
    static Connection connectionFromString(const QString &s);
};

// JSON, ADDED-KEY-SAFE: every key is read for PRESENCE rather than emptiness, so a type saved
// with an empty service type stays empty rather than coming back as `_ssh._tcp` — the trap the
// settings tree records for `contains("pattern")`. The password is written only when the user
// asked for it, so turning the option off does not leave the old secret in the file.
QJsonObject remoteTypeToJson(const RemoteType &type);
RemoteType remoteTypeFromJson(const QJsonObject &o);

} // namespace loftail
