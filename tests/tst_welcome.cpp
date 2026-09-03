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

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTemporaryDir>

#include "ConfigReset.h"
#include "HostBookmarkStore.h"
#include "MainWindow.h"
#include "TabLabels.h"
#include "WelcomeView.h"

using namespace loftail;

// The welcome screen (SPEC.md §3): what a window with no log open shows.
//
// It replaced one centred sentence naming two menus, so what is under test is that the
// two lists the menus keep are ON THE PAGE, named the way the menus name them, and that
// activating a row does what the menu entry does. Everything is found by object name; no
// case reads a visible label to decide which widget it has, which is the same contract
// tst_recentfiles works under and for the same reason.
class TestWelcome : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_good; // parses with the built-in defaults, so an open never prompts

    static bool write(const QString &path, const QByteArray &bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(bytes);
        return true;
    }

    static void seedRecent(const QStringList &paths)
    {
        QSettings settings;
        settings.setValue(QStringLiteral("recentFiles"), paths);
        settings.sync();
    }

    static WelcomeView  *welcome(MainWindow &w) { return w.findChild<WelcomeView *>(QStringLiteral("welcomeView")); }
    static QListWidget  *recentList(MainWindow &w) { return w.findChild<QListWidget *>(QStringLiteral("welcomeRecentList")); }
    static QListWidget  *remoteList(MainWindow &w) { return w.findChild<QListWidget *>(QStringLiteral("welcomeRemoteList")); }
    static QLabel       *recentEmpty(MainWindow &w) { return w.findChild<QLabel *>(QStringLiteral("welcomeRecentEmpty")); }
    static QLabel       *remoteEmpty(MainWindow &w) { return w.findChild<QLabel *>(QStringLiteral("welcomeRemoteEmpty")); }
    static QLabel       *message(MainWindow &w) { return w.findChild<QLabel *>(QStringLiteral("welcomeMessage")); }
    static QWidget      *remoteSection(MainWindow &w) { return w.findChild<QWidget *>(QStringLiteral("welcomeRemoteSection")); }
    static QTabWidget   *tabs(MainWindow &w) { return w.findChild<QTabWidget *>(QStringLiteral("documentTabs")); }

    // Which page of the centre is up. The stack has no object name of its own, and there
    // is exactly one under the window.
    static QWidget *centrePage(MainWindow &w)
    {
        auto *stack = w.findChild<QStackedWidget *>();
        return stack ? stack->currentWidget() : nullptr;
    }

    static QStringList rowLabels(QListWidget *list)
    {
        QStringList out;
        for (int i = 0; i < list->count(); ++i)
            out.append(list->item(i)->text());
        return out;
    }

private slots:
    void initTestCase();
    void init();
    void anEmptyWindowShowsTheWelcomeScreenAndALogReplacesIt();
    void everyRecentLogIsARowNamedLikeItsMenuEntry();
    void anAmpersandInANameIsNotDoubledOnAListRow();
    void everyRowCarriesItsWholeAddressAsATooltip();
    void activatingARecentRowOpensThatLog();
    void returnOnASelectedRowOpensItToo();
    void anEmptyListSaysWhichKindOfEmptyWithoutAddingARow();
    void clearingTheRecentListEmptiesThePageAsWellAsTheMenu();
    void theRecentListFollowsTheMenuAfterAnOpen();
    void openingALogClearsTheCouldNotBeReopenedMessage();
#if defined(LOFTAIL_HAVE_SSH)
    void aRememberedRemoteLogIsARowAndASavedHostWithNoneIsAnother();
#else
    void theRemotesSectionIsAbsentWithoutSsh();
#endif
};

void TestWelcome::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_good = m_dir.filePath(QStringLiteral("good.log"));
    QVERIFY(write(m_good,
        "2026-07-21 10:00:00,000 [main] INFO  net.io - starting\n"
        "2026-07-21 10:00:01,000 [work] ERROR db.pool - boom\n"));
}

void TestWelcome::init()
{
    QSettings settings;
    settings.remove(QStringLiteral("session")); // a window here must open nothing of its own
    settings.remove(QStringLiteral("recentFiles"));
    settings.sync();
    clearLogSettings();
    // The window reads the REAL hosts.json — HostBookmarkStore::defaultDir(), which the
    // isolated XDG_CONFIG_HOME in main() puts under the temporary tree — so it has to be
    // cleared here too, or a case that saves a host leaves it for every case after it.
    QFile::remove(HostBookmarkStore(HostBookmarkStore::defaultDir()).filePath());
}

void TestWelcome::anEmptyWindowShowsTheWelcomeScreenAndALogReplacesIt()
{
    MainWindow w;
    QVERIFY(welcome(w));
    QCOMPARE(centrePage(w), static_cast<QWidget *>(welcome(w)));

    QVERIFY(w.openFile(m_good));
    QCOMPARE(centrePage(w), static_cast<QWidget *>(tabs(w)));

    // And back again, because this is the state the screen exists for far more often
    // than the first launch: the well emptied by hand. Through the real File ▸ Close Tab
    // action rather than by deleting the page, since the order that path takes — out of
    // the tab widget first, destroyed second — is what updateEmptyState() is reached by.
    auto *close = w.findChild<QAction *>(QStringLiteral("closeTabAction"));
    QVERIFY(close);
    close->trigger();
    QCOMPARE(tabs(w)->count(), 0);
    QCOMPARE(centrePage(w), static_cast<QWidget *>(welcome(w)));
}

void TestWelcome::everyRecentLogIsARowNamedLikeItsMenuEntry()
{
    // One of each address shape the application accepts, and none of them is opened —
    // the page is built from the strings alone — so this holds whether or not SSH and
    // archive support were compiled in.
    const QStringList seeded{QDir::rootPath() + QStringLiteral("var/log/very/deep/tree/app.log"),
                             QStringLiteral("ssh://ops@prod-web/var/log/service.log"),
                             QDir::rootPath() + QStringLiteral("srv/bundle.tar.gz/var/log/inner.log")};
    seedRecent(seeded);

    MainWindow w;
    // The SAME rule the Open Recent menu uses, asserted against that rule's own output:
    // the two are rendered from one enumeration, so a divergence here means the page has
    // grown a naming rule of its own.
    QCOMPARE(rowLabels(recentList(w)), prefixedLabelsFor(seeded));
}

void TestWelcome::anAmpersandInANameIsNotDoubledOnAListRow()
{
    // The menu doubles an ampersand because a QMenu reads one as a mnemonic. A list does
    // not, so carrying that line over would show a literal "&&" — silently, and only for
    // logs whose name has an ampersand in it.
    const QString path = QDir::rootPath() + QStringLiteral("var/log/a&b.log");
    seedRecent({path});

    MainWindow w;
    QCOMPARE(recentList(w)->count(), 1);
    QCOMPARE(recentList(w)->item(0)->text(), QStringLiteral("a&b.log"));
}

void TestWelcome::everyRowCarriesItsWholeAddressAsATooltip()
{
    // What makes the short label safe, exactly as it does in the menu: the row is
    // shortened, the tooltip is the address verbatim, and the address is what opening
    // the row asks for.
    const QStringList seeded{QDir::rootPath() + QStringLiteral("var/log/very/deep/tree/app.log"),
                             QStringLiteral("ssh://ops@prod-web/var/log/service.log")};
    seedRecent(seeded);

    MainWindow w;
    QCOMPARE(recentList(w)->count(), seeded.size());
    for (int i = 0; i < seeded.size(); ++i)
        QCOMPARE(recentList(w)->item(i)->toolTip(), seeded.at(i));
}

void TestWelcome::activatingARecentRowOpensThatLog()
{
    seedRecent({m_good});
    MainWindow w;
    QCOMPARE(recentList(w)->count(), 1);

    emit recentList(w)->itemActivated(recentList(w)->item(0));
    QCOMPARE(tabs(w)->count(), 1);
    QCOMPARE(centrePage(w), static_cast<QWidget *>(tabs(w)));
}

void TestWelcome::returnOnASelectedRowOpensItToo()
{
    // The case that fails against an itemDoubleClicked wiring, and the only one that
    // does: a list reachable only by double-click is not reachable from a keyboard at
    // all, and nothing on screen would say so.
    seedRecent({m_good});
    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    QListWidget *list = recentList(w);
    list->setCurrentRow(0);
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Return);

    QCOMPARE(tabs(w)->count(), 1);
}

void TestWelcome::anEmptyListSaysWhichKindOfEmptyWithoutAddingARow()
{
    MainWindow w;
    // A LABEL over the viewport and never a row: a row is an entry to everything that
    // walks rows, and the count is what activation, selection and every future feature
    // read.
    QCOMPARE(recentList(w)->count(), 0);
    QVERIFY(recentEmpty(w));
    QVERIFY(!recentEmpty(w)->text().isEmpty());
    QVERIFY(recentEmpty(w)->isVisibleTo(recentList(w)));

    // And the two empties do not want the same words.
    if (remoteEmpty(w))
        QVERIFY(remoteEmpty(w)->text() != recentEmpty(w)->text());
}

void TestWelcome::clearingTheRecentListEmptiesThePageAsWellAsTheMenu()
{
    seedRecent({m_good});
    MainWindow w;
    QCOMPARE(recentList(w)->count(), 1);

    auto *clear = w.findChild<QPushButton *>(QStringLiteral("welcomeClearRecent"));
    QVERIFY(clear);
    QVERIFY(clear->isEnabled());
    clear->click();

    QCOMPARE(recentList(w)->count(), 0);
    QVERIFY(!clear->isEnabled()); // nothing left to forget
    QVERIFY(QSettings().value(QStringLiteral("recentFiles")).toStringList().isEmpty());
}

void TestWelcome::theRecentListFollowsTheMenuAfterAnOpen()
{
    // The page is refreshed by the menu's own funnel and has no schedule of its own, so
    // an open that moved the list moves the page with it. Without that, the screen shown
    // after closing the last tab still lists what was remembered when the window opened.
    MainWindow w;
    QCOMPARE(recentList(w)->count(), 0);

    QVERIFY(w.openFile(m_good));
    QCOMPARE(recentList(w)->count(), 1);
    QCOMPARE(recentList(w)->item(0)->toolTip(), m_good);
}

void TestWelcome::openingALogClearsTheCouldNotBeReopenedMessage()
{
    MainWindow w;
    QVERIFY(message(w));
    QVERIFY(!message(w)->isVisibleTo(welcome(w))); // nothing to say to begin with

    welcome(w)->setMessage(QStringLiteral("These files could not be reopened:"));
    QVERIFY(message(w)->isVisibleTo(welcome(w)));

    // Spent the moment anything opens. It matters more here than it did for the label
    // this replaced: that one was covered for the life of the window, while the welcome
    // screen comes back every time the last tab closes.
    QVERIFY(w.openFile(m_good));
    QVERIFY(message(w)->text().isEmpty());
    QVERIFY(!message(w)->isVisibleTo(welcome(w)));
}

#if defined(LOFTAIL_HAVE_SSH)
void TestWelcome::aRememberedRemoteLogIsARowAndASavedHostWithNoneIsAnother()
{
    const HostBookmarkStore store(HostBookmarkStore::defaultDir());
    HostBookmark withLog;
    withLog.label = QStringLiteral("prod");
    withLog.user = QStringLiteral("ops");
    withLog.host = QStringLiteral("prod-web");
    withLog.paths = {QStringLiteral("/var/log/app.log")};
    HostBookmark bare;
    bare.label = QStringLiteral("staging");
    bare.host = QStringLiteral("staging-box");
    QVERIFY(store.replaceAll({withLog, bare}));

    MainWindow w;
    QListWidget *list = remoteList(w);
    QVERIFY(list);
    QCOMPARE(list->count(), 2);

    // A remembered log: named host-and-path, and its tooltip is the address it opens.
    QCOMPARE(list->item(0)->text(), QStringLiteral("prod: /var/log/app.log"));
    QCOMPARE(list->item(0)->toolTip(), withLog.locationFor(withLog.paths.first()).toString());

    // A host with none: the trailing "..." is what says it opens the dialog instead, and
    // the empty path is what the window branches on.
    QCOMPARE(list->item(1)->text(), QStringLiteral("staging..."));
    QVERIFY(list->item(1)->data(Qt::UserRole + 2).toString().isEmpty());
}
#else
void TestWelcome::theRemotesSectionIsAbsentWithoutSsh()
{
    // Hidden entire rather than greyed, which is the opposite of what the File menu does
    // with the same two items — the menu is where the "built without SSH" sentence lives
    // and it keeps them for that, while an empty disabled column on a landing page
    // explains nothing the menu is not about to explain.
    MainWindow w;
    QVERIFY(remoteSection(w));
    QVERIFY(!remoteSection(w)->isVisibleTo(welcome(w)));
}
#endif

int main(int argc, char *argv[])
{
    // Isolate persistent state: the recent list, the saved hosts, the settings tree and
    // the session all live under the config directory, and a real one would make these
    // cases depend on what the developer last opened.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-welcome"));

    TestWelcome tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_welcome.moc"
