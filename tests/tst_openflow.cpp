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
#include "ConfigReset.h"
#include "LogFileStore.h"
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
    void aSystemLogOpensSplitBecauseOfTheSeededPattern();
    void aPatternThatDoesNotParseStillAsks();
    void aSymlinkedLogIsStoredUnderTheNameItWasOpenedBy();
    // --pattern (SPEC.md §3): it wins over every level of the tree, and is then judged
    // against the log exactly as a resolved node is. One case per branch of that rule.
    void aSuppliedPatternThatFitsIsRememberedForTheLog();
    void aSuppliedPatternEqualToWhatIsInheritedLeavesNoNode();
    void aSuppliedPatternThatDoesNotFitAsksAndSavesNothingWhenDismissed();
    void correctingASuppliedPatternPersistsTheCorrectionAndNotTheSwitch();
    void aDismissedSuppliedPatternLeavesAStoredEntryAlone();
    void anEmptyPatternValueIsTheBareLaunch();
    void applyingToTheCurrentFileWaitsForOkAndThenRereadsTheLog();
    void okAloneRereadsTheLogWhenItsOwnSettingsMoved();
    void okLeavesTheLogAloneWhenNothingAboutItMoved();
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
    clearLogSettings();
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
    connect(&d.timer, &QTimer::timeout, [&d]() {
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

// THE ONE-TIME PATTERN SEED, end to end (SPEC.md §4). tst_logsettings pins the seeding
// rule and the pattern's own regex; what only a real MainWindow can show is that the
// seed is actually reached from the constructor, so a system log opens SPLIT and with no
// dialog on a machine where nobody has configured anything.
void TestOpenFlow::aSystemLogOpensSplitBecauseOfTheSeededPattern()
{
    const QString messages = m_dir.filePath(QStringLiteral("messages"));
    QVERIFY(write(messages,
        "Aug 27 10:15:01 web1 sshd[1234]: Accepted publickey for root\n"
        "Aug 27 10:15:02 web1 kernel: eth0: link up\n"
        "Aug  5 09:00:00 web1 systemd: Started Daily Cleanup.\n"));

    // init()'s clearLogSettings() takes logsettings.json and the per-log pool; the seed
    // FLAG is deliberately not one of a log's settings and survives it, which is what
    // makes a deleted seed stay deleted. An earlier case in this process has already
    // spent it, so this case puts it back. Spelled out rather than reached through a
    // helper: if the key is ever renamed this case stops seeding and FAILS, which is the
    // right way round for a test to notice.
    {
        QSettings s;
        s.remove(QStringLiteral("builtInPatternSeed"));
        s.sync();
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape); // fires only if a dialog appears, which it must not
    w.openFile(messages);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(200);
    QVERIFY2(!d.seen, "Preferences was shown for a system log the seeded pattern parses");

    LogView *view = w.findChild<LogView *>(QStringLiteral("logView"));
    QVERIFY(view);
    // Three records and not one: the kernel's untagged-by-pid line has to START a record
    // rather than fold into the one above it, which is the whole reason the seed carries
    // the `%c:` variant of the syslog layout.
    QCOMPARE(view->recordCount(), 3);
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

// ONE LOG, ONE SPELLING (bugs.md 27). `latest.log` pointing at today's file is the
// logrotate convention, and it used to be read under one name and stored under another:
// the pattern was matched against the name as opened, because logMatchTarget() resolves
// no symbolic link, while logSettingsKey() answered canonicalFilePath(). So the log
// opened correctly through its pattern and then had its settings REDUCED against what
// the canonical name inherits — the defaults — which is a difference, so merely opening
// it left a per-log record behind, filed under a name its pattern does not claim and
// shadowing that pattern for ever afterwards. A daily-rotated symlink burned a fresh
// slot out of the pool of 500 every day, evicting records somebody had configured.
//
// The record count is what tells the fix from the bug: the tab, the dialog and the
// parsed columns were all correct before it. The accepted cost of the other direction
// is stated in logSettingsKey() — two symlinks to one file are now two logs here.
void TestOpenFlow::aSymlinkedLogIsStoredUnderTheNameItWasOpenedBy()
{
    const QString target = m_dir.filePath(QStringLiteral("2026-08-29.house"));
    QVERIFY(write(target,
        "03/12/26 11:50:47 DEBUG Vms::App [] - starting up\n"
        "03/12/26 11:50:48 INFO  Vms::Http [7f2a] - listening on 8080\n"));

    const QString latest = m_dir.filePath(QStringLiteral("latest.house"));
    if (!QFile::link(target, latest) || !QFileInfo(latest).isSymLink())
        QSKIP("this filesystem will not make a symlink");

    {
        LogSettingsStore store(LogSettingsStore::defaultDir());
        LogSettingsTree tree;
        // The DEFAULTS cannot parse the log, and the pattern names the LINK. That is the
        // whole arrangement: whichever spelling misses the pattern falls through to a
        // default it disagrees with, which is what a stored record is made of.
        LogProfile root;
        root.format.pattern = QStringLiteral("%p|%c|%m%n");
        tree.setDefaults(root);

        LogPatternNode n;
        n.match = QStringLiteral("latest.house");
        n.profile.format.pattern =
            QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n");
        tree.addPattern(n);
        QVERIFY(store.save(tree));
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape); // fires only if a dialog appears, which it must not
    w.openFile(latest);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(200);
    QVERIFY2(!d.seen, "Preferences was shown for a log its pattern parses");

    LogView *view = w.findChild<LogView *>(QStringLiteral("logView"));
    QVERIFY(view);
    QCOMPARE(view->recordCount(), 2);

    // Nothing left behind, ANYWHERE in the pool: asserting on read(latest) alone would
    // pass with the bug in place under a store that also canonicalises, since both ends
    // of that lookup would agree on the wrong name. The pool is empty or it is not.
    w.close();
    LogFileStore pool(LogFileStore::defaultDir());
    pool.load();
    QVERIFY2(pool.addresses().isEmpty(),
             "merely opening a symlinked log left a per-log record behind");
}

// The layout m_weird is written in, and one that compiles and matches nothing anywhere.
// Both are log4cplus conversion patterns and neither is translated.
static QString weirdPattern()
{
    return QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%t] - %m%n");
}
static QString uselessPattern()
{
    return QStringLiteral("%p|%c|%m%n");
}

// The tree as it stands on disk, which is the only place a persisted format can be read
// back from — the window's in-memory copy would answer even for a write that never
// reached the file.
static LogSettingsTree storedTree()
{
    return LogSettingsStore(LogSettingsStore::defaultDir()).load();
}

static void saveTree(const LogSettingsTree &tree)
{
    // BEFORE the window is constructed, always: MainWindow reads the tree once, in its
    // constructor.
    QVERIFY(LogSettingsStore(LogSettingsStore::defaultDir()).save(tree));
}

// One log's own record, straight off the disk — the level that used to be the tree's
// `files[]` and is now one file per log (M21). Read through a SECOND store, for the same
// reason storedTree() does: the window's in-memory copy would answer even for a write
// that never reached the disk.
static LogFileSettings storedRecord(const QString &address)
{
    LogFileStore store(LogFileStore::defaultDir());
    store.load();
    return store.read(address);
}

// Give `address` settings of its own, before the window exists.
static void saveRecord(const QString &address, const QString &pattern)
{
    LogFileStore store(LogFileStore::defaultDir());
    store.load();
    LogFileSettings s;
    s.address = address;
    s.profile = LogProfile::builtIn();
    s.profile->format.pattern = pattern;
    // Against the built-in, so the record is kept whatever the tree on disk says: what
    // these cases are arranging is a log that HAS an entry, and the redundancy rule
    // would quietly refuse to make one that agreed with what it inherits.
    LogProfile nothingLikeIt;
    nothingLikeIt.format.pattern = QStringLiteral("<<never matches>>");
    QVERIFY(store.save(s, nothingLikeIt));
}

// --pattern NAMES A FORMAT AND IS NOT A PROMISE THAT IT FITS (SPEC.md §3, bugs.md 15).
// It overrides whatever the tree resolved — that is the switch's whole job — and is then
// checked against the log like any other level. It earns a per-log node by fitting; it
// costs nothing at all when it does not, because the open stops at the dialog.
//
// This case is the fitting one, over defaults that cannot parse the log: no dialog, real
// records, and the pattern written under the log's own key. Before the fix the tab and
// the records were the same, so only the last assertion tells the two apart — and it was
// also true before the fix, for the wrong reason (a supplied pattern was persisted
// UNCHECKED, which is what let an unparseable one be saved).
void TestOpenFlow::aSuppliedPatternThatFitsIsRememberedForTheLog()
{
    {
        LogSettingsTree tree;
        LogProfile root;
        root.format.pattern = uselessPattern();
        tree.setDefaults(root);
        saveTree(tree);
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape); // fires only if a dialog appears, which it must not
    // What main() does with the command line, one file at a time (MainWindow::openFiles).
    w.openFile(m_weird, weirdPattern());
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(200);
    QVERIFY2(!d.seen, "Preferences was shown for a supplied pattern that parses the log");

    // Parsed, not opened as a wall of plain text: the subsystem column has this log's
    // logger names in it, which only the supplied pattern can produce.
    QTRY_VERIFY(documentOf(w)->index().loggers.names().contains(QStringLiteral("Vms::Http")));

    const LogFileSettings hit = storedRecord(m_weird);
    QVERIFY2(hit.profile.has_value(), "a pattern that fits left no per-log record");
    QCOMPARE(hit.profile->format.pattern, weirdPattern());
    w.close();
}

// The redundancy rule is not suspended for the command line: a node exists only while it
// says something its address would not inherit anyway (LogSettingsTree::setFileProfile).
// So the same pattern passed over defaults that already say it stores nothing, and a
// scripted open cannot silt the tree up with one node per log it names.
void TestOpenFlow::aSuppliedPatternEqualToWhatIsInheritedLeavesNoNode()
{
    {
        LogSettingsTree tree;
        LogProfile root;
        root.format.pattern = weirdPattern();
        tree.setDefaults(root);
        saveTree(tree);
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    w.openFile(m_weird, weirdPattern());
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(200);
    QVERIFY2(!d.seen, "Preferences was shown for a supplied pattern that parses the log");

    QVERIFY2(!storedRecord(m_weird).saysSomething(),
             "a supplied pattern the log already inherits was stored");
    QCOMPARE(storedTree().defaults().format.pattern, weirdPattern());
    w.close();
}

// The branch the bug was: a pattern that does not fit used to be applied silently AND
// saved, so the log opened as a wall of unparsed plain text and every LATER launch —
// with no switch at all — resolved to the pattern that had been written over the working
// one. It now asks, and a dismissed dialog cancels the open and writes nothing: the
// tree is byte-for-byte what it was, defaults included.
void TestOpenFlow::aSuppliedPatternThatDoesNotFitAsksAndSavesNothingWhenDismissed()
{
    {
        // Defaults that DO parse this log, so the only thing that can make it unreadable
        // is the switch — and the only thing that can make the tree change is the switch
        // being saved.
        LogSettingsTree tree;
        LogProfile root;
        root.format.pattern = weirdPattern();
        tree.setDefaults(root);
        saveTree(tree);
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    QVERIFY2(!w.openFile(m_weird, uselessPattern()), "a pattern that fits nothing opened the log");
    QVERIFY2(d.seen, "a supplied pattern that parses nothing was applied without asking");

    // No tab: the dialog was the open, and dismissing it cancelled it.
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 0);

    QVERIFY2(!storedRecord(m_weird).saysSomething(),
             "a dismissed --pattern was written under the log's key");
    QCOMPARE(storedTree().defaults().format.pattern, weirdPattern());
    w.close();
}

// Correcting it in the dialog is the other way out, and what is kept is what the dialog
// finally said — never the switch that raised it. The dialog is Preferences, so the
// correction could as easily have been made one level up; this case takes the offered
// node, which is the log's own.
void TestOpenFlow::correctingASuppliedPatternPersistsTheCorrectionAndNotTheSwitch()
{
    {
        LogSettingsTree tree;
        LogProfile root;
        root.format.pattern = uselessPattern();
        tree.setDefaults(root);
        saveTree(tree);
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    d.timer.setInterval(10);
    connect(&d.timer, &QTimer::timeout, [&d]() {
        auto *dlg = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        d.seen = true;
        d.timer.stop();
        auto *edit = dlg->findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
        QVERIFY(edit);
        edit->setText(weirdPattern());
        dlg->accept();
    });
    d.timer.start();

    // A pattern that compiles and fits nothing — the shape of a typo in a script.
    const QString typo = QStringLiteral("%d %-5p %c - %m%n");
    QVERIFY(w.openFile(m_weird, typo));
    QVERIFY2(d.seen, "the Preferences dialog was never shown");
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(200);

    QTRY_VERIFY(documentOf(w)->index().loggers.names().contains(QStringLiteral("Vms::Http")));

    const LogFileSettings hit = storedRecord(m_weird);
    QVERIFY(hit.profile.has_value());
    QCOMPARE(hit.profile->format.pattern, weirdPattern());
    QVERIFY2(hit.profile->format.pattern != typo,
             "the switch was stored instead of the correction");
    w.close();
}

// The destructive case, and the reason "not saved anywhere" is worth a case of its own: a
// log that already HAS a remembered format. A single command line naming a pattern that
// does not fit used to overwrite that entry — or, where the pattern happened to equal the
// defaults, delete it outright under the redundancy rule — with no dialog and no message.
// Nothing the user did not confirm may touch it.
void TestOpenFlow::aDismissedSuppliedPatternLeavesAStoredEntryAlone()
{
    {
        // Defaults that cannot parse m_good, and a per-log node that can: the node is
        // therefore worth storing, and it is also the only reason the log is readable.
        LogSettingsTree tree;
        LogProfile root;
        root.format.pattern = uselessPattern();
        tree.setDefaults(root);
        saveTree(tree);
        saveRecord(m_good, LogProfile::builtIn().format.pattern);
    }
    QVERIFY(storedRecord(m_good).profile.has_value());

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    QVERIFY(!w.openFile(m_good, weirdPattern())); // fits m_weird, not this log
    QVERIFY2(d.seen, "the overriding pattern was applied to a log it cannot parse");
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 0);

    const LogFileSettings kept = storedRecord(m_good);
    QVERIFY(kept.profile.has_value());
    QCOMPARE(kept.profile->format.pattern, LogProfile::builtIn().format.pattern);
    w.close();
}

// `--pattern "$FMT"` with FMT unset reaches openFile() as an empty string, and an empty
// string names no format: there is nothing to override with, so the tree answers exactly
// as it does for a launch with no switch at all. Deliberately not an error — refusing the
// launch would refuse an open that is perfectly well defined, and the failure it would
// guard against is gone now that a supplied pattern is judged rather than believed.
void TestOpenFlow::anEmptyPatternValueIsTheBareLaunch()
{
    {
        LogSettingsTree tree;
        LogProfile root;
        root.format.pattern = uselessPattern();
        tree.setDefaults(root);
        saveTree(tree);
        saveRecord(m_good, LogProfile::builtIn().format.pattern);
    }

    MainWindow w;
    w.resize(900, 600);
    w.show();

    Dismisser d;
    dismissWhenShown(d, Qt::Key_Escape);
    QVERIFY(w.openFile(m_good, QString::fromLatin1("")));
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTest::qWait(200);
    QVERIFY2(!d.seen, "an empty --pattern value asked about a log its own entry parses");

    QCOMPARE(w.findChild<LogView *>(QStringLiteral("logView"))->recordCount(), 2);
    QTRY_VERIFY(documentOf(w)->index().loggers.names().contains(QStringLiteral("net.io")));
    QCOMPARE(storedRecord(m_good).profile->format.pattern,
             LogProfile::builtIn().format.pattern);
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

// OK ALONE APPLIES, and this is the case the button above did not cover.
//
// The ordinary errand is "this log is not parsing; fix its pattern": open Preferences on
// the log in front of you, correct the format, press OK. Until now that stored the
// setting and left the tab exactly as it was — the dialog's preview showed the columns
// split correctly over a table still showing the whole file in the message column, with
// nothing on screen to say the change had been saved and would arrive the next time the
// log was opened. "Apply to current file" was the only route, and it reads as a button
// for trying a setting out rather than as the way to make OK mean anything.
void TestOpenFlow::okAloneRereadsTheLogWhenItsOwnSettingsMoved()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_good);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);

    Document *doc = documentOf(w);
    QVERIFY(doc->index().loggers.names().contains(QStringLiteral("net.io")));

    bool seen = false;
    QTimer timer;
    timer.setInterval(10);
    connect(&timer, &QTimer::timeout, [&]() {
        auto *dlg = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        seen = true;
        timer.stop();
        auto *pattern = dlg->findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
        auto *apply = dlg->findChild<QPushButton *>(QStringLiteral("applyToCurrentButton"));
        QVERIFY(pattern);
        QVERIFY(apply);
        // Dropping the %c leaves these lines parsing with nothing in them a subsystem,
        // which is a change only a re-read can show.
        pattern->setText(QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %m%n"));
        // DELIBERATELY NOT PRESSED: that button is the other test.
        QVERIFY2(!apply->isChecked(), "the apply request was armed by something else");
        dlg->accept();
    });
    timer.start();

    auto *prefs = w.findChild<QAction *>(QStringLiteral("preferencesAction"));
    QVERIFY(prefs);
    prefs->trigger();
    QVERIFY2(seen, "the Preferences dialog was never shown");

    QTRY_VERIFY(documentOf(w)->format().loggerGroup <= 0); // absent is the -1 sentinel
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);
    // Re-read, not merely re-compiled: the intern tables come from the scan, so a stale
    // one is proof the file was never read again.
    QVERIFY2(!documentOf(w)->index().loggers.names().contains(QStringLiteral("net.io")),
             "the pre-change intern table survived: the log was not re-read");
    QCOMPARE(documentOf(w), doc); // re-read in place, not reopened
    w.close();
}

// The other half of the same rule, and the reason it is a COMPARISON rather than an
// unconditional apply: a visit to Preferences that does not move what this log resolves
// to must not stop its workers, empty its index and scan it again. Editing the defaults
// while the log has a node of its own is exactly that — first match wins, so the log
// never sees it.
void TestOpenFlow::okLeavesTheLogAloneWhenNothingAboutItMoved()
{
    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(m_good);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);
    const RecordIndex *before = &documentOf(w)->index();
    QVERIFY(documentOf(w)->index().loggers.names().contains(QStringLiteral("net.io")));

    bool seen = false;
    QTimer timer;
    timer.setInterval(10);
    connect(&timer, &QTimer::timeout, [&]() {
        auto *dlg = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
        if (!dlg)
            return;
        seen = true;
        timer.stop();
        // Opened on the log's own row, so simply accepting changes nothing about it.
        dlg->accept();
    });
    timer.start();

    auto *prefs = w.findChild<QAction *>(QStringLiteral("preferencesAction"));
    QVERIFY(prefs);
    prefs->trigger();
    QVERIFY2(seen, "the Preferences dialog was never shown");

    QTest::qWait(200);
    QCOMPARE(&documentOf(w)->index(), before);
    QVERIFY2(documentOf(w)->index().loggers.names().contains(QStringLiteral("net.io")),
             "the log was re-read although nothing about it had changed");
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
