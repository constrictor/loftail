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
#include <QDialog>
#include <QHeaderView>
#include <QLineEdit>
#include <QToolButton>
#include <QScopedPointer>

#include "ArchiveFixtures.h"
#include "ConfigReset.h"
#include "LogModel.h"
#include "LogView.h"
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
    // The record count of ONE tab, background or not. statusText() cannot answer this:
    // updateStatus() writes m_statusLabel from the ACTIVE context only, so a background
    // tab's records are invisible to every existing assertion in this file — which is
    // the blind spot severalPickedMembersOpenAsSeveralTabs shipped in.
    static int recordsInTab(const MainWindow &w, int index)
    {
        QTabWidget *t = tabs(w);
        if (!t || index < 0 || index >= t->count())
            return -1;
        LogView *log = t->widget(index)->findChild<LogView *>(QStringLiteral("logView"));
        auto *model = log ? qobject_cast<LogModel *>(log->header()->model()) : nullptr;
        return model ? model->rowCount() : -1;
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

    // Anything modal that is NOT the picker is a failure, not something to wait out.
    // Without it a regression here does not fail the suite, it HANGS it: a member that
    // arrives as raw gzip matches no pattern, so the tab that is owed a format prompt
    // raises Preferences and exec()s forever with nobody to answer. Rejecting it turns
    // that back into an assertion someone can read.
    static QTimer *rejectUnexpectedDialogs(bool *seen)
    {
        auto *timer = new QTimer;
        timer->setInterval(200);
        QObject::connect(timer, &QTimer::timeout, [seen]() {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(w);
                if (!dialog || !dialog->isVisible())
                    continue;
                if (qobject_cast<OpenArchiveDialog *>(dialog))
                    continue;
                *seen = true;
                dialog->reject();
            }
        });
        timer->start();
        return timer;
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
    // A log's settings outlive its tab since M21 and the pool is a DIRECTORY, so
    // without this a case inherits whatever an earlier one left under the same path
    // and passes or fails on the order QtTest happened to run them in. Every other GUI
    // suite that opens logs does this; this one never had it.
    void init() { clearLogSettings(); }

    void aCompressedLogOpensWithoutAskingAnything();
    void aMultiMemberArchiveAsksWhichLog();
    void severalPickedMembersOpenAsSeveralTabs();
    void aCompressedMemberInsideAnArchiveIsDecompressedToo();
    void aTypedFilterNarrowsTheMemberList();
    void aNarrowedAwayRowIsNotOpened();
    void theFilterBoxIsNotARegularExpression();
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

    // AND EVERY ONE OF THEM HOLDS ITS OWN RECORDS. Three tabs was the whole of this
    // case for two milestones, which is exactly the blind spot the defect lived in:
    // each member expands on its own worker, so the tabs appear at once and fill in
    // afterwards, and nothing here — nor anywhere else in the suite — had ever looked
    // at a BACKGROUND archived tab's record count. statusText() cannot see one either
    // (recordsInTab). Asserted without switching tabs first, because a switch is a
    // gesture the bug's reporter did not have to make.
    for (int i = 0; i < 3; ++i)
        QTRY_COMPARE_WITH_TIMEOUT(recordsInTab(window, i), 2, 10000);
}

void TestArchiveOpen::aCompressedMemberInsideAnArchiveIsDecompressedToo()
{
    // A ROTATION BUNDLE — a directory of rolled logs archived whole — which is the
    // ordinary shape of a support bundle and the shape this failed on: libarchive's
    // filters apply to the container's own bytes and never to what comes out of a
    // member, so `app.log.1.gz` inside the zip was spooled as raw gzip. Nothing matched
    // it, the tab held no records, and the only thing anywhere that said so was a
    // format preview full of mojibake. `app.log` beside it worked perfectly, which is
    // precisely "only one of them contains data".
    const QByteArray plain = sampleLog();
    const QByteArray rolled = gzipBytes(sampleLog());
    QVERIFY(!rolled.isEmpty());

    const QString zip = path(QStringLiteral("rotation.zip"));
    QVERIFY(writeZip(zip, {{QStringLiteral("app.log"), plain},
                           {QStringLiteral("app.log.1.gz"), rolled}}));

    MainWindow window;
    window.show();

    bool unexpectedDialog = false;
    QScopedPointer<QTimer> guard(rejectUnexpectedDialogs(&unexpectedDialog));

    whenDialogShown([](OpenArchiveDialog *dialog) {
        auto *list = dialog->findChild<QTreeWidget *>(QStringLiteral("archiveMembers"));
        list->selectAll();
        dialog->accept();
    });

    window.openFile(zip, QString::fromLatin1(kPattern));
    QTRY_COMPARE(tabCount(window), 2);
    // Both, and the compressed one is the case: two records each, decompressed twice
    // over for the second — out of the zip, then out of the gzip.
    QTRY_COMPARE_WITH_TIMEOUT(recordsInTab(window, 0), 2, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(recordsInTab(window, 1), 2, 10000);
    QVERIFY2(!unexpectedDialog, "the format dialog was raised: a member arrived unparsed");
}

namespace {

// The zip every filter case narrows: two extensions, a rolled log and a non-log, so
// each of the three rules below has something it must NOT match as well as something
// it must.
QVector<Member> filterFixture()
{
    // Real records, not filler: a member whose lines do not match the pattern raises
    // the format dialog on whichever tab ends up in front, and a modal dialog with
    // nobody to answer it hangs the suite rather than failing it.
    const QByteArray body =
        "2026-08-05 00:00:01,000 [t0] INFO  logger.a - first\n"
        "2026-08-05 00:00:02,000 [t1] WARN  logger.b - second\n";
    return {{QStringLiteral("var/log/app.log"), body},
            {QStringLiteral("var/log/app.log.1"), body},
            {QStringLiteral("var/log/db.audit.log"), body},
            {QStringLiteral("notes.txt"), body}};
}

QStringList shownMembers(QTreeWidget *list)
{
    QStringList out;
    for (int i = 0; i < list->topLevelItemCount(); ++i) {
        if (!list->topLevelItem(i)->isHidden())
            out << list->topLevelItem(i)->text(0);
    }
    return out;
}

} // namespace

void TestArchiveOpen::aTypedFilterNarrowsTheMemberList()
{
    const QString zip = path(QStringLiteral("filtered.zip"));
    QVERIFY(writeZip(zip, filterFixture()));

    MainWindow window;
    window.show();

    QStringList substring, glob, cleared;
    whenDialogShown([&](OpenArchiveDialog *dialog) {
        auto *list = dialog->findChild<QTreeWidget *>(QStringLiteral("archiveMembers"));
        auto *filter = dialog->findChild<QLineEdit *>(QStringLiteral("archiveFilter"));
        QVERIFY(list && filter);

        // Plain text is a SUBSTRING, anywhere in the member path.
        filter->setText(QStringLiteral("audit"));
        substring = shownMembers(list);

        // A star makes it a wildcard over the WHOLE path — which is the difference
        // that matters: `*.log` must not take `app.log.1` with it, or filtering by
        // extension does not filter by extension.
        filter->setText(QStringLiteral("*.log"));
        glob = shownMembers(list);

        filter->clear();
        cleared = shownMembers(list);
        dialog->reject();
    });

    QString error;
    OpenArchiveDialog::chooseMembers(zip, &window, &error);

    QCOMPARE(substring, QStringList{QStringLiteral("var/log/db.audit.log")});
    QCOMPARE(glob, (QStringList{QStringLiteral("var/log/app.log"),
                                QStringLiteral("var/log/db.audit.log")}));
    QCOMPARE(cleared.size(), 4);
}

void TestArchiveOpen::aNarrowedAwayRowIsNotOpened()
{
    const QString zip = path(QStringLiteral("hidden.zip"));
    QVERIFY(writeZip(zip, filterFixture()));

    MainWindow window;
    window.show();

    bool unexpectedDialog = false;
    QScopedPointer<QTimer> guard(rejectUnexpectedDialogs(&unexpectedDialog));

    whenDialogShown([](OpenArchiveDialog *dialog) {
        auto *list = dialog->findChild<QTreeWidget *>(QStringLiteral("archiveMembers"));
        auto *filter = dialog->findChild<QLineEdit *>(QStringLiteral("archiveFilter"));
        auto *selectAll = dialog->findChild<QToolButton *>(QStringLiteral("archiveSelectAll"));
        QVERIFY(list && filter && selectAll);

        // Pick something, THEN narrow it away. Its selection survives in Qt's model,
        // and opening a tab for a row that is no longer on screen is the whole thing
        // this case exists to forbid.
        list->clearSelection();
        list->topLevelItem(0)->setSelected(true); // var/log/app.log
        filter->setText(QStringLiteral("audit"));
        selectAll->click();
        dialog->accept();
    });

    window.openFile(zip, QString::fromLatin1(kPattern));
    QTRY_COMPARE(tabCount(window), 1);
    QTRY_COMPARE(tabs(window)->tabText(0), QStringLiteral("db.audit.log"));
    QVERIFY(!unexpectedDialog);
}

void TestArchiveOpen::theFilterBoxIsNotARegularExpression()
{
    // Typed verbatim, a name full of regex punctuation finds itself and nothing else.
    // The rule is stated where it lives rather than through a dialog, because that is
    // all there is to it.
    QVERIFY(OpenArchiveDialog::memberMatches(QStringLiteral("app(1).log"),
                                             QStringLiteral("app(1).log")));
    QVERIFY(!OpenArchiveDialog::memberMatches(QStringLiteral("app1.log"),
                                              QStringLiteral("app(1).log")));
    // A dot is a dot, not "any character".
    QVERIFY(!OpenArchiveDialog::memberMatches(QStringLiteral("appXlog"),
                                              QStringLiteral("app.log")));
    // And an empty filter hides nothing.
    QVERIFY(OpenArchiveDialog::memberMatches(QStringLiteral("anything"), QString()));
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
