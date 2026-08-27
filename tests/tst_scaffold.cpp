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

#include "Version.h"

// M0 smoke test: proves the CMake + Qt Test + CTest wiring works and that core
// logic is reachable without a QApplication. Real coverage begins in M1.
class TestScaffold : public QObject
{
    Q_OBJECT

private slots:
    void applicationVersionIsNonEmpty();
    void theVersionStringCarriesTheBuildIdWithoutHidingTheRelease();
    void applicationIdentityIsSet();
};

void TestScaffold::applicationVersionIsNonEmpty()
{
    QVERIFY(!loftail::applicationVersion().isEmpty());
}

// Both configurations in one test, because which one this binary is depends on how it
// was configured and neither is the odd case: a local build carries no build id, a CI
// build does, and the relationship between the three strings is fixed either way.
void TestScaffold::theVersionStringCarriesTheBuildIdWithoutHidingTheRelease()
{
    const QString version = loftail::applicationVersion();
    const QString build = loftail::applicationBuildId();

    // The release stays readable at the front, so --version answers "which release"
    // for a stamped build exactly as it does for an unstamped one.
    QVERIFY(loftail::applicationVersionString().startsWith(version));

    if (build.isEmpty()) {
        QCOMPARE(loftail::applicationVersionString(), version);
    } else {
        QCOMPARE(loftail::applicationVersionString(), version + QLatin1Char('+') + build);
        // One token: --version reaches a Windows MessageBox and a grep in CI, and
        // '+' is the separator, so it cannot also appear inside the id.
        QVERIFY(!build.contains(QLatin1Char('+')));
        QVERIFY(!build.contains(QLatin1Char(' ')));
    }
}

void TestScaffold::applicationIdentityIsSet()
{
    QCOMPARE(QString::fromLatin1(loftail::applicationName), QStringLiteral("loftail"));
    QCOMPARE(QString::fromLatin1(loftail::organizationName), QStringLiteral("loftail"));
}

QTEST_APPLESS_MAIN(TestScaffold)
#include "tst_scaffold.moc"
