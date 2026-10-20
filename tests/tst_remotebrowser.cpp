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
    void connectingDirectlyFillsTheHostFormFromAType();
    void openWaitsForARowAndALog();
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
    QCOMPARE(lobby->text(2), QStringLiteral("10.0.0.5:22"));

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

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestRemoteBrowser test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_remotebrowser.moc"
