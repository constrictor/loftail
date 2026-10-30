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

// File ▸ Open Remote, the browser (SPEC.md §3): discovered devices under their types, saved
// hosts beside them, a filter over both, and an address typed outright — and what each of those
// hands back. A scripted DiscoveryBrowser stands in for the network through the factory seam,
// so every case here is about the dialog and none about a responder.

#include <QtTest>

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

#include "DiscoveryBrowser.h"
#include "FakeSecretStore.h"
#include "HostBookmarkStore.h"
#include "OpenRemoteDialog.h"
#include "RecentRemotes.h"
#include "RemoteBrowserDialog.h"
#include "RemoteLocation.h"
#include "SecretStore.h"
#include "SshPrompter.h"

using namespace loftail;

namespace {

// What the network holds, per type name: devices to report, or a reason the browse fails.
struct Script
{
    QHash<QString, QList<DiscoveredDevice>> devices;
    QHash<QString, QString> failures;
};

class ScriptedBrowser final : public DiscoveryBrowser
{
public:
    ScriptedBrowser(const Script *script, QObject *parent)
        : DiscoveryBrowser(parent), m_script(script)
    {
    }
    void start(const RemoteType &type) override
    {
        m_type = type;
        QTimer::singleShot(10, this, [this] {
            if (m_script->failures.contains(m_type.name)) {
                emit failed(m_script->failures.value(m_type.name));
                return;
            }
            for (const DiscoveredDevice &d : m_script->devices.value(m_type.name))
                report(m_type, d);
        });
    }
    void stop() override {}
    void loseIt(const QString &id) { emit lost(m_type.name, id); }
    // A device turning up later than the first answer, as one does while the dialog is open.
    void announce(const DiscoveredDevice &d) { report(m_type, d); }

private:
    const Script *m_script;
    RemoteType m_type;
};

DiscoveredDevice device(const QString &id, const QString &name, const QString &host,
                        const QStringList &txt)
{
    DiscoveredDevice d;
    d.id = id;
    d.displayName = name;
    d.host = host;
    d.port = 22;
    d.details = txt;
    return d;
}

QList<QTreeWidgetItem *> rowsUnder(QTreeWidget *tree, const QString &group)
{
    QList<QTreeWidgetItem *> out;
    for (int g = 0; g < tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *top = tree->topLevelItem(g);
        if (top->text(0) != group)
            continue;
        for (int i = 0; i < top->childCount(); ++i) {
            if (top->child(i)->flags() & Qt::ItemIsSelectable)
                out.append(top->child(i));
        }
    }
    return out;
}

QTreeWidgetItem *rowNamed(QTreeWidget *tree, const QString &group, const QString &name)
{
    for (QTreeWidgetItem *row : rowsUnder(tree, group)) {
        if (row->text(0) == name)
            return row;
    }
    return nullptr;
}

// The name of the selected row, or an empty string for none.
QString selectedName(QTreeWidget *tree)
{
    const auto items = tree->selectedItems();
    return items.isEmpty() ? QString() : items.first()->text(0);
}

// Typing as a person does: each character a real key press THROUGH THE QWindow, so it lands
// on whatever holds the focus rather than on a widget the test chose.
void typeAtFocus(QWindow *window, const QString &text)
{
    for (const QChar c : text)
        QTest::keyClick(window, c.toLatin1());
}

} // namespace

class TestRemoteBrowser : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    Script        m_script;
    FakeSecretStore m_secrets;

    HostBookmarkStore freshStore()
    {
        const QString sub = m_dir.filePath(QString::number(++m_serial));
        QDir().mkpath(sub);
        return HostBookmarkStore(sub);
    }
    int m_serial = 0;

    static RemoteType cameras()
    {
        RemoteType t;
        t.name = QStringLiteral("Cameras");
        t.serviceType = QStringLiteral("_ssh._tcp");
        t.txtRe = QStringLiteral("^model=X");
        t.connection = RemoteType::Connection::Telnet;
        t.user = QStringLiteral("root");
        t.defaultLogPath = QStringLiteral("/var/log/camera.log");
        return t;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QStandardPaths::setTestModeEnabled(true);
        setSecretStore(&m_secrets);
        setDiscoveryBrowserFactory([this](const RemoteType &, QObject *parent) {
            return std::make_unique<ScriptedBrowser>(&m_script, parent);
        });
    }
    void cleanupTestCase()
    {
        setDiscoveryBrowserFactory(nullptr);
        setSecretStore(nullptr);
    }
    void init()
    {
        m_script = Script{};
        clearDiscoveredDevices();
        SshCredentialCache::clear();
    }

    void discoveredDevicesAppearUnderTheirTypeAndOpenByReference();
    void aDeviceTheTypeExcludesIsNotListed();
    void aLostDeviceLeavesTheList();
    void theFilterNarrowsEveryGroupAtOnce();
    void aSavedHostIsChosenByNameWithItsRememberedLogs();
    void aTypeThatCannotBrowseSaysWhyInItsGroup();
    void anAddressIsShownWithoutAPortAndOnvifDetailsLeadWithTheHardware();
    void connectingDirectlyFillsTheHostFormFromAType();
    void openWaitsForARowAndALog();
    void typingKeepsTheFirstResultSelected();
    void upAndDownStepThroughTheResultsWithoutLeavingTheFilter();
    void aResultThatTurnsUpIsSelectedOnlyIfNothingWas();
    void returnInTheFilterOpensTheSelectedResult();
    void anOfferedLogGoesWithItsRowAndATypedOneStays();
    void theRecencyListIsOneEntryPerMachineNewestFirst();
    void recentlyOpenedRowsComeFirstAndTheRestKeepTheirOrder();

private:
    // Two devices under Cameras and one saved host, every one of them a result of an empty
    // filter: Lobby 10.0.0.5, Garage 10.0.0.6, then "yard server" 10.1.1.1.
    void scriptTwoCamerasAndAYard(HostBookmarkStore &store)
    {
        QVERIFY(store.saveType(cameras()));
        HostBookmark h;
        h.label = QStringLiteral("yard server");
        h.host = QStringLiteral("10.1.1.1");
        h.paths = {QStringLiteral("/var/log/ci.log"), QStringLiteral("/var/log/ci-old.log")};
        QVERIFY(store.save(h));
        m_script.devices[QStringLiteral("Cameras")] = {
            device(QStringLiteral("a"), QStringLiteral("Lobby"), QStringLiteral("10.0.0.5"),
                   {QStringLiteral("model=X1")}),
            device(QStringLiteral("b"), QStringLiteral("Garage"), QStringLiteral("10.0.0.6"),
                   {QStringLiteral("model=X2")})};
    }
    // Shown, active and typing into the filter — or the case cannot say where the focus is.
    static bool readyAtTheFilter(RemoteBrowserDialog &dialog, QLineEdit *filter)
    {
        dialog.show();
        dialog.activateWindow();
        if (!QTest::qWaitForWindowActive(&dialog))
            return false;
        filter->setFocus();
        return QTest::qWaitFor([&] { return dialog.focusWidget() == filter; });
    }
};

void TestRemoteBrowser::discoveredDevicesAppearUnderTheirTypeAndOpenByReference()
{
    HostBookmarkStore store = freshStore();
    QVERIFY(store.saveType(cameras()));
    m_script.devices[QStringLiteral("Cameras")] = {
        device(QStringLiteral("urn:uuid:cam-1"), QStringLiteral("Lobby"),
               QStringLiteral("10.0.0.5"), {QStringLiteral("model=X100")})};

    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    auto *log = dialog.findChild<QComboBox *>(QStringLiteral("remoteBrowserLog"));
    QVERIFY(tree && log);
    QTRY_VERIFY(rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Lobby")));
    QTreeWidgetItem *lobby = rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Lobby"));
    // The address alone: the advertised port (22 here) is not shown beside it.
    QCOMPARE(lobby->text(2), QStringLiteral("10.0.0.5"));

    tree->setCurrentItem(lobby);
    // The type's default log is offered as soon as the device is chosen.
    QCOMPARE(log->currentText(), QStringLiteral("/var/log/camera.log"));

    // RETURN ON THE ROW opens it — itemActivated, which is both the double-click and the key.
    dialog.show();
    tree->setFocus();
    QTest::keyClick(tree, Qt::Key_Return);
    QTRY_COMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.choice(), RemoteBrowserDialog::Choice::Discovered);
    // BY REFERENCE: the type and the device's own id, and no address anywhere in it.
    const auto opened = RemoteLocation::parse(dialog.chosenUrl());
    QVERIFY(opened.has_value());
    QCOMPARE(opened->transport, RemoteLocation::Transport::Discovered);
    QCOMPARE(opened->discoveryType, QStringLiteral("Cameras"));
    QCOMPARE(opened->host, QStringLiteral("urn:uuid:cam-1"));
    QCOMPARE(opened->path, QStringLiteral("/var/log/camera.log"));
    QVERIFY(!dialog.chosenUrl().contains(QStringLiteral("10.0.0.5")));
}

void TestRemoteBrowser::aDeviceTheTypeExcludesIsNotListed()
{
    HostBookmarkStore store = freshStore();
    QVERIFY(store.saveType(cameras()));
    m_script.devices[QStringLiteral("Cameras")] = {
        device(QStringLiteral("cam"), QStringLiteral("Camera"), QStringLiteral("10.0.0.5"),
               {QStringLiteral("model=X1")}),
        device(QStringLiteral("printer"), QStringLiteral("Printer"), QStringLiteral("10.0.0.6"),
               {QStringLiteral("model=LaserJet")})};
    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    QTRY_VERIFY(rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Camera")));
    QTest::qWait(100);
    QVERIFY(!rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Printer")));
}

void TestRemoteBrowser::aLostDeviceLeavesTheList()
{
    HostBookmarkStore store = freshStore();
    QVERIFY(store.saveType(cameras()));
    m_script.devices[QStringLiteral("Cameras")] = {
        device(QStringLiteral("cam"), QStringLiteral("Camera"), QStringLiteral("10.0.0.5"),
               {QStringLiteral("model=X1")})};
    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    QTRY_VERIFY(rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Camera")));
    auto browsers = dialog.findChildren<DiscoveryBrowser *>();
    QVERIFY(!browsers.isEmpty());
    static_cast<ScriptedBrowser *>(browsers.first())->loseIt(QStringLiteral("cam"));
    QVERIFY(!rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Camera")));
}

void TestRemoteBrowser::theFilterNarrowsEveryGroupAtOnce()
{
    HostBookmarkStore store = freshStore();
    QVERIFY(store.saveType(cameras()));
    HostBookmark h;
    h.label = QStringLiteral("build server");
    h.host = QStringLiteral("10.1.1.1");
    h.paths = {QStringLiteral("/var/log/ci.log")};
    QVERIFY(store.save(h));
    m_script.devices[QStringLiteral("Cameras")] = {
        device(QStringLiteral("a"), QStringLiteral("Lobby"), QStringLiteral("10.0.0.5"),
               {QStringLiteral("model=X1")}),
        device(QStringLiteral("b"), QStringLiteral("Garage"), QStringLiteral("10.0.0.6"),
               {QStringLiteral("model=X2"), QStringLiteral("room=lobby")})};
    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    auto *filter = dialog.findChild<QLineEdit *>(QStringLiteral("remoteBrowserFilter"));
    QTRY_COMPARE(rowsUnder(tree, QStringLiteral("Cameras")).size(), 2);

    // "lobby" is one device's name and the other's TXT record — both stay; the saved host goes.
    filter->setText(QStringLiteral("LOBBY"));
    QVERIFY(!rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Lobby"))->isHidden());
    QVERIFY(!rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Garage"))->isHidden());
    QVERIFY(rowNamed(tree, QStringLiteral("Saved hosts"), QStringLiteral("build server"))
                ->parent()->isHidden());

    filter->setText(QStringLiteral("10.1.1"));
    QVERIFY(rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Lobby"))->isHidden());
    QVERIFY(!rowNamed(tree, QStringLiteral("Saved hosts"), QStringLiteral("build server"))
                 ->isHidden());

    filter->clear();
    QVERIFY(!rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Garage"))->isHidden());
}

void TestRemoteBrowser::aSavedHostIsChosenByNameWithItsRememberedLogs()
{
    HostBookmarkStore store = freshStore();
    HostBookmark h;
    h.label = QStringLiteral("prod");
    h.host = QStringLiteral("web1");
    h.paths = {QStringLiteral("/var/log/app.log"), QStringLiteral("/var/log/sys.log")};
    QVERIFY(store.save(h));
    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    auto *log = dialog.findChild<QComboBox *>(QStringLiteral("remoteBrowserLog"));
    QTreeWidgetItem *row = rowNamed(tree, QStringLiteral("Saved hosts"), QStringLiteral("prod"));
    QVERIFY(row);
    tree->setCurrentItem(row);
    QCOMPARE(log->count(), 2);
    log->setCurrentIndex(1);
    dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Open)->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.choice(), RemoteBrowserDialog::Choice::SavedHost);
    // By NAME, so MainWindow::openRemoteBookmark() applies the host's own settings.
    QCOMPARE(dialog.chosenHost(), QStringLiteral("prod"));
    QCOMPARE(dialog.chosenPath(), QStringLiteral("/var/log/sys.log"));
}

void TestRemoteBrowser::aTypeThatCannotBrowseSaysWhyInItsGroup()
{
    HostBookmarkStore store = freshStore();
    QVERIFY(store.saveType(cameras()));
    m_script.failures[QStringLiteral("Cameras")] =
        QStringLiteral("The Avahi daemon is not running");
    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    QTRY_VERIFY([&] {
        for (int g = 0; g < tree->topLevelItemCount(); ++g) {
            QTreeWidgetItem *top = tree->topLevelItem(g);
            for (int i = 0; i < top->childCount(); ++i) {
                if (top->child(i)->text(0).contains(QStringLiteral("Avahi daemon")))
                    return true;
            }
        }
        return false;
    }());
}

void TestRemoteBrowser::anAddressIsShownWithoutAPortAndOnvifDetailsLeadWithTheHardware()
{
    HostBookmarkStore store = freshStore();
    QVERIFY(store.saveType(cameras()));
    RemoteType onvif;
    onvif.name = QStringLiteral("Doorbells");
    onvif.discovery = RemoteType::Discovery::Onvif;
    QVERIFY(store.saveType(onvif));
    HostBookmark h;
    h.label = QStringLiteral("forwarded");
    h.host = QStringLiteral("10.1.1.1");
    h.port = 2222;
    QVERIFY(store.save(h));

    DiscoveredDevice cam = device(
        QStringLiteral("urn:uuid:door-1"), QStringLiteral("Front door"),
        QStringLiteral("10.0.0.7"),
        {QStringLiteral("onvif://www.onvif.org/type/video_encoder"),
         QStringLiteral("onvif://www.onvif.org/hardware/DB-11"),
         QStringLiteral("onvif://www.onvif.org/name/Front%20door")});
    cam.port = 80;   // the camera's web service, which says nothing about reading its log
    m_script.devices[QStringLiteral("Doorbells")] = {cam};
    // A Bonjour device whose TXT record merely LOOKS like a scope is shown as advertised: the
    // reordering and the prefix are the ONVIF type's, not the text's.
    m_script.devices[QStringLiteral("Cameras")] = {
        device(QStringLiteral("cam"), QStringLiteral("Lobby"), QStringLiteral("10.0.0.5"),
               {QStringLiteral("model=X1"),
                QStringLiteral("onvif://www.onvif.org/hardware/not-a-scope")})};

    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    QTRY_VERIFY(rowNamed(tree, QStringLiteral("Doorbells"), QStringLiteral("Front door")));
    QTRY_VERIFY(rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Lobby")));

    QTreeWidgetItem *door = rowNamed(tree, QStringLiteral("Doorbells"),
                                     QStringLiteral("Front door"));
    QCOMPARE(door->text(2), QStringLiteral("10.0.0.7"));
    QCOMPARE(door->text(3),
             QStringLiteral("hardware/DB-11  type/video_encoder  name/Front door"));
    QCOMPARE(door->toolTip(3),
             QStringLiteral("hardware/DB-11\ntype/video_encoder\nname/Front door"));

    QTreeWidgetItem *lobby = rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Lobby"));
    QCOMPARE(lobby->text(3),
             QStringLiteral("model=X1  onvif://www.onvif.org/hardware/not-a-scope"));

    // A saved host on a port of its own: still the address alone.
    QCOMPARE(rowNamed(tree, QStringLiteral("Saved hosts"), QStringLiteral("forwarded"))->text(2),
             QStringLiteral("10.1.1.1"));
}

void TestRemoteBrowser::connectingDirectlyFillsTheHostFormFromAType()
{
    // The requirement's alternative to discovery: an IP typed outright, the host form opened to
    // settle the rest, and an existing remote type CHOSEN rather than every setting typed.
    HostBookmarkStore store = freshStore();
    QVERIFY(store.saveType(cameras()));
    RemoteBrowserDialog dialog(&store);
    auto *direct = dialog.findChild<QLineEdit *>(QStringLiteral("remoteBrowserDirect"));
    auto *connectButton = dialog.findChild<QPushButton *>(QStringLiteral("remoteBrowserDirectButton"));
    QVERIFY(direct && connectButton);
    QVERIFY(!connectButton->isEnabled());
    direct->setText(QStringLiteral("10.0.0.9"));
    QVERIFY(connectButton->isEnabled());

    bool sawForm = false;
    QTimer::singleShot(0, this, [&] {
        auto *form = qobject_cast<OpenRemoteDialog *>(QApplication::activeModalWidget());
        QVERIFY(form);
        sawForm = true;
        auto *host = form->findChild<QLineEdit *>(QStringLiteral("remoteHostField"));
        QCOMPARE(host->text(), QStringLiteral("10.0.0.9"));
        auto *from = form->findChild<QComboBox *>(QStringLiteral("remoteSettingsFrom"));
        const int at = from->findData(QStringLiteral("type:Cameras"));
        QVERIFY(at > 0);
        from->setCurrentIndex(at);
        emit from->activated(at);
        // The type's connection, account and default log came across; the host typed stayed.
        QCOMPARE(form->findChild<QComboBox *>(QStringLiteral("remoteConnectionCombo"))
                     ->currentData().toInt(),
                 int(RemoteType::Connection::Telnet));
        QCOMPARE(form->findChild<QLineEdit *>(QStringLiteral("remoteUserField"))->text(),
                 QStringLiteral("root"));
        QCOMPARE(host->text(), QStringLiteral("10.0.0.9"));
        form->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Open)->click();
    });
    connectButton->click();
    QVERIFY(sawForm);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.choice(), RemoteBrowserDialog::Choice::Direct);
    QCOMPARE(dialog.chosenUrl(), QStringLiteral("telnet://root@10.0.0.9:23/var/log/camera.log"));
}

void TestRemoteBrowser::openWaitsForARowAndALog()
{
    HostBookmarkStore store = freshStore();
    HostBookmark h;
    h.label = QStringLiteral("nologs");
    h.host = QStringLiteral("web2");
    QVERIFY(store.save(h));
    RemoteBrowserDialog dialog(&store);
    auto *open = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Open);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    auto *log = dialog.findChild<QComboBox *>(QStringLiteral("remoteBrowserLog"));
    QVERIFY(!open->isEnabled());
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("Saved hosts"), QStringLiteral("nologs")));
    QVERIFY2(!open->isEnabled(), "a host with no log is not something to open yet");
    log->setEditText(QStringLiteral("/var/log/a.log"));
    QVERIFY(open->isEnabled());
}

void TestRemoteBrowser::typingKeepsTheFirstResultSelected()
{
    HostBookmarkStore store = freshStore();
    scriptTwoCamerasAndAYard(store);
    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    auto *filter = dialog.findChild<QLineEdit *>(QStringLiteral("remoteBrowserFilter"));
    auto *log = dialog.findChild<QComboBox *>(QStringLiteral("remoteBrowserLog"));
    QTRY_COMPARE(rowsUnder(tree, QStringLiteral("Cameras")).size(), 2);
    if (!readyAtTheFilter(dialog, filter))
        QSKIP("The platform will not activate the window, so nothing can hold the focus.");
    QVERIFY2(selectedName(tree).isEmpty(), "nothing is chosen before anything is typed");

    // Every address holds a "1", so the first result is the first row of the whole list...
    typeAtFocus(dialog.windowHandle(), QStringLiteral("1"));
    QCOMPARE(selectedName(tree), QStringLiteral("Lobby"));
    // ...and narrowing to one that is not first moves the selection with it, keystroke by
    // keystroke, the log field following.
    typeAtFocus(dialog.windowHandle(), QStringLiteral("0.1"));
    QCOMPARE(filter->text(), QStringLiteral("10.1"));
    QCOMPARE(selectedName(tree), QStringLiteral("yard server"));
    QCOMPARE(log->currentText(), QStringLiteral("/var/log/ci.log"));

    // Deleting is typing too: the first result is first again.
    QTest::keyClick(dialog.windowHandle(), Qt::Key_Backspace);
    QCOMPARE(selectedName(tree), QStringLiteral("Lobby"));

    // A row picked with the arrows is replaced by the next keystroke's first result.
    QTest::keyClick(dialog.windowHandle(), Qt::Key_Down);
    QCOMPARE(selectedName(tree), QStringLiteral("Garage"));
    typeAtFocus(dialog.windowHandle(), QStringLiteral("0"));
    QCOMPARE(filter->text(), QStringLiteral("10.0"));
    QCOMPARE(selectedName(tree), QStringLiteral("Lobby"));

    // Nothing matches: nothing is selected, and Open has nothing to act on.
    typeAtFocus(dialog.windowHandle(), QStringLiteral("zz"));
    QVERIFY(selectedName(tree).isEmpty());
    QVERIFY(!dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Open)->isEnabled());
    QCOMPARE(dialog.focusWidget(), filter);
}

void TestRemoteBrowser::upAndDownStepThroughTheResultsWithoutLeavingTheFilter()
{
    HostBookmarkStore store = freshStore();
    scriptTwoCamerasAndAYard(store);
    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    auto *filter = dialog.findChild<QLineEdit *>(QStringLiteral("remoteBrowserFilter"));
    QTRY_COMPARE(rowsUnder(tree, QStringLiteral("Cameras")).size(), 2);
    if (!readyAtTheFilter(dialog, filter))
        QSKIP("The platform will not activate the window, so nothing can hold the focus.");
    QWindow *window = dialog.windowHandle();
    const auto pressAndExpect = [&](Qt::Key key, const char *expected) {
        QTest::keyClick(window, key);
        QCOMPARE(selectedName(tree), QString::fromLatin1(expected));
        // THE FOCUS STAYS IN THE FILTER, which is the whole of the request: the next
        // keystroke is still typing.
        QCOMPARE(dialog.focusWidget(), filter);
    };

    // From no selection Down starts at the top. The step from Garage to the saved host passes
    // the "Saved hosts" heading, which is never a result, and neither end wraps.
    pressAndExpect(Qt::Key_Down, "Lobby");
    pressAndExpect(Qt::Key_Down, "Garage");
    pressAndExpect(Qt::Key_Down, "yard server");
    pressAndExpect(Qt::Key_Down, "yard server");
    pressAndExpect(Qt::Key_Up, "Garage");
    pressAndExpect(Qt::Key_Up, "Lobby");
    pressAndExpect(Qt::Key_Up, "Lobby");

    // A row the filter hides is stepped over: "y" keeps Lobby and the yard and hides Garage.
    typeAtFocus(window, QStringLiteral("y"));
    QVERIFY(rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Garage"))->isHidden());
    QCOMPARE(selectedName(tree), QStringLiteral("Lobby"));
    pressAndExpect(Qt::Key_Down, "yard server");
    pressAndExpect(Qt::Key_Up, "Lobby");
    // The arrows moved the selection and left the text alone.
    QCOMPARE(filter->text(), QStringLiteral("y"));

    // A group the reader collapsed over the selected row is opened again by an arrow, rather
    // than Return being left to open a row nobody can see. Down at the end, so the selection
    // does not MOVE — the view's own scroll-to-current answers only a change of row.
    pressAndExpect(Qt::Key_Down, "yard server");
    QTreeWidgetItem *yard = rowNamed(tree, QStringLiteral("Saved hosts"),
                                     QStringLiteral("yard server"));
    yard->parent()->setExpanded(false);
    pressAndExpect(Qt::Key_Down, "yard server");
    QVERIFY(yard->parent()->isExpanded());
}

void TestRemoteBrowser::aResultThatTurnsUpIsSelectedOnlyIfNothingWas()
{
    HostBookmarkStore store = freshStore();
    QVERIFY(store.saveType(cameras()));
    HostBookmark h;
    h.label = QStringLiteral("garage server");
    h.host = QStringLiteral("10.1.1.1");
    h.paths = {QStringLiteral("/var/log/ci.log")};
    QVERIFY(store.save(h));
    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    auto *filter = dialog.findChild<QLineEdit *>(QStringLiteral("remoteBrowserFilter"));
    const auto browsers = dialog.findChildren<DiscoveryBrowser *>();
    QCOMPARE(browsers.size(), 1);
    auto *network = static_cast<ScriptedBrowser *>(browsers.first());

    // With nothing typed, a device turning up is listed and not chosen for the reader.
    network->announce(device(QStringLiteral("a"), QStringLiteral("Lobby"),
                             QStringLiteral("10.0.0.5"), {QStringLiteral("model=X1")}));
    QVERIFY(rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Lobby")));
    QVERIFY(selectedName(tree).isEmpty());

    // A device arriving ABOVE the selected result does not take the selection from it.
    filter->setText(QStringLiteral("garage"));
    QCOMPARE(selectedName(tree), QStringLiteral("garage server"));
    network->announce(device(QStringLiteral("b"), QStringLiteral("Garage"),
                             QStringLiteral("10.0.0.6"), {QStringLiteral("model=X2")}));
    QVERIFY(!rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Garage"))->isHidden());
    QCOMPARE(selectedName(tree), QStringLiteral("garage server"));

    // Typed and nothing matching yet: the first device that does is selected as it arrives.
    filter->setText(QStringLiteral("porch"));
    QVERIFY(selectedName(tree).isEmpty());
    network->announce(device(QStringLiteral("c"), QStringLiteral("Porch"),
                             QStringLiteral("10.0.0.7"), {QStringLiteral("model=X3")}));
    QCOMPARE(selectedName(tree), QStringLiteral("Porch"));

    // And a selected device that goes is replaced by the next result rather than by nothing.
    network->announce(device(QStringLiteral("d"), QStringLiteral("Porch 2"),
                             QStringLiteral("10.0.0.8"), {QStringLiteral("model=X4")}));
    network->loseIt(QStringLiteral("c"));
    QCOMPARE(selectedName(tree), QStringLiteral("Porch 2"));
}

void TestRemoteBrowser::returnInTheFilterOpensTheSelectedResult()
{
    HostBookmarkStore store = freshStore();
    scriptTwoCamerasAndAYard(store);
    HostBookmark bare;
    bare.label = QStringLiteral("yard spare");
    bare.host = QStringLiteral("10.1.1.2");
    QVERIFY(store.save(bare));
    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    auto *filter = dialog.findChild<QLineEdit *>(QStringLiteral("remoteBrowserFilter"));
    auto *log = dialog.findChild<QComboBox *>(QStringLiteral("remoteBrowserLog"));
    QTRY_COMPARE(rowsUnder(tree, QStringLiteral("Cameras")).size(), 2);
    if (!readyAtTheFilter(dialog, filter))
        QSKIP("The platform will not activate the window, so nothing can hold the focus.");

    // A host with no remembered log: Return says where the missing half goes, as Return on
    // its row does, rather than doing nothing.
    typeAtFocus(dialog.windowHandle(), QStringLiteral("spare"));
    QCOMPARE(selectedName(tree), QStringLiteral("yard spare"));
    QVERIFY(log->currentText().isEmpty());
    QTest::keyClick(dialog.windowHandle(), Qt::Key_Return);
    QVERIFY(dialog.isVisible());
    QTRY_VERIFY(log->hasFocus());

    // Type, arrow, Return: the second result is opened, by name.
    filter->setFocus();
    QTRY_COMPARE(dialog.focusWidget(), filter);
    filter->clear();
    typeAtFocus(dialog.windowHandle(), QStringLiteral("yard"));
    QCOMPARE(selectedName(tree), QStringLiteral("yard server"));
    QTest::keyClick(dialog.windowHandle(), Qt::Key_Down);
    QCOMPARE(selectedName(tree), QStringLiteral("yard spare"));
    log->setEditText(QStringLiteral("/var/log/spare.log"));
    QTest::keyClick(dialog.windowHandle(), Qt::Key_Return);
    QTRY_COMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.choice(), RemoteBrowserDialog::Choice::SavedHost);
    QCOMPARE(dialog.chosenHost(), QStringLiteral("yard spare"));
    QCOMPARE(dialog.chosenPath(), QStringLiteral("/var/log/spare.log"));
}

void TestRemoteBrowser::anOfferedLogGoesWithItsRowAndATypedOneStays()
{
    HostBookmarkStore store = freshStore();
    scriptTwoCamerasAndAYard(store);
    HostBookmark bare;
    bare.label = QStringLiteral("yard spare");
    bare.host = QStringLiteral("10.1.1.2");
    QVERIFY(store.save(bare));
    RemoteBrowserDialog dialog(&store);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    auto *log = dialog.findChild<QComboBox *>(QStringLiteral("remoteBrowserLog"));
    QTRY_COMPARE(rowsUnder(tree, QStringLiteral("Cameras")).size(), 2);
    QTreeWidgetItem *spare = rowNamed(tree, QStringLiteral("Saved hosts"),
                                      QStringLiteral("yard spare"));

    // The camera's default log was offered, not typed: it does not follow the selection to a
    // host that remembers no log, which would open the camera's log path on that host.
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Lobby")));
    QCOMPARE(log->currentText(), QStringLiteral("/var/log/camera.log"));
    tree->setCurrentItem(spare);
    QVERIFY(log->currentText().isEmpty());

    // Nor does the SECOND of a host's offers, picked from the list rather than typed.
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("Saved hosts"),
                                  QStringLiteral("yard server")));
    QCOMPARE(log->currentText(), QStringLiteral("/var/log/ci.log"));
    log->setCurrentIndex(1);
    QCOMPARE(log->currentText(), QStringLiteral("/var/log/ci-old.log"));
    tree->setCurrentItem(spare);
    QVERIFY(log->currentText().isEmpty());

    // An offer the reader edited is theirs now, and stays — as a path typed outright does.
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Garage")));
    log->setEditText(QStringLiteral("/var/log/camera.log.1"));
    tree->setCurrentItem(spare);
    QCOMPARE(log->currentText(), QStringLiteral("/var/log/camera.log.1"));
}

void TestRemoteBrowser::theRecencyListIsOneEntryPerMachineNewestFirst()
{
    // A MACHINE, not a log: the type and the device's id, or the connection to a host.
    QCOMPARE(recentRemoteMachineKey(QStringLiteral("discover://Lab%20Cams/cam-1/var/log/a.log")),
             QStringLiteral("discover:Lab Cams/cam-1"));
    QCOMPARE(recentRemoteMachineKey(QStringLiteral("ssh://deploy@web1/var/log/a.log")),
             QStringLiteral("deploy@web1:22"));
    QCOMPARE(recentRemoteMachineKey(QStringLiteral("telnet://root@10.0.0.9:23/var/log/a.log")),
             QStringLiteral("telnet:root@10.0.0.9:23"));
    // Nothing Open Remote could list, and never a password.
    QVERIFY(recentRemoteMachineKey(QStringLiteral("/var/log/a.log")).isEmpty());
    QVERIFY(recentRemoteMachineKey(QStringLiteral("serial://root@ttyUSB0/var/log/a.log")).isEmpty());
    QVERIFY(!recentRemoteMachineKey(QStringLiteral("ssh://deploy:hunter2@web1/var/log/a.log"))
                 .contains(QStringLiteral("hunter2")));

    // Two logs on one machine are one entry, moved to the front by the newer open.
    QStringList recent;
    recent = withRecentRemoteMachine(recent, QStringLiteral("ssh://deploy@web1/var/log/a.log"));
    recent = withRecentRemoteMachine(recent, QStringLiteral("discover://Cams/c1/var/log/a.log"));
    recent = withRecentRemoteMachine(recent, QStringLiteral("ssh://deploy@web1/var/log/b.log"));
    QCOMPARE(recent, (QStringList{QStringLiteral("deploy@web1:22"),
                                  QStringLiteral("discover:Cams/c1")}));
    // A local log leaves the list as it was.
    QCOMPARE(withRecentRemoteMachine(recent, QStringLiteral("/var/log/a.log")), recent);

    // The last twenty, the oldest dropped.
    for (int i = 0; i < kMaxRecentRemoteMachines + 5; ++i) {
        recent = withRecentRemoteMachine(
            recent, QStringLiteral("discover://Cams/d%1/var/log/a.log").arg(i));
    }
    QCOMPARE(recent.size(), kMaxRecentRemoteMachines);
    QCOMPARE(recent.first(),
             QStringLiteral("discover:Cams/d%1").arg(kMaxRecentRemoteMachines + 4));
    QCOMPARE(recent.last(), QStringLiteral("discover:Cams/d5"));
}

void TestRemoteBrowser::recentlyOpenedRowsComeFirstAndTheRestKeepTheirOrder()
{
    HostBookmarkStore store = freshStore();
    QVERIFY(store.saveType(cameras()));
    QVector<HostBookmark> hosts;
    for (const char *name : {"alpha", "bravo", "charlie"}) {
        HostBookmark h;
        h.label = QString::fromLatin1(name);
        h.host = QStringLiteral("10.2.0.") + QString::number(hosts.size() + 1);
        QVERIFY(store.save(h));
        hosts.append(h);
    }
    // Found in this order, which is the order the rest keeps.
    m_script.devices[QStringLiteral("Cameras")] = {
        device(QStringLiteral("a"), QStringLiteral("Lobby"), QStringLiteral("10.0.0.5"),
               {QStringLiteral("model=X1")}),
        device(QStringLiteral("b"), QStringLiteral("Garage door"), QStringLiteral("10.0.0.6"),
               {QStringLiteral("model=X2")}),
        device(QStringLiteral("c"), QStringLiteral("Porch door"), QStringLiteral("10.0.0.7"),
               {QStringLiteral("model=X3")}),
        device(QStringLiteral("d"), QStringLiteral("Attic"), QStringLiteral("10.0.0.8"),
               {QStringLiteral("model=X4")})};
    const auto namesUnder = [](QTreeWidget *tree, const QString &group) {
        QStringList names;
        for (QTreeWidgetItem *row : rowsUnder(tree, group))
            names.append(row->text(0));
        return names;
    };

    // THE CONTROL: with nothing opened yet, the order discovery found them in and the order
    // the hosts were saved in — so what the second dialog shows is the recency list's doing.
    {
        RemoteBrowserDialog dialog(&store);
        auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
        QTRY_COMPARE(rowsUnder(tree, QStringLiteral("Cameras")).size(), 4);
        QCOMPARE(namesUnder(tree, QStringLiteral("Cameras")),
                 (QStringList{QStringLiteral("Lobby"), QStringLiteral("Garage door"),
                              QStringLiteral("Porch door"), QStringLiteral("Attic")}));
        QCOMPARE(namesUnder(tree, QStringLiteral("Saved hosts")),
                 (QStringList{QStringLiteral("alpha"), QStringLiteral("bravo"),
                              QStringLiteral("charlie")}));
    }

    // Opened oldest first, through the addresses those opens record: device g and then
    // device a, host alpha, device c, host charlie, then device e — g and e being devices
    // nothing has found yet.
    QStringList recent;
    for (const QString &address :
         {QStringLiteral("discover://Cameras/g/var/log/camera.log"),
          QStringLiteral("discover://Cameras/a/var/log/camera.log"),
          hosts.at(0).locationFor(QStringLiteral("/var/log/app.log")).toString(),
          QStringLiteral("discover://Cameras/c/var/log/camera.log"),
          hosts.at(2).locationFor(QStringLiteral("/var/log/app.log")).toString(),
          QStringLiteral("discover://Cameras/e/var/log/camera.log")}) {
        recent = withRecentRemoteMachine(recent, address);
    }

    RemoteBrowserDialog dialog(&store, recent);
    auto *tree = dialog.findChild<QTreeWidget *>(QStringLiteral("remoteBrowserTree"));
    auto *filter = dialog.findChild<QLineEdit *>(QStringLiteral("remoteBrowserFilter"));
    QTRY_COMPARE(rowsUnder(tree, QStringLiteral("Cameras")).size(), 4);
    // Newest first, then the rest in the order they had anyway.
    QCOMPARE(namesUnder(tree, QStringLiteral("Cameras")),
             (QStringList{QStringLiteral("Porch door"), QStringLiteral("Lobby"),
                          QStringLiteral("Garage door"), QStringLiteral("Attic")}));
    QCOMPARE(namesUnder(tree, QStringLiteral("Saved hosts")),
             (QStringList{QStringLiteral("charlie"), QStringLiteral("alpha"),
                          QStringLiteral("bravo")}));

    // What the reader types is matched by a recent device first, though another was found
    // before it: the first result is the one they last opened.
    filter->setText(QStringLiteral("door"));
    QCOMPARE(selectedName(tree), QStringLiteral("Porch door"));
    filter->clear();

    // Devices still turning up: the most recently opened goes in at the top, one opened
    // longer ago than those listed goes in after them, one never opened at the bottom, and
    // the selection stays on the row it was on.
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("Cameras"), QStringLiteral("Garage door")));
    const auto browsers = dialog.findChildren<DiscoveryBrowser *>();
    QCOMPARE(browsers.size(), 1);
    auto *network = static_cast<ScriptedBrowser *>(browsers.first());
    // At an address nobody has seen it at: recency is the device's, not the address's.
    network->announce(device(QStringLiteral("e"), QStringLiteral("Shed"),
                             QStringLiteral("192.168.9.9"), {QStringLiteral("model=X5")}));
    network->announce(device(QStringLiteral("f"), QStringLiteral("Yard"),
                             QStringLiteral("10.0.0.10"), {QStringLiteral("model=X6")}));
    network->announce(device(QStringLiteral("g"), QStringLiteral("Gate"),
                             QStringLiteral("10.0.0.11"), {QStringLiteral("model=X7")}));
    QCOMPARE(namesUnder(tree, QStringLiteral("Cameras")),
             (QStringList{QStringLiteral("Shed"), QStringLiteral("Porch door"),
                          QStringLiteral("Lobby"), QStringLiteral("Gate"),
                          QStringLiteral("Garage door"), QStringLiteral("Attic"),
                          QStringLiteral("Yard")}));
    QCOMPARE(selectedName(tree), QStringLiteral("Garage door"));
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestRemoteBrowser test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_remotebrowser.moc"
