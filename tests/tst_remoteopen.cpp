#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMimeData>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <QAbstractButton>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

#include "FakeFetcher.h"
#include "HostBookmarkStore.h"
#include "PreferencesDialog.h"
#include "LogView.h"
#include "MainWindow.h"
#include "OpenRemoteDialog.h"
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
    // Distinct addresses for the format-dialog cases. The format cache is persistent and
    // shared across every case in this binary, and a cached format suppresses the prompt.
    static QString formatUrl()
    {
        return QStringLiteral("ssh://deploy@fmt1/var/log/app.log");
    }
    static QString bgUrl() { return QStringLiteral("ssh://deploy@bg1/var/log/app.log"); }

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

    // Click `button` on the next modal QMessageBox to appear. Armed BEFORE the call
    // that raises it: QMessageBox::question() runs its own event loop and does not
    // return until something answers, so there is no later moment to do this from.
    // Polls rather than firing once, because the box is not up yet when this is armed.
    static void answerNextMessageBox(QMessageBox::StandardButton button)
    {
        auto *timer = new QTimer(qApp);
        timer->setInterval(10);
        QObject::connect(timer, &QTimer::timeout, timer, [timer, button] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            timer->stop();
            timer->deleteLater();
            if (QAbstractButton *b = box->button(button))
                b->click();
            else
                box->reject();
        });
        timer->start();
    }

private slots:
    void opensARemoteUrlAsATab();
    void equivalentSpellingsRaiseTheSameTab();
    void dropOfAnSshUrlOpens();
    void remoteOpenIsRemembered();
    void sessionRoundTripsARemoteDocument();
    void menuEntriesExist();
    void savedHostsAreOneFlatLevel();
    void refusedRemoteReportsWithoutOpeningATab();
    void aTransportRefusalKeepsTheTabAndSaysWhy();
    void unreachableRemoteOpensAWaitingTab();
    void savingOverwritesTheBookmarkOfTheSameName();
    void anInteractiveRemoteOpenStillGetsTheFormatDialog();
    void aBackgroundResumeRaisesNoFormatDialog();
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

void TestRemoteOpen::savedHostsAreOneFlatLevel()
{
    // Every remembered log is one entry in Remote Hosts — "host: /path" — rather than
    // a per-host submenu holding its paths. A host with nothing remembered is still
    // listed on its own, because it reopens the form pre-filled.
    HostBookmarkStore store(HostBookmarkStore::defaultDir());
    HostBookmark web1;
    web1.user = QStringLiteral("deploy");
    web1.host = QStringLiteral("web1");
    web1.paths = {QStringLiteral("/var/log/app.log"), QStringLiteral("/var/log/other.log")};
    HostBookmark db1;
    db1.host = QStringLiteral("db1");
    db1.label = QStringLiteral("Database");
    QVERIFY(store.replaceAll({web1, db1}));

    MainWindow window;
    auto *hosts = window.findChild<QMenu *>(QStringLiteral("remoteHostsMenu"));
    QVERIFY(hosts);

    QStringList texts;
    for (QAction *action : hosts->actions()) {
        QVERIFY(!action->menu()); // nothing here opens a second level
        texts << action->text();
    }
    QCOMPARE(texts,
             QStringList({QStringLiteral("web1: /var/log/app.log"),
                          QStringLiteral("web1: /var/log/other.log"),
                          QStringLiteral("Database...")}));

    QVERIFY(store.replaceAll({})); // leave the store as the other cases expect it
}

void TestRemoteOpen::refusedRemoteReportsWithoutOpeningATab()
{
    // A refusal decided with NO I/O — no transport could be built for this address at
    // all. Those are still an open failure with no tab, exactly as a malformed path is,
    // because there is nothing to put in a tab and nothing that could change.
    // setStartFailure() is what models that since M17; a refusal by the far END is
    // aTransportRefusalKeepsTheTabAndSaysWhy() below.
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

    auto *view = window.findChild<LogView *>(QStringLiteral("logView"));
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

void TestRemoteOpen::savingOverwritesTheBookmarkOfTheSameName()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    HostBookmarkStore store(dir.path());
    OpenRemoteDialog dialog(&store);

    auto *list = dialog.findChild<QListWidget *>(QStringLiteral("remoteBookmarkList"));
    auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("remoteNameField"));
    auto *host = dialog.findChild<QLineEdit *>(QStringLiteral("remoteHostField"));
    auto *path = dialog.findChild<QLineEdit *>(QStringLiteral("remotePathField"));
    QVERIFY(list && name && host && path);

    // By object name, not by label: the button's text is now part of what it tells the
    // user — it reads "Update" whenever pressing it would replace an existing row —
    // and it carries an accelerator, so its text() is never the bare word either.
    auto *save = dialog.findChild<QPushButton *>(QStringLiteral("remoteSaveButton"));
    auto *remove = dialog.findChild<QPushButton *>(QStringLiteral("remoteRemoveButton"));
    QVERIFY(save && remove);

    const auto fill = [&](const QString &n, const QString &h, const QString &p) {
        name->setText(n);
        host->setText(h);
        path->setText(p);
    };

    fill(QStringLiteral("prod"), QStringLiteral("web1"), QStringLiteral("/var/log/a.log"));
    save->click();
    QCOMPARE(list->count(), 1);
    QCOMPARE(list->item(0)->text(), QStringLiteral("prod"));

    // Same name, same machine, another log: one entry still, now remembering both —
    // the Remote Hosts submenu lists a host's logs under it.
    fill(QStringLiteral("prod"), QStringLiteral("web1"), QStringLiteral("/var/log/b.log"));
    save->click();
    QCOMPARE(list->count(), 1);
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().at(0).paths.size(), 2);

    // Same name, different machine: the name has been reused for something else, so it
    // is overwritten outright and the old machine's paths do not follow it over.
    fill(QStringLiteral("prod"), QStringLiteral("web2"), QStringLiteral("/var/log/c.log"));
    save->click();
    QCOMPARE(list->count(), 1);
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().at(0).host, QStringLiteral("web2"));
    QCOMPARE(store.all().at(0).paths, QStringList{QStringLiteral("/var/log/c.log")});

    // A new name is a new entry, on the same machine or not.
    fill(QStringLiteral("staging"), QStringLiteral("web2"), QStringLiteral("/var/log/c.log"));
    save->click();
    QCOMPARE(list->count(), 2);

    // Remove goes by the same identity, so it takes the row that was clicked. It asks
    // first — a saved host may be carrying a remembered password and there is no undo.
    list->setCurrentRow(0);
    answerNextMessageBox(QMessageBox::No);
    remove->click();
    QCOMPARE(list->count(), 2); // declined: nothing removed

    answerNextMessageBox(QMessageBox::Yes);
    remove->click();
    QCOMPARE(list->count(), 1);
    QCOMPARE(list->item(0)->text(), QStringLiteral("staging"));
}

void TestRemoteOpen::aTransportRefusalKeepsTheTabAndSaysWhy()
{
    // A refusal by the far end — a rejected password, a changed host key. Since M17 the
    // tab is already on screen by the time the answer comes back, so tearing it down
    // would mean a tab appearing and vanishing on its own. It stays and reports the
    // transport's own words instead, and File ▸ Reconnect is how the user tries again
    // (SPEC.md §3).
    FakeRemoteFarm farm;
    auto remote = farm.at(url());
    remote->setInitialContent(sampleLog());
    remote->setConnectRefusal(QStringLiteral("Authentication to deploy@web1 failed."));

    MainWindow window;
    window.openFile(url(), QString::fromLatin1(kPattern));
    settle(200);

    QCOMPARE(tabCount(window), 1);
    QCOMPARE(tabs(window)->tabText(0), QStringLiteral("◦ app.log (web1)"));
    QVERIFY(statusText(window).contains(QStringLiteral("Authentication")));

    auto *view = window.findChild<LogView *>(QStringLiteral("logView"));
    QVERIFY(view);
    QCOMPARE(view->recordCount(), 0);
    QVERIFY(view->placeholderText().contains(QStringLiteral("Authentication")));

    // Reconnect is offered, because a refused spooled log still has its spool.
    auto *reconnect = window.findChild<QAction *>(QStringLiteral("reconnectAction"));
    QVERIFY(reconnect);
    QVERIFY(reconnect->isEnabled());
}

void TestRemoteOpen::anInteractiveRemoteOpenStillGetsTheFormatDialog()
{
    // THE SILENT REGRESSION THIS PREVENTS. openWithSettings() suppresses the format
    // prompt for a waiting document, and since M17 EVERY remote log opens waiting — so
    // without the deferral, no remote log would ever see Preferences again,
    // M8's autodetection would be unreachable for them, and nothing would be persisted,
    // so every reopen would repeat the non-offer. All without an error anywhere.
    FakeRemoteFarm farm;
    // A URL of its own: the format cache is shared across the cases in this binary,
    // and a cached format would suppress the very prompt being tested.
    auto remote = farm.at(formatUrl());
    // Content the default pattern cannot parse, so the dialog is genuinely warranted.
    remote->setInitialContent(QByteArrayLiteral("<12>Jul 21 00:00:01 host app: first\n"
                                                "<12>Jul 21 00:00:02 host app: second\n"));
    remote->setConnectDelayed();

    // A STACK timer, deliberately, and not one parented to qApp. It captures `shown` by
    // reference, and a QVERIFY that fails returns from this function immediately — so a
    // heap timer would outlive the frame it points into and fire again during a later
    // test, reading freed stack. That is a segfault whose cause is nowhere near where it
    // lands. A stack timer dies with the frame on every path, including that one.
    bool shown = false;
    QTimer watcher;
    watcher.setInterval(10);
    QObject::connect(&watcher, &QTimer::timeout, &watcher, [&watcher, &shown] {
        auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!qobject_cast<loftail::PreferencesDialog *>(dlg))
            return;
        shown = true;
        watcher.stop();
        dlg->reject(); // declining is enough; the point is that it was offered
    });
    watcher.start();

    MainWindow window;
    window.show();
    // No pattern passed, so this is the "prompt if the default does not match" path an
    // ordinary File ▸ Open takes.
    window.openFile(formatUrl());
    QCOMPARE(tabCount(window), 1); // the tab is there before anything has connected

    remote->becomeAvailable();
    QTRY_VERIFY_WITH_TIMEOUT(shown, 5000);
}

void TestRemoteOpen::aBackgroundResumeRaisesNoFormatDialog()
{
    // The other side of the same rule, and the one §6.5 has always insisted on: a log
    // arriving on a watch tick raises nothing, because the tab may not even be on screen
    // and the user is doing something else. Here the open passed a pattern, so no prompt
    // was ever owed — which is exactly the session-restore case.
    FakeRemoteFarm farm;
    auto remote = farm.at(bgUrl());
    remote->setInitialContent(QByteArrayLiteral("<12>Jul 21 00:00:01 host app: first\n"));
    remote->setConnectDelayed();

    // Stack-scoped for the reason given in the case above: it captures `shown`, and an
    // assertion that fails returns from here without running any teardown.
    bool shown = false;
    QTimer watcher;
    watcher.setInterval(10);
    QObject::connect(&watcher, &QTimer::timeout, &watcher, [&shown] {
        auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (qobject_cast<loftail::PreferencesDialog *>(dlg)) {
            shown = true;
            dlg->reject();
        }
    });
    watcher.start();

    MainWindow window;
    window.show();
    window.openFile(bgUrl(), QString::fromLatin1(kPattern));
    remote->becomeAvailable();

    // Wait for the resume to have happened, then check nothing was raised on the way.
    auto *view = window.findChild<LogView *>(QStringLiteral("logView"));
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(view->recordCount() > 0, 5000);
    settle(300);
    watcher.stop();

    QVERIFY(!shown);
    // It says where to fix it instead, which is the whole of §6.5's alternative.
    QVERIFY(statusText(window).contains(QStringLiteral("format not recognised")));
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
