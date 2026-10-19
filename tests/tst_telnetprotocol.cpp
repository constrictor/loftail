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

// The telnet layer, with nothing listening (ARCHITECTURE.md §6.12). TelnetProtocol is pure, so
// every rule it has is asserted here against bytes, and the live half — a real socket into a
// real /bin/sh — is tst_telnetsession's.

#include <QtTest>

#include "TelnetProtocol.h"

using namespace loftail;

namespace {

QByteArray cmd(unsigned char verb, unsigned char option)
{
    QByteArray out;
    out.append(char(TelnetProtocol::kIac));
    out.append(char(verb));
    out.append(char(option));
    return out;
}

constexpr unsigned char kTerminalType = 24;
constexpr unsigned char kNaws = 31;

} // namespace

class TestTelnetProtocol : public QObject
{
    Q_OBJECT

private slots:
    void plainDataPassesThroughUntouched();
    void everyCommandIsStrippedFromTheData();
    void aSequenceSplitAcrossReadsIsStillStripped();
    void aDoubledIacIsOneByteOfData();
    void whatWeDoNotDoIsRefusedAndWhatWeDoIsAgreedTo();
    void ourOwnRequestIsNotAnsweredAgainWhenItIsGranted();
    void aRepeatedOfferIsNotAnsweredTwice();
    void aSubnegotiationIsSkippedWhole();
    void outsideBinaryACrNulFoldsBackToCr();
    void inBinaryACrNulIsTwoBytesOfTheFile();
    void encodingDoublesIacAndOnlyTranslatesNewlinesOutsideBinary();
};

void TestTelnetProtocol::plainDataPassesThroughUntouched()
{
    TelnetProtocol p;
    QByteArray reply;
    QCOMPARE(p.receive("login: ", &reply), QByteArray("login: "));
    QVERIFY(reply.isEmpty());
}

void TestTelnetProtocol::everyCommandIsStrippedFromTheData()
{
    // What busybox telnetd opens with, around the prompt it then prints. Anything of this left
    // in the data would sit in front of `login:` and decide whether the prompt regex matches.
    TelnetProtocol p;
    QByteArray in = cmd(TelnetProtocol::kDo, TelnetProtocol::kEcho)
        + cmd(TelnetProtocol::kDo, kNaws) + cmd(TelnetProtocol::kWill, TelnetProtocol::kEcho)
        + cmd(TelnetProtocol::kWill, TelnetProtocol::kSuppressGoAhead) + "\r\nboard login: ";
    in.append(char(TelnetProtocol::kIac));
    in.append(char(241)); // NOP
    QByteArray reply;
    QCOMPARE(p.receive(in, &reply), QByteArray("\r\nboard login: "));
}

void TestTelnetProtocol::aSequenceSplitAcrossReadsIsStillStripped()
{
    // THE ORDINARY CASE ON A SLOW LINK, not a corner: state crosses reads, or the second half of
    // the command arrives as data in front of the prompt.
    TelnetProtocol p;
    const QByteArray whole = cmd(TelnetProtocol::kWill, TelnetProtocol::kEcho) + "login: ";
    QByteArray data;
    QByteArray reply;
    for (const char c : whole)
        data += p.receive(QByteArrayView(&c, 1), &reply);
    QCOMPARE(data, QByteArray("login: "));
    QCOMPARE(reply, cmd(TelnetProtocol::kDo, TelnetProtocol::kEcho));
}

void TestTelnetProtocol::aDoubledIacIsOneByteOfData()
{
    TelnetProtocol p;
    QByteArray in("a");
    in.append(char(0xff));
    in.append(char(0xff));
    in.append("b");
    QByteArray want("a");
    want.append(char(0xff));
    want.append("b");
    QCOMPARE(p.receive(in, nullptr), want);
}

void TestTelnetProtocol::whatWeDoNotDoIsRefusedAndWhatWeDoIsAgreedTo()
{
    TelnetProtocol p;
    QByteArray reply;
    p.receive(cmd(TelnetProtocol::kDo, kTerminalType) + cmd(TelnetProtocol::kDo, TelnetProtocol::kEcho)
                  + cmd(TelnetProtocol::kWill, TelnetProtocol::kEcho)
                  + cmd(TelnetProtocol::kWill, kNaws),
              &reply);
    // Terminal type and echo are things WE would have to do, and do not: refused. Their echo is
    // agreed to, because nearly every telnetd insists on it; their NAWS is not something they
    // perform for us and is refused too.
    QCOMPARE(reply, cmd(TelnetProtocol::kWont, kTerminalType)
                        + cmd(TelnetProtocol::kWont, TelnetProtocol::kEcho)
                        + cmd(TelnetProtocol::kDo, TelnetProtocol::kEcho)
                        + cmd(TelnetProtocol::kDont, kNaws));
    QVERIFY(p.theyEcho());
}

void TestTelnetProtocol::ourOwnRequestIsNotAnsweredAgainWhenItIsGranted()
{
    // RFC 854's loop: a WILL that confirms our own DO is an answer, not a question.
    TelnetProtocol p;
    const QByteArray opening = p.openingNegotiation();
    QVERIFY(opening.contains(cmd(TelnetProtocol::kDo, TelnetProtocol::kBinary)));
    QVERIFY(opening.contains(cmd(TelnetProtocol::kWill, TelnetProtocol::kBinary)));

    QByteArray reply;
    p.receive(cmd(TelnetProtocol::kWill, TelnetProtocol::kBinary)
                  + cmd(TelnetProtocol::kDo, TelnetProtocol::kBinary)
                  + cmd(TelnetProtocol::kWill, TelnetProtocol::kSuppressGoAhead),
              &reply);
    QVERIFY2(reply.isEmpty(), reply.toHex().constData());
    QVERIFY(p.weSendBinary());
    QVERIFY(p.theySendBinary());
}

void TestTelnetProtocol::aRepeatedOfferIsNotAnsweredTwice()
{
    TelnetProtocol p;
    QByteArray reply;
    p.receive(cmd(TelnetProtocol::kWill, TelnetProtocol::kEcho), &reply);
    QCOMPARE(reply, cmd(TelnetProtocol::kDo, TelnetProtocol::kEcho));
    reply.clear();
    p.receive(cmd(TelnetProtocol::kWill, TelnetProtocol::kEcho), &reply);
    QVERIFY(reply.isEmpty());
    // And withdrawing it is acknowledged once.
    p.receive(cmd(TelnetProtocol::kWont, TelnetProtocol::kEcho), &reply);
    QCOMPARE(reply, cmd(TelnetProtocol::kDont, TelnetProtocol::kEcho));
    QVERIFY(!p.theyEcho());
}

void TestTelnetProtocol::aSubnegotiationIsSkippedWhole()
{
    TelnetProtocol p;
    QByteArray in;
    in.append(char(TelnetProtocol::kIac));
    in.append(char(TelnetProtocol::kSb));
    in.append(char(kTerminalType));
    in.append(char(1));
    in.append("xterm");
    in.append(char(TelnetProtocol::kIac));
    in.append(char(TelnetProtocol::kSe));
    in.append("$ ");
    QCOMPARE(p.receive(in, nullptr), QByteArray("$ "));
}

void TestTelnetProtocol::outsideBinaryACrNulFoldsBackToCr()
{
    TelnetProtocol p;
    QByteArray in("a\r");
    QByteArray first = p.receive(in, nullptr);
    // The NUL arrives in the next read, and the fold still happens.
    QByteArray second = p.receive(QByteArray(1, '\0') + "b\r\n", nullptr);
    QCOMPARE(first + second, QByteArray("a\rb\r\n"));
}

void TestTelnetProtocol::inBinaryACrNulIsTwoBytesOfTheFile()
{
    TelnetProtocol p;
    p.openingNegotiation();
    p.receive(cmd(TelnetProtocol::kWill, TelnetProtocol::kBinary), nullptr);
    QVERIFY(p.theySendBinary());
    QByteArray in("a\r");
    in.append('\0');
    in.append('b');
    QCOMPARE(p.receive(in, nullptr), in);
}

void TestTelnetProtocol::encodingDoublesIacAndOnlyTranslatesNewlinesOutsideBinary()
{
    TelnetProtocol nvt;
    QByteArray data("echo x\n");
    data.append(char(0xff));
    QByteArray want("echo x\r\n");
    want.append(char(0xff));
    want.append(char(0xff));
    QCOMPARE(nvt.encode(data), want);

    TelnetProtocol binary;
    binary.openingNegotiation();
    binary.receive(cmd(TelnetProtocol::kDo, TelnetProtocol::kBinary), nullptr);
    QVERIFY(binary.weSendBinary());
    QByteArray wantBinary("echo x\n");
    wantBinary.append(char(0xff));
    wantBinary.append(char(0xff));
    QCOMPARE(binary.encode(data), wantBinary);
}

QTEST_GUILESS_MAIN(TestTelnetProtocol)
#include "tst_telnetprotocol.moc"
