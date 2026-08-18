#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>

#include "LogSettingsStore.h"
#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// File ▸ Open Recent (SPEC.md §3). The list used to show each address verbatim, which
// made the menu as wide as the longest thing ever opened — an `ssh://` URL or a path
// continuing through an archive especially — and offered no way to forget any of it.
// An entry now wears the label the same log would wear on a tab, carries its full
// address as a tooltip, and the menu ends in an item that empties the list.
//
// Drives the real MainWindow: the menu is built in its constructor and rebuilt on every
// open, and what is under test is what that build produces. Entries and the clear item
// are found by object name and by the address stashed on each action, never by their
// visible text — the text is the assertion.
class TestRecentFiles : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_good; // parses with the built-in defaults, so an open never prompts

    static bool write(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(bytes);
        return true;
    }

    static void seed(const QStringList &paths)
    {
        QSettings settings;
        settings.setValue(QStringLiteral("recentFiles"), paths);
        settings.sync();
    }

    static QStringList stored()
    {
        return QSettings().value(QStringLiteral("recentFiles")).toStringList();
    }

    static QMenu *recentMenu(MainWindow &w)
    {
        return w.findChild<QMenu *>(QStringLiteral("recentMenu"));
    }

    // The entries, in menu order — everything carrying an address, which is what tells
    // an entry from the separator, the "(none)" placeholder and the clear item.
    static QList<QAction *> entries(QMenu *menu)
    {
        QList<QAction *> out;
        const QList<QAction *> all = menu->actions();
        for (QAction *a : all) {
            if (a->data().isValid())
                out.append(a);
        }
        return out;
    }

    static QAction *clearAction(MainWindow &w)
    {
        return w.findChild<QAction *>(QStringLiteral("clearRecentFilesAction"));
    }

private slots:
    void initTestCase();
    void init();
    void anEntryIsNamedLikeATabRatherThanShownWhole();
    void twoEntriesWithOneNameGrowTheDirectoryThatTellsThemApart();
    void everyEntryCarriesItsWholeAddressAsATooltip();
    void anAmpersandInANameIsNotReadAsAMnemonic();
    void aLogThatIsNotThereKeepsItsEntry();
    void clearingTheListEmptiesTheMenuAndOutlivesTheWindow();
    void openingALogAlreadyListedMovesItUpRatherThanTwice();
    void theListStopsAtTenEntries();
};

void TestRecentFiles::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_good = m_dir.filePath(QStringLiteral("good.log"));
    QVERIFY(write(m_good,
        "2026-07-21 10:00:00,000 [main] INFO  net.io - starting\n"
        "2026-07-21 10:00:01,000 [work] ERROR db.pool - boom\n"));
}

void TestRecentFiles::init()
{
    QSettings settings;
    settings.remove(QStringLiteral("session")); // a window here must open nothing of its own
    settings.remove(QStringLiteral("recentFiles"));
    settings.sync();
    QFile::remove(LogSettingsStore(LogSettingsStore::defaultDir()).filePath());
}

void TestRecentFiles::anEntryIsNamedLikeATabRatherThanShownWhole()
{
    // One of each address shape the application accepts. None of them is opened — the
    // menu is built from the strings alone — so this holds whether or not SSH and
    // archive support were compiled in.
    seed({QStringLiteral("/var/log/very/deep/tree/app.log"),
          QStringLiteral("ssh://ops@prod-web/var/log/service.log"),
          QStringLiteral("/srv/bundle.tar.gz/var/log/inner.log")});

    MainWindow w;
    QMenu *menu = recentMenu(w);
    QVERIFY(menu);
    const QList<QAction *> items = entries(menu);
    QCOMPARE(items.size(), 3);
    QCOMPARE(items.at(0)->text(), QStringLiteral("app.log"));
    QCOMPARE(items.at(1)->text(), QStringLiteral("service.log (prod-web)"));
    QCOMPARE(items.at(2)->text(), QStringLiteral("inner.log (bundle.tar.gz)"));
    w.close();
}

void TestRecentFiles::twoEntriesWithOneNameGrowTheDirectoryThatTellsThemApart()
{
    // Shortening must never take away what tells two entries apart, which is exactly
    // the rule the tab labels already follow.
    seed({QStringLiteral("/srv/alpha/app.log"), QStringLiteral("/srv/beta/app.log")});

    MainWindow w;
    const QList<QAction *> items = entries(recentMenu(w));
    QCOMPARE(items.size(), 2);
    QCOMPARE(items.at(0)->text(), QStringLiteral("alpha/app.log"));
    QCOMPARE(items.at(1)->text(), QStringLiteral("beta/app.log"));
    w.close();
}

void TestRecentFiles::everyEntryCarriesItsWholeAddressAsATooltip()
{
    const QString local = QStringLiteral("/var/log/very/deep/tree/app.log");
    const QString remote = QStringLiteral("ssh://ops@prod-web/var/log/service.log");
    seed({local, remote});

    MainWindow w;
    QMenu *menu = recentMenu(w);
    QVERIFY(menu);
    // A menu shows an action's tooltip only when told to, and the tooltip is what makes
    // the short label safe — without this the full address would be unreachable.
    QVERIFY(menu->toolTipsVisible());

    const QList<QAction *> items = entries(menu);
    QCOMPARE(items.size(), 2);
    QCOMPARE(items.at(0)->toolTip(), local);
    QCOMPARE(items.at(1)->toolTip(), remote);
    w.close();
}

void TestRecentFiles::anAmpersandInANameIsNotReadAsAMnemonic()
{
    seed({QStringLiteral("/var/log/a&b.log")});

    MainWindow w;
    const QList<QAction *> items = entries(recentMenu(w));
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.at(0)->text(), QStringLiteral("a&&b.log"));
    QCOMPARE(items.at(0)->data().toString(), QStringLiteral("/var/log/a&b.log"));
    w.close();
}

void TestRecentFiles::aLogThatIsNotThereKeepsItsEntry()
{
    // M13: a log that is not there is a valid thing to open — it opens a waiting tab —
    // so an entry is never dropped for being missing. Pruning it would take away the
    // one route back to a log that has rotated away and will come back.
    const QString absent = m_dir.filePath(QStringLiteral("gone.log"));
    QVERIFY(!QFile::exists(absent));
    seed({absent});

    MainWindow w;
    const QList<QAction *> items = entries(recentMenu(w));
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.at(0)->data().toString(), absent);
    QVERIFY(items.at(0)->isEnabled());
    w.close();
}

void TestRecentFiles::clearingTheListEmptiesTheMenuAndOutlivesTheWindow()
{
    seed({QStringLiteral("/var/log/one.log"), QStringLiteral("/var/log/two.log")});

    {
        MainWindow w;
        QAction *clear = clearAction(w);
        QVERIFY(clear);
        QVERIFY(clear->isEnabled());
        QCOMPARE(entries(recentMenu(w)).size(), 2);

        clear->trigger();

        QVERIFY(entries(recentMenu(w)).isEmpty());
        QVERIFY(stored().isEmpty());
        // Nothing left to forget, so the item stays where it is and greys out rather
        // than coming and going as the list fills.
        QVERIFY(clearAction(w));
        QVERIFY(!clearAction(w)->isEnabled());
        w.close();
    }

    // Persisted, not merely blanked on screen.
    MainWindow fresh;
    QVERIFY(entries(recentMenu(fresh)).isEmpty());
    fresh.close();
}

void TestRecentFiles::openingALogAlreadyListedMovesItUpRatherThanTwice()
{
    seed({QStringLiteral("/var/log/other.log"), m_good});

    MainWindow w;
    QVERIFY(w.openFile(m_good));

    QCOMPARE(stored(),
             QStringList({m_good, QStringLiteral("/var/log/other.log")}));
    QCOMPARE(entries(recentMenu(w)).size(), 2);
    w.close();
}

void TestRecentFiles::theListStopsAtTenEntries()
{
    QStringList ten;
    for (int i = 0; i < 10; ++i)
        ten.append(QStringLiteral("/var/log/old%1.log").arg(i));
    seed(ten);

    MainWindow w;
    QVERIFY(w.openFile(m_good));

    const QStringList after = stored();
    QCOMPARE(after.size(), 10);
    QCOMPARE(after.first(), m_good);
    QCOMPARE(after.last(), QStringLiteral("/var/log/old8.log")); // the eldest fell off
    QCOMPARE(entries(recentMenu(w)).size(), 10);
    w.close();
}

int main(int argc, char *argv[])
{
    // Isolate persistent state: the recent list, the settings tree and the session all
    // live under the config directory, and a real one would make these cases depend on
    // what the developer last opened.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-recentfiles"));

    TestRecentFiles tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_recentfiles.moc"
