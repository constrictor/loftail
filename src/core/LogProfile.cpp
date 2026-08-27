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

#include "LogProfile.h"

#include "TimeDisplay.h"

namespace loftail {

namespace {
// The pattern loftail falls back to before anyone has configured one. NOT translated:
// it is a log4cplus conversion pattern, and translating it would stop it matching log
// text (ARCHITECTURE.md §9.1).
constexpr auto kBuiltInPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

// JSON keys. Never translated (ARCHITECTURE.md §9.1). These spellings are load-bearing:
// they are what M3's FormatCache and the QSettings session used for the same fields, so a
// value moves between stores with no mapping table. Changing one silently reverts every
// stored profile's value for that field to its struct default.
constexpr auto kPatternKey       = "pattern";
constexpr auto kEncodingKey      = "encoding";
constexpr auto kSourceZoneKey    = "sourceZone";
constexpr auto kTimeDisplayKey   = "timeDisplay";
constexpr auto kRunStartKey      = "runStartPattern";
constexpr auto kRunStartRegexKey = "runStartRegex";
constexpr auto kRunStartCaseKey  = "runStartCase";
constexpr auto kWrapModeKey      = "wrapMode";
constexpr auto kConfigPathKey    = "configPath";
constexpr auto kRestartScriptKey = "restartScript";
} // namespace

LogProfile LogProfile::builtIn()
{
    LogProfile p;
    p.format.pattern = QString::fromLatin1(kBuiltInPattern);
    // Everything else stays at its struct default — auto-detected encoding, zone
    // inferred from the pattern, timestamps as written, no run splitting, no wrapping —
    // which is what a log nobody has said anything about should get.
    return p;
}

QJsonObject logProfileToJson(const LogProfile &p)
{
    QJsonObject o;
    o.insert(QLatin1String(kPatternKey), p.format.pattern);
    o.insert(QLatin1String(kEncodingKey), int(p.format.encoding));
    o.insert(QLatin1String(kSourceZoneKey), p.format.sourceZone.toString());
    o.insert(QLatin1String(kTimeDisplayKey), timeDisplayToString(p.format.timeDisplay));
    o.insert(QLatin1String(kRunStartKey), p.format.runStartPattern);
    o.insert(QLatin1String(kRunStartRegexKey), p.format.runStartIsRegex);
    o.insert(QLatin1String(kRunStartCaseKey), p.format.runStartCaseSensitive);
    o.insert(QLatin1String(kWrapModeKey), int(p.wrapMode));
    o.insert(QLatin1String(kConfigPathKey), p.configPath);
    o.insert(QLatin1String(kRestartScriptKey), p.restartScript);
    return o;
}

LogProfile logProfileFromJson(const QJsonObject &o)
{
    LogProfile p;
    // PRESENCE, NOT EMPTINESS — see the header. An empty saved pattern is the answer
    // "ask me about every log", and reading it as silence makes that unreachable.
    p.format.pattern = o.contains(QLatin1String(kPatternKey))
        ? o.value(QLatin1String(kPatternKey)).toString()
        : LogProfile::builtIn().format.pattern;
    p.format.encoding =
        static_cast<Encoding>(o.value(QLatin1String(kEncodingKey)).toInt(int(Encoding::Auto)));
    p.format.sourceZone =
        ZoneChoice::fromString(o.value(QLatin1String(kSourceZoneKey)).toString());
    p.format.timeDisplay =
        timeDisplayFromString(o.value(QLatin1String(kTimeDisplayKey)).toString());
    p.format.runStartPattern = o.value(QLatin1String(kRunStartKey)).toString();
    p.format.runStartIsRegex = o.value(QLatin1String(kRunStartRegexKey)).toBool();
    p.format.runStartCaseSensitive = o.value(QLatin1String(kRunStartCaseKey)).toBool();
    p.wrapMode = static_cast<WrapMode>(o.value(QLatin1String(kWrapModeKey)).toInt(0));
    // A plain value read, NOT the presence test the pattern above needs: an empty config
    // path and an absent key mean the same thing here ("no config file configured"), so
    // there is nothing for presence to tell apart. The pattern's rule does not
    // generalise — it exists because an empty pattern is a DIFFERENT answer from an
    // unset one.
    p.configPath = o.value(QLatin1String(kConfigPathKey)).toString();
    // A plain value read for the same reason configPath is one: an empty restart script
    // and an absent key both mean "no restart command", so there is nothing for a
    // presence test to tell apart. Newlines survive JSON as \n, so a multi-line script
    // round-trips with no encoding of its own.
    p.restartScript = o.value(QLatin1String(kRestartScriptKey)).toString();
    return p;
}

} // namespace loftail
