#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>

#include "MnemonicCheck.h"
#include "PreferencesDialog.h"

using namespace loftail;

// The Preferences dialog (SPEC.md §4): the settings tree on the left, the selected
// node's settings on the right. What the tree DECIDES is tst_logsettings' job and what
// it then does on an open is tst_openflow's, because only the real MainWindow can show
// that no dialog appeared. What is pinned here is the dialog's own contract — it applies
// NOTHING, so Cancel is exact — and the things only a dialog can get wrong: which
// editors a node offers, which buttons a node enables, that a rebuild reselects the NODE
// rather than the row it used to occupy, and that the virtual "no pattern" parent cannot
// be selected.
//
// Constructed on the stack and never exec()'d: a modal dialog in a test is a nested
// event loop looking for somewhere to deadlock. Children are reached by OBJECT NAME,
// never by visible text.
class TestPreferences : public QObject
{
    Q_OBJECT

private:
    static QByteArray sample()
    {
        return "2026-08-05 00:00:01,000 [t0] INFO  logger.a - first\n"
               "2026-08-05 00:00:02,000 [t1] WARN  logger.b - second\n";
    }

    // A tree with one pattern, one log under it, and one log under nothing.
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

        LogProfile mine;
        mine.format.pattern = QStringLiteral("MINE");
        t.setFileProfile(QStringLiteral("/var/log/app.log"), mine);

        LogProfile orphan;
        orphan.format.pattern = QStringLiteral("ORPHAN");
        t.setFileProfile(QStringLiteral("/var/log/other.trace"), orphan);
        return t;
    }

    static QTreeWidget *treeOf(PreferencesDialog &dlg)
    {
        return dlg.findChild<QTreeWidget *>(QStringLiteral("settingsTree"));
    }

    // Depth-first walk for the row whose label is `text`. A tree ROW has no object name
    // to be found by — it is data, not a widget — so the label is the only handle it
    // has; every widget around it is still found by object name.
    static QTreeWidgetItem *rowNamed(QTreeWidget *tree, const QString &text)
    {
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if ((*it)->text(0) == text)
                return *it;
        }
        return nullptr;
    }

    // The virtual parent, identified by what hangs under it rather than by its prose.
    static QTreeWidgetItem *virtualParentOf(QTreeWidget *tree, const QString &childLabel)
    {
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            for (int i = 0; i < (*it)->childCount(); ++i) {
                if ((*it)->child(i)->text(0) == childLabel)
                    return *it;
            }
        }
        return nullptr;
    }

private slots:
    void theTreeShowsThreeLevels();
    void aLogWithNoPatternHangsUnderTheVirtualParent();
    void theVirtualParentCannotBeSelected();
    void thePatternEditorIsShownOnlyForAPattern();
    void editingAPatternRehomesItsLogsAndKeepsTheSelection();
    void reorderingChangesWhichPatternWins();
    void everyMnemonicHasABuddyAndNoLetterIsClaimedTwice();
    void aNewPatternStartsEmptyAndClaimsNoLogs();
    void promotingIsOfferedOnlyWithAParentPattern();
    void promotingAPatternMovesItsSettingsIntoTheDefaults();
    void promotingMovesTheSettingsUpAndRemovesTheLogEntry();
    void aScratchNodeSayingNothingNewIsNotKept();
    void applyToCurrentIsReportedNeverApplied();
    void everyProfileFieldRoundTripsThroughTheEditor();
    void emptySampleIsHarmless();
    void theTwoPanesKeepAGapBetweenThem();
    void enterFinishesAFieldWithoutClosingTheDialog();
};

void TestPreferences::theTreeShowsThreeLevels()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    QVERIFY(tree);

    QCOMPARE(tree->topLevelItemCount(), 1); // one root: the defaults
    QTreeWidgetItem *root = tree->topLevelItem(0);
    QTreeWidgetItem *pattern = rowNamed(tree, QStringLiteral("*.log"));
    QVERIFY(pattern);
    QCOMPARE(pattern->parent(), root);

    QTreeWidgetItem *log = rowNamed(tree, QStringLiteral("app.log"));
    QVERIFY(log);
    QCOMPARE(log->parent(), pattern);
}

void TestPreferences::aLogWithNoPatternHangsUnderTheVirtualParent()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);

    QTreeWidgetItem *orphanParent = virtualParentOf(tree, QStringLiteral("other.trace"));
    QVERIFY2(orphanParent, "a log matched by no pattern was not given the virtual parent");
    QCOMPARE(orphanParent->parent(), tree->topLevelItem(0));

    // And it is not created when nothing needs one — otherwise it is a row saying "none
    // of these", about nothing.
    LogSettingsTree covered = populated();
    covered.removeFile(QStringLiteral("/var/log/other.trace"));
    PreferencesDialog tidy(covered, QStringLiteral("app.log"), sample());
    QTreeWidget *tidyTree = treeOf(tidy);
    QCOMPARE(tidyTree->topLevelItem(0)->childCount(), 1); // the pattern, and nothing else
}

void TestPreferences::theVirtualParentCannotBeSelected()
{
    // It holds no settings and stores nothing — "no parent" is the absence of a match.
    // Unselectable by FLAG rather than by a branch in the editor, so there is no state
    // in which an editor could be shown for it.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);

    QTreeWidgetItem *orphanParent = virtualParentOf(tree, QStringLiteral("other.trace"));
    QVERIFY(orphanParent);
    QVERIFY(!(orphanParent->flags() & Qt::ItemIsSelectable));
}

void TestPreferences::thePatternEditorIsShownOnlyForAPattern()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    auto *group = dlg.findChild<QWidget *>(QStringLiteral("patternGroup"));
    auto *address = dlg.findChild<QLabel *>(QStringLiteral("fileAddressLabel"));
    QVERIFY(group);
    QVERIFY(address);

    tree->setCurrentItem(tree->topLevelItem(0)); // the root
    QVERIFY(group->isHidden());
    QVERIFY(address->isHidden());

    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    QVERIFY(!group->isHidden());
    QVERIFY(address->isHidden());

    tree->setCurrentItem(rowNamed(tree, QStringLiteral("app.log")));
    QVERIFY(group->isHidden());
    QVERIFY(!address->isHidden());
    QVERIFY(address->text().contains(QLatin1String("app.log")));
}

void TestPreferences::editingAPatternRehomesItsLogsAndKeepsTheSelection()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
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
    QCOMPARE(rowNamed(tree, QStringLiteral("other.trace"))->parent(), pattern);
    QVERIFY(rowNamed(tree, QStringLiteral("app.log"))->parent() != pattern);

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
    QCOMPARE(dlg.tree().resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
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
    QVERIFY2(loftail_test::mnemonicsAreSound(&dlg, &why, 6), qPrintable(why));
}

// Add Pattern creates a row that matches NOTHING until it is named. It used to seed the
// match — "*.log" from a constant, or from the selected log's extension — which claimed
// every log on the machine the moment the row appeared, and handed them either the
// defaults or that one log's settings while the user was still deciding what the pattern
// was for. The PROFILE seeding is the useful half and stays.
void TestPreferences::aNewPatternStartsEmptyAndClaimsNoLogs()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    auto *add = dlg.findChild<QToolButton *>(QStringLiteral("addPatternButton"));
    auto *match = dlg.findChild<QLineEdit *>(QStringLiteral("patternMatchEdit"));
    QVERIFY(add);
    QVERIFY(match);

    // From a selected log, which is where a derived "*.log" used to come from.
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("app.log")));
    add->click();

    QCOMPARE(dlg.tree().patterns().size(), 2);
    const LogPatternNode &added = dlg.tree().patterns().at(1);
    QVERIFY2(added.match.isEmpty(), "a new pattern arrived already matching something");
    QVERIFY(match->text().isEmpty());
    // Seeded from the log it was added from: that is what makes it worth adding there.
    QCOMPARE(added.profile.format.pattern, QStringLiteral("MINE"));

    // Nothing moved under it — the orphan is still an orphan and the existing pattern
    // still owns its log.
    QCOMPARE(dlg.tree().resolve(QStringLiteral("/var/log/other.trace")).patternIndex, -1);
    QCOMPARE(dlg.tree().resolve(QStringLiteral("/etc/never/seen.log")).profile.format.pattern,
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
    QTreeWidget *tree = treeOf(dlg);
    auto *promote = dlg.findChild<QPushButton *>(QStringLiteral("promoteToParentButton"));
    QVERIFY(promote);

    tree->setCurrentItem(rowNamed(tree, QStringLiteral("app.log")));
    QVERIFY(promote->isEnabled());

    // A pattern's parent is the defaults, so it promotes too — and the button says which
    // level it is aimed at, because "parent" is a different thing one row up.
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("*.log")));
    QVERIFY(promote->isEnabled());
    QVERIFY(promote->text().contains(QStringLiteral("Default")));
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("app.log")));
    QVERIFY(promote->text().contains(QStringLiteral("Pattern")));

    // Under the virtual parent there is nothing above but the defaults, and handing one
    // log's settings to every log in the world is not a level up but a level skipped.
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("other.trace")));
    QVERIFY(!promote->isEnabled());

    // The defaults are the top: nothing above them to promote to.
    tree->setCurrentItem(tree->topLevelItem(0));
    QVERIFY(!promote->isEnabled());
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

    // A log that matched nothing now opens on what the pattern said.
    QCOMPARE(out.resolve(QStringLiteral("/var/log/other.trace")).profile.format.pattern,
             QStringLiteral("ORPHAN")); // its own entry still outranks the defaults
    QCOMPARE(out.resolve(QStringLiteral("/etc/never/seen.trace")).profile.format.pattern,
             QStringLiteral("PATTERN"));
    // And the log under the pattern is untouched: it inherits from the pattern, which
    // has not changed, so there is nothing to re-prune.
    QCOMPARE(out.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("MINE"));
}

void TestPreferences::promotingMovesTheSettingsUpAndRemovesTheLogEntry()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("app.log")));
    dlg.findChild<QPushButton *>(QStringLiteral("promoteToParentButton"))->click();

    const LogSettingsTree &out = dlg.tree();
    QCOMPARE(out.patterns().at(0).profile.format.pattern, QStringLiteral("MINE"));
    // The log now says exactly what the pattern says, so its own entry has nothing left
    // to say — and it is gone, rather than left as a duplicate that would stop tracking
    // the pattern the moment the pattern changed.
    QCOMPARE(out.indexOfFile(QStringLiteral("/var/log/app.log")), -1);
    QVERIFY(!rowNamed(tree, QStringLiteral("app.log")));
    QCOMPARE(out.resolve(QStringLiteral("/var/log/app.log")).profile.format.pattern,
             QStringLiteral("MINE"));
}

void TestPreferences::aScratchNodeSayingNothingNewIsNotKept()
{
    // selectLog() creates a node so there is something to select and edit. If the user
    // leaves it saying what the log already inherits, OK must not keep it — otherwise
    // merely being ASKED about a log leaves an entry behind for ever.
    const LogSettingsTree base = populated();
    const LogProfile inherited = base.resolve(QStringLiteral("/var/log/fresh.log")).profile;

    PreferencesDialog kept(base, QStringLiteral("fresh.log"), sample());
    kept.selectLog(QStringLiteral("/var/log/fresh.log"), inherited);
    QVERIFY2(treeOf(kept)->currentItem(), "the scratch node was not selectable");
    kept.accept();
    QCOMPARE(kept.tree().indexOfFile(QStringLiteral("/var/log/fresh.log")), -1);

    // Changed, and it stays.
    PreferencesDialog edited(base, QStringLiteral("fresh.log"), sample());
    LogProfile changed = inherited;
    changed.format.pattern = QStringLiteral("SOMETHING ELSE");
    edited.selectLog(QStringLiteral("/var/log/fresh.log"), changed);
    edited.accept();
    QCOMPARE(edited.tree().resolve(QStringLiteral("/var/log/fresh.log")).profile.format.pattern,
             QStringLiteral("SOMETHING ELSE"));
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

    auto *pattern = dlg.findChild<QLineEdit *>(QStringLiteral("formatPatternEdit"));
    QVERIFY(pattern);
    pattern->setText(QStringLiteral("%p %m%n"));
    dlg.accept();
    QCOMPARE(dlg.tree().defaults().format.pattern, QStringLiteral("%p %m%n"));
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
