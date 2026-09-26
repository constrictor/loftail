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

// Which serial ports are worth offering, and what a device is CALLED (SPEC.md §3,
// ARCHITECTURE.md §6.11).
//
// QSerialPortInfo cannot be constructed synthetically, which is the whole reason the filter
// and the id derivation are pure functions over a plain struct and the enumeration is a
// separate, gated translation unit. This is the half that can be tested, and it is the half
// that holds the decisions.

#include <QtTest>

#include "SerialDevices.h"

using namespace loftail;

namespace {

SerialDeviceInfo usbAdapter()
{
    SerialDeviceInfo i;
    i.portName = QStringLiteral("ttyUSB0");
    i.systemLocation = QStringLiteral("/dev/ttyUSB0");
    i.description = QStringLiteral("FT232R USB UART");
    i.manufacturer = QStringLiteral("FTDI");
    i.serialNumber = QStringLiteral("A50285BI");
    i.vendorId = 0x0403;
    i.productId = 0x6001;
    i.hasVendorId = true;
    return i;
}

} // namespace

class TestSerialDevices : public QObject
{
    Q_OBJECT

private slots:
    void aUsbAdapterIsOffered();
    void aBuiltInPortIsNotOffered();
    void theMacOsTtyNodeIsExcludedAndItsCuTwinIsKept();
    void aStableIdIsPreferredOverThePortName();
    void theFirstByIdLinkIsChosenSoOneDeviceHasOneAddress();
    void aSerialNumberIsTheSecondRungAndThePortNameTheLast();
    void aByIdNameKeepsItsCaseAndItsUnderscores();
    void everyRealDeviceNameIsAValidAddressHalf();
    void aNameThatCouldNotSurviveAnAddressIsRefused();
    void theLabelNamesTheAdapterAndThePort();
    void aShortNameIsWhatATabBracketCanAfford();
};

void TestSerialDevices::aUsbAdapterIsOffered()
{
    QVERIFY(looksLikeUart(usbAdapter()));

    // A CDC-ACM device — an Arduino, an STM32 — is the other ordinary shape and has a
    // vendor id just the same.
    SerialDeviceInfo acm = usbAdapter();
    acm.portName = QStringLiteral("ttyACM0");
    acm.systemLocation = QStringLiteral("/dev/ttyACM0");
    QVERIFY(looksLikeUart(acm));
}

void TestSerialDevices::aBuiltInPortIsNotOffered()
{
    // A motherboard 16550: no vendor identifier, which is the rule, and a PC enumerates
    // dozens of them.
    SerialDeviceInfo builtIn;
    builtIn.portName = QStringLiteral("ttyS0");
    builtIn.systemLocation = QStringLiteral("/dev/ttyS0");
    builtIn.hasVendorId = false;
    QVERIFY(!looksLikeUart(builtIn));

    // And excluded even WITH one, because the name is the giveaway and eight dead ports on
    // a menu is worse than none.
    SerialDeviceInfo claimsAVendor = builtIn;
    claimsAVendor.hasVendorId = true;
    claimsAVendor.vendorId = 0x1234;
    QVERIFY(!looksLikeUart(claimsAVendor));

    // ANCHORED, though: a device that merely begins with those letters is not a ttyS.
    SerialDeviceInfo notReally = usbAdapter();
    notReally.portName = QStringLiteral("ttySomething0");
    QVERIFY(looksLikeUart(notReally));

    // A port with no name at all is nothing to offer.
    QVERIFY(!looksLikeUart(SerialDeviceInfo{}));
}

void TestSerialDevices::theMacOsTtyNodeIsExcludedAndItsCuTwinIsKept()
{
    // THE SAME HARDWARE, TWICE, AND ONLY ONE OF THEM MAY BE OPENED. The `tty.` node blocks
    // until the far end asserts carrier detect, so a board that is powered off would hang
    // the open rather than be waited for — which is the freeze M17 exists to have removed,
    // and requirement 7's own case.
    SerialDeviceInfo tty = usbAdapter();
    tty.portName = QStringLiteral("tty.usbserial-A50285BI");
    tty.systemLocation = QStringLiteral("/dev/tty.usbserial-A50285BI");
    QVERIFY(!looksLikeUart(tty));

    SerialDeviceInfo cu = tty;
    cu.portName = QStringLiteral("cu.usbserial-A50285BI");
    cu.systemLocation = QStringLiteral("/dev/cu.usbserial-A50285BI");
    QVERIFY(looksLikeUart(cu));

    // And the exclusion is on `tty.` WITH the dot, so Linux's dotless ttyUSB0 is untouched
    // — which is the whole reason the test is spelled out rather than "starts with tty".
    QVERIFY(looksLikeUart(usbAdapter()));
}

void TestSerialDevices::aStableIdIsPreferredOverThePortName()
{
    // The defect this exists against: ttyUSB0 is allocated in plug order, so unplugging two
    // adapters and plugging them back the other way round makes the address name the OTHER
    // board — along with that board's remembered format, filters and settings record, with
    // nothing on screen to say so.
    SerialDeviceInfo i = usbAdapter();
    i.byIdLinks = QStringList{QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0")};
    QCOMPARE(stableIdFor(i), QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0"));
}

void TestSerialDevices::theFirstByIdLinkIsChosenSoOneDeviceHasOneAddress()
{
    // udev may make several links to one node. WHICH one is chosen has to be the same answer
    // every time, or one device acquires two addresses and therefore two settings records —
    // the "one log, one spelling" rule arriving from the hardware end. The enumerator sorts
    // them; taking the front is what makes that sort load-bearing.
    SerialDeviceInfo i = usbAdapter();
    i.byIdLinks = QStringList{QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00"),
                              QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0")};
    const QString first = stableIdFor(i);
    QCOMPARE(first, QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00"));

    // Asked again, the same answer. (Trivially true here, and the assertion is what would
    // catch a derivation that ever started depending on anything but the input.)
    QCOMPARE(stableIdFor(i), first);

    // A by-id name that could NOT survive an address is skipped rather than returned, or the
    // address it produced would be refused at parse and the device would be unopenable.
    SerialDeviceInfo odd = usbAdapter();
    odd.byIdLinks = QStringList{QStringLiteral("usb-Some Vendor_With Spaces-if00"),
                                QStringLiteral("usb-Some_Vendor-if00")};
    QCOMPARE(stableIdFor(odd), QStringLiteral("usb-Some_Vendor-if00"));
}

void TestSerialDevices::aSerialNumberIsTheSecondRungAndThePortNameTheLast()
{
    SerialDeviceInfo noLinks = usbAdapter();          // has a serial number, no by-id link
    QCOMPARE(stableIdFor(noLinks), QStringLiteral("A50285BI"));

    SerialDeviceInfo bare = usbAdapter();
    bare.serialNumber.clear();
    // Not stable, and that is a fact about the adapter rather than a failure here: plenty
    // ship with no serial number at all.
    QCOMPARE(stableIdFor(bare), QStringLiteral("ttyUSB0"));

    // A serial number with something an address cannot carry falls through to the port name
    // rather than producing an address that will not parse.
    SerialDeviceInfo awkward = usbAdapter();
    awkward.serialNumber = QStringLiteral("A5/02 85");
    QCOMPARE(stableIdFor(awkward), QStringLiteral("ttyUSB0"));
}

void TestSerialDevices::aByIdNameKeepsItsCaseAndItsUnderscores()
{
    // THE FINDING THIS WHOLE NAMING SCHEME HAD TO BE BUILT AROUND. QUrl lowercases a host,
    // and `/dev/serial/by-id` is case-sensitive, so a device id that went through QUrl's
    // authority parsing would come back as a path that does not exist — the address would
    // parse, its normal form would be a fixed point, every test would pass, and the device
    // would simply never be found. Nothing here may fold case or drop a character.
    const QString name = QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0");
    SerialDeviceInfo i = usbAdapter();
    i.byIdLinks = QStringList{name};
    const QString id = stableIdFor(i);
    QCOMPARE(id, name);
    QVERIFY(id.contains(QLatin1Char('_')));
    QVERIFY(id != id.toLower());
    QVERIFY(isValidSerialDeviceName(id));
}

void TestSerialDevices::everyRealDeviceNameIsAValidAddressHalf()
{
    // The narrow character set has to cover every platform's real names, or a device is
    // unaddressable on the platform whose convention was forgotten.
    const QStringList real{
        QStringLiteral("ttyUSB0"),
        QStringLiteral("ttyACM0"),
        QStringLiteral("ttyAMA0"),
        QStringLiteral("COM3"),
        QStringLiteral("COM12"),
        QStringLiteral("cu.usbserial-A50285BI"),
        QStringLiteral("cu.usbmodem14201"),
        QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0"),
        QStringLiteral("usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0"),
        QStringLiteral("usb-1a86_USB_Single_Serial_54D2036731-if00"),
    };
    for (const QString &name : real)
        QVERIFY2(isValidSerialDeviceName(name), qUtf8Printable(name));
}

void TestSerialDevices::aNameThatCouldNotSurviveAnAddressIsRefused()
{
    // Each of these would either change meaning inside an address or need encoding, and
    // encoding is the thing the narrow set exists to avoid: `/` ends the authority, `@`
    // starts it, `:` spells a password, `%` opens an escape, and a space and a noncharacter
    // are what the whole idempotence argument turns on.
    const QStringList refused{
        QString(),
        QStringLiteral("tty USB0"),
        QStringLiteral("ttyUSB0/extra"),
        QStringLiteral("root@ttyUSB0"),
        QStringLiteral("ttyUSB0:9600"),
        QStringLiteral("tty%55SB0"),
        QStringLiteral("tty\\USB0"),
        QStringLiteral("tty?USB0"),
        QString::fromUtf8("tty\xEF\xBF\xBF" "0"),
        QStringLiteral("dev/ttyUSB0"),
    };
    for (const QString &name : refused)
        QVERIFY2(!isValidSerialDeviceName(name), qUtf8Printable(name));
}

void TestSerialDevices::theLabelNamesTheAdapterAndThePort()
{
    // The menu shows this, NOT the address: a by-id name is forty characters of vendor
    // string, which is the right thing to key settings on and the wrong thing to aim at.
    QCOMPARE(serialDeviceLabel(usbAdapter()), QStringLiteral("FT232R USB UART (ttyUSB0)"));

    SerialDeviceInfo anonymous = usbAdapter();
    anonymous.description.clear();
    QCOMPARE(serialDeviceLabel(anonymous), QStringLiteral("ttyUSB0"));
}

void TestSerialDevices::aShortNameIsWhatATabBracketCanAfford()
{
    // A TAB BRACKET CANNOT CARRY A VENDOR STRING, and it cannot be shortened downstream either:
    // TabLabels.cpp elides the path run and deliberately NOT the device, on the reasoning that a
    // machine's name is short — true of a host name, false of `usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0`.
    // So the shortening is this function's job.
    const QString byId = QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0");

    // PLUGGED IN: the port name, which is what a person recognises and what the menu brackets.
    SerialDeviceInfo i = usbAdapter();
    i.byIdLinks = QStringList{byId};
    QCOMPARE(shortSerialDeviceName(byId, {i}), QStringLiteral("ttyUSB0"));
    QCOMPARE(shortSerialDeviceName(QStringLiteral("ttyUSB0"), {i}), QStringLiteral("ttyUSB0"));
    QCOMPARE(shortSerialDeviceName(QStringLiteral("A50285BI"), {i}), QStringLiteral("ttyUSB0"));

    // NOT PLUGGED IN, WHICH IS THE COMMON CASE — a restored session names a board that is not
    // connected yet. The serial number is peeled out of the by-id shape: short, unique, and what
    // is printed on the adapter.
    QCOMPARE(shortSerialDeviceName(byId, {}), QStringLiteral("A50285BI"));
    QCOMPARE(shortSerialDeviceName(
                 QStringLiteral("usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0"),
                 {}),
             QStringLiteral("0001"));
    QCOMPARE(shortSerialDeviceName(QStringLiteral("usb-1a86_USB_Single_Serial_54D2036731-if00"), {}),
             QStringLiteral("54D2036731"));

    // And it is always SHORTER than what it stands in for, which is the whole claim — asserted as
    // a relation rather than against each spelling.
    for (const QString &id : {byId,
                              QStringLiteral("usb-1a86_USB_Single_Serial_54D2036731-if00"),
                              QStringLiteral("usb-Silicon_Labs_CP2102_0001-if00-port0")}) {
        QVERIFY2(shortSerialDeviceName(id, {}).size() < id.size(), qUtf8Printable(id));
    }

    // A plain port name is already short and comes back untouched, plugged in or not.
    QCOMPARE(shortSerialDeviceName(QStringLiteral("ttyUSB0"), {}), QStringLiteral("ttyUSB0"));
    QCOMPARE(shortSerialDeviceName(QStringLiteral("COM3"), {}), QStringLiteral("COM3"));
    QCOMPARE(shortSerialDeviceName(QStringLiteral("cu.usbserial-A50285BI"), {}),
             QStringLiteral("cu.usbserial-A50285BI"));
    // An id this shape does not recognise is answered as itself: a name is better than none, and
    // the bracket is the caller's to bound.
    QCOMPARE(shortSerialDeviceName(QStringLiteral("whatever"), {}), QStringLiteral("whatever"));
    QCOMPARE(shortSerialDeviceName(QString(), {}), QString());
}

QTEST_APPLESS_MAIN(TestSerialDevices)
#include "tst_serialdevices.moc"
