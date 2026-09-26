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

#include "SerialProfile.h"

#include <QJsonArray>

namespace loftail {
namespace {

// JSON spellings. Untranslated, and the same-name-as-the-field convention the other
// stores use so a value moves between them with no mapping table (§8).
constexpr auto kName = "name";
constexpr auto kBaud = "baud";
constexpr auto kDataBits = "dataBits";
constexpr auto kParity = "parity";
constexpr auto kStopBits = "stopBits";
constexpr auto kFlow = "flow";
constexpr auto kLoginPrompt = "loginPrompt";
constexpr auto kPasswordPrompt = "passwordPrompt";
constexpr auto kPromptTimeoutMs = "promptTimeoutMs";
constexpr auto kAttempts = "attempts";
constexpr auto kStartupCommands = "startupCommands";
constexpr auto kRebootPattern = "rebootPattern";
constexpr auto kPollMs = "pollMs";

// The three enums travel as WORDS and not as integers, for the reason
// TimeDisplay's string round trip exists: a value added to the middle of one of
// these would otherwise reinterpret every stored preset, silently and one step
// off. An unrecognised word reads as the default, which is the benign answer.
QString parityName(SerialProfile::Parity p)
{
    switch (p) {
    case SerialProfile::Parity::None:  return QStringLiteral("none");
    case SerialProfile::Parity::Even:  return QStringLiteral("even");
    case SerialProfile::Parity::Odd:   return QStringLiteral("odd");
    case SerialProfile::Parity::Mark:  return QStringLiteral("mark");
    case SerialProfile::Parity::Space: return QStringLiteral("space");
    }
    return QStringLiteral("none");
}

SerialProfile::Parity parityFromName(const QString &s)
{
    if (s == QLatin1String("even"))  return SerialProfile::Parity::Even;
    if (s == QLatin1String("odd"))   return SerialProfile::Parity::Odd;
    if (s == QLatin1String("mark"))  return SerialProfile::Parity::Mark;
    if (s == QLatin1String("space")) return SerialProfile::Parity::Space;
    return SerialProfile::Parity::None;
}

QString stopBitsName(SerialProfile::StopBits s)
{
    switch (s) {
    case SerialProfile::StopBits::One:          return QStringLiteral("1");
    case SerialProfile::StopBits::OneAndHalf:   return QStringLiteral("1.5");
    case SerialProfile::StopBits::Two:          return QStringLiteral("2");
    }
    return QStringLiteral("1");
}

SerialProfile::StopBits stopBitsFromName(const QString &s)
{
    if (s == QLatin1String("1.5")) return SerialProfile::StopBits::OneAndHalf;
    if (s == QLatin1String("2"))   return SerialProfile::StopBits::Two;
    return SerialProfile::StopBits::One;
}

QString flowName(SerialProfile::Flow f)
{
    switch (f) {
    case SerialProfile::Flow::None:     return QStringLiteral("none");
    case SerialProfile::Flow::Hardware: return QStringLiteral("hardware");
    case SerialProfile::Flow::Software: return QStringLiteral("software");
    }
    return QStringLiteral("none");
}

SerialProfile::Flow flowFromName(const QString &s)
{
    if (s == QLatin1String("hardware")) return SerialProfile::Flow::Hardware;
    if (s == QLatin1String("software")) return SerialProfile::Flow::Software;
    return SerialProfile::Flow::None;
}

} // namespace

SerialProfile SerialProfile::builtIn()
{
    // Every field at its declared default. Spelled as a function rather than as a
    // default-constructed struct so the one place that says what an unconfigured serial
    // line is stays greppable — LogProfile::builtIn()'s own reason.
    return SerialProfile{};
}

SerialChangeCost serialChangeCost(const SerialProfile &from, const SerialProfile &to)
{
    if (from == to)
        return SerialChangeCost::Nothing;

    // Most expensive first, so the answer is the highest tier ANY changed field implies.
    // Asking cheapest-first and returning early would report an in-place change for an
    // edit that also moved the baud rate.
    if (from.baud != to.baud || from.dataBits != to.dataBits || from.parity != to.parity
        || from.stopBits != to.stopBits || from.flow != to.flow) {
        return SerialChangeCost::ReopenPort;
    }
    if (from.loginPromptRe != to.loginPromptRe || from.passwordPromptRe != to.passwordPromptRe
        || from.promptTimeoutMs != to.promptTimeoutMs || from.attempts != to.attempts
        || from.startupCommands != to.startupCommands) {
        return SerialChangeCost::Relogin;
    }
    // What is left is rebootRe, pollMs and the preset's own NAME. A rename alone moves
    // nothing about the line, so it costs nothing beyond being noticed.
    return SerialChangeCost::InPlace;
}

QJsonObject serialProfileToJson(const SerialProfile &p)
{
    QJsonObject o;
    o[QLatin1String(kName)] = p.name;
    o[QLatin1String(kBaud)] = p.baud;
    o[QLatin1String(kDataBits)] = p.dataBits;
    o[QLatin1String(kParity)] = parityName(p.parity);
    o[QLatin1String(kStopBits)] = stopBitsName(p.stopBits);
    o[QLatin1String(kFlow)] = flowName(p.flow);
    o[QLatin1String(kLoginPrompt)] = p.loginPromptRe;
    o[QLatin1String(kPasswordPrompt)] = p.passwordPromptRe;
    o[QLatin1String(kPromptTimeoutMs)] = p.promptTimeoutMs;
    o[QLatin1String(kAttempts)] = p.attempts;
    o[QLatin1String(kRebootPattern)] = p.rebootRe;
    o[QLatin1String(kPollMs)] = p.pollMs;
    QJsonArray commands;
    for (const QString &c : p.startupCommands)
        commands.append(c);
    o[QLatin1String(kStartupCommands)] = commands;
    return o;
}

SerialProfile serialProfileFromJson(const QJsonObject &o)
{
    SerialProfile p;
    p.name = o.value(QLatin1String(kName)).toString();
    // Every numeric key falls back to the struct's own default rather than to 0: an
    // absent key is an older binary's file, and a baud rate of 0 is not a line setting.
    p.baud = o.value(QLatin1String(kBaud)).toInt(p.baud);
    p.dataBits = o.value(QLatin1String(kDataBits)).toInt(p.dataBits);
    p.parity = parityFromName(o.value(QLatin1String(kParity)).toString());
    p.stopBits = stopBitsFromName(o.value(QLatin1String(kStopBits)).toString());
    p.flow = flowFromName(o.value(QLatin1String(kFlow)).toString());
    // PRESENCE, NOT EMPTINESS, for the two prompts. An empty pattern is a real answer —
    // it matches nothing, so the step it belongs to is skipped, which is how a device
    // that asks for no password is described. Reading empty as "nothing saved" would
    // silently reinstate the default prompt and make that setting unreachable
    // (logProfileFromJson()'s rule for `pattern`, same trap).
    if (o.contains(QLatin1String(kLoginPrompt)))
        p.loginPromptRe = o.value(QLatin1String(kLoginPrompt)).toString();
    if (o.contains(QLatin1String(kPasswordPrompt)))
        p.passwordPromptRe = o.value(QLatin1String(kPasswordPrompt)).toString();
    p.promptTimeoutMs = o.value(QLatin1String(kPromptTimeoutMs)).toInt(p.promptTimeoutMs);
    p.attempts = o.value(QLatin1String(kAttempts)).toInt(p.attempts);
    p.rebootRe = o.value(QLatin1String(kRebootPattern)).toString();
    p.pollMs = o.value(QLatin1String(kPollMs)).toInt(p.pollMs);
    const QJsonArray commands = o.value(QLatin1String(kStartupCommands)).toArray();
    for (const QJsonValue &v : commands) {
        const QString c = v.toString();
        if (!c.isEmpty())
            p.startupCommands.append(c);
    }
    return p;
}

} // namespace loftail
