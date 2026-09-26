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

// The named serial presets and the remembered devices (SPEC.md §3).
//
// HostBookmarkStore's own rules, asserted for the store that copies them: the NAME is the
// identity, an added key never bumps the schema, and a preset named by a log but missing from the
// file FALLS BACK rather than refusing the open.

#include <QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "SerialProfile.h"
#include "SerialProfileStore.h"

using namespace loftail;

class TestSerialStore : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    SerialProfile board(const QString &name) const
    {
        SerialProfile p = SerialProfile::builtIn();
        p.name = name;
        p.baud = 9600;
        p.startupCommands = QStringList{QStringLiteral("dmesg -n 1")};
        p.rebootRe = QStringLiteral("U-Boot");
        return p;
    }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }
    void init();

    void aPresetRoundTripsEveryField();
    void theNameIsTheIdentityAndSavingReplacesInPlace();
    void aNameComparisonIgnoresCaseAndSurroundingSpace();
    void aMissingPresetFallsBackRatherThanRefusing();
    void anAddedKeyIsReadWithoutBumpingTheSchema();
    void aDeviceRemembersItsPathsAndCanForgetOne();
    void anEditToOneFieldIsSeenByTheChangeComparison();
    void theChangeCostIsTheMostExpensiveTierAnyFieldImplies();
};

void TestSerialStore::init()
{
    QFile::remove(SerialProfileStore(m_dir.path()).filePath());
}

void TestSerialStore::aPresetRoundTripsEveryField()
{
    const SerialProfileStore store(m_dir.path());
    SerialProfile p = board(QStringLiteral("MyBoard"));
    p.dataBits = 7;
    p.parity = SerialProfile::Parity::Even;
    p.stopBits = SerialProfile::StopBits::Two;
    p.flow = SerialProfile::Flow::Hardware;
    p.loginPromptRe = QStringLiteral("anmelden:");
    p.passwordPromptRe = QStringLiteral("Kennwort:");
    p.promptTimeoutMs = 1234;
    p.attempts = 7;
    p.pollMs = 3000;
    p.startupCommands = QStringList{QStringLiteral("dmesg -n 1"), QStringLiteral("stty -echo")};
    QVERIFY(store.savePreset(p));

    const QVector<SerialProfile> back = store.presets();
    QCOMPARE(back.size(), 1);
    // EVERY FIELD, through operator==, which is itself the thing a field added without a clause
    // would make blind — so this case and the per-field one below are two halves of one guard.
    QCOMPARE(back.first(), p);
}

void TestSerialStore::theNameIsTheIdentityAndSavingReplacesInPlace()
{
    const SerialProfileStore store(m_dir.path());
    QVERIFY(store.savePreset(board(QStringLiteral("First"))));
    QVERIFY(store.savePreset(board(QStringLiteral("Second"))));
    QVERIFY(store.savePreset(board(QStringLiteral("Third"))));

    SerialProfile edited = board(QStringLiteral("Second"));
    edited.baud = 115200;
    QVERIFY(store.savePreset(edited));

    const QVector<SerialProfile> back = store.presets();
    QCOMPARE(back.size(), 3);
    // REPLACED IN PLACE, so the list order does not shift under the person reading it — which is
    // the whole reason the name rather than the position is the identity.
    QCOMPARE(back.at(1).name, QStringLiteral("Second"));
    QCOMPARE(back.at(1).baud, 115200);
    QCOMPARE(back.at(0).name, QStringLiteral("First"));
    QCOMPARE(back.at(2).name, QStringLiteral("Third"));

    // Removing a name that is not there is not a failure: there is nothing to do.
    QVERIFY(store.removePreset(QStringLiteral("Nonexistent")));
    QVERIFY(store.removePreset(QStringLiteral("Second")));
    QCOMPARE(store.presets().size(), 2);
}

void TestSerialStore::aNameComparisonIgnoresCaseAndSurroundingSpace()
{
    const SerialProfileStore store(m_dir.path());
    QVERIFY(store.savePreset(board(QStringLiteral("MyBoard"))));
    SerialProfile same = board(QStringLiteral("  myboard "));
    same.baud = 19200;
    QVERIFY(store.savePreset(same));
    // ONE entry, because two the reader cannot tell apart are one entry.
    QCOMPARE(store.presets().size(), 1);
    QCOMPARE(store.presets().first().baud, 19200);
}

void TestSerialStore::aMissingPresetFallsBackRatherThanRefusing()
{
    const SerialProfileStore store(m_dir.path());
    QVERIFY(store.savePreset(board(QStringLiteral("MyBoard"))));

    // The settings tree and this file are separate documents a user may edit or copy
    // independently, so a name with nothing behind it is an ORDINARY state — and a log that will
    // not open because a preset was renamed is worse than one that opens on the defaults.
    QCOMPARE(store.presetNamed(QStringLiteral("Gone")), SerialProfile::builtIn());
    QCOMPARE(store.presetNamed(QString()), SerialProfile::builtIn());
    QCOMPARE(store.presetNamed(QStringLiteral("MyBoard")).baud, 9600);
}

void TestSerialStore::anAddedKeyIsReadWithoutBumpingTheSchema()
{
    // HostBookmarkStore's rule, and the reason it is a rule: an older binary reads an absent key
    // as the struct's default, which is benign — where a BUMPED version makes it refuse the whole
    // file and forget every preset the user has.
    const SerialProfileStore store(m_dir.path());
    QVERIFY(store.savePreset(board(QStringLiteral("MyBoard"))));

    // A file written by some later loftail, carrying a key this one has never heard of.
    QFile f(store.filePath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    QJsonArray presets = root.value(QStringLiteral("presets")).toArray();
    QJsonObject first = presets.at(0).toObject();
    first[QStringLiteral("somethingFromTheFuture")] = 42;
    presets.replace(0, first);
    root[QStringLiteral("presets")] = presets;
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(root).toJson());
    f.close();

    // Read perfectly well, the unknown key ignored.
    const QVector<SerialProfile> back = store.presets();
    QCOMPARE(back.size(), 1);
    QCOMPARE(back.first().baud, 9600);
}

void TestSerialStore::aDeviceRemembersItsPathsAndCanForgetOne()
{
    const SerialProfileStore store(m_dir.path());
    SerialDeviceBookmark d;
    d.id = QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0");
    d.paths = QStringList{QStringLiteral("/var/log/messages"), QStringLiteral("/var/log/app.log")};
    QVERIFY(store.saveDevice(d));

    QVector<SerialDeviceBookmark> back = store.devices();
    QCOMPARE(back.size(), 1);
    QCOMPARE(back.first().id, d.id);
    QCOMPARE(back.first().paths, d.paths);
    // The label falls back to the id, so a row is never nameless.
    QCOMPARE(back.first().displayName(), d.id);

    QVERIFY(store.forgetPath(d.id, QStringLiteral("/var/log/messages")));
    QCOMPARE(store.devices().first().paths, QStringList{QStringLiteral("/var/log/app.log")});

    // And the presets are untouched by a device write, which is what sharing one file requires.
    QVERIFY(store.savePreset(board(QStringLiteral("MyBoard"))));
    QVERIFY(store.saveDevice(back.first()));
    QCOMPARE(store.presets().size(), 1);
    QCOMPARE(store.devices().size(), 1);
}

void TestSerialStore::anEditToOneFieldIsSeenByTheChangeComparison()
{
    // SerialProfile::operator== is what every change guard here asks, so a field added above
    // without a clause in it is a field two presets can never differ in — an edit to it is
    // silently dropped by the on-the-fly tiers. LogProfile's own rule, one struct over, and the
    // same case shape (tst_logsettings::aProfileDiffersWhenAnyOneFieldOfItDoes).
    const SerialProfile base = SerialProfile::builtIn();
    const auto differs = [&base](auto mutate) {
        SerialProfile p = base;
        mutate(p);
        return p != base;
    };
    QVERIFY(differs([](SerialProfile &p) { p.name = QStringLiteral("x"); }));
    QVERIFY(differs([](SerialProfile &p) { p.baud = 9600; }));
    QVERIFY(differs([](SerialProfile &p) { p.dataBits = 7; }));
    QVERIFY(differs([](SerialProfile &p) { p.parity = SerialProfile::Parity::Even; }));
    QVERIFY(differs([](SerialProfile &p) { p.stopBits = SerialProfile::StopBits::Two; }));
    QVERIFY(differs([](SerialProfile &p) { p.flow = SerialProfile::Flow::Hardware; }));
    QVERIFY(differs([](SerialProfile &p) { p.loginPromptRe = QStringLiteral("x:"); }));
    QVERIFY(differs([](SerialProfile &p) { p.passwordPromptRe = QStringLiteral("x:"); }));
    QVERIFY(differs([](SerialProfile &p) { p.promptTimeoutMs = 1; }));
    QVERIFY(differs([](SerialProfile &p) { p.attempts = 9; }));
    QVERIFY(differs([](SerialProfile &p) {
        p.startupCommands = QStringList{QStringLiteral("dmesg -n 1")};
    }));
    QVERIFY(differs([](SerialProfile &p) { p.rebootRe = QStringLiteral("U-Boot"); }));
    QVERIFY(differs([](SerialProfile &p) { p.pollMs = 500; }));
    // And an untouched copy is equal, or every assertion above passes for the wrong reason.
    QVERIFY(SerialProfile::builtIn() == base);
}

void TestSerialStore::theChangeCostIsTheMostExpensiveTierAnyFieldImplies()
{
    // What an edited preset COSTS a tab already reading a log — applySettings()'s
    // rescan/reparse/repaint diff one layer down (SPEC.md §3).
    const SerialProfile base = SerialProfile::builtIn();
    QCOMPARE(serialChangeCost(base, base), SerialChangeCost::Nothing);

    SerialProfile inPlace = base;
    inPlace.rebootRe = QStringLiteral("U-Boot");
    QCOMPARE(serialChangeCost(base, inPlace), SerialChangeCost::InPlace);
    SerialProfile polled = base;
    polled.pollMs = 500;
    QCOMPARE(serialChangeCost(base, polled), SerialChangeCost::InPlace);

    SerialProfile relogin = base;
    relogin.startupCommands = QStringList{QStringLiteral("dmesg -n 1")};
    QCOMPARE(serialChangeCost(base, relogin), SerialChangeCost::Relogin);

    SerialProfile reopen = base;
    reopen.baud = 9600;
    QCOMPARE(serialChangeCost(base, reopen), SerialChangeCost::ReopenPort);

    // THE MOST EXPENSIVE TIER ANY CHANGED FIELD IMPLIES, which is why the comparison asks
    // cheapest-last: an edit that moved the reboot pattern AND the baud rate must reopen the
    // port, and a cheapest-first answer would report it as an in-place change and leave the line
    // at the old rate — after which the login state stops being knowable at all.
    SerialProfile both = base;
    both.rebootRe = QStringLiteral("U-Boot");
    both.startupCommands = QStringList{QStringLiteral("dmesg -n 1")};
    both.baud = 9600;
    QCOMPARE(serialChangeCost(base, both), SerialChangeCost::ReopenPort);

    SerialProfile twoCheaper = base;
    twoCheaper.rebootRe = QStringLiteral("U-Boot");
    twoCheaper.attempts = 9;
    QCOMPARE(serialChangeCost(base, twoCheaper), SerialChangeCost::Relogin);
}

QTEST_APPLESS_MAIN(TestSerialStore)
#include "tst_serialstore.moc"
