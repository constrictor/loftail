#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>

#include "MainWindow.h"
#include "MnemonicCheck.h"
#include "PreferencesDialog.h"

using namespace loftail;

// The Preferences dialog (SPEC.md §4): the settings tree on the left, the selected
// node's settings on the right. What the tree DECIDES is tst_logsettings' job and what
// it then does on an open is tst_openflow's, because only the real MainWindow can show
// that no dialog appeared. What is pinned here is the dialog's own contract — it applies
// NOTHING, so Cancel is exact — and the things only a dialog can get wrong: which
// editors a node offers, which buttons a node enables, that a rebuild reselects the NODE
// rather than the row it used to occupy, and that the file level lists the log that is
// open and nothing else.
//
// Constructed on the stack and never exec()'d: a modal dialog in a test is a nested
// event loop looking for somewhere to deadlock. Children are reached by OBJECT NAME,
// never by visible text.
class TestPreferences : public QObject
{
    Q_OBJECT

private:
    // A log address that is absolute on EVERY platform. "/var/log/app.log" is absolute
    // on POSIX and is not on Windows, where a path with no drive is resolved against the
    // current one — so logSettingsKey(), which keys a file node by its absolute path,
    // stores "D:/var/log/app.log" and every expectation spelled the POSIX way misses by
    // a drive letter. QDir::rootPath() supplies whatever "the root" is called here, and
    // the result is already absolute, so the key comes back exactly as it went in.
    static QString logPath(const QString &name)
    {
        return QDir::rootPath() + QStringLiteral("var/log/") + name;
    }

    static QByteArray sample()
    {
        return "2026-08-05 00:00:01,000 [t0] INFO  logger.a - first\n"
               "2026-08-05 00:00:02,000 [t1] WARN  logger.b - second\n";
    }

    // The two INHERITED levels: distinguishable defaults and one pattern that claims
    // `*.log` and nothing else. A log's OWN settings are no longer in here at all (M21) —
    // they are one record per log in the pool, handed to the dialog through selectLog()
    // and taken back through fileProfile(), which is why the two helpers below exist.
    static LogSettingsTree populated()
    {
        LogSettingsTree t;
        LogProfile root;
        root.format.pattern = QStringLiteral("ROOT");
        t.setDefaults(root);

        LogPatternNode n;
        n.match = QStringLiteral("*.log");
        n.profile.format.pattern = QStringLiteral("PATTERN");
        t.addPattern(n);
        return t;
    }

    static LogProfile profileWith(const QString &pattern)
    {
        LogProfile p;
        p.format.pattern = pattern;
        return p;
    }

    static QTreeWidget *treeOf(PreferencesDialog &dlg)
    {
        return dlg.findChild<QTreeWidget *>(QStringLiteral("settingsTree"));
    }

    // What a row is OF, with the display decoration taken back off: a pattern row wears
    // quotes so that it reads as a pattern, and may carry a "(regex)" or "(whole path)"
    // note after them. None of that is identity, and matching on it would tie two dozen
    // lookups here to a punctuation choice in the dialog.
    static QString rowIdentity(QTreeWidgetItem *item)
    {
        const QString text = item->text(0);
        if (!text.startsWith(QLatin1Char('"')))
            return text;
        const int close = text.lastIndexOf(QLatin1Char('"'));
        return close > 0 ? text.mid(1, close - 1) : text;
    }

    // Depth-first walk for the row whose label is `text`. A tree ROW has no object name
    // to be found by — it is data, not a widget — so the label is the only handle it
    // has; every widget around it is still found by object name.
    static QTreeWidgetItem *rowNamed(QTreeWidget *tree, const QString &text)
    {
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if (rowIdentity(*it) == text)
                return *it;
        }
        return nullptr;
    }

    // The one per-log row, found by what it POINTS AT rather than by what it says. Its
    // label is translated prose ("Current file") and its tooltip is itself asserted on
    // elsewhere in this file, so neither can be the handle; the NodeRef the dialog keeps
    // in Qt::UserRole is the row's identity and is what the dialog itself reselects by.
    static QTreeWidgetItem *currentFileRow(QTreeWidget *tree)
    {
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString().startsWith(QLatin1String("file:")))
                return *it;
        }
        return nullptr;
    }

    // The address that row stands for, straight out of its ref — so a test can say WHICH
    // log is listed without going through the label or the tooltip.
    static QString rowAddress(QTreeWidgetItem *item)
    {
        return item ? item->data(0, Qt::UserRole).toString().mid(5) : QString();
    }

    // What File ▸ Preferences does: open the dialog on a log that has nothing stored, so
    // the row shows what it inherits. showPreferences()'s call verbatim.
    static void openOn(PreferencesDialog &dlg, const QString &address)
    {
        dlg.selectLog(address, std::nullopt, dlg.tree().inherited(address));
    }

    // The same for a log that HAS settings of its own — what the pool would have handed
    // back. The seed is unreachable in this case and is passed as the inherited value to
    // say so.
    static void openOnStored(PreferencesDialog &dlg, const QString &address,
                             const LogProfile &own)
    {
        dlg.selectLog(address, own, dlg.tree().inherited(address));
    }

private slots:
    void theTreeShowsThreeLevels();
    void onlyTheLogThatIsOpenGetsARow();
    void withNoLogOpenTheTreeIsPatternsOnly();
    void theCurrentFileRowIsNamedForItsRoleAndThePanelForTheLog();
    void aLogNoPatternMatchesSitsUnderTheDefaultsAndSaysWhyItCannotBePromoted();
    void theOpenLogsEntrySurvivesTheSweepUntilOk();
    void deletingTheCurrentFileEntryLeavesTheRowShowingWhatItInherits();
    void thePatternEditorIsShownOnlyForAPattern();
    void editingAPatternRehomesItsLogsAndKeepsTheSelection();
    void reorderingChangesWhichPatternWins();
    void everyMnemonicHasABuddyAndNoLetterIsClaimedTwice();
    void aNewPatternStartsEmptyAndClaimsNoLogs();
    void promotingIsOfferedOnlyWithAParentPattern();
    void promotingAPatternMovesItsSettingsIntoTheDefaults();
    void promotingMovesTheSettingsUpAndRemovesTheLogEntry();
    void aLogEntryGoesWhenItsPatternCatchesUpWithIt();
    void aScratchNodeSayingNothingNewIsNotKept();
    void applyToCurrentIsReportedNeverApplied();
    void askingToApplyLeavesTheDialogOpenAndSaysWhatIsStillToHappen();
    void askingAgainOnTheSameEntryWithdrawsTheRequest();
    void theSettingsAppliedAreWhatTheEntrySaysWhenOkIsPressed();
    void everyProfileFieldRoundTripsThroughTheEditor();
    void emptySampleIsHarmless();
    void theEmptyPreviewSaysSoOnTheTableAndTheMatchCountBelowIt();
    void theMenuEntryHasAnAcceleratorOnEveryPlatform();
    void thePreviewKeepsTheFormatSectionsMargins();
    void aPatternRowIsQuotedAndALogRowIsNot();
    void eachLevelIsHeadedByItsOwnNameWithTheLevelUnderIt();
    void aPatternHeadingFollowsTheFieldAsItIsTyped();
    void whatTheNodeIsSitsAboveTheHeadingWithARuleBetweenThem();
    void bothCaptionsAreBoldAndCentred();
    void anEmptyPatternIsNotReportedAsAnErrorAndARealOneFitsItsRow();
    void noMessageFitsInLessRoomThanItsTextNeeds();
    void theDetectedEncodingIsReportedOnlyForTheLogItRead();
    void theTwoPanesKeepAGapBetweenThem();
    void theTreePaneOpensWideEnoughForItsLongestRow();
    void oneEnormousPatternDoesNotHandTheTreeTheDialog();
    void aSplitAlreadySettledIsNotRecomputed();
    void everyTreeRowSaysItsWholeSelfOnHover();
    void enterFinishesAFieldWithoutClosingTheDialog();
};

void TestPreferences::theTreeShowsThreeLevels()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOn(dlg, logPath(QStringLiteral("app.log")));
    QTreeWidget *tree = treeOf(dlg);
    QVERIFY(tree);

    QCOMPARE(tree->topLevelItemCount(), 1); // one root: the defaults
    QTreeWidgetItem *root = tree->topLevelItem(0);
    QTreeWidgetItem *pattern = rowNamed(tree, QStringLiteral("*.log"));
    QVERIFY(pattern);
    QCOMPARE(pattern->parent(), root);

    QTreeWidgetItem *log = currentFileRow(tree);
    QVERIFY(log);
    QCOMPARE(log->parent(), pattern);
}

// THE HEADLINE CONTRACT. The file level used to list every entry in the store, which is a
// list of logs nobody is looking at; it now lists the one the dialog was opened on. The
// others are HIDDEN and not deleted — still in tree(), still resolving to what they say —
// so the only thing lost is reaching another log's entry without opening that log.
void TestPreferences::onlyTheLogThatIsOpenGetsARow()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOn(dlg, logPath(QStringLiteral("app.log")));
    QTreeWidget *tree = treeOf(dlg);

    int fileRows = 0;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString().startsWith(QLatin1String("file:")))
            ++fileRows;
    }
    QCOMPARE(fileRows, 1);
    QCOMPARE(rowAddress(currentFileRow(tree)), logPath(QStringLiteral("app.log")));

    // Every other log's settings are in the pool and this dialog cannot reach them at
    // all (M21) — which is a stronger form of the same guarantee than the one this case
    // used to make, when they were in the working copy and merely not drawn. What it
    // hands back is one log's settings and the two inherited levels, and nothing else.
    dlg.accept();
    QCOMPARE(dlg.tree().patterns().size(), 1);
    QCOMPARE(dlg.tree().defaults().format.pattern, QStringLiteral("ROOT"));
}

// Preferences is reachable from an empty window — which is when somebody most wants to
// set the defaults up — and then there is no current file, so there is no row for one.
void TestPreferences::withNoLogOpenTheTreeIsPatternsOnly()
{
    PreferencesDialog dlg(populated(), QString(), QByteArray());
    QTreeWidget *tree = treeOf(dlg);

    QVERIFY(!currentFileRow(tree));
    QCOMPARE(tree->topLevelItem(0)->childCount(), 1); // the pattern, and nothing else
    // And with no log named there is nothing for a log to have stored either.
    QVERIFY(!dlg.fileProfile().has_value());
}

// The row is named for its ROLE and the panel for the LOG, deliberately: with one row
// there is nothing to tell it apart from, so a file name would be a shorter way of
// saying "the file" — while the panel is where the reader checks WHICH log they are
// about to change, and the address is a hover away from both.
void TestPreferences::theCurrentFileRowIsNamedForItsRoleAndThePanelForTheLog()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOn(dlg, logPath(QStringLiteral("app.log")));
    QTreeWidget *tree = treeOf(dlg);

    QTreeWidgetItem *row = currentFileRow(tree);
    QVERIFY(row);
    QVERIFY2(row->text(0) != QStringLiteral("app.log"),
             "the row still names the log rather than its role");
    QCOMPARE(row->toolTip(0), logPath(QStringLiteral("app.log")));

    auto *name = dlg.findChild<QLabel *>(QStringLiteral("nodeNameLabel"));
    auto *level = dlg.findChild<QLabel *>(QStringLiteral("nodeLevelLabel"));
    QVERIFY(name);
    QVERIFY(level);
    QCOMPARE(name->text(), QStringLiteral("app.log"));
    QVERIFY(!level->text().isEmpty());
    QCOMPARE(name->toolTip(), logPath(QStringLiteral("app.log")));
}

// The virtual "Logs with no matching pattern" parent is gone: it was a grouping row, and
// one row does not need grouping. The absence of a match is the row's POSITION now — it
// hangs under the defaults, which is the level it inherits from. What that heading used
// to explain has to survive it, so the panel says it where the reader is already looking.
void TestPreferences::aLogNoPatternMatchesSitsUnderTheDefaultsAndSaysWhyItCannotBePromoted()
{
    PreferencesDialog dlg(populated(), QStringLiteral("other.trace"), sample());
    openOn(dlg, logPath(QStringLiteral("other.trace")));
    QTreeWidget *tree = treeOf(dlg);

    QTreeWidgetItem *row = currentFileRow(tree);
    QVERIFY(row);
    QCOMPARE(row->parent(), tree->topLevelItem(0));

    auto *promote = dlg.findChild<QPushButton *>(QStringLiteral("promoteToParentButton"));
    QVERIFY(promote);
    QVERIFY2(!promote->isEnabled(),
             "a log with no pattern was offered a promotion to a pattern that is not there");

    auto *title = dlg.findChild<QLabel *>(QStringLiteral("nodeTitleLabel"));
    QVERIFY(title);
    QVERIFY2(!title->isHidden() && !title->text().isEmpty(),
             "nothing on screen said why the promotion is refused");

    // A log the pattern DOES claim says nothing extra: it has a level above it, and the
    // heading would be a sentence about a case that is not this one.
    PreferencesDialog claimed(populated(), QStringLiteral("app.log"), sample());
    openOn(claimed, logPath(QStringLiteral("app.log")));
    QVERIFY(claimed.findChild<QLabel *>(QStringLiteral("nodeTitleLabel"))->text().isEmpty());
}

// THE ROW IS A FACT, not something a rebuild has to keep putting back. A log whose stored
// settings happen to agree with the pattern above it is exactly the node the old sweep ate,
// which left the reader looking at no row at all for the log in front of them; there is
// nothing to sweep here now, so the row cannot go — and OK still stores nothing, because
// what it says is what the log inherits anyway.
void TestPreferences::theOpenLogsEntrySurvivesTheSweepUntilOk()
{
    PreferencesDialog dlg(populated(), QStringLiteral("agrees.log"), sample());
    // Stored settings that say exactly what *.log says.
    openOnStored(dlg, logPath(QStringLiteral("agrees.log")),
                 profileWith(QStringLiteral("PATTERN")));
    QTreeWidget *tree = treeOf(dlg);

    QTreeWidgetItem *row = currentFileRow(tree);
    QVERIFY2(row, "the open log's row was swept away before it could be edited");
    QCOMPARE(rowAddress(row), logPath(QStringLiteral("agrees.log")));

    // And it survives a rebuild provoked by an unrelated mutation, which is where the
    // sweep used to run.
    dlg.findChild<QToolButton *>(QStringLiteral("addPatternButton"))->click();
    QVERIFY2(currentFileRow(tree), "the open log's row went on the next rebuild");

    dlg.accept();
    QVERIFY(!dlg.fileProfile().has_value());
}

// Delete on the one file row does not take the row away — it takes away what the log said
// of its OWN, leaving it following what it inherits. The row is the whole file level of
// this tree, so removing it would take the only route to the open log's settings off the
// screen for the rest of the visit.
void TestPreferences::deletingTheCurrentFileEntryLeavesTheRowShowingWhatItInherits()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOnStored(dlg, logPath(QStringLiteral("app.log")),
                 profileWith(QStringLiteral("MINE")));
    QTreeWidget *tree = treeOf(dlg);
    auto *format = dlg.findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
    QVERIFY(format);
    QCOMPARE(format->text(), QStringLiteral("MINE")); // its own, to start with

    dlg.findChild<QToolButton *>(QStringLiteral("deleteNodeButton"))->click();

    QTreeWidgetItem *row = currentFileRow(tree);
    QVERIFY2(row, "deleting the entry took the current file's row with it");
    QCOMPARE(tree->currentItem(), row);
    QCOMPARE(format->text(), QStringLiteral("PATTERN")); // what *.log gives it now

    // Untouched afterwards, it says nothing its parent does not, so OK keeps nothing.
    dlg.accept();
    QVERIFY(!dlg.fileProfile().has_value());
    QCOMPARE(dlg.tree().inherited(logPath(QStringLiteral("app.log"))).format.pattern,
             QStringLiteral("PATTERN"));
}

void TestPreferences::thePatternEditorIsShownOnlyForAPattern()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOn(dlg, logPath(QStringLiteral("app.log")));
    QTreeWidget *tree = treeOf(dlg);
    auto *group = dlg.findChild<QWidget *>(QStringLiteral("patternGroup"));
    // The GROUP, not a field inside it: what is shown and hidden per node is the whole
    // block, and a child of a hidden parent is not itself "hidden" as Qt uses the word.
    auto *identity = dlg.findChild<QWidget *>(QStringLiteral("nodeIdentityGroup"));
    auto *name = dlg.findChild<QLabel *>(QStringLiteral("nodeNameLabel"));
    QVERIFY(group);
    QVERIFY(identity);
    QVERIFY(name);

    tree->setCurrentItem(tree->topLevelItem(0)); // the root
    QVERIFY(group->isHidden());
    QVERIFY(!identity->isHidden()); // every level names itself, the defaults included

    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    QVERIFY(!group->isHidden());

    tree->setCurrentItem(currentFileRow(tree));
    QVERIFY(group->isHidden());
    QVERIFY(!identity->isHidden());
    // The panel names the LOG even where the row names its role — see
    // theCurrentFileRowIsNamedForItsRoleAndThePanelForTheLog.
    QCOMPARE(name->text(), QStringLiteral("app.log"));
}

void TestPreferences::eachLevelIsHeadedByItsOwnNameWithTheLevelUnderIt()
{
    // "Concrete file" was the heading and the log's address was the muted line under it
    // — the model's word for a level, in the one place the panel says which node it is
    // showing, over the one thing that actually identifies it. So: the NAME heads the
    // panel and the LEVEL is the muted line, on all three, because a heading that says
    // the same three words whatever is selected identifies nothing.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOn(dlg, logPath(QStringLiteral("app.log")));
    QTreeWidget *tree = treeOf(dlg);
    auto *name = dlg.findChild<QLabel *>(QStringLiteral("nodeNameLabel"));
    auto *level = dlg.findChild<QLabel *>(QStringLiteral("nodeLevelLabel"));
    QVERIFY(name);
    QVERIFY(level);

    tree->setCurrentItem(tree->topLevelItem(0));
    QCOMPARE(name->text(), QStringLiteral("Default settings"));
    QVERIFY(!level->text().isEmpty());
    QVERIFY2(!level->text().contains(name->text()), qPrintable(level->text()));

    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    QVERIFY2(name->text().contains(QLatin1String("*.log")),
             qPrintable(QStringLiteral("a pattern node is headed %1").arg(name->text())));
    QCOMPARE(level->text(), QStringLiteral("File pattern"));

    // The log's readable name, not its address — and the address a hover away, on both
    // lines, since either is what a pointer aimed at the heading lands on.
    tree->setCurrentItem(currentFileRow(tree));
    QCOMPARE(name->text(), QStringLiteral("app.log"));
    QCOMPARE(level->text(), QStringLiteral("This log only"));
    QCOMPARE(name->toolTip(), logPath(QStringLiteral("app.log")));
    QCOMPARE(level->toolTip(), logPath(QStringLiteral("app.log")));

    // And nothing to hover on the levels that name no single log: a tooltip repeating
    // the heading is a tooltip that says nothing.
    tree->setCurrentItem(tree->topLevelItem(0));
    QVERIFY(name->toolTip().isEmpty());
}

void TestPreferences::aPatternHeadingFollowsTheFieldAsItIsTyped()
{
    // The heading names the node the reader is looking at. The tree row waits for the
    // commit, because that is what re-homes the logs under it; the heading must not,
    // or it names the pattern as it was three keystrokes ago.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    auto *name = dlg.findChild<QLabel *>(QStringLiteral("nodeNameLabel"));
    auto *match = dlg.findChild<QLineEdit *>(QStringLiteral("patternMatchEdit"));
    QVERIFY(name);
    QVERIFY(match);

    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    match->setText(QStringLiteral("*.audit.log"));
    QVERIFY2(name->text().contains(QLatin1String("*.audit.log")), qPrintable(name->text()));

    // An empty pattern is prose about a pattern that says nothing yet, in the heading
    // exactly as in the row: a pair of quotes round nothing is a name that is not one.
    match->setText(QString());
    QVERIFY2(!name->text().contains(QLatin1Char('"')), qPrintable(name->text()));
    QVERIFY(!name->text().isEmpty());
}

void TestPreferences::editingAPatternRehomesItsLogsAndKeepsTheSelection()
{
    // Opened on other.trace, the log the edited pattern is about to CLAIM: it is the one
    // that gets a row, so it is the one whose re-homing can be seen in the widget.
    PreferencesDialog dlg(populated(), QStringLiteral("other.trace"), sample());
    openOn(dlg, logPath(QStringLiteral("other.trace")));
    QTreeWidget *tree = treeOf(dlg);
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));

    auto *match = dlg.findChild<QLineEdit *>(QStringLiteral("patternMatchEdit"));
    QVERIFY(match);
    match->setText(QStringLiteral("*.trace"));
    emit match->editingFinished();

    // app.log no longer matches and other.trace now does — with no stored parent link to
    // update anywhere, because a log's parent is DERIVED by running the matcher.
    QTreeWidgetItem *pattern = rowNamed(tree, QStringLiteral("*.trace"));
    QVERIFY(pattern);
    // The open log is other.trace here, so the re-homing is visible in the widget; what
    // happened to app.log is a MODEL claim, since only one log is ever listed.
    QCOMPARE(currentFileRow(tree)->parent(), pattern);
    QCOMPARE(dlg.tree().matchingPattern(logPath(QStringLiteral("app.log"))), -1);

    // The rebuild reselected the NODE, not the row it used to occupy.
    QCOMPARE(tree->currentItem(), pattern);
    QCOMPARE(match->text(), QStringLiteral("*.trace"));
}

void TestPreferences::reorderingChangesWhichPatternWins()
{
    LogSettingsTree t;
    LogPatternNode first;
    first.match = QStringLiteral("app.*");
    first.profile.format.pattern = QStringLiteral("FIRST");
    t.addPattern(first);
    LogPatternNode second;
    second.match = QStringLiteral("*.log");
    second.profile.format.pattern = QStringLiteral("SECOND");
    t.addPattern(second);

    PreferencesDialog dlg(t, QString(), QByteArray());
    QTreeWidget *tree = treeOf(dlg);
    auto *down = dlg.findChild<QToolButton *>(QStringLiteral("moveDownButton"));
    auto *up = dlg.findChild<QToolButton *>(QStringLiteral("moveUpButton"));
    QVERIFY(down);
    QVERIFY(up);

    tree->setCurrentItem(rowNamed(tree, QStringLiteral("app.*")));
    QVERIFY(!up->isEnabled()); // already first
    QVERIFY(down->isEnabled());
    down->click();

    QCOMPARE(dlg.tree().patterns().at(0).match, QStringLiteral("*.log"));
    QCOMPARE(dlg.tree().inherited(logPath(QStringLiteral("app.log"))).format.pattern,
             QStringLiteral("SECOND"));
    // Still on the pattern that moved, not on whatever now occupies its old row.
    QCOMPARE(tree->currentItem(), rowNamed(tree, QStringLiteral("app.*")));
}

// Mnemonics over the whole dialog: none dead, none shared (MnemonicCheck.h explains
// both). Three rows of the format editor read "&Encoding:", "&Source time zone:" and
// "&Conversion pattern:" on screen before this, because QFormLayout's QLayout-field
// overload cannot give its label a buddy. Checked over the dialog rather than on those
// three, because the trap is the overload and not the rows.
void TestPreferences::everyMnemonicHasABuddyAndNoLetterIsClaimedTwice()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    dlg.setApplyTarget(QStringLiteral("app.log")); // the one mnemonic taken from data
    // A pattern node, so the pattern editor is on screen with the rest.
    treeOf(dlg)->setCurrentItem(rowNamed(treeOf(dlg), QStringLiteral("*.log")));

    QString why;
    // Five, not six: "&Forget Individual Files" is gone with the bulk command, and F
    // with it. The count is a MINIMUM, so it is what stops a mnemonic quietly going
    // missing rather than a tally of what is on screen.
    QVERIFY2(loftail_test::mnemonicsAreSound(&dlg, &why, 5), qPrintable(why));
}

// Add Pattern creates a row that matches NOTHING until it is named. It used to seed the
// match — "*.log" from a constant, or from the selected log's extension — which claimed
// every log on the machine the moment the row appeared, and handed them either the
// defaults or that one log's settings while the user was still deciding what the pattern
// was for. The PROFILE seeding is the useful half and stays.
void TestPreferences::aNewPatternStartsEmptyAndClaimsNoLogs()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOnStored(dlg, logPath(QStringLiteral("app.log")),
                 profileWith(QStringLiteral("MINE")));
    QTreeWidget *tree = treeOf(dlg);
    auto *add = dlg.findChild<QToolButton *>(QStringLiteral("addPatternButton"));
    auto *match = dlg.findChild<QLineEdit *>(QStringLiteral("patternMatchEdit"));
    QVERIFY(add);
    QVERIFY(match);

    // From a selected log, which is where a derived "*.log" used to come from.
    tree->setCurrentItem(currentFileRow(tree));
    add->click();

    QCOMPARE(dlg.tree().patterns().size(), 2);
    const LogPatternNode &added = dlg.tree().patterns().at(1);
    QVERIFY2(added.match.isEmpty(), "a new pattern arrived already matching something");
    QVERIFY(match->text().isEmpty());
    // Seeded from the log it was added from: that is what makes it worth adding there.
    QCOMPARE(added.profile.format.pattern, QStringLiteral("MINE"));

    // Nothing moved under it — a log matching nothing still matches nothing, and the
    // existing pattern still owns its logs.
    QCOMPARE(dlg.tree().matchingPattern(logPath(QStringLiteral("other.trace"))), -1);
    QCOMPARE(dlg.tree().inherited(QStringLiteral("/etc/never/seen.log")).format.pattern,
             QStringLiteral("PATTERN")); // *.log, the pattern that was already there

    // And from a non-file node, which is where the "*.log" constant used to come from.
    tree->setCurrentItem(tree->topLevelItem(0));
    add->click();
    QCOMPARE(dlg.tree().patterns().size(), 3);
    QVERIFY(dlg.tree().patterns().at(2).match.isEmpty());
}

void TestPreferences::promotingIsOfferedOnlyWithAParentPattern()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOn(dlg, logPath(QStringLiteral("app.log")));
    QTreeWidget *tree = treeOf(dlg);
    auto *promote = dlg.findChild<QPushButton *>(QStringLiteral("promoteToParentButton"));
    QVERIFY(promote);

    tree->setCurrentItem(currentFileRow(tree));
    QVERIFY(promote->isEnabled());

    // A pattern's parent is the defaults, so it promotes too — and the button says which
    // level it is aimed at, because "parent" is a different thing one row up.
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    QVERIFY(promote->isEnabled());
    QVERIFY(promote->text().contains(QStringLiteral("Default")));
    tree->setCurrentItem(currentFileRow(tree));
    QVERIFY(promote->text().contains(QStringLiteral("Pattern")));

    // The defaults are the top: nothing above them to promote to.
    tree->setCurrentItem(tree->topLevelItem(0));
    QVERIFY(!promote->isEnabled());

    // A log no pattern claims has nothing above it but the defaults, and handing one
    // log's settings to every log in the world is not a level up but a level skipped.
    // Its own case, since only one log is listed at a time —
    // aLogNoPatternMatchesSitsUnderTheDefaultsAndSaysWhyItCannotBePromoted carries the
    // rest of what that state has to say.
    PreferencesDialog unmatched(populated(), QStringLiteral("other.trace"), sample());
    openOn(unmatched, logPath(QStringLiteral("other.trace")));
    treeOf(unmatched)->setCurrentItem(currentFileRow(treeOf(unmatched)));
    QVERIFY(!unmatched.findChild<QPushButton *>(QStringLiteral("promoteToParentButton"))
                 ->isEnabled());
}

// A pattern promotes its settings to the DEFAULTS, and stays where it is. It is not only
// settings but a matcher, and its place in the order is what keeps a later pattern off
// its logs — so unlike a promoted file node it is not removed for having nothing left to
// say. (SPEC.md §4.)
void TestPreferences::promotingAPatternMovesItsSettingsIntoTheDefaults()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    dlg.findChild<QPushButton *>(QStringLiteral("promoteToParentButton"))->click();

    const LogSettingsTree &out = dlg.tree();
    QCOMPARE(out.defaults().format.pattern, QStringLiteral("PATTERN"));
    // Still there, still first, still matching what it matched.
    QCOMPARE(out.patterns().size(), 1);
    QCOMPARE(out.patterns().at(0).match, QStringLiteral("*.log"));
    QVERIFY(rowNamed(tree, QStringLiteral("*.log")));
    QCOMPARE(tree->currentItem(), rowNamed(tree, QStringLiteral("*.log")));

    // A log that matched nothing now inherits what the pattern said.
    QCOMPARE(out.inherited(QStringLiteral("/etc/never/seen.trace")).format.pattern,
             QStringLiteral("PATTERN"));
    // And a log the pattern claims is untouched: it inherits from the pattern, which has
    // not changed, so the caller's sweep finds nothing to do under it either.
    QCOMPARE(out.inherited(logPath(QStringLiteral("app.log"))).format.pattern,
             QStringLiteral("PATTERN"));
}

void TestPreferences::promotingMovesTheSettingsUpAndRemovesTheLogEntry()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOnStored(dlg, logPath(QStringLiteral("app.log")),
                 profileWith(QStringLiteral("MINE")));
    QTreeWidget *tree = treeOf(dlg);
    tree->setCurrentItem(currentFileRow(tree));
    dlg.findChild<QPushButton *>(QStringLiteral("promoteToParentButton"))->click();

    QCOMPARE(dlg.tree().patterns().at(0).profile.format.pattern, QStringLiteral("MINE"));
    // The log now says exactly what the pattern says, so its own entry has nothing left
    // to say — and it goes, rather than staying as a duplicate that would stop tracking
    // the pattern the moment the pattern changed. The ROW stays, showing the promoted
    // settings, which is the same thing the log now inherits: promoting is not a way of
    // taking the log off the screen.
    QTreeWidgetItem *row = currentFileRow(tree);
    QVERIFY2(row, "promoting took the open log's row away");
    QCOMPARE(rowAddress(row), logPath(QStringLiteral("app.log")));

    dlg.accept();
    QVERIFY2(!dlg.fileProfile().has_value(),
             "the promoted log kept a copy of what its pattern now says");
    QCOMPARE(dlg.tree().inherited(logPath(QStringLiteral("app.log"))).format.pattern,
             QStringLiteral("MINE"));
}

// A per-log entry lasts only as long as it says something its parent does not, and the
// half a WRITE cannot see is the one where the PARENT moved: nothing writes those records,
// so before the rule existed they sat under the pattern shadowing it for ever — the pattern
// was still editable and its logs no longer followed it.
//
// The OPEN LOG's half is here, where the dialog can answer it; every other log's is
// LogFileStore::pruneAgainst(), which MainWindow drives once per visit
// (tst_logfilestore::aPatternTaughtWhatItsLogsSaidLeavesThemNothingToSay).
void TestPreferences::aLogEntryGoesWhenItsPatternCatchesUpWithIt()
{
    // Redundant on arrival: settings written before the pattern that now covers them.
    PreferencesDialog opened(populated(), QStringLiteral("agrees.log"), sample());
    openOnStored(opened, logPath(QStringLiteral("agrees.log")),
                 profileWith(QStringLiteral("PATTERN")));
    QVERIFY2(!opened.fileProfile().has_value(),
             "a log whose stored settings already agreed with its pattern kept them");

    // Taught mid-dialog, which is the gesture this is really about: give the pattern what
    // the log's own entry said, and the entry has nothing left to say.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOnStored(dlg, logPath(QStringLiteral("app.log")), profileWith(QStringLiteral("MINE")));
    QTreeWidget *tree = treeOf(dlg);
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    auto *format = dlg.findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
    auto *match = dlg.findChild<QLineEdit *>(QStringLiteral("patternMatchEdit"));
    QVERIFY(format);
    QVERIFY(match);
    format->setText(QStringLiteral("MINE"));
    emit match->editingFinished(); // any commit-and-rebuild gesture

    QVERIFY(!dlg.fileProfile().has_value());
    // It opens on exactly what it opened on — through the pattern it now follows.
    QCOMPARE(dlg.tree().inherited(logPath(QStringLiteral("app.log"))).format.pattern,
             QStringLiteral("MINE"));

    // A log under NO pattern still differs from the defaults, so it stays.
    PreferencesDialog orphan(populated(), QStringLiteral("other.trace"), sample());
    openOnStored(orphan, logPath(QStringLiteral("other.trace")),
                 profileWith(QStringLiteral("ORPHAN")));
    QVERIFY(orphan.fileProfile().has_value());

    // And on OK, for an edit that never provoked a rebuild: commitCurrent() writes the
    // panel back, so the comparison has to see the last keystroke.
    PreferencesDialog closed(populated(), QStringLiteral("app.log"), sample());
    openOnStored(closed, logPath(QStringLiteral("app.log")),
                 profileWith(QStringLiteral("MINE")));
    treeOf(closed)->setCurrentItem(rowNamed(treeOf(closed), QStringLiteral("*.log")));
    closed.findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"))
        ->setText(QStringLiteral("MINE"));
    closed.accept();
    QVERIFY(!closed.fileProfile().has_value());
    QCOMPARE(closed.tree().inherited(logPath(QStringLiteral("app.log"))).format.pattern,
             QStringLiteral("MINE"));
}

void TestPreferences::aScratchNodeSayingNothingNewIsNotKept()
{
    // A log with nothing stored still gets a row, so there is something to select and
    // edit. If the user leaves it saying what the log already inherits, OK must store
    // nothing — otherwise merely being ASKED about a log leaves an entry behind for ever.
    const LogSettingsTree base = populated();
    const LogProfile inherited = base.inherited(logPath(QStringLiteral("fresh.log")));

    PreferencesDialog kept(base, QStringLiteral("fresh.log"), sample());
    kept.selectLog(logPath(QStringLiteral("fresh.log")), std::nullopt, inherited);
    // The ROW, not merely a current item: the sweep every rebuild used to run would eat
    // such a node the moment it was created — it says nothing new, which is exactly the
    // state it is meant to be edited out of — and the selection would fall back to the
    // root, which is a current item too.
    QTreeWidgetItem *scratch = currentFileRow(treeOf(kept));
    QVERIFY2(scratch, "the log with nothing stored was given no row to be edited in");
    QCOMPARE(rowAddress(scratch), logPath(QStringLiteral("fresh.log")));
    QCOMPARE(treeOf(kept)->currentItem(), scratch);
    kept.accept();
    QVERIFY(!kept.fileProfile().has_value());

    // Changed, and it is stored.
    PreferencesDialog edited(base, QStringLiteral("fresh.log"), sample());
    LogProfile changed = inherited;
    changed.format.pattern = QStringLiteral("SOMETHING ELSE");
    edited.selectLog(logPath(QStringLiteral("fresh.log")), std::nullopt, changed);
    edited.accept();
    QVERIFY(edited.fileProfile().has_value());
    QCOMPARE(edited.fileProfile()->format.pattern, QStringLiteral("SOMETHING ELSE"));
}

void TestPreferences::applyToCurrentIsReportedNeverApplied()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    auto *apply = dlg.findChild<QPushButton *>(QStringLiteral("applyToCurrentButton"));
    QVERIFY(apply);
    QVERIFY(apply->isHidden()); // nothing to apply to until a target is named
    QVERIFY(!dlg.applyRequested());

    dlg.setApplyTarget(QStringLiteral("app.log"));
    QVERIFY(!apply->isHidden());

    QTreeWidget *tree = treeOf(dlg);
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    apply->click();

    // Reported, never performed: applying reindexes the log and destroys the Document
    // this dialog's preview is reading.
    QVERIFY(dlg.applyRequested());
    QCOMPARE(dlg.applyProfile().format.pattern, QStringLiteral("PATTERN"));
}

void TestPreferences::askingToApplyLeavesTheDialogOpenAndSaysWhatIsStillToHappen()
{
    // The press used to accept() the dialog, which put a control that ENDS THE SESSION
    // beside Promote, whose press edits in place and leaves everything on screen. Two
    // adjacent buttons with opposite consequences. The request is now armed and waits for
    // OK along with every other edit — so what has to be pinned is that the press is
    // still unmistakable: the button stays down and a notice says when it happens.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    dlg.selectLog(logPath(QStringLiteral("app.log")), LogProfile{},
                  LogProfile{}); // as MainWindow does before naming the target
    dlg.setApplyTarget(QStringLiteral("app.log"));

    auto *apply = dlg.findChild<QPushButton *>(QStringLiteral("applyToCurrentButton"));
    auto *notice = dlg.findChild<QLabel *>(QStringLiteral("applyNoticeLabel"));
    QVERIFY(apply);
    QVERIFY(notice);
    dlg.show();
    QVERIFY(!notice->isVisible()); // nothing asked for yet

    QSignalSpy accepted(&dlg, &QDialog::accepted);
    apply->click();

    QVERIFY2(accepted.isEmpty(), "asking to apply closed the dialog");
    QVERIFY(dlg.isVisible());
    QVERIFY2(apply->isChecked(), "the button did not stay down on an armed request");
    QVERIFY(notice->isVisible());
    // It names the LOG, because that is what a request is about. Only the name is
    // asserted on — it is data this test supplied, while the sentence around it is prose
    // that a translated build moves, which is why no test here matches on wording.
    QVERIFY2(notice->text().contains(QStringLiteral("app.log")),
             qPrintable(notice->text()));

    // The button is a claim about the entry on screen, so it comes back up on another —
    // pressing it there arms that one rather than reading as a second press on this one.
    // The request itself stands, and the notice then names the entry it came from.
    QTreeWidget *tree = treeOf(dlg);
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    QVERIFY(!apply->isChecked());
    QVERIFY(dlg.applyRequested());
    QVERIFY(notice->isVisible());

    // And OK still carries it out, along with the sweep and the commit that every OK does.
    dlg.accept();
    QCOMPARE(accepted.count(), 1);
    QVERIFY(dlg.applyRequested());
}

void TestPreferences::askingAgainOnTheSameEntryWithdrawsTheRequest()
{
    // A button that no longer closes the dialog needs a way back that is not Cancel —
    // which would discard every other edit made in the same visit.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    dlg.selectLog(logPath(QStringLiteral("app.log")), LogProfile{}, LogProfile{});
    dlg.setApplyTarget(QStringLiteral("app.log"));

    auto *apply = dlg.findChild<QPushButton *>(QStringLiteral("applyToCurrentButton"));
    auto *notice = dlg.findChild<QLabel *>(QStringLiteral("applyNoticeLabel"));
    QVERIFY(apply);
    QVERIFY(notice);
    dlg.show();

    apply->click();
    QVERIFY(dlg.applyRequested());
    apply->click();
    QVERIFY2(!dlg.applyRequested(), "a second press did not withdraw the request");
    QVERIFY(!apply->isChecked());
    QVERIFY2(!notice->isVisible(), "the notice outlived the request it was about");

    dlg.accept();
    QVERIFY(!dlg.applyRequested());
}

void TestPreferences::theSettingsAppliedAreWhatTheEntrySaysWhenOkIsPressed()
{
    // The consequence of arming a request instead of performing it: the user goes on
    // editing afterwards, and what reaches the log has to be what the entry FINALLY says.
    // A profile snapshotted when the button went down would apply the settings as they
    // were three keystrokes ago, silently, and only to the log — the tree would hold the
    // other ones.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOnStored(dlg, logPath(QStringLiteral("app.log")),
                 profileWith(QStringLiteral("MINE")));
    dlg.setApplyTarget(QStringLiteral("app.log"));

    auto *apply = dlg.findChild<QPushButton *>(QStringLiteral("applyToCurrentButton"));
    auto *pattern = dlg.findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
    QVERIFY(apply);
    QVERIFY(pattern);

    apply->click();
    QCOMPARE(dlg.applyProfile().format.pattern, QStringLiteral("MINE"));

    pattern->setText(QStringLiteral("EDITED AFTER ASKING"));
    dlg.accept();

    QVERIFY(dlg.applyRequested());
    QCOMPARE(dlg.applyProfile().format.pattern, QStringLiteral("EDITED AFTER ASKING"));
    // And the log's own entry agrees with what was applied, which is the other half of
    // the same claim: one gesture, not two answers.
    QVERIFY(dlg.fileProfile().has_value());
    QCOMPARE(dlg.fileProfile()->format.pattern,
             QStringLiteral("EDITED AFTER ASKING"));
}

void TestPreferences::everyProfileFieldRoundTripsThroughTheEditor()
{
    // The guard against a field added to LogProfile and not to the editor. It would
    // otherwise be silently reset for every node the user so much as looks at — the trap
    // FormatEditor's carry-through stash exists for, one level up.
    LogSettingsTree t;
    LogProfile p;
    p.format.pattern = QStringLiteral("%d{%Y-%m-%d} %-5p %c - %m%n");
    p.format.encoding = Encoding::Utf16LE;
    p.format.sourceZone = ZoneChoice{ZoneChoice::Kind::FixedOffset, -5 * 3600};
    p.format.timeDisplay = TimeDisplay::RunSeconds;
    p.format.runStartPattern = QStringLiteral("=== BOOT ===");
    p.format.runStartIsRegex = true;
    p.format.runStartCaseSensitive = true;
    p.wrapMode = WrapMode::AlwaysOn;
    t.setDefaults(p);

    PreferencesDialog dlg(t, QStringLiteral("app.log"), sample());
    auto *runStart = dlg.findChild<QLineEdit *>(QStringLiteral("profileRunStartPattern"));
    QVERIFY(runStart);
    QCOMPARE(runStart->text(), QStringLiteral("=== BOOT ==="));
    QVERIFY(dlg.findChild<QCheckBox *>(QStringLiteral("profileRunStartRegex"))->isChecked());
    QVERIFY(dlg.findChild<QCheckBox *>(QStringLiteral("profileRunStartCase"))->isChecked());
    QCOMPARE(dlg.findChild<QComboBox *>(QStringLiteral("profileTimeDisplay"))
                 ->currentData().toInt(),
             int(TimeDisplay::RunSeconds));
    QCOMPARE(dlg.findChild<QComboBox *>(QStringLiteral("profileWrapMode"))
                 ->currentData().toInt(),
             int(WrapMode::AlwaysOn));

    // Read back off the TREE, not off the editor: what matters is that a trip through
    // the dialog leaves the node holding what it held.
    dlg.accept();
    QVERIFY(dlg.tree().defaults() == p);
}

void TestPreferences::emptySampleIsHarmless()
{
    // The ordinary state of this dialog on a fresh start: no log open, so no bytes to
    // preview or guess from. Everything is still editable.
    LogSettingsTree empty;
    PreferencesDialog dlg(empty, QString(), QByteArray());
    auto *detect = dlg.findChild<QPushButton *>(QStringLiteral("formatDetectButton"));
    QVERIFY(detect);
    QVERIFY(!detect->isEnabled());

    // And auto-detect says NOTHING, rather than reporting the fallback it lands on when
    // it is handed no bytes. sniff("") finds no BOM and no UTF-16 pattern in nothing at
    // all and returns UTF-8, which the label used to present as a detection — on a fresh
    // start, with no log open, and just as confidently under a pattern node.
    auto *detected = dlg.findChild<QLabel *>(QStringLiteral("formatDetectedLabel"));
    QVERIFY(detected);
    QVERIFY2(detected->text().isEmpty(),
             qPrintable(QStringLiteral("claimed a detection with no sample: \"%1\"")
                            .arg(detected->text())));

    auto *pattern = dlg.findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
    QVERIFY(pattern);
    pattern->setText(QStringLiteral("%p %m%n"));
    dlg.accept();
    QCOMPARE(dlg.tree().defaults().format.pattern, QStringLiteral("%p %m%n"));
}

void TestPreferences::theEmptyPreviewSaysSoOnTheTableAndTheMatchCountBelowIt()
{
    // Reported from the real dialog: with no log open, "No sample lines to preview."
    // sat under a full-height empty grid, so the reader met the blank table first and
    // reached the explanation last. It is the caption that blankness is missing, so it
    // is centred ON the grid. Asserted on the rectangles at several dialog sizes,
    // because the label exists, holds the right text and answers isVisible() wherever
    // it is put; only its geometry says it is on the table rather than beside it.
    LogSettingsTree emptyTree;
    PreferencesDialog dlg(emptyTree, QString(), QByteArray());
    auto *notice = dlg.findChild<QLabel *>(QStringLiteral("formatPreviewEmptyLabel"));
    auto *match = dlg.findChild<QLabel *>(QStringLiteral("formatMatchLabel"));
    auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("formatPreviewTable"));
    QVERIFY(notice);
    QVERIFY(match);
    QVERIFY(table);

    dlg.show();
    for (const QSize size : {QSize(980, 700), QSize(760, 560), QSize(1300, 900)}) {
        dlg.resize(size);
        QCoreApplication::processEvents();

        QVERIFY2(notice->isVisible(), "the empty preview said nothing at all");
        QVERIFY2(!match->isVisible(), "a match count was reported over no samples");

        // Centred on the grid the notice is about — its own centre against the
        // viewport's, in the viewport's coordinates, where the label lives.
        const QRect view = table->viewport()->rect();
        const QRect label = notice->geometry();
        QVERIFY2(view.contains(label.center()),
                 qPrintable(QStringLiteral("the notice sits at %1,%2, off a %3x%4 grid")
                                .arg(label.center().x()).arg(label.center().y())
                                .arg(view.width()).arg(view.height())));
        QVERIFY2(qAbs(label.center().x() - view.center().x()) <= 1
                     && qAbs(label.center().y() - view.center().y()) <= 1,
                 qPrintable(QStringLiteral("notice centre %1,%2 vs grid centre %3,%4 at %5x%6")
                                .arg(label.center().x()).arg(label.center().y())
                                .arg(view.center().x()).arg(view.center().y())
                                .arg(size.width()).arg(size.height())));

        // In the UI font, not the table's. A child of the viewport inherits the
        // fixed-pitch font the sample lines are rendered in, and a sentence in it reads
        // as a sample line — an empty preview appearing to contain one entry.
        QCOMPARE(notice->font().family(), QApplication::font().family());
    }

    // And with a sample it is gone entirely, with the count back under the rows it
    // counts — the one message that IS a result of what the reader just looked at.
    PreferencesDialog withSample(populated(), QStringLiteral("app.log"), sample());
    withSample.show();
    QCoreApplication::processEvents();
    auto *notice2 = withSample.findChild<QLabel *>(QStringLiteral("formatPreviewEmptyLabel"));
    auto *match2 = withSample.findChild<QLabel *>(QStringLiteral("formatMatchLabel"));
    auto *table2 = withSample.findChild<QTableWidget *>(QStringLiteral("formatPreviewTable"));
    QVERIFY2(!notice2->isVisible(), "claimed there was nothing to preview over a sample");
    QVERIFY(match2->isVisible());
    const int tableBottom = table2->mapTo(&withSample, QPoint(0, table2->height())).y();
    const int labelTop = match2->mapTo(&withSample, QPoint(0, 0)).y();
    QVERIFY2(labelTop >= tableBottom,
             qPrintable(QStringLiteral("the count starts at y=%1, the table ends at y=%2")
                            .arg(labelTop)
                            .arg(tableBottom)));
}

// With bytes to look at it reports what it made of them — but ONLY under the node for
// the log those bytes came from. A pattern node and the defaults are about a CLASS of
// logs, so a reading taken from whichever file happens to be open is a fact about a
// different file printed under this entry's heading, and nothing on screen tells the
// reader that the encoding shown is not the one this entry will give the next log it
// claims. Naming the sample in the words ("detected in the sample: UTF-8") was the
// earlier attempt and did not go far enough.
void TestPreferences::noMessageFitsInLessRoomThanItsTextNeeds()
{
    // Every label here that can WRAP, at widths where it does. Measured before
    // MessageLabel: the format warning was given 25 px for text needing 34 at a 560 px
    // dialog and drew its second line over the row below, while the error beside it — in
    // the same form, one row apart — came out right, because a word-wrapped QLabel's
    // sizeHint() is a guess and whether the layout chain asks heightForWidth() instead
    // depends on what sits between the label and the window.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    dlg.show();
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));

    auto *kind = dlg.findChild<QComboBox *>(QStringLiteral("patternKindCombo"));
    auto *match = dlg.findChild<QLineEdit *>(QStringLiteral("patternMatchEdit"));
    auto *pattern = dlg.findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
    QVERIFY(kind);
    QVERIFY(match);
    QVERIFY(pattern);

    // One of each message in force at once: a broken regexp, and a format pattern that
    // compiles but carries neither %p nor %c, which is the LONGEST of the three.
    kind->setCurrentIndex(kind->findData(int(LogPatternNode::Kind::Regex)));
    match->setText(QStringLiteral("[unclosed"));
    pattern->setText(QStringLiteral("%m%n"));

    for (const int width : {980, 760, 620}) {
        dlg.resize(width, 760);
        for (int settle = 0; settle < 4; ++settle)
            QCoreApplication::processEvents();

        for (const QString &name : {QStringLiteral("formatErrorLabel"),
                                    QStringLiteral("formatWarnLabel"),
                                    QStringLiteral("patternErrorLabel")}) {
            auto *label = dlg.findChild<QLabel *>(name);
            QVERIFY2(label, qPrintable(name));
            if (!label->isVisible())
                continue;
            QVERIFY2(label->height() >= label->heightForWidth(label->width()),
                     qPrintable(QStringLiteral("%1 needs %2 px at %3 wide and was given %4"
                                               " (dialog %5)")
                                    .arg(name)
                                    .arg(label->heightForWidth(label->width()))
                                    .arg(label->width())
                                    .arg(label->height())
                                    .arg(width)));
        }
    }
}

void TestPreferences::theDetectedEncodingIsReportedOnlyForTheLogItRead()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    auto *detected = dlg.findChild<QLabel *>(QStringLiteral("formatDetectedLabel"));
    auto *encoding = dlg.findChild<QComboBox *>(QStringLiteral("formatEncodingCombo"));
    QVERIFY(detected);
    QVERIFY(encoding);

    // The defaults, and a pattern: both silent, however good the sample is.
    QVERIFY2(detected->text().isEmpty(), "the defaults claimed a detection about one log");
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    QVERIFY2(detected->text().isEmpty(), "a pattern claimed a detection about one log");

    // The log the bytes came from — which is the one selectLog() names, the call both of
    // MainWindow's entry points make.
    dlg.selectLog(logPath(QStringLiteral("app.log")), LogProfile{}, LogProfile{});
    QVERIFY(detected->text().contains(QStringLiteral("UTF-8")));
    QVERIFY2(detected->text().contains(QStringLiteral("sample")),
             "the label must name what it looked at, not just what it found");

    // Back to a pattern: the reading is a fact about one file, and this level is about a
    // class of them. ANOTHER CONCRETE FILE used to be the case here and cannot be any
    // more — only the open log is listed, and that is by construction the log the bytes
    // came from — so what is left to be told apart is the log from the levels above it.
    // The comparison in loadNode() stays all the same: it is the statement of the rule,
    // and a rule stated only in the one arrangement that can violate it is a rule that
    // comes back the next time the arrangement changes.
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    QVERIFY2(detected->text().isEmpty(), "a pattern claimed this log's detection");

    // And nothing to report once the encoding is stated outright rather than guessed.
    tree->setCurrentItem(currentFileRow(tree));
    QVERIFY(!detected->text().isEmpty());
    encoding->setCurrentIndex(encoding->findText(QStringLiteral("UTF-16 LE")));
    QVERIFY(detected->text().isEmpty());
}

void TestPreferences::theTwoPanesKeepAGapBetweenThem()
{
    // Reported from the real dialog: with both splitter panels flush, the only thing
    // between the tree's frame and the right pane's prose is the splitter handle, and
    // the text reads as though it were spilling into the tree. Asserted on the child
    // RECTANGLES at several widths, because every widget was present, correct and
    // visible while it was wrong — which is invisible to every other kind of test.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    auto *title = dlg.findChild<QLabel *>(QStringLiteral("nodeTitleLabel"));
    auto *group = dlg.findChild<QWidget *>(QStringLiteral("patternGroup"));
    QVERIFY(tree);
    QVERIFY(title);
    QVERIFY(group);

    // A pattern node, so the framed section on the right is the widget nearest the
    // tree — a rounded border flush against the boundary is the worse-looking half.
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));

    dlg.show();
    for (const int width : {760, 900, 1100, 1400}) {
        dlg.resize(width, 700);
        QCoreApplication::processEvents();

        const int treeRight = tree->mapTo(&dlg, QPoint(tree->width(), 0)).x();
        for (QWidget *w : {static_cast<QWidget *>(title), group}) {
            const int left = w->mapTo(&dlg, QPoint(0, 0)).x();
            QVERIFY2(left - treeRight >= 8,
                     qPrintable(QStringLiteral("%1 is %2 px from the tree at width %3")
                                    .arg(w->objectName())
                                    .arg(left - treeRight)
                                    .arg(width)));
        }
    }
}

// What a row needs to be readable in full: one indentation per level, plus one more for
// the branch decoration the root rows also get, plus the label itself. The same sum
// applyInitialSplit() derives the pane's width from, and the offset the delegate elides
// the text within.
static int rowWidthNeeded(QTreeWidget *tree, QTreeWidgetItem *item)
{
    int level = 1;
    for (QTreeWidgetItem *p = item->parent(); p; p = p->parent())
        ++level;
    return level * tree->indentation()
        + tree->fontMetrics().horizontalAdvance(item->text(0));
}

void TestPreferences::theTreePaneOpensWideEnoughForItsLongestRow()
{
    // Reported from the real dialog: at the split it opened with, "Logs with no matching
    // pattern" was cut off, on a window with room to spare on the other side. A fraction
    // of the dialog knows nothing about the labels, which is why this runs at TWO FONT
    // SIZES: a fixed fraction happens to be enough for the offscreen default and is not
    // enough for a desktop whose font is larger, which is the difference between this
    // test and the eye that saw the bug. Both sizes stay under the pane's cap, so what
    // fails here is a width that does not follow the labels rather than the cap biting.
    //
    // The row that reported it is gone with the virtual parent, and every row populated()
    // has left is short enough for the pane's 220 px FLOOR to cover it — which would pass
    // this test without measuring anything at all. So a long PATTERN takes its place: a
    // pattern is a line the user types, so it makes the same argument.
    //
    // And the dialog is widened first, which is the other half of not testing the wrong
    // thing. The floor and the 40% cap are ~220 px apart at the dialog's own size, so a
    // label long enough to clear the floor at scale 1.0 is into the cap at 1.5 — and this
    // case would then be failing on the cap, which is
    // oneEnormousPatternDoesNotHandTheTreeTheDialog's subject and not this one's. A wider
    // dialog moves the cap out of the way and leaves the claim here about the labels.
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve here, so no width measured from them means anything");

    LogSettingsTree wide = populated();
    LogPatternNode longName;
    longName.match = QStringLiteral("*.audit-trail-archive-rotated-nightly.log");
    wide.addPattern(longName);

    for (const double scale : {1.0, 1.5}) {
        PreferencesDialog dlg(wide, QStringLiteral("app.log"), sample());
        openOn(dlg, logPath(QStringLiteral("app.log")));
        dlg.resize(1500, 700);
        QFont font = dlg.font();
        if (font.pointSizeF() > 0)
            font.setPointSizeF(font.pointSizeF() * scale);
        else
            font.setPixelSize(int(font.pixelSize() * scale));
        dlg.setFont(font);

        QTreeWidget *tree = treeOf(dlg);
        QVERIFY(tree);
        dlg.show();
        QCoreApplication::processEvents();

        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            const int needed = rowWidthNeeded(tree, *it);
            QVERIFY2(needed <= tree->columnWidth(0),
                     qPrintable(QStringLiteral("\"%1\" needs %2 px at font scale %3 and "
                                               "the column is %4")
                                    .arg((*it)->text(0))
                                    .arg(needed)
                                    .arg(scale)
                                    .arg(tree->columnWidth(0))));
        }
    }
}

void TestPreferences::oneEnormousPatternDoesNotHandTheTreeTheDialog()
{
    // A pattern is a line the user types, so its length is unbounded — and the settings
    // the tree exists to reach are on the other side of the handle.
    if (QFontDatabase::families().isEmpty())
        QSKIP("no fonts resolve here, so no width measured from them means anything");

    LogSettingsTree t;
    LogPatternNode n;
    n.match = QString(400, QLatin1Char('x')) + QStringLiteral("*.log");
    t.addPattern(n);

    PreferencesDialog dlg(t, QString(), QByteArray());
    QTreeWidget *tree = treeOf(dlg);
    auto *splitter = dlg.findChild<QSplitter *>(QStringLiteral("settingsSplitter"));
    QVERIFY(tree);
    QVERIFY(splitter);
    dlg.show();
    QCoreApplication::processEvents();

    QVERIFY2(tree->width() * 2 <= splitter->width(),
             qPrintable(QStringLiteral("the tree took %1 px of %2")
                            .arg(tree->width())
                            .arg(splitter->width())));
}

void TestPreferences::aSplitAlreadySettledIsNotRecomputed()
{
    // The computed width is an opening position, not a policy: the moment anyone else
    // has an opinion — the user dragging the handle, or the same dialog shown again —
    // it stands. Nothing here recomputes on a rebuild either, which is what would take
    // the handle back off the user every time they added a pattern.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    auto *splitter = dlg.findChild<QSplitter *>(QStringLiteral("settingsSplitter"));
    QVERIFY(splitter);
    dlg.show();
    QCoreApplication::processEvents();

    const QList<int> moved{140, splitter->width() - 140 - splitter->handleWidth()};
    splitter->setSizes(moved);
    QCoreApplication::processEvents();
    const QList<int> settled = splitter->sizes();

    // A rebuild, by adding a pattern, and then the dialog shown again.
    dlg.findChild<QToolButton *>(QStringLiteral("addPatternButton"))->click();
    QCoreApplication::processEvents();
    dlg.hide();
    dlg.show();
    QCoreApplication::processEvents();

    QCOMPARE(splitter->sizes(), settled);
}

void TestPreferences::everyTreeRowSaysItsWholeSelfOnHover()
{
    // A row the widget elides is readable only if the rest is one hover away, and the
    // pane is narrow on purpose whenever the user drags it there. The log's row does the
    // most work of the three, because its label is now a ROLE — "Current file" — and so
    // is not even short for the thing it names: the address is the only place the row
    // says WHICH log it is about.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOn(dlg, logPath(QStringLiteral("app.log")));
    QTreeWidget *tree = treeOf(dlg);
    QVERIFY(tree);

    QTreeWidgetItem *root = tree->topLevelItem(0);
    QTreeWidgetItem *pattern = rowNamed(tree, QStringLiteral("*.log"));
    QTreeWidgetItem *log = currentFileRow(tree);
    QVERIFY(pattern);
    QVERIFY(log);

    // Its own full text, for the row a narrow pane cuts short.
    QCOMPARE(pattern->toolTip(0), pattern->text(0));
    // The address, not the label — a tooltip repeating the label says nothing.
    QCOMPARE(log->toolTip(0), logPath(QStringLiteral("app.log")));
    QVERIFY(log->toolTip(0) != log->text(0));
    // And the defaults, whose label is whole at any width, say what they cover instead.
    QVERIFY(!root->toolTip(0).isEmpty());
    QVERIFY(root->toolTip(0) != root->text(0));
}

void TestPreferences::enterFinishesAFieldWithoutClosingTheDialog()
{
    // This dialog is mostly text fields, and Return is how a person finishes one.
    // QDialog reads a bare Return as "click the default button", and a QLineEdit passes
    // it up after emitting editingFinished — so finishing a field used to close the
    // dialog. The commit must still happen; only the closing must not.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    dlg.show();

    QSignalSpy accepted(&dlg, &QDialog::accepted);
    QSignalSpy rejected(&dlg, &QDialog::rejected);

    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    auto *match = dlg.findChild<QLineEdit *>(QStringLiteral("patternMatchEdit"));
    QVERIFY(match);
    match->setFocus();
    match->setText(QStringLiteral("*.trace"));
    QTest::keyClick(match, Qt::Key_Return);

    QCOMPARE(accepted.count(), 0);
    QVERIFY(dlg.isVisible());
    // …and the edit was committed all the same, because that rides on editingFinished.
    QCOMPARE(dlg.tree().patterns().at(0).match, QStringLiteral("*.trace"));

    // Not even with OK focused: the user asked for Enter not to reach it at all.
    auto *box = dlg.findChild<QDialogButtonBox *>(QStringLiteral("preferencesButtons"));
    QVERIFY(box);
    QPushButton *ok = box->button(QDialogButtonBox::Ok);
    QVERIFY(ok);
    ok->setFocus();
    QTest::keyClick(&dlg, Qt::Key_Return);
    QTest::keyClick(&dlg, Qt::Key_Enter);
    QCOMPARE(accepted.count(), 0);
    QVERIFY(dlg.isVisible());

    // TWO mechanisms, tested apart, because either alone would leave this passing while
    // half the fix was gone.
    //
    // One: showEvent clears the default-button ring QDialogButtonBox draws on OK, so
    // nothing on screen promises an Enter that does not happen.
    for (QPushButton *b : dlg.findChildren<QPushButton *>())
        QVERIFY2(!b->isDefault(), qPrintable(b->objectName() + QLatin1String(" is default")));

    // Two: keyPressEvent swallows the key regardless. Put the default back by hand —
    // which is what a Qt version delivering the button box's Show event AFTER the
    // dialog's own would leave behind, and only CI checks the 6.4 the project targets.
    ok->setDefault(true);
    QTest::keyClick(&dlg, Qt::Key_Return);
    QCOMPARE(accepted.count(), 0);
    QVERIFY(dlg.isVisible());
    ok->setDefault(false);

    // Clicking it still works — the button is not disabled, only unreachable by Enter.
    ok->click();
    QCOMPARE(accepted.count(), 1);

    // ESCAPE IS UNTOUCHED. Cancelling a Preferences dialog raised mid-open abandons the
    // open (SPEC.md §4), and tst_openflow drives exactly that with Esc — swallowing it
    // here would leave those cases hanging on a dialog that never closes.
    PreferencesDialog second(populated(), QStringLiteral("app.log"), sample());
    second.show();
    QSignalSpy secondRejected(&second, &QDialog::rejected);
    QTest::keyClick(&second, Qt::Key_Escape);
    QCOMPARE(secondRejected.count(), 1);
    QCOMPARE(rejected.count(), 0);
}

void TestPreferences::theMenuEntryHasAnAcceleratorOnEveryPlatform()
{
    // QKeySequence::Preferences on its own is not an accelerator anybody can press: it
    // is empty on Windows, and on X11/Wayland it resolves to Qt::Key_Settings, a system
    // key virtually no keyboard carries — so the entry read "Settings" and answered
    // nothing. Hence a check on the SEQUENCE and not merely on the list being non-empty,
    // which the broken state passes.
    MainWindow w;
    auto *action = w.findChild<QAction *>(QStringLiteral("preferencesAction"));
    QVERIFY(action);
    QVERIFY2(!action->shortcuts().isEmpty(), "File ▸ Preferences has no accelerator");
#ifndef Q_OS_MACOS
    QVERIFY2(action->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_P)),
             qPrintable(QStringLiteral("expected Ctrl+P, got \"%1\"")
                            .arg(action->shortcut().toString())));
    // And it is the one the menu shows, which is the first in the list.
    QCOMPARE(action->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_P));
#endif
}

void TestPreferences::thePreviewKeepsTheFormatSectionsMargins()
{
    // The preview shows these settings applied to the sample, so it sits INSIDE the File
    // format section — which is also what indents it to the same left and right margins
    // as the fields above it. In the editor's own layout it was flush to both panel
    // edges while every field beside it was inset, and the table's frame made that
    // half-alignment the most visible edge on the page.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    auto *group = dlg.findChild<QWidget *>(QStringLiteral("formatGroup"));
    auto *table = dlg.findChild<QTableWidget *>(QStringLiteral("formatPreviewTable"));
    auto *detect = dlg.findChild<QPushButton *>(QStringLiteral("formatDetectButton"));
    QVERIFY(group);
    QVERIFY(table);
    QVERIFY(detect);

    dlg.show();
    for (const int width : {760, 980, 1300}) {
        dlg.resize(width, 700);
        QCoreApplication::processEvents();

        auto leftIn = [&dlg](QWidget *w) { return w->mapTo(&dlg, QPoint(0, 0)).x(); };
        auto rightIn = [&dlg](QWidget *w) { return w->mapTo(&dlg, QPoint(w->width(), 0)).x(); };

        // Inside the section on both sides, by the same margin — the alignment a reader
        // sees, and the one a flush table breaks asymmetrically.
        const int leftMargin = leftIn(table) - leftIn(group);
        const int rightMargin = rightIn(group) - rightIn(table);
        QVERIFY2(leftMargin > 0 && qAbs(leftMargin - rightMargin) <= 1,
                 qPrintable(QStringLiteral("margins %1 / %2 at width %3")
                                .arg(leftMargin).arg(rightMargin).arg(width)));

        // And in the same content band as the fields: the row above ends where it does.
        QCOMPARE(rightIn(table), rightIn(detect));
    }
}

void TestPreferences::aPatternRowIsQuotedAndALogRowIsNot()
{
    // The two kinds of row sit at neighbouring indents and one of them is not a name:
    // `*.log` over a row of ordinary words reads as a file with an odd spelling until
    // the quotes say otherwise. The log's row is prose and stays bare for the same
    // reason an empty pattern does, below: quotes round something that is not a name
    // are quotes round nothing.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    openOn(dlg, logPath(QStringLiteral("app.log")));
    QTreeWidget *tree = treeOf(dlg);
    QTreeWidgetItem *pattern = rowNamed(tree, QStringLiteral("*.log"));
    QTreeWidgetItem *log = currentFileRow(tree);
    QVERIFY(pattern);
    QVERIFY(log);
    QCOMPARE(pattern->text(0), QStringLiteral("\"*.log\""));
    QVERIFY(!log->text(0).contains(QLatin1Char('"')));
    QVERIFY(!log->text(0).isEmpty());

    // A pattern that says nothing yet is prose about the absence of one, so it is NOT
    // quoted: an empty pair of quotes is a row wearing a name of no characters.
    auto *add = dlg.findChild<QToolButton *>(QStringLiteral("addPatternButton"));
    QVERIFY(add);
    add->click();
    QTreeWidgetItem *fresh = tree->currentItem();
    QVERIFY(fresh);
    QVERIFY2(!fresh->text(0).contains(QLatin1Char('"')),
             qPrintable(QStringLiteral("an empty pattern shows as %1").arg(fresh->text(0))));
}

void TestPreferences::whatTheNodeIsSitsAboveTheHeadingWithARuleBetweenThem()
{
    // "Matches" says which logs a pattern claims. It is NOT one of the settings those
    // logs open with, and under the heading it read as the first of them. So the panel
    // is identity, rule, then heading-and-settings — and the heading introduces what is
    // below it. Asserted on the y order, which is the whole claim.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    auto *group = dlg.findChild<QWidget *>(QStringLiteral("patternGroup"));
    auto *title = dlg.findChild<QLabel *>(QStringLiteral("nodeTitleLabel"));
    auto *rule = dlg.findChild<QWidget *>(QStringLiteral("nodeDividerLine"));
    auto *editor = dlg.findChild<QWidget *>(QStringLiteral("profileEditor"));
    QVERIFY(group);
    QVERIFY(title);
    QVERIFY(rule);
    QVERIFY(editor);

    dlg.show();
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    QCoreApplication::processEvents();

    auto top = [&dlg](QWidget *w) { return w->mapTo(&dlg, QPoint(0, 0)).y(); };
    auto bottom = [&dlg](QWidget *w) { return w->mapTo(&dlg, QPoint(0, w->height())).y(); };

    QVERIFY(rule->isVisible());
    QVERIFY2(bottom(group) <= top(rule),
             qPrintable(QStringLiteral("Matches ends at %1, the rule starts at %2")
                            .arg(bottom(group)).arg(top(rule))));
    QVERIFY2(bottom(rule) <= top(title),
             qPrintable(QStringLiteral("the rule ends at %1, the heading starts at %2")
                            .arg(bottom(rule)).arg(top(title))));
    QVERIFY2(bottom(title) <= top(editor),
             qPrintable(QStringLiteral("the heading ends at %1, the settings start at %2")
                            .arg(bottom(title)).arg(top(editor))));

    // Identity first on every node: the block naming it sits above the fields defining
    // it, which is why the pattern's own name is not one of those fields' labels.
    auto *identity = dlg.findChild<QWidget *>(QStringLiteral("nodeIdentityGroup"));
    QVERIFY(identity);
    QVERIFY2(bottom(identity) <= top(group),
             qPrintable(QStringLiteral("the heading ends at %1, Matches starts at %2")
                            .arg(bottom(identity)).arg(top(group))));

    // A log node has NO heading under the rule, because the block above already says
    // which level this is and a second bold line saying so at greater length is the
    // caption twice.
    // app.log, which the *.log pattern DOES claim — a log no pattern claims heads itself
    // with the one sentence a log node has to say, which is a different case
    // (aLogNoPatternMatchesSitsUnderTheDefaultsAndSaysWhyItCannotBePromoted).
    tree->setCurrentItem(currentFileRow(tree));
    QCoreApplication::processEvents();
    QVERIFY2(!title->isVisible(), "a log node drew a heading as well as its identity");
    // And no rule either. It earns its place on a pattern node, where the match fields
    // are a row of editable controls above another row of them; two lines of centred
    // text could not be mistaken for a settings block, so there the line only cuts the
    // panel in half.
    QVERIFY2(!rule->isVisible(), "a log node drew a rule under its name");
    QVERIFY2(bottom(identity) <= top(editor),
             qPrintable(QStringLiteral("the heading ends at %1, the settings start at %2")
                            .arg(bottom(identity)).arg(top(editor))));

    // Same on the defaults, which have nothing between their name and their settings.
    tree->setCurrentItem(tree->topLevelItem(0)); // the defaults
    QCoreApplication::processEvents();
    QVERIFY2(!rule->isVisible(), "the defaults drew a rule with nothing above it");
}

void TestPreferences::bothCaptionsAreBoldAndCentred()
{
    // "Matches" was a SectionBox heading, whose bold lives in a QGroupBox::title style
    // sheet — and neither Breeze nor Fusion draws the title bold from it, while obeying
    // the same sheet's centring. The caption came out at normal weight on a render with
    // every other test still green, which is exactly what this asserts instead. The
    // caption it was is gone; the two left come from the same helper and are as exposed.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    for (const QString &name : {QStringLiteral("nodeTitleLabel"),
                                QStringLiteral("nodeNameLabel")}) {
        auto *caption = dlg.findChild<QLabel *>(name);
        QVERIFY2(caption, qPrintable(name));
        QVERIFY2(caption->font().bold(), qPrintable(name + QLatin1String(" is not bold")));
        QVERIFY2(caption->alignment() & Qt::AlignHCenter,
                 qPrintable(name + QLatin1String(" is not centred")));
    }
}

void TestPreferences::anEmptyPatternIsNotReportedAsAnErrorAndARealOneFitsItsRow()
{
    LogSettingsTree empty;
    PreferencesDialog dlg(empty, QString(), QByteArray());
    auto *error = dlg.findChild<QLabel *>(QStringLiteral("formatErrorLabel"));
    auto *edit = dlg.findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
    QVERIFY(error);
    QVERIFY(edit);

    dlg.show();
    QCoreApplication::processEvents();

    // A field nobody has typed in yet is not a fault, and it is the state a fresh pattern
    // node and an unconfigured defaults node both open in — so "Error at position 0:
    // Pattern is empty", in red, was the first thing the dialog said in half the cases it
    // opens in.
    // Cleared by hand, because the DEFAULTS node ships a built-in pattern; the empty
    // state is what a freshly added pattern node and a cleared field both show.
    edit->setText(QString());
    QCoreApplication::processEvents();
    QVERIFY2(!error->isVisible(), "an untyped pattern was reported as an error");

    // A real error still is one — and it FITS. Measured under Breeze before the message
    // rows were made to span both columns: a word-wrapped QLabel in the field column got
    // 105x17 px against a 105x34 hint, because Breeze answers
    // SH_FormLayoutFieldGrowthPolicy with FieldsStayAtSizeHint, so the second line landed
    // on the row below. Fusion grows the field and showed nothing wrong, which is why
    // this asserts on the HEIGHT the row was given rather than on the text.
    edit->setText(QStringLiteral("%d{"));
    QCoreApplication::processEvents();
    QVERIFY(error->isVisible());
    QVERIFY2(error->height() >= error->heightForWidth(error->width()),
             qPrintable(QStringLiteral("the message needs %1 px at %2 wide and was given %3")
                            .arg(error->heightForWidth(error->width()))
                            .arg(error->width())
                            .arg(error->height())));
}

int main(int argc, char *argv[])
{
    // Isolate persistent state: nothing here writes settings, but the widgets it builds
    // read QSettings-backed values, and these must not touch the developer's own.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-preferences"));

    TestPreferences tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_preferences.moc"
