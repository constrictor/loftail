#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
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
    static Document *documentOf(MainWindow &w)
    {
        const auto views = w.findChildren<DocumentView *>();
        return views.isEmpty() ? nullptr : views.first()->context()->doc.get();
    }

    // The tab showing `path`, whichever tab that is. Several are open in the cases
    // below, and the one being asserted about is deliberately not the active one.
    static DocumentView *viewOf(MainWindow &w, const QString &path)
    {
        const auto views = w.findChildren<DocumentView *>();
        for (DocumentView *v : views) {
            if (v->context() && v->context()->doc && v->context()->doc->path() == path)
                return v;
        }
        return nullptr;
    }

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
    void anEmptyFileOpensATabInsteadOfAskingAboutNothing();
    void aLogThatTurnsUpEmptyIsAskedAboutOnlyWhenItHasLines();
    void aBackgroundTabIsNotToldAnEmptyFileHasAnUnrecognisedFormat();
    // The settings tree's three levels (M20): the DEFAULTS a log nothing matches is
    // tried with, and a FILE PATTERN covering a class of logs. These are the cases that
    // show each doing its job and knowing its limits.
    void savedDefaultOpensWithoutADialog();
    void aDefaultThatDoesNotParseStillAsks();
    void aPatternMatchOpensWithoutADialog();
    void aPatternThatDoesNotParseStillAsks();
    void applyingToTheCurrentFileWaitsForOkAndThenRereadsTheLog();
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

// (iii) The direct open of a log that exists and is empty — `: > app.log; loftail
// app.log`, which is what a service that has not logged yet leaves behind and the very
// file somebody opens to watch it start. It used to be REFUSED: formatFits() is false by
// construction over 0 bytes, so Preferences appeared previewing "No sample lines to
// preview." with Detect greyed out, and the only sensible answer — Escape — cancelled the
// open, leaving the status bar reading "Open cancelled" and no tab at all.
void TestOpenFlow::anEmptyFileOpensATabInsteadOfAskingAboutNothing()
{
    const QString blank = m_dir.filePath(QStringLiteral("blank.log"));
    QVERIFY(write(blank, QByteArray()));
    QVERIFY(QFileInfo(blank).size() == 0);

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape); // fires only if a dialog appears, which it must not
    w.openFile(blank);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QVERIFY2(!d.seen, "Preferences was shown for a log with no lines in it");
    QCOMPARE(w.windowTitle(), QStringLiteral("loftail — blank.log"));

    LogView *view = w.findChild<LogView *>(QStringLiteral("logView"));
    QVERIFY(view);
    QCOMPARE(view->recordCount(), 0);
    // Nothing was judged, so nothing is reported: "format not recognised" about a file
    // with no lines in it is an accusation nobody can act on.
    DocumentView *dv = w.findChild<DocumentView *>();
    QVERIFY(dv);
    QVERIFY(dv->context()->formatNotice.isEmpty());
    QVERIFY(!dv->context()->doc->formatSettled());

    // The service starts logging. The real watcher brings the lines in, the format is
    // judged against them for the first time, it fits, and nothing is asked.
    QVERIFY(write(blank,
        "2026-07-21 10:00:00,000 [main] INFO  net.io - starting\n"
        "2026-07-21 10:00:01,000 [work] ERROR db.pool - boom\n"));
    QTRY_VERIFY_WITH_TIMEOUT(view->recordCount() == 2, 5000);
    QVERIFY2(!d.seen, "Preferences was shown when the first lines arrived and parsed");
    QVERIFY(dv->context()->formatNotice.isEmpty());
    QVERIFY(dv->context()->doc->formatSettled());
    w.close();
}

// (i) The headline case, end to end through the window: a log that is not there, then
// created EMPTY, then written to. The dialog the open owes is spent on the lines that
// eventually arrive — not on the instant the file came into existence, where it
// previewed nothing and could detect nothing.
void TestOpenFlow::aLogThatTurnsUpEmptyIsAskedAboutOnlyWhenItHasLines()
{
    const QString later = m_dir.filePath(QStringLiteral("later.log"));
    QFile::remove(later);
    QVERIFY(!QFile::exists(later));

    {
        // Defaults that compile and match nothing here, so the arrival of real lines
        // MUST produce the dialog — which is what makes "no dialog yet" meaningful
        // rather than vacuous.
        LogSettingsStore store(LogSettingsStore::defaultDir());
        LogSettingsTree tree;
        LogProfile root;
        root.format.pattern = QStringLiteral("%p|%c|%m%n");
        tree.setDefaults(root);
        QVERIFY(store.save(tree));
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    // Not the shared dismisser: this one reads the dialog before dismissing it, because
    // the whole complaint was that it appeared with nothing to show.
    bool seen = false;
    bool hadASample = false;
    bool couldDetect = false;
    QTimer timer;
    timer.setInterval(10);
    connect(&timer, &QTimer::timeout, [&]() {
        auto *dlg = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        seen = true;
        timer.stop();
        auto *empty = dlg->findChild<QLabel *>(QStringLiteral("formatPreviewEmptyLabel"));
        auto *detect = dlg->findChild<QPushButton *>(QStringLiteral("formatDetectButton"));
        hadASample = empty && empty->text().isEmpty();
        couldDetect = detect && detect->isEnabled();
        QTest::keyClick(dlg, Qt::Key_Escape);
    });
    timer.start();

    w.openFile(later);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QVERIFY2(!seen, "Preferences was shown for a log that is not there");
    LogView *view = w.findChild<LogView *>(QStringLiteral("logView"));
    QVERIFY(view);
    DocumentView *dv = w.findChild<DocumentView *>();
    QVERIFY(dv);

    // The file is CREATED, and that is all. It stops waiting, because it is there — but
    // nothing about it has been judged, so nothing is asked and nothing is reported.
    QVERIFY(write(later, QByteArray()));
    QTRY_VERIFY_WITH_TIMEOUT(!dv->context()->doc->isWaiting(), 5000);
    QTest::qWait(1200); // two poll ticks: a dialog would have had every chance
    QVERIFY2(!seen, "Preferences was shown the instant the file existed");
    QVERIFY(dv->context()->formatNotice.isEmpty());
    QVERIFY(!dv->context()->doc->formatSettled());

    // The first records. NOW there is something to judge, so the dialog the open owed
    // appears — with lines in its preview and its Detect button live.
    QVERIFY(write(later,
        "03/12/26 11:50:47 DEBUG Vms::App [] - starting up\n"
        "03/12/26 11:50:48 INFO  Vms::Http [7f2a] - listening on 8080\n"));
    QTRY_VERIFY_WITH_TIMEOUT(seen, 5000);
    QVERIFY2(hadASample, "the dialog previewed no sample lines");
    QVERIFY2(couldDetect, "Detect was greyed out, so there was nothing to detect from");
    // Declined, so the log stays readable and the status bar says where to fix it —
    // which is now an honest report about lines that really do not parse.
    QTRY_VERIFY(!dv->context()->formatNotice.isEmpty());
    w.close();
}

// (iv) The silent half of the same fault. A tab that is not on screen is never asked
// anything, so what it got instead was the notice — latched, for the session, against a
// file that had just been created and had nothing in it. Nothing later cleared it, and
// with no dialog there was nothing to see it happen.
void TestOpenFlow::aBackgroundTabIsNotToldAnEmptyFileHasAnUnrecognisedFormat()
{
    const QString behind = m_dir.filePath(QStringLiteral("behind.log"));
    QFile::remove(behind);

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    w.openFile(behind);  // waiting, and about to be pushed into the background
    w.openFile(m_good);  // parses with the built-in defaults: this tab is now the active one
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 2);
    QVERIFY2(!d.seen, "a dialog was shown for a log with no bytes");

    DocumentView *dv = viewOf(w, behind);
    QVERIFY2(dv, "the waiting tab is gone");

    QVERIFY(write(behind, QByteArray()));
    QTRY_VERIFY_WITH_TIMEOUT(!dv->context()->doc->isWaiting(), 5000);
    QTest::qWait(1200);
    // The whole point: no dialog was raised (it is a background tab, and §6.5 says
    // nothing pops up on arrival) and no notice was written either.
    QVERIFY2(!d.seen, "Preferences was shown for a background tab");
    QVERIFY2(dv->context()->formatNotice.isEmpty(),
             "a background tab was told its format was not recognised by an empty file");

    QVERIFY(write(behind, "2026-07-21 10:00:00,000 [main] INFO  net.io - at last\n"));
    QTRY_VERIFY_WITH_TIMEOUT(dv->logView()->recordCount() == 1, 5000);
    // Judged against real lines, silently, exactly as a background tab should be: the
    // defaults fit, so there is nothing to say about them.
    QVERIFY(dv->context()->doc->formatSettled());
    QVERIFY(dv->context()->formatNotice.isEmpty());
    QVERIFY2(!d.seen, "Preferences was shown for a background tab when its log arrived");
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

void TestOpenFlow::applyingToTheCurrentFileWaitsForOkAndThenRereadsTheLog()
{
    // "Apply to current file" used to accept() the dialog, which is how the request it
    // records got carried out at all — and made it the one button on a panel of in-place
    // edits whose press ended the session, beside Promote, whose press does not. It now
    // arms the request and OK performs it, so this drives the whole round trip: the press
    // leaves the dialog standing, the user goes on editing, and OK re-reads the log with
    // what the entry FINALLY says.
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_good);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);

    Document *doc = documentOf(w);
    QVERIFY(doc->format().loggerGroup > 0);
    QVERIFY(doc->index().loggers.names().contains(QStringLiteral("net.io")));

    bool seen = false;
    bool stayedOpen = false;
    bool armedVisibly = false;
    QTimer timer;
    timer.setInterval(10);
    connect(&timer, &QTimer::timeout, [&]() {
        auto *dlg = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        seen = true;
        timer.stop();

        // By object name, never by label: this dialog has several buttons and the label
        // is prose that a translated build moves.
        auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("applyToCurrentButton"));
        auto *notice = dlg->findChild<QLabel *>(QStringLiteral("applyNoticeLabel"));
        auto *pattern = dlg->findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
        QVERIFY(apply);
        QVERIFY(notice);
        QVERIFY(pattern);
        QVERIFY2(!apply->isHidden(), "the open log was never named as a target");

        apply->click();
        stayedOpen = dlg->isVisible();
        armedVisibly = apply->isChecked() && notice->isVisible();

        // Asked for, and then edited further — the request names the ENTRY, not a copy of
        // what it said when the button went down. Dropping the %c leaves the same lines
        // parsing with nothing in them a subsystem.
        pattern->setText(QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %m%n"));
        dlg->accept();
    });
    timer.start();

    auto *prefs = w.findChild<QAction *>(QStringLiteral("preferencesAction"));
    QVERIFY(prefs);
    prefs->trigger(); // modal: returns once the timer above has accepted the dialog

    QVERIFY2(seen, "the Preferences dialog was never shown");
    QVERIFY2(stayedOpen, "asking to apply closed the dialog");
    QVERIFY2(armedVisibly, "the request left no mark on screen");

    // The request reached the window and the log was re-read with the edited settings.
    QTRY_VERIFY(documentOf(w)->format().loggerGroup <= 0); // absent is the -1 sentinel
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);
    // Re-read, not merely re-compiled: the intern tables come from the scan, so a stale
    // one is proof the file was never read again.
    QVERIFY2(!documentOf(w)->index().loggers.names().contains(QStringLiteral("net.io")),
             "the pre-change intern table survived: the log was not re-read");
    QCOMPARE(documentOf(w), doc); // re-read in place, not reopened
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
