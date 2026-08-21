#include <QtTest>

#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>

#include "ArchiveFixtures.h"
#include "MainWindow.h"
#include "OpenArchiveDialog.h"
#include "LogFileStore.h"
#include "LogSettingsStore.h"
#include "SessionStore.h"

using namespace loftail;
using namespace loftail::fixtures;

// M12 — an archived log through the real MainWindow (SPEC.md §3). The promise is that
// an archived path works everywhere a path does, so the assertions are about the
// APPLICATION treating it as an ordinary log: one tab per member however it is spelled,
// a tab title naming the log rather than the container, a remembered format, and a
// session that round-trips.
//
// The picker is asserted from both sides: shown when there is a genuine choice, and
// NOT shown when there is not — a .gz has one member by construction, and asking would
// be asking nothing.
class TestArchiveOpen : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";

    static QByteArray sampleLog()
    {
        return "2026-08-05 00:00:01,000 [t0] INFO  logger.a - first\n"
               "2026-08-05 00:00:02,000 [t1] WARN  logger.b - second\n";
    }

    QString path(const QString &name) const { return m_dir.path() + u'/' + name; }

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
        auto *label = w.findChild<QLabel *>(QStringLiteral("statusLabel"));
        return label ? label->text() : QString();
    }

    // Put a container at `p` in ONE step. Written in place it would be opened
    // half-written by the fetcher's own 100 ms retry, which is a race and not the
    // subject of anything here.
    bool placeTarGz(const QString &p, const QVector<Member> &members)
    {
        const QString staging = m_dir.path() + QStringLiteral("/staging.bin");
        QFile::remove(staging);
        if (!writeTarGz(staging, members))
            return false;
        return QFile::rename(staging, p);
    }

    // The picker is modal, so drive it from a timer once it is up.
    static void whenDialogShown(std::function<void(OpenArchiveDialog *)> act)
    {
        QTimer::singleShot(0, [act = std::move(act)]() {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (auto *dialog = qobject_cast<OpenArchiveDialog *>(w)) {
                    act(dialog);
                    return;
                }
            }
        });
    }

private slots:
    void aCompressedLogOpensWithoutAskingAnything();
    void aMultiMemberArchiveAsksWhichLog();
    void severalPickedMembersOpenAsSeveralTabs();
    void oneMemberIsOneTabHoweverItIsSpelled();
    void theFormatAndTheSessionRememberAnArchivedPath();
    void aTabOpenedBeforeItsContainerExistsFillsInWhenItAppears();
    void aRestoredArchivedLogWaitsForItsContainerAndPicksItUp();

private:
    QTemporaryDir m_dir;
};

void TestArchiveOpen::aCompressedLogOpensWithoutAskingAnything()
{
    const QString gz = path(QStringLiteral("app.log.gz"));
    QVERIFY(writeGzip(gz, sampleLog()));

    MainWindow window;
    window.show();
    // No dialog is driven here on purpose: a bare compressed stream holds exactly one
    // member, so asking would be asking nothing. If a picker did appear this would
    // hang, which is the assertion.
    window.openFile(gz, QString::fromLatin1(kPattern));
    QTRY_COMPARE(tabCount(window), 1);

    // The tab names the log the writer meant, not the container it arrived in — and
    // names it ONCE: "app.log (app.log.gz)" would say the same thing twice.
    // QTRY, because an archived log now opens WAITING and settles a moment later: the
    // member is expanded on a worker, so the tab exists (marked "◦") before it has a
    // record in it. Which is the point — it appears at once instead of after the scan.
    QTRY_COMPARE(tabs(window)->tabText(0), QStringLiteral("app.log"));
    QTRY_VERIFY(statusText(window).contains(QStringLiteral("2 records")));
}

void TestArchiveOpen::aMultiMemberArchiveAsksWhichLog()
{
    const QString zip = path(QStringLiteral("bundle.zip"));
    QVERIFY(writeZip(zip, {{QStringLiteral("app.log"), sampleLog()},
                           {QStringLiteral("db.log"), sampleLog()},
                           {QStringLiteral("web.log"), sampleLog()}}));

    MainWindow window;
    window.show();

    bool shown = false;
    whenDialogShown([&shown](OpenArchiveDialog *dialog) {
        shown = true;
        auto *list = dialog->findChild<QTreeWidget *>(QStringLiteral("archiveMembers"));
        QVERIFY(list);
        QCOMPARE(list->topLevelItemCount(), 3);
        // One is pre-selected, so Open is meaningful the moment the dialog appears.
        QVERIFY(!list->selectedItems().isEmpty());
        list->clearSelection();
        list->topLevelItem(1)->setSelected(true); // db.log
        dialog->accept();
    });

    window.openFile(zip, QString::fromLatin1(kPattern));
    QTRY_VERIFY(shown);
    QTRY_COMPARE(tabCount(window), 1);
    // The container is worth naming where two tabs would otherwise both read "db.log"
    // — and only there (TabLabels.h). One log is open, so the tab is the log's own name
    // and the container lives on the tooltip with the rest of the address.
    QTRY_COMPARE(tabs(window)->tabText(0), QStringLiteral("db.log"));
    QVERIFY(tabs(window)->tabToolTip(0).contains(QStringLiteral("bundle.zip")));
}

void TestArchiveOpen::severalPickedMembersOpenAsSeveralTabs()
{
    const QString zip = path(QStringLiteral("several.zip"));
    QVERIFY(writeZip(zip, {{QStringLiteral("app.log"), sampleLog()},
                           {QStringLiteral("app.log.1"), sampleLog()},
                           {QStringLiteral("app.log.2"), sampleLog()}}));

    MainWindow window;
    window.show();

    whenDialogShown([](OpenArchiveDialog *dialog) {
        auto *list = dialog->findChild<QTreeWidget *>(QStringLiteral("archiveMembers"));
        list->selectAll();
        dialog->accept();
    });

    // SPEC.md §3 already says dropping several files opens all of them; several logs
    // picked out of one archive is the same case.
    window.openFile(zip, QString::fromLatin1(kPattern));
    QTRY_COMPARE(tabCount(window), 3);
}

void TestArchiveOpen::oneMemberIsOneTabHoweverItIsSpelled()
{
    const QString tgz = path(QStringLiteral("logs.tar.gz"));
    QVERIFY(writeTarGz(tgz, {{QStringLiteral("var/log/app.log"), sampleLog()}}));

    MainWindow window;
    window.show();

    // A single-member container is not asked about either — there is still nothing to
    // choose, even though the container could have held several.
    window.openFile(tgz, QString::fromLatin1(kPattern));
    QTRY_COMPARE(tabCount(window), 1);

    // Reopening by the fully spelled address raises the same tab rather than opening a
    // second one: both spellings normalize to one Document path.
    window.openFile(tgz + QStringLiteral("/var/log/app.log"), QString::fromLatin1(kPattern));
    QCOMPARE(tabCount(window), 1);

    // And so does a relative spelling of the same file.
    const QString previous = QDir::currentPath();
    QVERIFY(QDir::setCurrent(m_dir.path()));
    window.openFile(QStringLiteral("logs.tar.gz/var/log/app.log"), QString::fromLatin1(kPattern));
    QVERIFY(QDir::setCurrent(previous));
    QCOMPARE(tabCount(window), 1);
}

void TestArchiveOpen::theFormatAndTheSessionRememberAnArchivedPath()
{
    const QString tgz = path(QStringLiteral("remembered.tar.gz"));
    QVERIFY(writeTarGz(tgz, {{QStringLiteral("app.log"), sampleLog()}}));
    const QString address = tgz + QStringLiteral("/app.log");

    {
        // Defaults that cannot parse this log, so the pattern passed below is the only
        // thing that can — which is what makes the reopen at the bottom prove that the
        // settings were found under the ARCHIVED key rather than inherited.
        LogSettingsStore store(LogSettingsStore::defaultDir());
        LogSettingsTree tree;
        LogProfile root;
        root.format.pattern = QStringLiteral("%p|%c|%m%n");
        tree.setDefaults(root);
        QVERIFY(store.save(tree));
    }

    {
        MainWindow window;
        window.show();
        window.openFile(address, QString::fromLatin1(kPattern));
        QTRY_COMPARE(tabCount(window), 1);
        QTRY_VERIFY(statusText(window).contains(QStringLiteral("2 records")));
        window.close();
    }

    // The per-log record keyed on the archived address, not on a mangled one — the bug
    // M11 fixed for URLs, in its archived form.
    QSettings settings;
    const Session session = SessionStore::load(settings);
    QCOMPARE(session.documents.size(), 1);
    QCOMPARE(session.documents.at(0).path, address);

    LogFileStore store(LogFileStore::defaultDir());
    store.load();
    const LogFileSettings hit = store.read(address);
    QVERIFY2(hit.profile.has_value(), "no per-log record was written for the archived address");
    QCOMPARE(hit.profile->format.pattern, QString::fromLatin1(kPattern));

    {
        // Restored WITHOUT a pattern argument: it opens correctly only because the
        // remembered format was found under the archived key.
        MainWindow window;
        window.show();
        window.openFile(address);
        QTRY_COMPARE(tabCount(window), 1);
        QTRY_VERIFY(statusText(window).contains(QStringLiteral("2 records")));
    }
}

void TestArchiveOpen::aTabOpenedBeforeItsContainerExistsFillsInWhenItAppears()
{
    // An archived log is a log, and SPEC.md §3 promises the same of every log: a path
    // that is not there yet opens a tab that waits and fills in the moment it appears.
    // It did not hold here for four milestones — the tab appeared and then waited for
    // ever, because the fetcher that would have looked for the container was never
    // built (ARCHITECTURE.md §6.4).
    {
        QSettings settings;
        settings.clear(); // no tab but this one: an earlier case left a session behind
    }

    const QString tgz = path(QStringLiteral("late.tar.gz"));
    const QString address = tgz + QStringLiteral("/var/log/app.log");
    QVERIFY(!QFileInfo::exists(tgz));

    MainWindow window;
    window.show();
    window.openFile(address, QString::fromLatin1(kPattern));

    QTRY_COMPARE(tabCount(window), 1);
    QTabWidget *bar = tabs(window);
    QVERIFY(bar);
    // The hollow mark: a log that is not there, as against one that is merely empty.
    QTRY_VERIFY(bar->tabText(0).startsWith(QStringLiteral("◦")));
    // And the tooltip says WHAT it is waiting for. The member is not what is missing
    // and cannot be looked for; the container is.
    QTRY_VERIFY2(bar->tabToolTip(0).contains(QStringLiteral("late.tar.gz")),
                 qPrintable(bar->tabToolTip(0)));

    QVERIFY(placeTarGz(tgz, {{QStringLiteral("var/log/app.log"), sampleLog()}}));

    QTRY_VERIFY_WITH_TIMEOUT(statusText(window).contains(QStringLiteral("2 records")), 30000);
    QVERIFY(!bar->tabText(0).startsWith(QStringLiteral("◦")));
}

void TestArchiveOpen::aRestoredArchivedLogWaitsForItsContainerAndPicksItUp()
{
    // SPEC.md §98's own example, in its archived form: a session restored with a log on
    // a share that is not mounted yet. Reachable by construction, since restore goes
    // through the same Document::prepare() an open does.
    {
        QSettings settings;
        settings.clear(); // this test owns the session; earlier cases left their own
    }

    const QString tgz = path(QStringLiteral("session.tar.gz"));
    const QString address = tgz + QStringLiteral("/app.log");
    QVERIFY(placeTarGz(tgz, {{QStringLiteral("app.log"), sampleLog()}}));

    {
        MainWindow window;
        window.show();
        window.openFile(address, QString::fromLatin1(kPattern));
        QTRY_COMPARE(tabCount(window), 1);
        QTRY_VERIFY_WITH_TIMEOUT(statusText(window).contains(QStringLiteral("2 records")),
                                 30000);
        window.close(); // saves the session
    }

    QVERIFY(QFile::remove(tgz)); // the share goes away between sessions

    MainWindow window; // the constructor restores, before show()
    window.show();
    QTRY_COMPARE(tabCount(window), 1);
    QTabWidget *bar = tabs(window);
    QVERIFY(bar);
    QTRY_VERIFY(bar->tabText(0).startsWith(QStringLiteral("◦")));

    QVERIFY(placeTarGz(tgz, {{QStringLiteral("app.log"), sampleLog()}}));

    QTRY_VERIFY_WITH_TIMEOUT(statusText(window).contains(QStringLiteral("2 records")), 30000);
    QVERIFY(!bar->tabText(0).startsWith(QStringLiteral("◦")));
}

int main(int argc, char *argv[])
{
    // Isolate persistent state: recent files, the format cache and the session must all
    // start empty, and spool files must not land in the developer's real cache.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("XDG_CACHE_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-archiveopen"));

    TestArchiveOpen test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_archiveopen.moc"
