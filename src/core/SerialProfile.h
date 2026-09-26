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

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace loftail {

// Everything a serial line needs to be told, apart from WHICH line and WHO logs in
// (SPEC.md §3). A named, free-standing preset: the whole point is that it is NOT bound to
// a device, so one "every board of ours is a Linux box" answer serves every device, and a
// LogProfile names which preset applies to which log through the ordinary three-level
// settings tree.
//
// THREE THINGS ARE DELIBERATELY NOT IN HERE.
//
// The DEVICE is not, because that is what the preset must not be bound to. The LOGIN USER
// is not, because it lives in the ADDRESS (`serial://root@ttyUSB0/var/log/app.log`) and
// therefore in RemoteLocation::target(), which is what SshCredentialCache, the keychain
// and SshConnectHold are all keyed on: a preset supplying a default user would key the
// address one way and the connect another, which is exactly the remembered-password
// defect RemoteLocation::effectiveUser() exists to have removed ("one address had two
// keys at once"). And the PASSWORD is not, because it belongs in the OS keychain under
// that same target, with the plain-text fallback living in its own store beside the
// saved-host one — a secret in a settings value would travel into logsettings.json,
// which is neither private nor meant to hold one.
//
// The line settings are plain ints and plain enums rather than QSerialPort::BaudRate and
// friends, which is what keeps this struct — and its JSON, and the login machine that
// reads it — ALWAYS COMPILED. The one translation into Qt's enums happens in the single
// gated translation unit that opens a port.
struct SerialProfile
{
    // Which of the five the line is set to. Spelled out here rather than reusing
    // QSerialPort's, so a build with no serial support still reads, writes, compares and
    // displays a preset identically (RemoteLocation's rule, one dependency over).
    enum class Parity { None, Even, Odd, Mark, Space };
    enum class StopBits { One, OneAndHalf, Two };
    enum class Flow { None, Hardware, Software };

    // The preset's IDENTITY, exactly as a saved host's name is (HostBookmarkStore.h):
    // what the list shows is what tells two entries apart, so saving replaces by name.
    QString  name;

    int      baud = 115200;
    int      dataBits = 8;
    Parity   parity = Parity::None;
    StopBits stopBits = StopBits::One;
    Flow     flow = Flow::None;

    // The login conversation. Unanchored regular expressions, matched against a sliding
    // window of what has arrived (SerialLoginMachine.h) — the same ruling
    // LogPatternNode::Kind::Regex already takes, and for the same reason: a prompt is
    // something you recognise in a stream, not something you anchor.
    //
    // AN INVALID PATTERN MATCHES NOTHING, never everything. That is LogPatternNode's own
    // rule and it matters more here: a half-typed pattern that matched everything would
    // tear the session down and log in again once a second.
    QString loginPromptRe = QStringLiteral("(?:login|[Uu]sername)\\s*:");
    QString passwordPromptRe = QStringLiteral("[Pp]assword\\s*:");

    // How long one step of the conversation waits before nudging, and how many nudges it
    // spends before giving up. Bounded for ReconnectGrace's reason: an unbounded wait on
    // a device that will never answer is a tab that says "signing in" for the rest of the
    // day.
    int promptTimeoutMs = 5000;
    int attempts = 3;

    // Run once the shell is up, in order, each waited for like any other step (SPEC.md
    // §3). The reason the setting exists is to SILENCE the console — `dmesg -n 1`, a
    // service's own verbosity — because everything a device prints unasked arrives on the
    // same line loftail is reading the log over.
    QStringList startupCommands;

    // What a reboot looks like in what the device prints. Empty means "off", and it is
    // NATURALLY inert when empty — an empty pattern matches nothing — which is why there
    // is no enable flag beside it to fall out of step (AxisEditor::addTextExtra()'s
    // argument, one pane over).
    QString rebootRe;

    // How often to ask the device whether the log has grown. Slower than the network
    // default of 1000 on purpose: a round trip here is a command, a shell, and a framed
    // answer over a line that moves ~11 KB/s, so polling as fast as SSH does would spend
    // the link on asking rather than on reading.
    int pollMs = 2000;

    // What a preset nobody has written gets: 115200 8N1, the two conventional prompts, no
    // startup commands and no reboot pattern. LogProfile::builtIn()'s shape and its role
    // — the value a missing preset falls back to, which is why a renamed preset costs a
    // diagnostic line rather than a log that will not open.
    static SerialProfile builtIn();

    // EVERY FIELD, and a field added above without a clause here is a field two presets
    // can never differ in — so an edit to it is silently dropped by every change guard
    // that asks whether anything moved. LogProfile::operator==' own rule, and it bites
    // the same way: the on-the-fly tiers in SerialFetchOptions decide what an edit COSTS
    // by comparing two of these.
    bool operator==(const SerialProfile &o) const
    {
        return name == o.name && baud == o.baud && dataBits == o.dataBits
            && parity == o.parity && stopBits == o.stopBits && flow == o.flow
            && loginPromptRe == o.loginPromptRe && passwordPromptRe == o.passwordPromptRe
            && promptTimeoutMs == o.promptTimeoutMs && attempts == o.attempts
            && startupCommands == o.startupCommands && rebootRe == o.rebootRe
            && pollMs == o.pollMs;
    }
    bool operator!=(const SerialProfile &o) const { return !(*this == o); }
};

// What an edit to a preset COSTS a tab that is already reading a log through it
// (SPEC.md §3, ARCHITECTURE.md §6.11). This is applySettings()'s rescan/reparse/repaint
// diff one layer down, and the three tiers are ordered by expense.
enum class SerialChangeCost {
    Nothing,     // the two presets agree
    InPlace,     // rebootRe, pollMs — recompiled, used from the next turn
    Relogin,     // the conversation, the startup commands — drop the session, sign in again
    ReopenPort,  // the line settings — close the port and start over
};

// Which tier separates these two presets.
//
// THE LINE SETTINGS CANNOT BE APPLIED TO AN OPEN PORT, and that is why they are their own
// tier rather than folded into Relogin. QSerialPort::setBaudRate() on an open port is
// legal, but the far end has been receiving bytes at the OLD rate, so what state the
// login is in stops being knowable — the machine would sit in Ready over a session that
// may not be there. Reopening is the only state that can be reasoned about, and it is
// reconnect()'s existing shape, which already publishes Waiting and counts down.
//
// AND startupCommands FORCE A RELOGIN rather than merely running the new ones, because
// their purpose is to change what the device prints: running the new set on top of the
// old leaves the console in the union of two settings, which is what neither of them
// asked for.
SerialChangeCost serialChangeCost(const SerialProfile &from, const SerialProfile &to);

// The preset's JSON form. Keys are JSON keys and are NEVER translated (§9.1).
//
// AN ADDED KEY NEVER BUMPS THE SCHEMA — SerialProfileStore's rule, taken from
// HostBookmarkStore: an older binary reads an absent key as this struct's default, which
// is benign, where a bumped version makes it refuse the whole file and forget every
// preset the user has.
QJsonObject serialProfileToJson(const SerialProfile &p);
SerialProfile serialProfileFromJson(const QJsonObject &o);

} // namespace loftail
