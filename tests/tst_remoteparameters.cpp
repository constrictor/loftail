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

// File ▸ Remote Parameters (SPEC.md §3, §4): the remote types, edited as a saved host is, and
// the saved hosts, edited by the very form File ▸ Open Remote used to be. Every widget is found
// by its object name.

#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include "DiscoveryBrowser.h"
#include "FakeSecretStore.h"
#include "HostBookmarkStore.h"
#include "OpenRemoteDialog.h"
#include "RemoteParametersDialog.h"
#include "SecretStore.h"
#include "SshPrompter.h"

using namespace loftail;

namespace {

template <typename T>
T *child(QWidget &w, const char *name)
{
    return w.findChild<T *>(QString::fromLatin1(name));
}

class OneDeviceBrowser final : public DiscoveryBrowser
{
public:
    using DiscoveryBrowser::DiscoveryBrowser;
    void start(const RemoteType &type) override
    {
        m_type = type;
        QTimer::singleShot(10, this, [this] {
            DiscoveredDevice d;
            d.id = QStringLiteral("cam-1");
            d.displayName = QStringLiteral("Lobby camera");
            d.host = QStringLiteral("10.0.0.5");
            d.port = 22;
            d.details = {QStringLiteral("model=X100")};
            report(m_type, d);
        });
    }
    void stop() override {}

private:
    RemoteType m_type;
};

} // namespace

class TestRemoteParameters : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir   m_dir;
    FakeSecretStore m_secrets;
    int             m_serial = 0;

    HostBookmarkStore freshStore()
    {
        const QString sub = m_dir.filePath(QString::number(++m_serial));
        QDir().mkpath(sub);
        return HostBookmarkStore(sub);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QStandardPaths::setTestModeEnabled(true);
        setSecretStore(&m_secrets);
        setDiscoveryBrowserFactory([](const RemoteType &, QObject *parent) {
            return std::make_unique<OneDeviceBrowser>(parent);
        });
    }
    void cleanupTestCase()
    {
        setDiscoveryBrowserFactory(nullptr);
        setSecretStore(nullptr);
    }
    void init()
    {
        m_secrets = FakeSecretStore();
        SshCredentialCache::clear();
    }

    void everyTypeFieldRoundTripsThroughTheForm();
    void thePasswordWaitsForAnAccount();
    void aRememberedPasswordGoesToTheKeychainAndNotTheFile();
    void saveSaysUpdateWhenTheNameAlreadyNamesAType();
    void theDiscoveryFieldsFollowTheKindOfDiscovery();
    void testDiscoveryShowsWhatTheFormWouldFind();
    void theHostsPageIsTheHostFormWithNothingToOpen();
};

void TestRemoteParameters::everyTypeFieldRoundTripsThroughTheForm()
{
    HostBookmarkStore store = freshStore();
    m_secrets.setAvailable(false);   // so a remembered password lands in the file, visibly
    {
        RemoteParametersDialog dialog(&store);
        child<QLineEdit>(dialog, "remoteTypeName")->setText(QStringLiteral("Cameras"));
        auto *discovery = child<QComboBox>(dialog, "remoteTypeDiscovery");
        discovery->setCurrentIndex(discovery->findData(int(RemoteType::Discovery::Onvif)));
        child<QLineEdit>(dialog, "remoteTypeService")->setText(QStringLiteral("_telnet._tcp"));
        child<QLineEdit>(dialog, "remoteTypeDomain")->setText(QStringLiteral("lab."));
        child<QLineEdit>(dialog, "remoteTypeInstance")->setText(QStringLiteral("^cam"));
        child<QLineEdit>(dialog, "remoteTypeTxt")->setText(QStringLiteral("^model=X"));
        child<QLineEdit>(dialog, "remoteTypeScope")->setText(QStringLiteral("/hardware/Acme"));
        auto *connection = child<QComboBox>(dialog, "remoteTypeConnection");
        connection->setCurrentIndex(connection->findData(int(RemoteType::Connection::Telnet)));
        child<QSpinBox>(dialog, "remoteTypePort")->setValue(2323);
        child<QLineEdit>(dialog, "remoteTypeUser")->setText(QStringLiteral("admin"));
        child<QLineEdit>(dialog, "remoteTypePassword")->setText(QStringLiteral("s3cret"));
        child<QCheckBox>(dialog, "remoteTypeRemember")->setChecked(true);
        child<QLineEdit>(dialog, "remoteTypeDefaultLog")->setText(QStringLiteral("/var/log/c.log"));
        child<QSpinBox>(dialog, "remoteTypePoll")->setValue(3000);
        child<QCheckBox>(dialog, "remoteTypeTailOnly")->setChecked(true);
        child<QSpinBox>(dialog, "remoteTypeTailMb")->setValue(2);
        child<QCheckBox>(dialog, "remoteTypeCompress")->setChecked(true);
        child<QPushButton>(dialog, "remoteTypeSave")->click();
    }

    const QVector<RemoteType> saved = store.types();
    QCOMPARE(saved.size(), 1);
    const RemoteType &t = saved.at(0);
    QCOMPARE(t.name, QStringLiteral("Cameras"));
    QCOMPARE(t.discovery, RemoteType::Discovery::Onvif);
    QCOMPARE(t.serviceType, QStringLiteral("_telnet._tcp"));
    QCOMPARE(t.domain, QStringLiteral("lab."));
    QCOMPARE(t.instanceRe, QStringLiteral("^cam"));
    QCOMPARE(t.txtRe, QStringLiteral("^model=X"));
    QCOMPARE(t.scopeRe, QStringLiteral("/hardware/Acme"));
    QCOMPARE(t.connection, RemoteType::Connection::Telnet);
    QCOMPARE(t.port, 2323);
    QCOMPARE(t.user, QStringLiteral("admin"));
    QVERIFY(t.savePassword);
    QCOMPARE(t.password, QStringLiteral("s3cret"));
    QCOMPARE(t.defaultLogPath, QStringLiteral("/var/log/c.log"));
    QCOMPARE(t.pollMs, 3000);
    QCOMPARE(t.tailStartBytes, 2 * 1024 * 1024);
    QVERIFY(t.compress);

    // And back into a fresh dialog, field for field.
    RemoteParametersDialog again(&store);
    child<QListWidget>(again, "remoteTypeList")->setCurrentRow(0);
    QCOMPARE(child<QLineEdit>(again, "remoteTypeScope")->text(), QStringLiteral("/hardware/Acme"));
    QCOMPARE(child<QSpinBox>(again, "remoteTypePort")->value(), 2323);
    QCOMPARE(child<QLineEdit>(again, "remoteTypeDefaultLog")->text(),
             QStringLiteral("/var/log/c.log"));
    QCOMPARE(child<QComboBox>(again, "remoteTypeConnection")->currentData().toInt(),
             int(RemoteType::Connection::Telnet));
}

void TestRemoteParameters::thePasswordWaitsForAnAccount()
{
    // THE REQUIREMENT AS WRITTEN: a password only if a user name is given.
    HostBookmarkStore store = freshStore();
    RemoteParametersDialog dialog(&store);
    auto *user = child<QLineEdit>(dialog, "remoteTypeUser");
    auto *password = child<QLineEdit>(dialog, "remoteTypePassword");
    auto *remember = child<QCheckBox>(dialog, "remoteTypeRemember");
    QVERIFY(!password->isEnabled());
    QVERIFY(!remember->isEnabled());
    user->setText(QStringLiteral("root"));
    QVERIFY(password->isEnabled());
    QVERIFY(remember->isEnabled());
    remember->setChecked(true);
    user->clear();
    QVERIFY(!password->isEnabled());
    QVERIFY2(!remember->isChecked(), "a remembered password with no account to belong to");

    // And a password typed with no user is never saved.
    child<QLineEdit>(dialog, "remoteTypeName")->setText(QStringLiteral("NoUser"));
    password->setText(QStringLiteral("orphan"));
    child<QPushButton>(dialog, "remoteTypeSave")->click();
    QVERIFY(store.types().at(0).password.isEmpty());
    QVERIFY(!store.types().at(0).savePassword);
}

void TestRemoteParameters::aRememberedPasswordGoesToTheKeychainAndNotTheFile()
{
    HostBookmarkStore store = freshStore();
    m_secrets.setAvailable(true);
    m_secrets.setBackendName(QStringLiteral("FakeWallet"));
    RemoteParametersDialog dialog(&store);
    child<QLineEdit>(dialog, "remoteTypeName")->setText(QStringLiteral("Boards"));
    child<QLineEdit>(dialog, "remoteTypeUser")->setText(QStringLiteral("root"));
    child<QLineEdit>(dialog, "remoteTypePassword")->setText(QStringLiteral("hunter2"));
    child<QCheckBox>(dialog, "remoteTypeRemember")->setChecked(true);
    QVERIFY(child<QLabel>(dialog, "remoteTypeConsent")->text().contains(QStringLiteral("FakeWallet")));
    child<QPushButton>(dialog, "remoteTypeSave")->click();

    const RemoteType saved = store.types().at(0);
    QVERIFY(saved.savePassword);
    QVERIFY2(saved.password.isEmpty(), "the keychain holds it; the file must not");
    QVERIFY(m_secrets.holds(sshSecretKey(saved.credentialTarget())));
    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(!file.readAll().contains("hunter2"));
    // And it serves this session straight away.
    QCOMPARE(SshCredentialCache::password(saved.credentialTarget()), QStringLiteral("hunter2"));
}

void TestRemoteParameters::saveSaysUpdateWhenTheNameAlreadyNamesAType()
{
    HostBookmarkStore store = freshStore();
    RemoteType t;
    t.name = QStringLiteral("Cameras");
    QVERIFY(store.saveType(t));
    RemoteParametersDialog dialog(&store);
    auto *save = child<QPushButton>(dialog, "remoteTypeSave");
    auto *name = child<QLineEdit>(dialog, "remoteTypeName");
    QVERIFY(!save->isEnabled());
    name->setText(QStringLiteral("Routers"));
    QCOMPARE(save->text(), QStringLiteral("Save"));
    name->setText(QStringLiteral(" cameras "));
    QCOMPARE(save->text(), QStringLiteral("Update"));
    QVERIFY(!child<QPushButton>(dialog, "remoteTypeRemove")->isEnabled());
    child<QListWidget>(dialog, "remoteTypeList")->setCurrentRow(0);
    QVERIFY(child<QPushButton>(dialog, "remoteTypeRemove")->isEnabled());
}

void TestRemoteParameters::theDiscoveryFieldsFollowTheKindOfDiscovery()
{
    HostBookmarkStore store = freshStore();
    RemoteParametersDialog dialog(&store);
    dialog.show();
    auto *discovery = child<QComboBox>(dialog, "remoteTypeDiscovery");
    auto *service = child<QLineEdit>(dialog, "remoteTypeService");
    auto *scope = child<QLineEdit>(dialog, "remoteTypeScope");
    QVERIFY(service->isVisible());
    QVERIFY(!scope->isVisible());
    discovery->setCurrentIndex(discovery->findData(int(RemoteType::Discovery::Onvif)));
    QVERIFY(!service->isVisible());
    QVERIFY(scope->isVisible());
    // The console preset is a telnet setting and compression an SSH one.
    auto *connection = child<QComboBox>(dialog, "remoteTypeConnection");
    QVERIFY(!child<QComboBox>(dialog, "remoteTypeConsolePreset")->isEnabled());
    connection->setCurrentIndex(connection->findData(int(RemoteType::Connection::Telnet)));
    QVERIFY(child<QComboBox>(dialog, "remoteTypeConsolePreset")->isEnabled());
    QVERIFY(!child<QCheckBox>(dialog, "remoteTypeCompress")->isEnabled());
}

void TestRemoteParameters::testDiscoveryShowsWhatTheFormWouldFind()
{
    HostBookmarkStore store = freshStore();
    RemoteParametersDialog dialog(&store);
    child<QLineEdit>(dialog, "remoteTypeTxt")->setText(QStringLiteral("^model=X"));
    child<QPushButton>(dialog, "remoteTypeTest")->click();
    auto *results = child<QListWidget>(dialog, "remoteTypeTestResults");
    QTRY_COMPARE(results->count(), 1);
    QVERIFY(results->item(0)->text().contains(QStringLiteral("Lobby camera")));
    // A TXT rule the device does not meet finds nothing — the form AS IT STANDS, unsaved.
    child<QLineEdit>(dialog, "remoteTypeTxt")->setText(QStringLiteral("^model=Y"));
    child<QPushButton>(dialog, "remoteTypeTest")->click();
    QTest::qWait(100);
    QCOMPARE(results->count(), 0);
    QVERIFY(store.types().isEmpty());
}

void TestRemoteParameters::theHostsPageIsTheHostFormWithNothingToOpen()
{
    HostBookmarkStore store = freshStore();
    RemoteParametersDialog dialog(&store);
    dialog.showHostsPage();
    dialog.show();
    auto *form = dialog.findChild<OpenRemoteDialog *>();
    QVERIFY(form);
    QVERIFY(form->isVisible());
    // Manage mode: Save and Remove, no Open.
    QVERIFY(!form->findChild<QDialogButtonBox *>()->isVisible());
    form->findChild<QLineEdit *>(QStringLiteral("remoteHostField"))->setText(QStringLiteral("web7"));
    form->findChild<QPushButton *>(QStringLiteral("remoteSaveButton"))->click();
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().at(0).host, QStringLiteral("web7"));
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestRemoteParameters test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_remoteparameters.moc"
