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

#include <QtTest>

#include "FormatSettings.h"
#include "ManualFormatProvider.h"

using namespace loftail;

// M3 — the format-provider seam. ManualFormatProvider is the only thing that turns a
// pattern string into a LogFormat (invariant #3, §9). The per-file memory that used to
// be tested here is now one level of the settings tree — see tst_logsettings.
// Core-only.
class TestFormatProvider : public QObject
{
    Q_OBJECT

private slots:
    void manualProviderCompilesGoodPattern();
    void manualProviderSurfacesCompileError();
    void zoneChoiceStringRoundTrip();
};

void TestFormatProvider::manualProviderCompilesGoodPattern()
{
    ManualFormatProvider provider(QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"));
    // The provider ignores the sample — the manual path needs no file content.
    auto result = provider.formatFor(QByteArrayView());
    QVERIFY2(bool(result), "a valid pattern must compile through the provider");

    const LogFormat &f = result.value();
    QVERIFY(f.dateGroup > 0);
    QVERIFY(f.threadGroup > 0);
    QVERIFY(f.prioGroup > 0);
    QVERIFY(f.loggerGroup > 0);
    QVERIFY(f.msgGroup > 0);
    QCOMPARE(provider.pattern(), QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"));
}

void TestFormatProvider::manualProviderSurfacesCompileError()
{
    ManualFormatProvider provider(QStringLiteral("%p %z %m")); // %z is unknown
    auto result = provider.formatFor(QByteArrayView());
    QVERIFY2(!result, "an invalid pattern must yield a CompileError, not throw");
    QCOMPARE(int(result.error().code), int(CompileError::Code::UnknownSpecifier));
    QCOMPARE(result.error().offset, 4); // points at the offending 'z'
}

void TestFormatProvider::zoneChoiceStringRoundTrip()
{
    for (const ZoneChoice z : {
             ZoneChoice{ZoneChoice::Kind::Default, 0},
             ZoneChoice{ZoneChoice::Kind::Local, 0},
             ZoneChoice{ZoneChoice::Kind::Utc, 0},
             ZoneChoice{ZoneChoice::Kind::FixedOffset, -5 * 3600},
         }) {
        const ZoneChoice back = ZoneChoice::fromString(z.toString());
        QCOMPARE(back, z);
    }
}

QTEST_GUILESS_MAIN(TestFormatProvider)
#include "tst_formatprovider.moc"
