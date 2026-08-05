#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMimeData>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <QLabel>
#include <QTabWidget>

#include "FakeFetcher.h"
#include "LogView.h"
#include "MainWindow.h"
#include "RemoteLocation.h"
#include "SessionStore.h"

using namespace loftail;

// dropEvent() is protected, and a synthesized QDropEvent does not survive
// QApplication's drag machinery when no drag is actually in progress — so call the
// handler directly. What is under test is loftail's decision about which URLs to
// accept, not Qt's drag-and-drop plumbing.
class DroppableWindow : public loftail::MainWindow
{
public:
    using loftail::MainWindow::dropEvent;
};

// M11 — a remote log through the real MainWindow (SPEC.md §3). The promise is that
// an ssh:// URL works everywhere a path does, so the assertions here are about the
// APPLICATION treating it as an ordinary path: one tab per file however it is
// spelled, a sensible tab title, recent files, and a session that round-trips.
//
// The transport is a fake (tests/FakeFetcher.h), so this runs offscreen with no
// network — which is the whole reason SourceFetcher exists as a seam.
class TestRemoteOpen : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
    static constexpr auto kUrl = "ssh://deploy@web1/var/log/app.log";

    static QString url() { return QString::fromLatin1(kUrl); }

    static QByteArray sampleLog()
    {
        return "2026-07-21 00:00:01,000 [t0] INFO  logger.a - first\n"
               "2026-07-21 00:00:02,000 [t1] WARN  logger.b - second\n";
    }

    // The window exposes itself to tests through object names, as the other UI
    // tests do (tst_multidoc), rather than through accessors that exist only for them.
    static QTabWidget *tabs(const MainWindow &w)
    {
        return w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    }
    static int tabCount(const MainWindow &w)
    {
        QTabWidget *t = tabs(w);
        return t ? t->count() : -1;
    }
    static QString statusText(const MainWindow &w)
    {
        QLabel *label = w.findChild<QLabel *>(QStringLiteral("statusLabel"));
        return label ? label->text() : QString();
    }

    // Let the queued work of an open settle (the scan runs on a worker thread).
    static void settle(int ms = 600) { QTest::qWait(ms); }

private slots:
    void opensARemoteUrlAsATab();
    void equivalentSpellingsRaiseTheSameTab();
    void dropOfAnSshUrlOpens();
    void remoteOpenIsRemembered();
    void sessionRoundTripsARemoteDocument();
    void menuEntriesExist();
    void refusedRemoteReportsWithoutOpeningATab();
    void unreachableRemoteOpensAWaitingTab();
};

void TestRemoteOpen::opensARemoteUrlAsATab()
{
    FakeRemoteFarm farm;
    farm.at(url())->setInitialContent(sampleLog());

    MainWindow window;
    window.openFile(url(), QString::fromLatin1(kPattern));
    settle();

    QCOMPARE(tabCount(window), 1);
    // The host is what tells two same-named logs from different machines apart, in
    // the tab and in the title bar alike.
    QCOMPARE(tabs(window)->tabText(0), QStringLiteral("app.log (web1)"));
    QCOMPARE(window.windowTitle(), QStringLiteral("loftail — app.log (web1)"));
    QVERIFY(statusText(window).contains(QStringLiteral("app.log (web1)")));
    QVERIFY(statusText(window).contains(QStringLiteral("2 records")));
}

void TestRemoteOpen::equivalentSpellingsRaiseTheSameTab()
{
    FakeRemoteFarm farm;
    farm.at(url())->setInitialContent(sampleLog());

    MainWindow window;
    window.openFile(url(), QString::fromLatin1(kPattern));
    settle();
    QCOMPARE(tabCount(window), 1);

    // The same file written four ways. Without normalization each would open its own
    // tab on one file — and remember its format four times over.
    window.openFile(QStringLiteral("ssh://deploy@web1:22/var/log/app.log"),
                    QString::fromLatin1(kPattern));
    window.openFile(QStringLiteral("sftp://deploy@web1/var/log/app.log"),
                    QString::fromLatin1(kPattern));
    window.openFile(QStringLiteral("SSH://deploy@web1/var/log/app.log"),
                    QString::fromLatin1(kPattern));
    settle(300);

    QCOMPARE(tabCount(window), 1);
    // And one connection, not four.
    QCOMPARE(farm.at(url())->startCount(), 1);
}

void TestRemoteOpen::dropOfAnSshUrlOpens()
{
    FakeRemoteFarm farm;
    farm.at(url())->setInitialContent(sampleLog());

    DroppableWindow window;

    // Dragging a file out of a file manager's SSH mount produces an sftp:// URL,
    // which is not a local file and so used to be silently ignored.
    QMimeData mime;
    mime.setUrls({QUrl(QStringLiteral("sftp://deploy@web1/var/log/app.log"))});
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    window.dropEvent(&drop);
    settle();

    QCOMPARE(tabCount(window), 1);
    QCOMPARE(tabs(window)->tabText(0), QStringLiteral("app.log (web1)"));
}

void TestRemoteOpen::remoteOpenIsRemembered()
{
    FakeRemoteFarm farm;
    farm.at(url())->setInitialContent(sampleLog());

    QSettings().remove(QStringLiteral("recentFiles"));
    {
        MainWindow window;
        window.openFile(url(), QString::fromLatin1(kPattern));
        settle();
    }

    const QStringList recent = QSettings().value(QStringLiteral("recentFiles")).toStringList();
    QVERIFY(recent.contains(RemoteLocation::normalize(url())));
    // The recent-files list is written to a settings file and shown in a menu, so it
    // must carry no credential even when one was typed into the address.
    for (const QString &entry : recent)
        QVERIFY(!entry.contains(QStringLiteral("hunter2")));
}

void TestRemoteOpen::sessionRoundTripsARemoteDocument()
{
    FakeRemoteFarm farm;
    farm.at(url())->setInitialContent(sampleLog());

    {
        MainWindow window;
        window.openFile(url(), QString::fromLatin1(kPattern));
        settle();
        QCOMPARE(tabCount(window), 1);
        window.close(); // triggers saveSession(), as a real quit does
    }

    // The session stores the URL in the existing `path` string — no schema change was
    // needed for any of this, which is the point worth pinning.
    QSettings store;
    const Session session = SessionStore::load(store);
    QCOMPARE(session.documents.size(), 1);
    QCOMPARE(session.documents.at(0).path, RemoteLocation::normalize(url()));

    {
        MainWindow window; // the constructor restores the session
        settle();
        QCOMPARE(tabCount(window), 1);
        QCOMPARE(tabs(window)->tabText(0), QStringLiteral("app.log (web1)"));
        QVERIFY(statusText(window).contains(QStringLiteral("2 records")));
    }
}

void TestRemoteOpen::menuEntriesExist()
{
    MainWindow window;
    auto *openRemote = window.findChild<QAction *>(QStringLiteral("openRemoteAction"));
    QVERIFY(openRemote);
    auto *hosts = window.findChild<QMenu *>(QStringLiteral("remoteHostsMenu"));
    QVERIFY(hosts);

#if defined(LOFTAIL_HAVE_SSH)
    QVERIFY(openRemote->isEnabled());
    QVERIFY(hosts->isEnabled());
#else
    // Present but disabled, with a tooltip saying why — a missing menu item would
    // just look like the feature does not exist.
    QVERIFY(!openRemote->isEnabled());
    QVERIFY(openRemote->toolTip().contains(QStringLiteral("without SSH support")));
#endif
}

void TestRemoteOpen::refusedRemoteReportsWithoutOpeningATab()
{
    // A REFUSAL — a changed host key, a rejected password — which is what
    // setStartFailure() models. It gets the same answer however long loftail waits, so
    // it stays an open failure with no tab, exactly as a malformed path does.
    FakeRemoteFarm farm;
    farm.at(url())->setStartFailure(QStringLiteral("Authentication to deploy@web1 failed."));

    MainWindow window;
    window.openFile(url(), QString::fromLatin1(kPattern));
    settle(200);

    // A failed open leaves the window exactly as it was — the same contract a bad
    // local path has — and explains itself in the status bar rather than a dialog.
    QCOMPARE(tabCount(window), 0);
    QVERIFY(statusText(window).contains(QStringLiteral("Authentication")));
    QVERIFY(statusText(window).contains(QStringLiteral("app.log (web1)")));
}

void TestRemoteOpen::unreachableRemoteOpensAWaitingTab()
{
    // M13, and the contrast with the case above is the whole point: a host that is
    // DOWN is not a host that says no. The tab opens, says it is waiting, and picks the
    // log up when the host comes back — without the user reopening anything.
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(sampleLog());
    remote->setInitiallyUnavailable(QStringLiteral("Cannot reach web1:22 — Connection refused"));

    MainWindow window;
    window.openFile(url(), QString::fromLatin1(kPattern));
    settle(200);

    QCOMPARE(tabCount(window), 1);
    // Marked in the tab bar, so a log that is not there is tellable from an empty one.
    QCOMPARE(tabs(window)->tabText(0), QStringLiteral("◦ app.log (web1)"));
    QVERIFY(statusText(window).contains(QStringLiteral("Connection refused")));

    auto *view = window.findChild<LogView *>();
    QVERIFY(view);
    QCOMPARE(view->recordCount(), 0);
    QVERIFY(!view->placeholderText().isEmpty()); // and it says so in the view itself

    // The host comes back. The watch tick — a real one, on the real 750 ms timer —
    // brings the log in with no reopening and no dialog.
    remote->becomeAvailable();
    QTRY_VERIFY_WITH_TIMEOUT(view->recordCount() > 0, 5000);
    QCOMPARE(tabs(window)->tabText(0), QStringLiteral("app.log (web1)"));
    QVERIFY(statusText(window).contains(QStringLiteral("2 records")));
}

int main(int argc, char *argv[])
{
    // Isolate persistent state: recent files, the format cache and the session must
    // all start empty, and spool files must not land in the developer's real cache.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("XDG_CACHE_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-remoteopen"));

    TestRemoteOpen tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_remoteopen.moc"
