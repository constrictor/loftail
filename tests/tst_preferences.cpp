#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

    // The virtual parent, identified by what hangs under it rather than by its prose.
    static QTreeWidgetItem *virtualParentOf(QTreeWidget *tree, const QString &childLabel)
    {
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            for (int i = 0; i < (*it)->childCount(); ++i) {
                if (rowIdentity((*it)->child(i)) == childLabel)
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
    void theEmptyPreviewSaysSoOnTheTableAndTheMatchCountBelowIt();
    void theMenuEntryHasAnAcceleratorOnEveryPlatform();
    void thePreviewKeepsTheFormatSectionsMargins();
    void aPatternRowIsQuotedAndALogRowIsNot();
    void whatTheNodeIsSitsAboveTheHeadingWithARuleBetweenThem();
    void bothCaptionsAreBoldAndCentred();
    void anEmptyPatternIsNotReportedAsAnErrorAndARealOneFitsItsRow();
    void theDetectedEncodingIsReportedAgainstTheSample();
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
    // The GROUP, not the address label inside it: what is shown and hidden per node is
    // the whole identity block, caption included, and a child of a hidden parent is not
    // itself "hidden" as Qt uses the word.
    auto *address = dlg.findChild<QWidget *>(QStringLiteral("fileGroup"));
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
    auto *addressText = dlg.findChild<QLabel *>(QStringLiteral("fileAddressLabel"));
    QVERIFY(addressText);
    QVERIFY(addressText->text().contains(QLatin1String("app.log")));
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

// With bytes to look at it reports what it made of THEM, and says so: these settings may
// belong to a pattern or to the defaults, which are about a class of logs, while the
// sample is whichever log happens to be open.
void TestPreferences::theDetectedEncodingIsReportedAgainstTheSample()
{
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    auto *detected = dlg.findChild<QLabel *>(QStringLiteral("formatDetectedLabel"));
    auto *encoding = dlg.findChild<QComboBox *>(QStringLiteral("formatEncodingCombo"));
    QVERIFY(detected);
    QVERIFY(encoding);

    QVERIFY(detected->text().contains(QStringLiteral("UTF-8")));
    QVERIFY2(detected->text().contains(QStringLiteral("sample")),
             "the label must name what it looked at, not just what it found");

    // Nothing to report once the encoding is stated outright rather than guessed.
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
    // `*.log` above `app.log` reads as a file with an odd spelling until the quotes say
    // otherwise. A log's address is a name and stays bare.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    QTreeWidget *tree = treeOf(dlg);
    QTreeWidgetItem *pattern = rowNamed(tree, QStringLiteral("*.log"));
    QTreeWidgetItem *log = rowNamed(tree, QStringLiteral("app.log"));
    QVERIFY(pattern);
    QVERIFY(log);
    QCOMPARE(pattern->text(0), QStringLiteral("\"*.log\""));
    QCOMPARE(log->text(0), QStringLiteral("app.log"));

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

    // A log's address is the same kind of thing and goes in the same place.
    auto *address = dlg.findChild<QLabel *>(QStringLiteral("fileAddressLabel"));
    QVERIFY(address);
    tree->setCurrentItem(rowNamed(tree, QStringLiteral("app.log")));
    QCoreApplication::processEvents();
    QVERIFY(rule->isVisible());
    QVERIFY2(bottom(address) <= top(rule),
             qPrintable(QStringLiteral("the address ends at %1, the rule starts at %2")
                            .arg(bottom(address)).arg(top(rule))));

    // And on a node with no identity block there is nothing to cut off: a rule under
    // nothing is a line across the top of the panel, separating it from its own edge.
    tree->setCurrentItem(tree->topLevelItem(0)); // the defaults
    QCoreApplication::processEvents();
    QVERIFY2(!rule->isVisible(), "the defaults drew a rule with nothing above it");
}

void TestPreferences::bothCaptionsAreBoldAndCentred()
{
    // "Matches" was a SectionBox heading, whose bold lives in a QGroupBox::title style
    // sheet — and neither Breeze nor Fusion draws the title bold from it, while obeying
    // the same sheet's centring. The caption came out at normal weight on a render with
    // every other test still green, which is exactly what this asserts instead.
    PreferencesDialog dlg(populated(), QStringLiteral("app.log"), sample());
    for (const QString &name : {QStringLiteral("nodeTitleLabel"),
                                QStringLiteral("patternCaption")}) {
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
