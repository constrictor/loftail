#include <QtTest>

#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QGroupBox>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "Filter.h"
#include "Highlight.h"
#include "LogProfile.h"
#include "LogSettingsStore.h"
#include "LogView.h"
#include "MainWindow.h"
#include "Priority.h"
#include "RecordIndex.h"

using namespace loftail;

// View ▸ Reload / F5 (SPEC.md §3 "Reloading by hand"). Drives the real MainWindow under
// the offscreen platform, because the whole claim is about what SURVIVES the reload —
// the tab, its views, the filters, the highlight rules — and none of that lives on the
// Document alone.
//
// The reload deliberately re-reads with the format the log already has (invariant #3),
// so those cases change the FILE and never the pattern. The FORMAT-CHANGE cases at the
// bottom are the OTHER caller of the same rebuild path — the one that had done nothing at
// all since M9, because it routed through openWithSettings(), whose first act is to raise
// the tab of a file that is already open.
class TestReload : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    static QByteArray rec(int sec, const char *prio, const char *logger, const char *msg)
    {
        return QByteArray("2026-07-21 10:00:")
            + QByteArray::number(sec).rightJustified(2, '0')
            + ",000 [main] " + prio + "  " + logger + " - " + msg + "\n";
    }

    static bool writeWhole(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        f.write(bytes);
        return true;
    }

    // The action as the user reaches it, never by calling the private slot: the binding
    // is half of what is being claimed here.
    static QAction *reloadAction(MainWindow &w)
    {
        return w.findChild<QAction *>(QStringLiteral("reloadAction"));
    }

    // Through the view's CONTEXT rather than the LogView, which holds only a const
    // Document and does not publish it.
    static Document *documentOf(MainWindow &w)
    {
        const auto views = w.findChildren<DocumentView *>();
        return views.isEmpty() ? nullptr : views.first()->context()->doc.get();
    }

private slots:
    void init();
    void itIsOnF5AndDisabledWithNothingOpen();
    void itRereadsAFileChangedBehindTheApplication();
    void itKeepsTheTabTheFiltersAndTheHighlightRules();
    void itKeepsWatchingAfterwards();
    void reloadingAVanishedLogWaitsForItRatherThanFailing();

    // The other caller of the rebuild path: a settings change on a log already open.
    void changingThePatternOfAnOpenLogRereadsIt();
    void changingThePatternKeepsTheDocumentAndItsHighlightRules();
    void aFormatChangeRebindsThePanesToTheNewFormat();
};

void TestReload::init()
{
    QVERIFY(m_dir.isValid());
    QSettings settings;
    settings.remove(QStringLiteral("session"));
    settings.sync();
    // A per-log node left by a previous case would decide this one's format; the reload
    // is not supposed to consult the tree at all, and a leaked node would hide it if it
    // ever started to.
    QFile::remove(LogSettingsStore(LogSettingsStore::defaultDir()).filePath());
}

void TestReload::itIsOnF5AndDisabledWithNothingOpen()
{
    MainWindow w;
    w.show();
    QAction *reload = reloadAction(w);
    QVERIFY2(reload, "View ▸ Reload is missing");
    // QKeySequence::Refresh, which is F5 on every platform loftail targets. Asserted as
    // the resolved sequence rather than the enum, since that is what a user presses.
    QCOMPARE(reload->shortcut(), QKeySequence(Qt::Key_F5));
    QVERIFY2(!reload->isEnabled(), "there is nothing to reload with no log open");
    w.close();
}

void TestReload::itRereadsAFileChangedBehindTheApplication()
{
    const QString path = m_dir.filePath(QStringLiteral("reload.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "old.logger", "one")
                             + rec(1, "INFO ", "old.logger", "two")));

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(path);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);

    // Rewritten wholesale with different content. The live watch would catch this too
    // now, but the point here is the deliberate gesture: whatever has gone wrong, F5 is
    // the way back to what is actually in the file.
    QVERIFY(writeWhole(path, rec(0, "ERROR", "new.logger", "a")
                             + rec(1, "ERROR", "new.logger", "b")
                             + rec(2, "ERROR", "new.logger", "c")));

    reloadAction(w)->trigger();
    QTRY_COMPARE(documentOf(w)->index().records.size(), 3);

    Document *doc = documentOf(w);
    QCOMPARE(doc->index().records.at(0).priorityEnum(), Priority::Error);
    QVERIFY(doc->index().loggers.names().contains(QStringLiteral("new.logger")));
    // Emptied, never appended to. A reload that left the old records in place would show
    // one copy of the log after another, growing by a whole file each time.
    QVERIFY2(!doc->index().loggers.names().contains(QStringLiteral("old.logger")),
             "the pre-reload records are still in the index");
    // One tab still, not a second one: a reload is not an open.
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    w.close();
}

void TestReload::itKeepsTheTabTheFiltersAndTheHighlightRules()
{
    const QString path = m_dir.filePath(QStringLiteral("keep.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "a.one", "keep me")
                             + rec(1, "ERROR", "a.two", "and me")));

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(path);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);

    Document *doc = documentOf(w);

    // THROUGH THE PANE, never straight onto the Document. The pane holds the widget
    // state that is not derivable from a FilterSet and pushes it back on every refresh,
    // so a filter written directly onto the Document is one the next refresh overwrites —
    // which is a property of the pane, not of the reload, and would make this case fail
    // for a reason it is not about.
    // Scoped to the Filters dock. The Highlighters pane embeds the SAME AxisEditor with
    // the same object names, so an unscoped findChild is a coin toss between them.
    auto *filtersDock = w.findChild<QDockWidget *>(QStringLiteral("filtersDock"));
    QVERIFY2(filtersDock, "the Filters dock is missing");
    auto *priorityGroup = filtersDock->findChild<QGroupBox *>(QStringLiteral("priorityGroup"));
    auto *priorityCombo = filtersDock->findChild<QComboBox *>(QStringLiteral("priorityCombo"));
    QVERIFY(priorityGroup && priorityCombo);
    priorityGroup->setChecked(true);
    priorityCombo->setCurrentIndex(priorityCombo->findData(int(Priority::Error)));
    QTRY_COMPARE(doc->filtered().recordCount(), 1);

    HighlightRule rule;
    rule.enabled = true;
    rule.match.text.enabled = true;
    rule.match.text.matcher.set(QStringLiteral("and me"), false, Qt::CaseInsensitive);
    doc->highlighters().rules.append(rule);
    doc->resolveHighlighters();

    reloadAction(w)->trigger();
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);
    // The SAME Document, not a replacement: everything per-file hangs off it (invariant
    // #7), and a reload that swapped it would strand every pane bound to it.
    QCOMPARE(documentOf(w), doc);

    // Both survive, and the filter is back IN FORCE over the rebuilt index rather than
    // merely remembered — the intern ids it compares against were rebuilt from scratch.
    QVERIFY(doc->filters().anyActive());
    QCOMPARE(doc->filtered().recordCount(), 1);
    QCOMPARE(doc->highlighters().rules.size(), 1);
    QVERIFY(doc->highlighters().rules.first().enabled);
    w.close();
}

void TestReload::itKeepsWatchingAfterwards()
{
    // stopWorkers() destroys the live controller along with the index worker, so a reload
    // that forgot to re-arm it would look perfect right up until the log next grew — and
    // then never update again, which is the exact fault a reload is reached for.
    const QString path = m_dir.filePath(QStringLiteral("watch.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "w.one", "first")));

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(path);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 1);

    reloadAction(w)->trigger();
    QTRY_COMPARE(documentOf(w)->index().records.size(), 1);

    QFile f(path);
    QVERIFY(f.open(QIODevice::Append));
    f.write(rec(1, "INFO ", "w.one", "appended after the reload"));
    f.close();

    // The watch polls at 750 ms; give it a couple of ticks before believing it is dead.
    QTRY_VERIFY_WITH_TIMEOUT(documentOf(w)->index().records.size() == 2, 5000);
    w.close();
}

void TestReload::reloadingAVanishedLogWaitsForItRatherThanFailing()
{
    // A log deleted between the key press and the reopen is not an error — it is the
    // waiting state, exactly as a deletion noticed by the watch would be (§6.5). And the
    // watch has to be re-armed on that path too, or nothing would ever bring it back.
    const QString path = m_dir.filePath(QStringLiteral("gone.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "g.one", "here for now")));

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(path);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 1);

    QVERIFY(QFile::remove(path));
    reloadAction(w)->trigger();
    QTRY_VERIFY(documentOf(w)->isWaiting());
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);

    // Back again, and the re-armed watch is what notices.
    QVERIFY(writeWhole(path, rec(0, "WARN ", "g.two", "and back")
                             + rec(1, "WARN ", "g.two", "with more")));
    QTRY_VERIFY_WITH_TIMEOUT(!documentOf(w)->isWaiting(), 5000);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);
    w.close();
}

// --- a settings change on a log that is already open ------------------------
//
// Driven through applyProfileToActive(), which is exactly what Preferences ▸ OK calls when
// "Apply to current file" was ticked — the dialog itself applies nothing, it only records
// the request. Public for that reason, so these need no modal dialog on screen.
//
// THE M9 REGRESSION these exist for: applySettings() routed a pattern change through
// openWithSettings(), which begins "reopening a file already open just raises its view" —
// so the newly prepared Document was dropped and the tab merely raised. Before tabs
// (f197c0d) that line read teardownDocument(), which is what had made a format change
// reindex. Preferences ▸ Apply to current file, the timestamp header menu and the deferred
// format prompt therefore all did nothing, silently, for four milestones.

// A pattern that parses the SAME lines with one field fewer. That is what makes "did it
// actually re-read?" answerable from the format and the intern tables rather than from a
// record count that would not move either way.
static LogProfile withPattern(const QString &pattern)
{
    LogProfile p = LogProfile::builtIn();
    p.format.pattern = pattern;
    return p;
}

void TestReload::changingThePatternOfAnOpenLogRereadsIt()
{
    const QString path = m_dir.filePath(QStringLiteral("format.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "fmt.sys", "one")
                             + rec(1, "INFO ", "fmt.sys", "two")));

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(path);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);
    Document *doc = documentOf(w);
    QVERIFY(doc->format().loggerGroup > 0);
    QVERIFY(doc->index().loggers.names().contains(QStringLiteral("fmt.sys")));

    // Drop the %c: same lines, still parsed, but nothing is a subsystem any more.
    w.applyProfileToActive(
        withPattern(QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %m%n")));

    QTRY_VERIFY(documentOf(w)->format().loggerGroup <= 0); // absent is the -1 sentinel
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);
    // Re-read, not merely re-compiled: the intern tables come from the scan, so a stale
    // one is proof the file was never read again.
    QVERIFY2(!documentOf(w)->index().loggers.names().contains(QStringLiteral("fmt.sys")),
             "the pre-change intern table survived: the log was not re-read");
    QCOMPARE(documentOf(w)->index().records.at(0).priorityEnum(), Priority::Info);
    // A format change re-reads a log; it does not open one.
    QCOMPARE(documentOf(w), doc);
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    w.close();
}

void TestReload::changingThePatternKeepsTheDocumentAndItsHighlightRules()
{
    const QString path = m_dir.filePath(QStringLiteral("formatkeep.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "k.sys", "alpha")
                             + rec(1, "ERROR", "k.sys", "beta")));

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(path);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);
    Document *doc = documentOf(w);

    HighlightRule rule;
    rule.enabled = true;
    rule.match.text.enabled = true;
    rule.match.text.matcher.set(QStringLiteral("beta"), false, Qt::CaseInsensitive);
    doc->highlighters().rules.append(rule);
    doc->resolveHighlighters();

    w.applyProfileToActive(
        withPattern(QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %m%n")));
    QTRY_VERIFY(documentOf(w)->format().loggerGroup <= 0); // absent is the -1 sentinel

    // Everything per-file survives, because the Document does (invariant #7) — which is
    // the thing openWithSettings() could never have delivered even had it worked, since
    // it builds a new Document and a new context.
    QCOMPARE(documentOf(w), doc);
    QCOMPARE(doc->highlighters().rules.size(), 1);
    QVERIFY(doc->highlighters().rules.first().enabled);
    QCOMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    w.close();
}

void TestReload::aFormatChangeRebindsThePanesToTheNewFormat()
{
    // An axis is usable only where the format can fill it (SPEC.md §6). The panes learn
    // that in setDocument(), which nothing would call again after a format change — the
    // Document pointer has not moved, so activeDocumentChanged does not fire on its own.
    // Without the explicit rebind the pane goes on offering a Thread axis for a pattern
    // that has no %t, and a filter set through it would match nothing with no way to see
    // why.
    const QString path = m_dir.filePath(QStringLiteral("axes.log"));
    QVERIFY(writeWhole(path, rec(0, "INFO ", "ax.sys", "one")
                             + rec(1, "INFO ", "ax.sys", "two")));

    MainWindow w;
    w.resize(900, 600);
    w.show();
    w.openFile(path);
    QTRY_COMPARE(w.findChildren<LogView *>(QStringLiteral("logView")).size(), 1);
    QTRY_COMPARE(documentOf(w)->index().records.size(), 2);

    auto *filtersDock = w.findChild<QDockWidget *>(QStringLiteral("filtersDock"));
    QVERIFY(filtersDock);
    auto *threadGroup = filtersDock->findChild<QGroupBox *>(QStringLiteral("threadGroup"));
    QVERIFY2(threadGroup, "the Thread axis is missing");
    QVERIFY2(threadGroup->isEnabled(), "the opening pattern has a %t, so Thread is usable");

    // Same lines, still parsed, no thread field.
    w.applyProfileToActive(
        withPattern(QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} %-5p %c - %m%n")));
    QTRY_VERIFY(documentOf(w)->format().threadGroup <= 0); // absent is the -1 sentinel

    QVERIFY2(!threadGroup->isEnabled(),
             "the Thread axis is still usable for a format that carries no %t");
    w.close();
}

QTEST_MAIN(TestReload)
#include "tst_reload.moc"
