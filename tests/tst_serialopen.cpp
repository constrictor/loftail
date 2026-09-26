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

// Opening a log on a device, driven through a real MainWindow (SPEC.md §3).
//
// The transport is faked — `tests/FakeFetcher.h`, as every above-the-transport remote case uses —
// and that costs nothing here, because what is being asserted is the half the pty test cannot
// reach: that a `serial://` address travels through the window's own funnels as an ordinary
// spooled log. The conversation itself is tst_serialdevice's.
//
// It works at all because `isRemote()` means "read through a spool" rather than "on the network",
// which is the single line that made this milestone additive — so `FakeRemoteFarm` intercepts a
// serial address with no change of its own, and so do the tab label, the waiting state, the
// settings key and the refusal strip.

#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include "ConfigFileIO.h"
#include "ConfigReset.h"
#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "FakeFetcher.h"
#include "LogView.h"
#include "MainWindow.h"
#include "RemoteLocation.h"
#include "SerialFetchOptions.h"
#include "SerialProfileStore.h"

using namespace loftail;

namespace {

constexpr auto kDevice = "usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0";

// Content that PARSES under the seeded default pattern, and that is not a nicety: a log whose
// format does not fit raises Preferences, which `exec()`s — so the test does not fail, it HANGS.
// CLAUDE.md records the same trap for tst_archiveopen, and it cost this file three runs.
QByteArray parseableLog(const char *message)
{
    return QByteArrayLiteral("2026-01-01 10:00:00,000 [main] INFO  boot - ") + message + "\n";
}

QString addressFor(const QString &path = QStringLiteral("/var/log/app.log"))
{
    return QStringLiteral("serial://root@%1%2").arg(QLatin1String(kDevice), path);
}

// The document area, reached by object name: the test contract is the object name and never the
// visible text (CLAUDE.md conventions).
QTabWidget *tabsOf(const MainWindow &w)
{
    return w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
}

} // namespace

class TestSerialOpen : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aSerialLogOpensAsAnOrdinarySpooledTab();
    void aDeviceThatIsNotThereOpensAWaitingTab();
    void aTransportRefusalKeepsItsTabAndSaysWhy();
    void theTabIsLabelledWithTheDeviceRatherThanTheAddress();
    void theFileMenuHasASerialSubmenuWithItsSettingsEntry();
    void aSerialConfigFileIsRefusedByNameRatherThanAttempted();
    void theSettingsKeyIsTheAddressAndSurvivesReopening();
    void aSerialTabComesBackWithTheSession();
};

namespace {

// ANY MODAL THAT APPEARS IS A FAILURE, NOT A HANG. Without this a log whose format does not fit
// raises Preferences and `exec()`s for ever, so the case does not go red — it stops. CLAUDE.md
// records the same guard for tst_archiveopen, in the same words and for the same reason.
class ModalGuard : public QObject
{
public:
    explicit ModalGuard(QObject *parent = nullptr) : QObject(parent)
    {
        m_timer.setInterval(100);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            if (QWidget *modal = QApplication::activeModalWidget()) {
                m_seen = modal->objectName().isEmpty() ? modal->metaObject()->className()
                                                       : modal->objectName();
                modal->close();
            }
        });
        m_timer.start();
    }
    QString seen() const { return m_seen; }

private:
    QTimer  m_timer;
    QString m_seen;
};

} // namespace

void TestSerialOpen::init()
{
    clearLogSettings();
    clearSerialSettings();
}

void TestSerialOpen::cleanup()
{
    clearLogSettings();
    clearSerialSettings();
}

void TestSerialOpen::aSerialLogOpensAsAnOrdinarySpooledTab()
{
    FakeRemoteFarm farm;
    farm.at(addressFor())->setInitialContent(QByteArrayLiteral(
        "2026-01-01 10:00:00,000 [main] INFO  boot - started\n"
        "2026-01-01 10:00:01,000 [main] WARN  boot - something\n"));

    MainWindow w;
    QVERIFY(w.openFile(addressFor()));
    QTRY_COMPARE(tabsOf(w)->count(), 1);

    auto *view = qobject_cast<DocumentView *>(tabsOf(w)->widget(0));
    QVERIFY(view);
    QTRY_VERIFY(view->context()->doc.get()->index().records.size() >= 2);
    // A spooled log, read through the ordinary local source over the spool — nothing above the
    // transport knows it came off a serial line.
    QVERIFY(logPathIsSpooled(view->context()->doc.get()->path()));
    QCOMPARE(view->context()->doc.get()->path(), addressFor());
}

void TestSerialOpen::aDeviceThatIsNotThereOpensAWaitingTab()
{
    // An unplugged board is a log that has not turned up, which M13 already handles — so it keeps
    // its tab and waits, rather than failing the open.
    FakeRemoteFarm farm;
    farm.at(addressFor())->setInitiallyUnavailable(
        QStringLiteral("usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0 is not plugged in — "
                       "waiting for it"));

    MainWindow w;
    QVERIFY(w.openFile(addressFor()));
    QTRY_COMPARE(tabsOf(w)->count(), 1);
    auto *view = qobject_cast<DocumentView *>(tabsOf(w)->widget(0));
    QVERIFY(view);
    QTRY_VERIFY(view->context()->doc.get()->isWaiting());
    QVERIFY(view->context()->doc.get()->waitReason().contains(QStringLiteral("not plugged in")));

    // And plugging it in ends the wait with no gesture, exactly as a host coming back does.
    farm.at(addressFor())->setInitialContent(parseableLog("up"));
    farm.at(addressFor())->becomeAvailable();
    QTRY_VERIFY(!view->context()->doc.get()->isWaiting());
    QTRY_VERIFY(view->context()->doc.get()->index().records.size() >= 1);
}

void TestSerialOpen::aTransportRefusalKeepsItsTabAndSaysWhy()
{
    // M17's rule, and the two refusals that reach it here are the login being told no and the
    // byte-safety ladder finding neither raw nor base64 — neither of which another poll fixes.
    FakeRemoteFarm farm;
    farm.at(addressFor())->setConnectRefusal(
        QStringLiteral("The line to /var/log/app.log changes the bytes that pass over it, and "
                       "the far end has neither a working `stty raw` nor a `base64`."));

    MainWindow w;
    QVERIFY(w.openFile(addressFor()));
    QTRY_COMPARE(tabsOf(w)->count(), 1);   // the tab STAYS
    auto *view = qobject_cast<DocumentView *>(tabsOf(w)->widget(0));
    QVERIFY(view);
    QTRY_VERIFY(view->logView()->placeholderText().contains(QStringLiteral("base64")));
}

void TestSerialOpen::theTabIsLabelledWithTheDeviceRatherThanTheAddress()
{
    FakeRemoteFarm farm;
    farm.at(addressFor())->setInitialContent(parseableLog("one"));
    farm.at(addressFor(QStringLiteral("/var/log/other.log")))
        ->setInitialContent(parseableLog("two"));

    ModalGuard guard;
    MainWindow w;
    QVERIFY(w.openFile(addressFor()));
    QTRY_COMPARE(tabsOf(w)->count(), 1);
    // THE DEVICE IS ALWAYS IN THE BRACKET — the device axis is spent unconditionally for a
    // remote log and a serial one alike — BUT NEVER AS ITS FORTY-CHARACTER ID. A stable id is a
    // vendor string, which is the right thing to key settings on and an unreadable thing to put
    // beside a log's name, so what appears is the port name it resolves to, or the label the user
    // gave the device. The id itself belongs on the tooltip.
    //
    // QTRY, because a tab wears an `— indexing N%` suffix while it is being scanned — the label is
    // only the label once the scan is done.
    QTRY_VERIFY(!tabsOf(w)->tabText(0).contains(QStringLiteral("indexing")));
    const QString label = tabsOf(w)->tabText(0);
    QVERIFY2(label.startsWith(QStringLiteral("app.log")), qUtf8Printable(label));
    QVERIFY2(!label.contains(QLatin1String(kDevice)),
             qUtf8Printable(QStringLiteral("the tab wears the whole device id: %1").arg(label)));
    // Asserted as a LENGTH relation rather than a spelling: what the bracket resolves to depends
    // on what is plugged into the machine running the test, and the claim is only that it is short.
    QVERIFY2(label.size() < int(qstrlen(kDevice)),
             qUtf8Printable(QStringLiteral("the label is longer than the id it stands in for: %1")
                                .arg(label)));
    // The full address, id and all, is what the tooltip carries.
    QVERIFY(tabsOf(w)->tabToolTip(0).contains(QLatin1String(kDevice)));

    // Two logs on one device: each keeps its own name, and the device bracket does not make them
    // read alike — which is what the distinct-label count is for.
    QVERIFY(w.openFile(addressFor(QStringLiteral("/var/log/other.log"))));
    QTRY_COMPARE(tabsOf(w)->count(), 2);
    QTRY_VERIFY(!tabsOf(w)->tabText(1).contains(QStringLiteral("indexing")));
    QVERIFY(tabsOf(w)->tabText(0).startsWith(QStringLiteral("app.log")));
    QVERIFY(tabsOf(w)->tabText(1).startsWith(QStringLiteral("other.log")));
    QVERIFY(tabsOf(w)->tabText(0) != tabsOf(w)->tabText(1));
    QVERIFY2(guard.seen().isEmpty(), qUtf8Printable(guard.seen()));
}

void TestSerialOpen::theFileMenuHasASerialSubmenuWithItsSettingsEntry()
{
    MainWindow w;
    auto *menu = w.findChild<QMenu *>(QStringLiteral("serialMenu"));
    QVERIFY2(menu, "the File menu has no Serial submenu");

    // POPULATED ON aboutToShow, which is the whole refresh schedule — so before it is shown there
    // is deliberately nothing in it, and asserting that is what says the population is not a
    // standing timer.
    QCOMPARE(menu->actions().size(), 0);
    emit menu->aboutToShow();
    QVERIFY(!menu->actions().isEmpty());

    // The Settings entry is always there, whatever is plugged in — a person with no device yet is
    // exactly the person who needs to write a preset.
    QVERIFY2(w.findChild<QAction *>(QStringLiteral("serialSettingsAction")),
             "no Settings entry under File > Serial");
}

void TestSerialOpen::aSerialConfigFileIsRefusedByNameRatherThanAttempted()
{
    // `isRemote()` is true for a serial address, so without the explicit refusal the editor would
    // reach ConfigTransfer and try a TCP connect to a host called `usb-FTDI_...` — reported as
    // though the device had refused the connection, which is a sentence about the wrong thing.
    QString reason;
    QVERIFY(!configAddressIsWritable(addressFor(QStringLiteral("/etc/log4cplus.properties")),
                                     &reason));
    QVERIFY2(reason.contains(QStringLiteral("serial")), qUtf8Printable(reason));
    // And an ssh address is unaffected, or the refusal is too wide.
    QString sshReason;
    QVERIFY(configAddressIsWritable(QStringLiteral("ssh://h/etc/log4cplus.properties"),
                                    &sshReason));
}

void TestSerialOpen::theSettingsKeyIsTheAddressAndSurvivesReopening()
{
    // ONE LOG, ONE SPELLING, for the new scheme: the settings key is the normalized address, and
    // a device opened twice is one record rather than two out of the pool of 500.
    const QString address = addressFor();
    QCOMPARE(logSettingsKey(address), address);
    QCOMPARE(logSettingsKey(logSettingsKey(address)), address);
    // The file-pattern target sees the log's own name, and the whole address where asked — which
    // is what makes `serial://*` able to claim every device (SPEC.md §4).
    QCOMPARE(logMatchTarget(address, /*fullPath=*/false), QStringLiteral("app.log"));
    QCOMPARE(logMatchTarget(address, /*fullPath=*/true), address);
}

void TestSerialOpen::aSerialTabComesBackWithTheSession()
{
    // FOR FREE, and that is the assertion: the session stores a Document's path and nothing
    // else about how it is reached, so a serial tab restores because `serial://…` IS a path.
    // No schema version moved and nothing in SessionStore knows the scheme exists.
    FakeRemoteFarm farm;
    farm.at(addressFor())->setInitialContent(parseableLog("before the quit"));

    {
        ModalGuard guard;
        MainWindow w;
        QVERIFY(w.openFile(addressFor()));
        QTRY_COMPARE(tabsOf(w)->count(), 1);
        QTRY_VERIFY(tabsOf(w)->tabText(0).startsWith(QStringLiteral("app.log")));
        w.close();   // writes the session
        QVERIFY2(guard.seen().isEmpty(), qUtf8Printable(guard.seen()));
    }

    ModalGuard guard;
    MainWindow again;
    // Restored in the constructor, before show() — which is why a refusal there goes to the
    // message strip rather than a modal, and why this guard is worth having here too.
    QTRY_COMPARE(tabsOf(again)->count(), 1);
    auto *view = qobject_cast<DocumentView *>(tabsOf(again)->widget(0));
    QVERIFY(view);
    QCOMPARE(view->context()->doc->path(), addressFor());
    QTRY_VERIFY(view->context()->doc->index().records.size() >= 1);
    QVERIFY2(guard.seen().isEmpty(), qUtf8Printable(guard.seen()));
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("XDG_CACHE_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-serialopen"));

    TestSerialOpen tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_serialopen.moc"
