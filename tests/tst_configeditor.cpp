#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QCloseEvent>

#include "ConfigReset.h"
#include "ConfigView.h"
#include "DocumentView.h"
#include "FindBar.h"
#include "Fonts.h"
#include "LogFileStore.h"
#include "LogProfile.h"
#include "LogSettings.h"
#include "LogSettingsStore.h"
#include "MainWindow.h"

using namespace loftail;

// The config-file editor as a page in the document well (SPEC.md §4), driven through a
// real MainWindow.
//
// Everything here is about the SEAM rather than about editing text: a second kind of page
// in a well that has held exactly one kind since M9. What that seam gets wrong, it gets
// wrong silently — an action that acts on the log behind the editor, a tab that cannot be
// closed, a font that stops following the zoom — so the assertions are on the surfaces
// those failures actually show up on.
class TestConfigEditor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void theEditorOpensAsATabBesideTheLog();
    void aMissingConfigFileOpensEmptyAndSayingSo();
    void theEditorUsesTheLogFontAndFollowsAZoom();
    void saveIsVisibleOnlyOnAnEditorTab();
    void anEditedTabIsMarkedAndSavingClearsTheMark();
    void savingWritesTheBytesAndPreservesPermissions();
    void aMissingDirectoryIsRefusedByName();
    void perLogActionsDoNotActOnTheLogBehindTheEditor();
    void findSearchesTheEditorAndNotTheLog();
    void theSyntaxIsShownAndCanBeOverridden();
    void closingAnEditorTabWorksAndDoesNotDisturbTheLogs();
    void anEditorTabComesBackInPlaceAfterARelaunch();
    void cancellingTheUnsavedPromptAbortsTheQuitAndWritesNoSession();
    void aRemoteConfigPutsItsTabUpBeforeTheFarEndAnswers();
    void closingATabMidConnectDoesNotWaitForIt();

private:
    QString writeLog(const QString &name);
    QString configPathFor(const QString &logPath, const QString &configName);
    // Open `log`, having pointed it at `configName` in its own settings node.
    MainWindow *openWithConfig(const QString &logPath, const QString &configRelative);

    QTemporaryDir m_dir;
};

void TestConfigEditor::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

void TestConfigEditor::init()
{
    QSettings settings;
    settings.remove(QStringLiteral("session"));
    settings.sync();
    // A log's settings now OUTLIVE its tab, so a config path left by an earlier case
    // would silently supply the very setting the next case is about — the failure that
    // broke six suites in M21.
    clearLogSettings();
}

QString TestConfigEditor::writeLog(const QString &name)
{
    const QString path = m_dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write("2026-01-01 00:00:00,000 [main] INFO  app - hello\n");
    f.write("2026-01-01 00:00:01,000 [main] WARN  app - careful\n");
    return path;
}

QString TestConfigEditor::configPathFor(const QString &logPath, const QString &configName)
{
    return QFileInfo(logPath).absoluteDir().filePath(configName);
}

MainWindow *TestConfigEditor::openWithConfig(const QString &logPath,
                                             const QString &configRelative)
{
    // Through the settings TREE's defaults rather than a per-log node, so the case is
    // also exercising that a config path resolves per log against its own directory.
    LogSettingsStore store(LogSettingsStore::defaultDir());
    LogSettingsTree tree = store.load();
    LogProfile defaults = tree.defaults();
    defaults.configPath = configRelative;
    tree.setDefaults(defaults);
    store.save(tree);

    auto *w = new MainWindow;
    w->show();
    w->openFile(logPath);
    return w;
}

void TestConfigEditor::theEditorOpensAsATabBesideTheLog()
{
    const QString log = writeLog(QStringLiteral("app.log"));
    QVERIFY(!log.isEmpty());
    const QString config = configPathFor(log, QStringLiteral("log4cplus.properties"));
    QFile f(config);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("log4cplus.rootLogger=DEBUG, STDOUT\n");
    f.close();

    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("log4cplus.properties")));
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 1);

    auto *open = w->findChild<QAction *>(QStringLiteral("openConfigAction"));
    QVERIFY(open);
    QVERIFY(open->isEnabled());
    open->trigger();

    // BESIDE the log, not instead of it: two pages, of two kinds.
    QCOMPARE(tabs->count(), 2);
    auto *editor = qobject_cast<ConfigView *>(tabs->currentWidget());
    QVERIFY(editor);
    QVERIFY(editor->editor()->toPlainText().contains(QStringLiteral("rootLogger")));
    // And the log tab is untouched.
    QVERIFY(qobject_cast<DocumentView *>(tabs->widget(0)));
}

void TestConfigEditor::aMissingConfigFileOpensEmptyAndSayingSo()
{
    const QString log = writeLog(QStringLiteral("missing-config.log"));
    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("not-there.properties")));

    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    auto *editor = qobject_cast<ConfigView *>(tabs->currentWidget());
    // THE SUPPORTED CASE, not a refusal: the buffer opens empty and Save will create it.
    QVERIFY(editor);
    QVERIFY(editor->editor()->toPlainText().isEmpty());
    QVERIFY(!editor->fileExisted());
    // And it SAYS so, rather than looking like an empty config file that already exists —
    // which would invite the reader to save over nothing and wonder where it went.
    auto *notice = editor->findChild<QLabel *>(QStringLiteral("configNotice"));
    QVERIFY(notice);
    QVERIFY(notice->isVisible());
    QVERIFY(!notice->text().isEmpty());
}

void TestConfigEditor::theEditorUsesTheLogFontAndFollowsAZoom()
{
    if (QFontDatabase::families().isEmpty())
        QSKIP("no font database (Windows offscreen ships no fonts)");

    const QString log = writeLog(QStringLiteral("font.log"));
    QVERIFY(QFile(configPathFor(log, QStringLiteral("f.properties"))).open(QIODevice::WriteOnly));
    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("f.properties")));
    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    auto *editor = qobject_cast<ConfigView *>(tabs->currentWidget());
    QVERIFY(editor);

    QCOMPARE(editor->editor()->font().family(), logTextFont().family());
    const int before = editor->editor()->font().pointSize();

    // ONLY OBSERVABLE THIS WAY. The failure shape is "the application-wide size moved and
    // this widget did not", which no visible text shows: the editor keeps rendering
    // perfectly, at the wrong size, for the rest of the session.
    w->findChild<QAction *>(QStringLiteral("zoomInAction"))->trigger();
    QVERIFY(editor->editor()->font().pointSize() > before);
}

void TestConfigEditor::saveIsVisibleOnlyOnAnEditorTab()
{
    const QString log = writeLog(QStringLiteral("save-vis.log"));
    QVERIFY(QFile(configPathFor(log, QStringLiteral("v.properties"))).open(QIODevice::WriteOnly));
    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("v.properties")));
    auto *save = w->findChild<QAction *>(QStringLiteral("saveConfigAction"));
    QVERIFY(save);
    // On a log tab: not merely disabled, ABSENT — which is what was asked for.
    QVERIFY(!save->isVisible());

    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    QVERIFY(save->isVisible());

    // Back to the log tab and it goes away again. Asserted on the ACTION's visibility
    // rather than on an internal flag, because that is the surface the requirement
    // names and the only one a reader can see.
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    tabs->setCurrentIndex(0);
    QVERIFY(!save->isVisible());
}

void TestConfigEditor::anEditedTabIsMarkedAndSavingClearsTheMark()
{
    const QString log = writeLog(QStringLiteral("mark.log"));
    const QString config = configPathFor(log, QStringLiteral("m.properties"));
    {
        QFile f(config);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("a=1\n");
    }
    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("m.properties")));
    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    const int index = tabs->currentIndex();
    auto *editor = qobject_cast<ConfigView *>(tabs->widget(index));
    QVERIFY(editor);

    const QString clean = tabs->tabText(index);
    QVERIFY(!clean.contains(QChar(0x2022)));

    // TYPED, not setPlainText(): QPlainTextEdit::setPlainText() resets the document's
    // modified flag, so a test written that way asserts the mark against a document that
    // says it was never edited — and passes only if the mark is wrong.
    QTextCursor typing = editor->editor()->textCursor();
    typing.movePosition(QTextCursor::End);
    typing.insertText(QStringLiteral("b=2\n"));
    // On the TAB TEXT, which is where the requirement says the mark lives.
    QVERIFY(tabs->tabText(index).contains(QChar(0x2022)));

    auto *save = w->findChild<QAction *>(QStringLiteral("saveConfigAction"));
    QVERIFY(save->isEnabled());
    save->trigger();
    QCOMPARE(tabs->tabText(index), clean);
    QVERIFY(!save->isEnabled()); // nothing left to save
}

void TestConfigEditor::savingWritesTheBytesAndPreservesPermissions()
{
    const QString log = writeLog(QStringLiteral("perm.log"));
    const QString config = configPathFor(log, QStringLiteral("p.properties"));
    {
        QFile f(config);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("a=1\n");
    }
    // A config that is deliberately NOT world-readable, which is the ordinary state of
    // one holding anything sensitive.
    const QFile::Permissions restricted =
        QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup;
    QVERIFY(QFile::setPermissions(config, restricted));

    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("p.properties")));
    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    auto *editor = qobject_cast<ConfigView *>(tabs->currentWidget());
    QTextCursor typing = editor->editor()->textCursor();
    typing.movePosition(QTextCursor::End);
    typing.insertText(QStringLiteral("b=3\n"));
    w->findChild<QAction *>(QStringLiteral("saveConfigAction"))->trigger();

    // THE BYTES ON DISK, never the widget's text, which is identical either way.
    QFile back(config);
    QVERIFY(back.open(QIODevice::ReadOnly));
    QCOMPARE(back.readAll(), QByteArray("a=1\nb=3\n"));
    back.close();

#ifndef Q_OS_WIN
    // The save is a temp-file-and-rename, so it creates a NEW inode: without an explicit
    // restore this file comes back 0644. A config that quietly became world-readable
    // because a viewer saved it is the worst thing this feature could do, and nothing on
    // screen would say it had happened.
    QCOMPARE(QFile::permissions(config) & (QFile::ReadOther | QFile::WriteOther),
             QFile::Permissions());
#endif
}

void TestConfigEditor::aMissingDirectoryIsRefusedByName()
{
    const QString log = writeLog(QStringLiteral("nodir.log"));
    std::unique_ptr<MainWindow> w(
        openWithConfig(log, QStringLiteral("no-such-dir/x.properties")));
    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    auto *editor = qobject_cast<ConfigView *>(tabs->currentWidget());
    QVERIFY(editor);

    QTextCursor typing = editor->editor()->textCursor();
    typing.insertText(QStringLiteral("a=1\n"));
    w->findChild<QAction *>(QStringLiteral("saveConfigAction"))->trigger();

    // BOTH halves. The reason has to NAME the directory, or the reader cannot act on it;
    // and the directory must still not exist, or the refusal was a refusal to report
    // rather than a refusal to create.
    auto *notice = editor->findChild<QLabel *>(QStringLiteral("configNotice"));
    QVERIFY(notice->isVisible());
    QVERIFY2(notice->text().contains(QStringLiteral("no-such-dir")),
             qPrintable(notice->text()));
    QVERIFY(!QDir(QFileInfo(log).absoluteDir().filePath(QStringLiteral("no-such-dir")))
                 .exists());
}

void TestConfigEditor::perLogActionsDoNotActOnTheLogBehindTheEditor()
{
    const QString log = writeLog(QStringLiteral("actions.log"));
    QVERIFY(QFile(configPathFor(log, QStringLiteral("a.properties"))).open(QIODevice::WriteOnly));
    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("a.properties")));
    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();

    // THE WIDEST RISK IN THE FEATURE. The bound document deliberately does NOT move when
    // an editor page comes forward — unbinding it would cost a per-log file write on every
    // tab flip — so `hasFile` stays true here. Every per-log action therefore has to ask a
    // different question, and one that forgets acts on a log the reader is not looking at:
    // Reload would re-read it, Copy would copy from it, New View would open a second view
    // of it, all with no visible connection to the tab in front.
    for (const char *name : {"copyAction", "copyColumnsAction", "selectAllAction",
                             "reloadAction", "newViewAction", "followAction",
                             "toggleWrapAction"}) {
        auto *a = w->findChild<QAction *>(QLatin1String(name));
        QVERIFY2(a, name);
        QVERIFY2(!a->isEnabled(), name);
    }

    // Closing the tab, by contrast, must STAY available — an editor page is closable.
    QVERIFY(w->findChild<QAction *>(QStringLiteral("closeTabAction"))->isEnabled());
}

void TestConfigEditor::findSearchesTheEditorAndNotTheLog()
{
    const QString log = writeLog(QStringLiteral("find.log"));
    const QString config = configPathFor(log, QStringLiteral("find.properties"));
    {
        QFile f(config);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("alpha=1\nbeta=2\nalpha=3\n");
    }
    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("find.properties")));
    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    auto *editor = qobject_cast<ConfigView *>(tabs->currentWidget());
    QVERIFY(editor);

    // Find stays ENABLED on an editor page — the one deliberate exception to the rule
    // the previous case pins, because a config editor searches too.
    QVERIFY(w->findChild<QAction *>(QStringLiteral("findAction"))->isEnabled());
    QVERIFY(w->findChild<QAction *>(QStringLiteral("findNextAction"))->isEnabled());

    auto *bar = editor->findBar();
    QVERIFY(bar);
    auto *edit = bar->findChild<QLineEdit *>(QStringLiteral("findEdit"));
    QVERIFY(edit);
    edit->setText(QStringLiteral("alpha"));

    // The bar has to be VISIBLE as well as answered: a report written into a hidden bar
    // satisfies status() perfectly and tells the reader nothing, which is exactly how
    // that bug shipped once already.
    QVERIFY(bar->isVisible());
    QVERIFY2(bar->status().contains(QStringLiteral("of 2")), qPrintable(bar->status()));
    QVERIFY(editor->editor()->textCursor().hasSelection());
    QCOMPARE(editor->editor()->textCursor().selectedText(), QStringLiteral("alpha"));
}

void TestConfigEditor::theSyntaxIsShownAndCanBeOverridden()
{
    const QString log = writeLog(QStringLiteral("syntax.log"));
    const QString config = configPathFor(log, QStringLiteral("s.properties"));
    {
        QFile f(config);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("log4cplus.rootLogger=DEBUG\n");
    }
    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("s.properties")));
    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    auto *editor = qobject_cast<ConfigView *>(tabs->currentWidget());
    QVERIFY(editor);

    // `.properties` is log4cplus's own extension, so it decides — no sniffing needed.
    QCOMPARE(editor->syntax(), ConfigSyntax::Ini);
    auto *source = editor->findChild<QLabel *>(QStringLiteral("configSyntaxSource"));
    QVERIFY(source);
    QVERIFY(!source->text().isEmpty()); // it SAYS where the choice came from

    // And it is one click from being changed. The claim is that the COMBO DRIVES THE
    // HIGHLIGHTER — asserting the combo merely has entries would guard nothing.
    auto *box = editor->findChild<QComboBox *>(QStringLiteral("configSyntax"));
    QVERIFY(box);
    box->setCurrentIndex(box->findData(int(ConfigSyntax::Xml)));
    QCOMPARE(editor->syntax(), ConfigSyntax::Xml);
    QVERIFY(editor->syntaxWasChosen());
}

void TestConfigEditor::closingAnEditorTabWorksAndDoesNotDisturbTheLogs()
{
    const QString log = writeLog(QStringLiteral("close.log"));
    QVERIFY(QFile(configPathFor(log, QStringLiteral("c.properties"))).open(QIODevice::WriteOnly));
    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("c.properties")));
    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QCOMPARE(tabs->count(), 2);

    // Ctrl+W on the editor page. Before the editor branch in closeViewAt() this did
    // NOTHING AT ALL — the qobject_cast to DocumentView failed and the function returned,
    // so the tab's own ✕ button was inert too, with nothing on screen to say why.
    w->findChild<QAction *>(QStringLiteral("closeTabAction"))->trigger();
    QCOMPARE(tabs->count(), 1);
    QVERIFY(qobject_cast<DocumentView *>(tabs->widget(0)));

    // Re-opening finds it again rather than being permanently gone.
    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    QCOMPARE(tabs->count(), 2);
}

void TestConfigEditor::anEditorTabComesBackInPlaceAfterARelaunch()
{
    const QString log = writeLog(QStringLiteral("restore.log"));
    const QString config = configPathFor(log, QStringLiteral("r.properties"));
    {
        QFile f(config);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("a=1\n");
    }

    {
        std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("r.properties")));
        w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
        auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
        QCOMPARE(tabs->count(), 2);
        // A real quit, so the session is written by the path that actually writes it.
        QCloseEvent e;
        QApplication::sendEvent(w.get(), &e);
        QVERIFY(e.isAccepted());
    }

    // The relaunch.
    std::unique_ptr<MainWindow> back(new MainWindow);
    back->show();
    auto *tabs = back->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 2);
    QVERIFY(qobject_cast<DocumentView *>(tabs->widget(0)));
    auto *editor = qobject_cast<ConfigView *>(tabs->widget(1));
    QVERIFY(editor);
    // The FILE is re-read from disk rather than restored from the session — a session is
    // not a backing store for unsaved work.
    QVERIFY(editor->editor()->toPlainText().contains(QStringLiteral("a=1")));
}

void TestConfigEditor::cancellingTheUnsavedPromptAbortsTheQuitAndWritesNoSession()
{
    const QString log = writeLog(QStringLiteral("cancel.log"));
    const QString config = configPathFor(log, QStringLiteral("c2.properties"));
    {
        QFile f(config);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("a=1\n");
    }
    std::unique_ptr<MainWindow> w(openWithConfig(log, QStringLiteral("c2.properties")));
    w->findChild<QAction *>(QStringLiteral("openConfigAction"))->trigger();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    auto *editor = qobject_cast<ConfigView *>(tabs->currentWidget());
    QVERIFY(editor);
    QTextCursor typing = editor->editor()->textCursor();
    typing.movePosition(QTextCursor::End);
    typing.insertText(QStringLiteral("b=2\n"));
    QVERIFY(editor->isModified());

    // Dismiss the modal with Escape, which the box maps to Cancel.
    QTimer::singleShot(0, [] {
        if (QWidget *modal = QApplication::activeModalWidget())
            QTest::keyClick(modal, Qt::Key_Escape);
    });
    QCloseEvent e;
    QApplication::sendEvent(w.get(), &e);

    // BOTH halves, and the second is the one that would go wrong silently. The event is
    // refused, so the window stays — and the session on disk must be UNTOUCHED, because
    // saveSession() does not merely write the session: it persists every context's
    // settings and flushes the per-log pool. A Cancel that ran after that would have
    // performed half a quit, and checking only that the window survived would pass
    // against exactly that bug.
    QVERIFY(!e.isAccepted());
    QSettings check;
    QVERIFY(!check.childGroups().contains(QStringLiteral("session"))
            || check.value(QStringLiteral("session/schemaVersion")).isNull());

    // The edits are still there to be saved.
    QVERIFY(editor->isModified());
}

// An address that cannot answer. TEST-NET-1 (RFC 5737) is reserved for documentation and
// is not routed anywhere, so a connect to it hangs rather than being refused — which is
// what makes it a connect worth proving we did not run.
// Guarded like its two callers: without SSH built in, both of them are a QSKIP and this
// is a function nobody names.
#if defined(LOFTAIL_HAVE_SSH)
static QString blackHoleConfig()
{
    return QStringLiteral("ssh://198.51.100.7/etc/log4cplus.properties");
}
#endif

void TestConfigEditor::aRemoteConfigPutsItsTabUpBeforeTheFarEndAnswers()
{
#if !defined(LOFTAIL_HAVE_SSH)
    QSKIP("SSH support is not built into this copy");
#else
    std::unique_ptr<MainWindow> w(new MainWindow);
    w->show();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(tabs);

    QElapsedTimer clock;
    clock.start();
    ConfigView *view = w->openConfigAt(blackHoleConfig());
    const qint64 took = clock.elapsed();

    // M17's rule, one level over: the tab is on screen BEFORE the far end answers. A
    // connect is up to twenty seconds and may stop to ask for a password, so running it
    // on this thread would freeze the window on a gesture that is supposed to open a tab.
    QVERIFY(view);
    QCOMPARE(tabs->count(), 1);
    QVERIFY2(took < 1000, qPrintable(QStringLiteral("opening blocked for %1 ms").arg(took)));

    // And it SAYS what it is doing. This assertion holds however the network behaves:
    // even a connect that fails instantly reports through a queued call, which cannot
    // have been delivered before this line runs.
    QVERIFY(view->isBusy());
    auto *notice = view->findChild<QLabel *>(QStringLiteral("configNotice"));
    QVERIFY(notice);
    QVERIFY(notice->isVisible());
    QVERIFY(!notice->text().isEmpty());

    // The text is not editable while there is nothing in it yet — typing into a buffer
    // about to be replaced by the file would throw the work away.
    QVERIFY(view->editor()->isReadOnly());
#endif
}

void TestConfigEditor::closingATabMidConnectDoesNotWaitForIt()
{
#if !defined(LOFTAIL_HAVE_SSH)
    QSKIP("SSH support is not built into this copy");
#else
    std::unique_ptr<MainWindow> w(new MainWindow);
    w->show();
    auto *tabs = w->findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(w->openConfigAt(blackHoleConfig()));
    QCOMPARE(tabs->count(), 1);

    // CLOSING A TAB IS ABANDONMENT, NOT A JOIN, and that is what keeps it instant.
    // Waiting for the worker here would make closing a tab on a host that is not
    // answering cost the whole connect timeout — and because that worker can be blocked
    // asking THIS thread for a password, the wait could be a deadlock rather than merely
    // slow. The transfer's destructor sets the abandon flag and aborts the session.
    QElapsedTimer clock;
    clock.start();
    w->findChild<QAction *>(QStringLiteral("closeTabAction"))->trigger();
    const qint64 took = clock.elapsed();

    QCOMPARE(tabs->count(), 0);
    QVERIFY2(took < 1000, qPrintable(QStringLiteral("closing waited %1 ms").arg(took)));

    // TEARING THE WINDOW DOWN IS A DIFFERENT CLAIM, and deliberately a weaker one. The
    // window drains on the way out — it waits, bounded, for the workers it abandoned —
    // because Qt's globals go with the application object and a worker still inside
    // QTcpSocket at that moment writes through a pointer that has just become null. That
    // is a real crash, caught by AddressSanitizer in CI, so the wait is the fix and not
    // an oversight.
    //
    // What is asserted is therefore that the teardown is BOUNDED, not that it is
    // instant: it must not wait out the connect. The bound is the drain budget plus
    // slack, and it is nowhere near the 20 s a connect is allowed. A worker can genuinely
    // take a couple of seconds to notice — Qt looks up the system proxy inside
    // connectToHost(), before anything of loftail's is consulted again.
    for (int round = 0; round < 3; ++round) {
        std::unique_ptr<MainWindow> victim(new MainWindow);
        victim->show();
        for (int i = 0; i < 2; ++i) {
            QVERIFY(victim->openConfigAt(
                QStringLiteral("ssh://198.51.100.%1/etc/x.properties").arg(10 + i)));
        }
        QCoreApplication::processEvents();
        clock.restart();
        victim.reset();
        QVERIFY2(clock.elapsed() < 6000,
                 qPrintable(QStringLiteral("shutdown waited %1 ms").arg(clock.elapsed())));
    }
#endif
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-configeditor"));

    TestConfigEditor tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_configeditor.moc"
