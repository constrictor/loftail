#include <QtTest>

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include "LogSettingsStore.h"
#include "PreferencesDialog.h"
#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// The open flow around the Preferences dialog (SPEC.md §4). It appears only when the
// settings that resolved for a log cannot parse it, and dismissing it (Esc) CANCELS THE
// OPEN: no log is opened, whatever was on screen stays, and the node it created for the
// log is discarded with the rest of its working copy. Accepting it opens the log with
// what was entered. Drives the real MainWindow under the offscreen platform, dismissing
// the modal dialog from a timer.
class TestOpenFlow : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_good;   // parses with the app's default pattern — never prompts
    QString       m_weird;  // a real-world log4cplus layout the default cannot parse

    static bool write(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(bytes);
        return true;
    }

    // Dismiss the modal dialog once it appears, by the given key. Returns a flag the
    // caller reads afterwards to assert the dialog really was shown — an assertion
    // that would otherwise pass vacuously if the prompt silently stopped appearing.
    struct Dismisser
    {
        bool seen = false;
        QTimer timer;
    };
    static void dismissWhenShown(Dismisser &d, Qt::Key key)
    {
        d.timer.setInterval(10);
        QObject::connect(&d.timer, &QTimer::timeout, [&d, key]() {
            auto *dlg = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
            if (!dlg)
                return;
            d.seen = true;
            d.timer.stop();
            QTest::keyClick(dlg, key);
        });
        d.timer.start();
    }

private slots:
    void initTestCase();
    // Each case closes its window, which saves a session that the NEXT case's window
    // would restore — and an open now ADDS a tab rather than replacing one, so a
    // leaked session would show up as an extra view. Start each case clean.
    void init();
    // First: it needs a MainWindow with nothing open, which only holds before any
    // case below closes a window and saves a session pointing at its file.
    void escapeWithNothingOpenLeavesEmptyView();
    void escapeCancelsOpenAndKeepsCurrentFile();
    void acceptedPatternOpensTheFile();
    void absentFileOpensAWaitingTabWithNoDialog();
    // The settings tree's three levels (M20): the DEFAULTS a log nothing matches is
    // tried with, and a FILE PATTERN covering a class of logs. These are the cases that
    // show each doing its job and knowing its limits.
    void savedDefaultOpensWithoutADialog();
    void aDefaultThatDoesNotParseStillAsks();
    void aPatternMatchOpensWithoutADialog();
    void aPatternThatDoesNotParseStillAsks();
};

void TestOpenFlow::init()
{
    QSettings settings;
    settings.remove(QStringLiteral("session"));
    settings.sync();
    // Each case decides for itself what the settings tree holds. A per-log node left by
    // the previous case suppresses the very prompt under test, and leaked defaults
    // change what a never-seen log is tried with — either would make a case pass or fail
    // depending on what ran before it.
    QFile::remove(LogSettingsStore(LogSettingsStore::defaultDir()).filePath());
}

void TestOpenFlow::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_good = m_dir.filePath(QStringLiteral("good.log"));
    m_weird = m_dir.filePath(QStringLiteral("weird.log"));

    QVERIFY(write(m_good,
        "2026-07-21 10:00:00,000 [main] INFO  net.io - starting\n"
        "2026-07-21 10:00:01,000 [work] ERROR db.pool - boom\n"));

    // %D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n, with a multi-line first record.
    QVERIFY(write(m_weird,
        "03/12/26 11:50:47 DEBUG Vms::App [] - log4cplus config:\n"
        "log4cplus.threadPoolSize=1\n"
        "03/12/26 11:50:48 INFO  Vms::Http [7f2a] - listening on 8080\n"));
}

void TestOpenFlow::escapeWithNothingOpenLeavesEmptyView()
{
    MainWindow w;
    w.show();
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 0);

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    w.openFile(m_weird);
    QVERIFY2(d.seen, "the Preferences dialog was never shown");

    // Cancelled with nothing to fall back to: no view at all, rather than a table
    // of unparsed plain text.
    QTest::qWait(100);
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 0);
    w.close();
}

void TestOpenFlow::escapeCancelsOpenAndKeepsCurrentFile()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    w.openFile(m_good); // parses with the default pattern: no prompt
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(200); // let indexing finish
    LogView *before = w.findChild<LogView *>(QStringLiteral("logView"));
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail — good.log"));

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    w.openFile(m_weird); // the default cannot parse it: prompts, and we press Esc
    QVERIFY2(d.seen, "the Preferences dialog was never shown");

    // The cancelled open changed nothing: same file, same view, still usable.
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail — good.log"));
    // A cancelled open must create NOTHING: with several files openable, "the view is
    // unchanged" also has to mean "no second tab appeared".
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QCOMPARE(w.findChild<LogView *>(QStringLiteral("logView")), before);
    w.close();
}

void TestOpenFlow::acceptedPatternOpensTheFile()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();

    // Type the layout that does parse this file, then accept.
    Dismisser d;
    d.timer.setInterval(10);
    connect(&d.timer, &QTimer::timeout, [&d, &w]() {
        auto *dlg = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        d.seen = true;
        d.timer.stop();
        // By object name, never "the first QLineEdit": this dialog has several.
        auto *edit = dlg->findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
        QVERIFY(edit);
        edit->setText(QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n"));
        dlg->accept();
    });
    d.timer.start();

    w.openFile(m_weird);
    QVERIFY2(d.seen, "the Preferences dialog was never shown");
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(200);
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail — weird.log"));
    w.close();
}

void TestOpenFlow::absentFileOpensAWaitingTabWithNoDialog()
{
    // M13, and this case belongs HERE because it is about the dialog: a log that is not
    // there has no bytes to preview, autodetect from or seed a dialog with, so opening
    // one must not prompt. It opens a waiting tab, and settles its format later against
    // the bytes that actually arrive — still with no dialog, because that happens on a
    // watch tick and could land while the user is reading another tab (SPEC.md §3, §4).
    const QString absent = m_dir.filePath(QStringLiteral("notyet.log"));
    QVERIFY(!QFile::exists(absent));

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape); // fires only if a dialog appears, which it must not
    w.openFile(absent);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QVERIFY2(!d.seen, "Preferences was shown for a log with no bytes in it");

    LogView *view = w.findChild<LogView *>(QStringLiteral("logView"));
    QVERIFY(view);
    QCOMPARE(view->recordCount(), 0);
    QVERIFY(!view->placeholderText().isEmpty());

    // The log turns up. The real watcher and poll timer bring it in — no reopening, no
    // dialog — and it parses, because the format was settled from these bytes rather
    // than guessed at the empty open.
    QVERIFY(write(absent, "2026-07-21 10:00:00,000 [main] INFO  net.io - at last\n"));
    QTRY_VERIFY_WITH_TIMEOUT(view->recordCount() == 1, 5000);
    QVERIFY2(!d.seen, "Preferences was shown when the log arrived");
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail — notyet.log"));
    w.close();
}

void TestOpenFlow::savedDefaultOpensWithoutADialog()
{
    // THE feature (SPEC.md §4 "Default log format"): a user whose logs all share one
    // house layout sets it once, and every later log in that layout opens with no
    // prompt. Nothing below the MainWindow can show this — "no dialog appeared" is only
    // observable from the real open path, where offerFormat() decides.
    const QString house = m_dir.filePath(QStringLiteral("house.log"));
    QVERIFY(write(house,
        "03/12/26 11:50:47 DEBUG Vms::App [] - starting up\n"
        "03/12/26 11:50:48 INFO  Vms::Http [7f2a] - listening on 8080\n"));

    {
        // Saved BEFORE the window exists: MainWindow reads the tree once, in its
        // constructor, so a test that saved it afterwards would be testing nothing.
        LogSettingsStore store(LogSettingsStore::defaultDir());
        LogSettingsTree tree;
        LogProfile p;
        p.format.pattern = QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n");
        tree.setDefaults(p);
        QVERIFY(store.save(tree));
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape); // fires only if a dialog appears, which it must not
    w.openFile(house);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(200);
    QVERIFY2(!d.seen, "Preferences was shown for a log the saved defaults parse");

    LogView *view = w.findChild<LogView *>(QStringLiteral("logView"));
    QVERIFY(view);
    // Parsed, not opened as a wall of unparsed plain text — the default carried its
    // pattern through, rather than merely suppressing the prompt.
    QCOMPARE(view->recordCount(), 2);
    w.close();
}

void TestOpenFlow::aDefaultThatDoesNotParseStillAsks()
{
    // The limit, and the reason the default is fed through the ORDINARY open path rather
    // than applied on the way past: a wrong default costs a dialog, never a silently
    // mis-split table (SPEC.md §4). Route it around offerFormat() and this is what breaks.
    const QString other = m_dir.filePath(QStringLiteral("other.log"));
    QVERIFY(write(other,
        "03/12/26 11:50:47 DEBUG Vms::App [] - starting up\n"));

    {
        LogSettingsStore store(LogSettingsStore::defaultDir());
        LogSettingsTree tree;
        LogProfile p;
        p.format.pattern = QStringLiteral("%p|%c|%m%n"); // compiles; matches nothing here
        tree.setDefaults(p);
        QVERIFY(store.save(tree));
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    w.openFile(other);
    QVERIFY2(d.seen, "a default that cannot parse the file was applied without asking");
    w.close();
}

// A pattern node covers a CLASS of logs, so one house layout is entered once and every
// log named like it opens silently — the level the two-store arrangement had no room for.
void TestOpenFlow::aPatternMatchOpensWithoutADialog()
{
    const QString housed = m_dir.filePath(QStringLiteral("service.house"));
    QVERIFY(write(housed,
        "03/12/26 11:50:47 DEBUG Vms::App [] - starting up\n"
        "03/12/26 11:50:48 INFO  Vms::Http [7f2a] - listening on 8080\n"));

    {
        LogSettingsStore store(LogSettingsStore::defaultDir());
        LogSettingsTree tree;
        // The DEFAULTS deliberately cannot parse it. Only the pattern can, so a dialog
        // appearing would mean the pattern level was skipped.
        LogProfile root;
        root.format.pattern = QStringLiteral("%p|%c|%m%n");
        tree.setDefaults(root);

        LogPatternNode n;
        n.match = QStringLiteral("*.house");
        n.profile.format.pattern =
            QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n");
        tree.addPattern(n);
        QVERIFY(store.save(tree));
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    w.openFile(housed);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(200);
    QVERIFY2(!d.seen, "a matching file pattern was not applied silently");

    LogView *view = w.findChild<LogView *>(QStringLiteral("logView"));
    QVERIFY(view);
    QCOMPARE(view->recordCount(), 2);
    w.close();
}

// And the same limit the defaults have: a pattern is checked against the file like
// anything else, so a house layout that has drifted asks rather than mis-splitting.
void TestOpenFlow::aPatternThatDoesNotParseStillAsks()
{
    const QString housed = m_dir.filePath(QStringLiteral("drifted.house"));
    QVERIFY(write(housed,
        "03/12/26 11:50:47 DEBUG Vms::App [] - starting up\n"));

    {
        LogSettingsStore store(LogSettingsStore::defaultDir());
        LogSettingsTree tree;
        LogPatternNode n;
        n.match = QStringLiteral("*.house");
        n.profile.format.pattern = QStringLiteral("%p|%c|%m%n");
        tree.addPattern(n);
        QVERIFY(store.save(tree));
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    w.openFile(housed);
    QVERIFY2(d.seen, "a file pattern that cannot parse the log was applied without asking");
    w.close();
}

int main(int argc, char *argv[])
{
    // Isolate persistent state: the settings tree must start empty, or a remembered
    // node would suppress the very prompt under test.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-openflow"));

    TestOpenFlow tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_openflow.moc"
