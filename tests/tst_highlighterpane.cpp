#include <QtTest>

#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QFrame>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QHeaderView>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleOptionViewItem>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QToolButton>
#include <QTemporaryFile>

#include "Document.h"
#include "Highlight.h"
#include "FilterPane.h"
#include "HighlighterPane.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"
#include "SectionBox.h"

using namespace loftail;

// The Highlighters pane (SPEC.md §7, §8). A highlight rule now offers the same five
// match axes a filter does, edited through the shared AxisEditor — which is exactly
// where a per-RULE editor can go wrong in ways a per-FILE one cannot:
//
//   1. The axis controls have to reach the rule at all: typing a regex must land in
//      the Document's HighlighterSet, not just in a widget.
//   2. Selecting another rule must show THAT rule's subsystems. The Filters pane's
//      discovery rule ("a name never listed before arrives checked") is right for a
//      statement about the whole file and wrong for one rule among several — it would
//      leak one rule's selection into the next.
//   3. A rule whose subsystem the scan has not reached yet must keep it, or the
//      selection evaporates the moment the editor repopulates.
//   4. An invalid regex must be visible. It matches nothing, so a rule carrying one
//      silently colors nothing.
//
// Runs under an offscreen QApplication; HighlighterPane is a QWidget.
class TestHighlighterPane : public QObject
{
    Q_OBJECT

private:
    static constexpr auto kPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
    static constexpr auto kLog =
        "2026-07-21 12:00:00,000 [main] INFO  net.socket - a\n"
        "2026-07-21 12:00:01,000 [main] WARN  db.pool - b\n";

    static bool openLog(Document &doc, QTemporaryFile &file)
    {
        if (!file.open())
            return false;
        file.write(QByteArray(kLog));
        file.flush();
        return doc.open(file.fileName(), QString::fromLatin1(kPattern), Encoding::Utf8,
                        QTimeZone::utc());
    }

    // Each of the FIVE axes is a checkable QGroupBox whose title row IS the enable
    // control — priority included, which used to be a bare checkbox and combo on a
    // row of its own. Found by OBJECT NAME, never by the title it shows — a visible
    // string is a translator's to change (CLAUDE.md), and these names are the test
    // contract precisely because they are not.
    static QGroupBox *axis(QWidget &w, const char *name)
    {
        return w.findChild<QGroupBox *>(QString::fromLatin1(name));
    }

    static QGroupBox *priorityEnable(QWidget &w)
    {
        return axis(w, "priorityGroup");
    }

    // By object name, never by visible text: the pane embeds an AxisEditor, so more
    // than one button here can carry the same label, and a translated build would
    // change every one of them (ARCHITECTURE.md §9.1).
    static QPushButton *button(QWidget &w, const QString &objectName)
    {
        return w.findChild<QPushButton *>(objectName);
    }

    static QLineEdit *patternEdit(QWidget &w)
    {
        return w.findChild<QLineEdit *>(QStringLiteral("messageText"));
    }

    // The rules are a TABLE — a tick, what the rule matches, its two colours and its
    // three remaining actions, one rule per row. Found by object name, and its columns
    // by the enum, never by the header they show: four of the seven headers are drawn
    // glyphs and the fifth is translated prose.
    static QTableWidget *ruleTable(QWidget &w)
    {
        return w.findChild<QTableWidget *>(QStringLiteral("ruleTable"));
    }

    static void selectRule(QWidget &w, int row)
    {
        ruleTable(w)->setCurrentCell(row, HighlighterPane::kColRule);
    }

    // One row's swatch picker. Both live in ONE cell, so this is a cell lookup and then
    // an object name inside it — the pane-wide name reaches the first row only.
    static QComboBox *swatch(QWidget &w, int row, HighlighterPane::ColourRole role)
    {
        QTableWidget *t = ruleTable(w);
        QWidget *cell = t ? t->cellWidget(row, HighlighterPane::kColColours) : nullptr;
        return cell ? cell->findChild<QComboBox *>(
                          role == HighlighterPane::ColourRole::Foreground
                              ? QStringLiteral("textColor")
                              : QStringLiteral("backgroundColor"))
                    : nullptr;
    }

    // One row's action toggle. An icon BUTTON, not a tick: three ticks in a row say only
    // that three things are set, where a glyph says which three.
    static QToolButton *actionButton(QWidget &w, int row, HighlighterPane::Column column)
    {
        QTableWidget *t = ruleTable(w);
        QWidget *cell = t ? t->cellWidget(row, column) : nullptr;
        return cell ? cell->findChild<QToolButton *>() : nullptr;
    }

    static void setAction(QWidget &w, int row, HighlighterPane::Column column, bool on)
    {
        actionButton(w, row, column)->setChecked(on);
    }

    static bool actionIsOn(QWidget &w, int row, HighlighterPane::Column column)
    {
        return actionButton(w, row, column)->isChecked();
    }

    static void setEnabled(QWidget &w, int row, bool on)
    {
        ruleTable(w)
            ->item(row, HighlighterPane::kColEnabled)
            ->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }

    // What an empty table says for itself. By object name, never by its wording — the
    // wording is translated prose and the two empty states differ in exactly that.
    static QLabel *placeholder(QWidget &w)
    {
        return w.findChild<QLabel *>(QStringLiteral("ruleTablePlaceholder"));
    }

    static bool ruleIsEnabled(QWidget &w, int row)
    {
        return ruleTable(w)->item(row, HighlighterPane::kColEnabled)->checkState() == Qt::Checked;
    }

    // *Default* on both roles is what unticking Highlight used to be: the swatches ARE
    // the colour control now, so a rule that names no palette entry does not colour.
    static void clearColours(QWidget &w, int row)
    {
        for (HighlighterPane::ColourRole role : {HighlighterPane::ColourRole::Foreground,
                                                 HighlighterPane::ColourRole::Background})
            swatch(w, row, role)->setCurrentIndex(0);
    }

    // One entry of one picker, rendered at the size the table actually draws it. The
    // icons are what the two pickers are *made* of — the closed combo clears its text —
    // so a claim about what a reader can tell apart is a claim about these pixels.
    static QImage swatchEntry(QWidget &w, int row, HighlighterPane::ColourRole role, int slot)
    {
        QComboBox *combo = swatch(w, row, role);
        if (!combo)
            return QImage();
        const int item = slot == HighlightPalette::kDefault ? 0 : combo->findData(slot);
        return combo->itemIcon(item)
            .pixmap(combo->iconSize())
            .toImage()
            .convertToFormat(QImage::Format_ARGB32);
    }

    // What a picker shows while it is closed: the entry it is sitting on.
    static QImage swatchShown(QWidget &w, int row, HighlighterPane::ColourRole role)
    {
        QComboBox *combo = swatch(w, row, role);
        if (!combo)
            return QImage();
        return combo->itemIcon(combo->currentIndex())
            .pixmap(combo->iconSize())
            .toImage()
            .convertToFormat(QImage::Format_ARGB32);
    }

    // How many pixels of two icons a reader could tell apart. Antialiased strokes make
    // "different" a distance rather than an inequality, and the ALPHA channel counts as
    // much as the colour: one picker's tile is painted where the other's margin is not.
    static int differingPixels(const QImage &a, const QImage &b)
    {
        if (a.isNull() || a.size() != b.size())
            return -1;
        int n = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const QColor p = a.pixelColor(x, y);
                const QColor q = b.pixelColor(x, y);
                if (qAbs(p.red() - q.red()) + qAbs(p.green() - q.green())
                            + qAbs(p.blue() - q.blue()) + qAbs(p.alpha() - q.alpha())
                    > 40)
                    ++n;
            }
        }
        return n;
    }

    // Whether a colour was painted anywhere solid enough to be seen as itself.
    static bool carriesColour(const QImage &image, const QColor &c, int minAlpha)
    {
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor px = image.pixelColor(x, y);
                if (px.alpha() >= minAlpha
                    && qAbs(px.red() - c.red()) + qAbs(px.green() - c.green())
                            + qAbs(px.blue() - c.blue()) <= 12)
                    return true;
            }
        }
        return false;
    }

    static QListWidget *listContaining(QWidget &w, const QString &name)
    {
        const QList<QListWidget *> lists = w.findChildren<QListWidget *>();
        for (QListWidget *l : lists)
            for (int i = 0; i < l->count(); ++i)
                if (l->item(i)->text() == name)
                    return l;
        return nullptr;
    }

    static void check(QListWidget *list, const QString &name, bool on)
    {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text() == name)
                list->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }

    static bool isChecked(QListWidget *list, const QString &name)
    {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text() == name)
                return list->item(i)->checkState() == Qt::Checked;
        return false;
    }

private slots:
    void everyAxisIsOfferedAndOptIn();
    void aTimeBoundIsTypedInTheColumnsOwnUnits();
    void typingARegexReachesTheDocument();
    void switchingRulesShowsThatRulesSelection();
    void invalidRegexIsFlagged();
    void addedRuleIsInertUntilConfigured();
    void newCopiesTheSelectedRule();
    void tableRowsWearTheirRuleColours();
    void clearRemovesEveryRuleAndTheTabStaysMarked();
    void swatchMenuIsBandedAndFitsAShortScreen();
    void aSwatchPreviewsTheRulesPair();
    void theTwoPickersAreTellableApartAtTheSizeTheyAreDrawn();
    void aDefaultEntryStillNamesNoColourInEitherPicker();
    void oneClickRuleSetsBothColours();
    void reloadingTheListKeepsRulesEnabled();

    // M19 — a rule's effect is a set of actions, and colour is one of them. All four
    // are now set in the rule's own table row: three ticks and a pair of swatch columns.
    void everyActionIsAColumnAndOnlyColourStartsOn();
    void anActionThatIsOnLooksIt();
    void togglingAnActionReachesTheDocument();
    void clickingAnywhereInACheckCellTogglesIt();
    void aRuleWithNoColourDoesNotColour();
    void pickingAColourMakesTheRuleColourAgain();
    void newCopiesTheSelectedRulesActions();
    void theOneClickRuleColoursOnly();
    void reloadingTheListKeepsActions();
    void notifySaysWhyWhenTheDesktopOffersNoNotifications();

    // The pane's shape: actions in the table, the condition alone below it, and an
    // axis the format cannot fill left out.
    void switchedOffAxesStayVisibleAndGreyed();
    void anAxisTheFormatLacksIsNotShownAtAll();
    void theTwoColourPickersAreCellsOfTheirOwnRow();
    void theEditorIsTheConditionAlone();
    void theValueListsTakeTheSpareHeight();
    void noSectionClipsItsOwnTitle();
    void theAxesSitWhereTheFiltersPanesDo();

    // The empty table is not a void: it says which of the two empty states it is in.
    void theEmptyTableSaysThereIsNoFileOpen();
    void theEmptyTableAsksForARuleWhileAFileIsOpen();
    void thePlaceholderGoesAsSoonAsThereIsARuleAndComesBackWithTheLast();

    // A re-render of the time editors is not an edit to the rules (bugs.md #5).
    void aRerenderThatMovesNoZoneLeavesTheSeededRulesAlone();
    void aDisplayZoneChangeMovesEveryRulesBoundsAndNothingElse();

    // A button under the table is live only while it has something to act on.
    void theRuleButtonsAreDeadWhileThereIsNothingToActOn();
    void upAndDownAreDeadAtTheEndsOfTheList();
    void theButtonsFollowTheSelectionAcrossAReorder();
};

void TestHighlighterPane::everyAxisIsOfferedAndOptIn()
{
    HighlighterPane pane;
    // All five axes a filter offers are present (SPEC.md §7)...
    QVERIFY(priorityEnable(pane));
    QVERIFY(axis(pane, "subsystemGroup"));
    QVERIFY(axis(pane, "threadGroup"));
    QVERIFY(axis(pane, "messageGroup"));
    QVERIFY(axis(pane, "timeGroup"));
    // ...and every one of them starts OFF. Unlike the Filters pane, where the two
    // metadata axes ship enabled so their controls act on the first click, a highlight
    // rule must be inert until the user configures an axis.
    QVERIFY(!priorityEnable(pane)->isChecked());
    QVERIFY(!axis(pane, "subsystemGroup")->isChecked());
}

// The time axis asks for a bound in the units the timestamp column is showing, and a
// highlight rule's axis is the SAME widget as a filter's, so it does it here too — with
// the rule still storing wall clock, because a rule is portable and a count of seconds
// is relative to this file's baseline (tst_filterpane holds the units cases themselves).
void TestHighlighterPane::aTimeBoundIsTypedInTheColumnsOwnUnits()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    auto *date = pane.findChild<QDateTimeEdit *>(QStringLiteral("timeStart"));
    auto *secs = pane.findChild<QDoubleSpinBox *>(QStringLiteral("timeStartSeconds"));
    QVERIFY(date && secs);
    QVERIFY(secs->isHidden());

    doc.setTimeDisplay(TimeDisplay::EpochSeconds);
    pane.refreshTimeBounds();
    QVERIFY(date->isHidden());
    QVERIFY(!secs->isHidden());

    const qint64 chosen = doc.index().records.last().timestamp;
    secs->setValue(double(chosen) / 1000.0);
    axis(pane, "timeGroup")->setChecked(true);

    // Stored as the wall clock of that instant in the file's zone (UTC here, both as
    // written and as displayed) — the number the user typed reaches the rule as a date.
    const HighlightRule &r = doc.highlighters().rules.first();
    QVERIFY(r.match.timeEnabled);
    QCOMPARE(r.match.start.date(), QDate(2026, 7, 21));
    QCOMPARE(r.match.start.time(), QTime(12, 0, 1));
}

void TestHighlighterPane::typingARegexReachesTheDocument()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();
    QCOMPARE(doc.highlighters().rules.size(), 1);

    axis(pane, "messageGroup")->setChecked(true);
    QLineEdit *edit = patternEdit(pane);
    QVERIFY(edit);
    edit->setText(QStringLiteral("timeout.*retry"));
    pane.findChild<QAbstractButton *>(QStringLiteral("messageRegex"))->setChecked(true);

    const HighlightRule &r = doc.highlighters().rules.first();
    QVERIFY(r.match.text.enabled);
    QVERIFY(r.match.text.matcher.isRegex());
    QCOMPARE(r.match.text.matcher.pattern(), QStringLiteral("timeout.*retry"));

    // And it round-trips through the pane's portable snapshot.
    const QJsonObject match = pane.saveState()
                                  .value(QStringLiteral("rules"))
                                  .toArray()
                                  .first()
                                  .toObject()
                                  .value(QStringLiteral("match"))
                                  .toObject();
    QCOMPARE(match.value(QStringLiteral("text")).toString(), QStringLiteral("timeout.*retry"));
    QVERIFY(match.value(QStringLiteral("textRegex")).toBool());
}

void TestHighlighterPane::switchingRulesShowsThatRulesSelection()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    QPushButton *add = button(pane, QStringLiteral("ruleNew"));
    add->click();
    add->click();
    QCOMPARE(doc.highlighters().rules.size(), 2);

    // Rule 1 (the second, currently selected) matches only db.pool.
    axis(pane, "subsystemGroup")->setChecked(true);
    QListWidget *loggers = listContaining(pane, QStringLiteral("db.pool"));
    QVERIFY(loggers);
    check(loggers, QStringLiteral("net.socket"), false);
    check(loggers, QStringLiteral("db.pool"), true);
    QCOMPARE(doc.highlighters().rules.at(1).match.loggerNames,
             QStringList{QStringLiteral("db.pool")});

    // Go back to rule 0, which never had the subsystem axis on. It must show its own
    // (empty) selection rather than inheriting rule 1's, and must not acquire one
    // just by being looked at.
    QTableWidget *rules = ruleTable(pane);
    QVERIFY(rules);
    QCOMPARE(rules->rowCount(), 2);
    selectRule(pane, 0);

    QVERIFY(!axis(pane, "subsystemGroup")->isChecked());
    QVERIFY(!isChecked(loggers, QStringLiteral("db.pool")));
    QVERIFY(doc.highlighters().rules.at(0).match.loggerNames.isEmpty());

    // Rule 1 still has its own.
    QCOMPARE(doc.highlighters().rules.at(1).match.loggerNames,
             QStringList{QStringLiteral("db.pool")});
}

void TestHighlighterPane::invalidRegexIsFlagged()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    axis(pane, "messageGroup")->setChecked(true);
    pane.findChild<QAbstractButton *>(QStringLiteral("messageRegex"))->setChecked(true);
    QLineEdit *edit = patternEdit(pane);
    edit->setText(QStringLiteral("("));

    // A regex that will not compile matches nothing, so the rule silently colors
    // nothing. Say so rather than leaving the user to infer it.
    QVERIFY(!edit->toolTip().isEmpty());
    QVERIFY(!doc.highlighters().rules.first().match.text.matcher.isValid());

    edit->setText(QStringLiteral("(ok)"));
    QVERIFY(edit->toolTip().isEmpty());
    QVERIFY(doc.highlighters().rules.first().match.text.matcher.isValid());
}

void TestHighlighterPane::addedRuleIsInertUntilConfigured()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    // With nothing selected, New makes an EMPTY rule: every axis off, so it matches
    // nothing at all — not everything, which is what an "all axes inactive" filter
    // would mean. It still takes a colour, so it is visible in the list on arrival.
    QCOMPARE(doc.highlighters().rules.size(), 1);
    const HighlightRule &fresh = doc.highlighters().rules.first();
    QVERIFY(!fresh.match.anyActive());
    QVERIFY(!doc.highlighters().anyEnabled());
    QVERIFY(HighlightPalette::isSlot(fresh.background));
    QCOMPARE(fresh.foreground, HighlightPalette::readableTextSlot(fresh.background));

    // And configuring an axis is what wakes it up.
    priorityEnable(pane)->setChecked(true);
    QVERIFY(doc.highlighters().anyEnabled());
    QVERIFY(doc.highlighters().rules.first().match.anyActive());
}

void TestHighlighterPane::newCopiesTheSelectedRule()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    QPushButton *newBtn = button(pane, QStringLiteral("ruleNew"));
    QVERIFY(newBtn);
    newBtn->click();

    // Configure rule 0, then ask for another. A second rule is nearly always a variant
    // of the one on screen, so New starts from it — criteria and colours alike — rather
    // than from a blank the user has to rebuild.
    axis(pane, "subsystemGroup")->setChecked(true);
    QListWidget *loggers = listContaining(pane, QStringLiteral("db.pool"));
    QVERIFY(loggers);
    check(loggers, QStringLiteral("net.socket"), false);
    check(loggers, QStringLiteral("db.pool"), true);
    priorityEnable(pane)->setChecked(true);

    const HighlightRule source = doc.highlighters().rules.first();
    newBtn->click();

    QCOMPARE(doc.highlighters().rules.size(), 2);
    const HighlightRule &copy = doc.highlighters().rules.at(1);
    QCOMPARE(copy.match.loggerEnabled, source.match.loggerEnabled);
    QCOMPARE(copy.match.loggerNames, source.match.loggerNames);
    QCOMPARE(copy.match.priorityEnabled, source.match.priorityEnabled);
    QCOMPARE(copy.match.minPriority, source.match.minPriority);
    QCOMPARE(copy.background, source.background);
    QCOMPARE(copy.foreground, source.foreground);
    // The copy is the one being edited, so the editor is already showing it.
    QCOMPARE(ruleTable(pane)->currentRow(), 1);

    // A copy of a rule the user switched OFF still arrives on: a rule that appeared
    // dead on the click that asked for it would read as the button having failed.
    setEnabled(pane, 1, false);
    QVERIFY(!doc.highlighters().rules.at(1).enabled);
    selectRule(pane, 1);
    newBtn->click();
    QCOMPARE(doc.highlighters().rules.size(), 3);
    QVERIFY(doc.highlighters().rules.at(2).enabled);
}

void TestHighlighterPane::tableRowsWearTheirRuleColours()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);

    MatchCriteria c;
    c.priorityEnabled = true;
    c.minPriority = Priority::Error;
    pane.addRule(c);

    // The row is a preview of the rule, not a description of it: its summary cell is
    // painted in the rule's own two palette slots, resolved for the theme the pane is
    // showing. Only that cell — a tick painted on a Deep fill stops being legible, and
    // a swatch picker sitting on the colour it offers says nothing at all.
    QTableWidget *table = ruleTable(pane);
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 1);
    const HighlightRule &r = doc.highlighters().rules.first();
    const bool dark = pane.palette().base().color().lightness()
                      < pane.palette().text().color().lightness();
    QTableWidgetItem *summary = table->item(0, HighlighterPane::kColRule);
    QVERIFY(summary);
    QCOMPARE(summary->background().color(), HighlightPalette::color(r.background, dark));
    QCOMPARE(summary->foreground().color(), HighlightPalette::color(r.foreground, dark));
    QCOMPARE(table->item(0, HighlighterPane::kColEnabled)->background(), QBrush());

    // ...and it follows that row's own colour picker, which is an edit like any other.
    QComboBox *bg = swatch(pane, 0, HighlighterPane::ColourRole::Background);
    QVERIFY(bg);
    const int other = (r.background + 3) % HighlightPalette::count();
    bg->setCurrentIndex(bg->findData(other));
    QCOMPARE(doc.highlighters().rules.first().background, other);
    QCOMPARE(summary->background().color(), HighlightPalette::color(other, dark));
}

// Clear, and the dock marker beside it. The marker asks whether this log's rules are
// still the ones loftail seeds (HighlighterPane::hasCustomRules) — NOT whether there
// are any, which was true of every log from the moment it opened once a fresh log
// started arriving with three default rules.
void TestHighlighterPane::clearRemovesEveryRuleAndTheTabStaysMarked()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));
    // A freshly opened log as MainWindow hands one over: the seeded rules, which are
    // rules the reader has not set.
    doc.highlighters() = HighlighterSet::defaults();

    HighlighterPane pane;
    QSignalSpy activity(&pane, &HighlighterPane::activityChanged);
    pane.setDocument(&doc);

    QPushButton *clear = button(pane, QStringLiteral("ruleClear"));
    QVERIFY(clear);
    // Something to clear, and nothing to report: three rules are colouring the log and
    // the tab says nothing, because nobody chose them.
    QVERIFY(clear->isEnabled());
    QVERIFY(!pane.hasCustomRules());
    QCOMPARE(activity.count(), 0);

    MatchCriteria c;
    c.priorityEnabled = true;
    c.minPriority = Priority::Error;
    pane.addRule(c);
    pane.addRule(c);
    QVERIFY(pane.hasCustomRules());
    QVERIFY(clear->isEnabled());
    QCOMPARE(activity.count(), 1);
    QCOMPARE(activity.takeFirst().at(0).toBool(), true);

    // One action back to an uncoloured log, where before it was Remove per rule.
    clear->click();
    QVERIFY(doc.highlighters().rules.isEmpty());
    QVERIFY(!clear->isEnabled());
    // And the tab stays marked, deliberately: an emptied list is as much a departure
    // from the seed as an extra rule is, and it is the only thing on screen that
    // explains a log whose ERRORs are not red when the next tab's are. So the marker
    // never went out and nothing was emitted.
    QVERIFY(pane.hasCustomRules());
    QCOMPARE(activity.count(), 0);

    // Edge-triggered: adding to a pane that is already reporting custom rules must not
    // rewrite the dock title, which is a QTabBar entry while the panes are tabbed.
    pane.addRule(c);
    pane.addRule(c);
    QCOMPARE(activity.count(), 0);

    // Putting the seed back exactly puts the marker out — the answer is the list, not
    // a latch on having ever edited it.
    doc.highlighters() = HighlighterSet::defaults();
    pane.setDocument(&doc);
    QVERIFY(!pane.hasCustomRules());
    QCOMPARE(activity.count(), 1);
    QCOMPARE(activity.takeFirst().at(0).toBool(), false);
}

void TestHighlighterPane::swatchMenuIsBandedAndFitsAShortScreen()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click(); // a picker belongs to a rule's row

    for (HighlighterPane::ColourRole role : {HighlighterPane::ColourRole::Background,
                                             HighlighterPane::ColourRole::Foreground}) {
        QComboBox *combo = swatch(pane, 0, role);
        QVERIFY(combo);
        // Default, twenty-seven slots, and a separator between each pair of tone
        // bands. The separators are what make a list this long readable, and they
        // must stay unselectable data-less rows so currentData() is either a slot
        // index or the default sentinel and never a stray QVariant.
        QCOMPARE(combo->count(), 1 + HighlightPalette::count() + (HighlightPalette::kBandCount - 1));
        int found = 0, separators = 0;
        for (int row = 0; row < combo->count(); ++row) {
            const QVariant v = combo->itemData(row);
            if (!v.isValid()) {
                ++separators;
                continue;
            }
            if (v.toInt() != HighlightPalette::kDefault) {
                QCOMPARE(v.toInt(), found);   // in table order, so findData() is exact
                ++found;
            }
        }
        QCOMPARE(found, HighlightPalette::count());
        QCOMPARE(separators, HighlightPalette::kBandCount - 1);

        // Every slot is still reachable by index, separators notwithstanding — this is
        // what setSwatchCombo() relies on to show a loaded rule's colour.
        for (int i = 0; i < HighlightPalette::count(); ++i)
            QVERIFY(combo->findData(i) >= 0);

        // The popup must not need more room than a short screen has. Qt scrolls past
        // maxVisibleItems, so the bound is that cap and not the item count; without it
        // twenty-seven swatches plus chrome run off a 768-high display.
        QVERIFY2(combo->maxVisibleItems() < combo->count(),
                 "the popup would try to show every slot at once");
        const int rowHeight = combo->view()->sizeHintForRow(0);
        QVERIFY(rowHeight > 0);
        QVERIFY2(combo->maxVisibleItems() * rowHeight < 600,
                 qPrintable(QStringLiteral("popup wants %1 px")
                                .arg(combo->maxVisibleItems() * rowHeight)));
    }
}

void TestHighlighterPane::aSwatchPreviewsTheRulesPair()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click(); // a picker belongs to a rule's row

    QComboBox *text = swatch(pane, 0, HighlighterPane::ColourRole::Foreground);
    QComboBox *back = swatch(pane, 0, HighlighterPane::ColourRole::Background);
    QVERIFY(text && back);

    // The pane's own cue for the theme, so the expected colours are the variants it drew.
    const bool dark = pane.palette().base().color().lightness()
                      < pane.palette().text().color().lightness();
    const int textSlot = HighlightPalette::kPaper;
    const int backSlot = 0; // Deep Red — nothing like Paper in either theme
    text->setCurrentIndex(text->findData(textSlot));
    back->setCurrentIndex(back->findData(backSlot));

    const auto iconOf = [](QComboBox *combo, int slot) {
        return combo->itemIcon(combo->findData(slot)).pixmap(combo->iconSize()).toImage();
    };
    // Antialiased strokes, so "this colour was painted here" is a near miss, not an
    // equality: the letterform's core lands exact and its edges blend into the tile.
    const auto carries = [](const QImage &image, const QColor &c) {
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor px = image.pixelColor(x, y);
                if (qAbs(px.red() - c.red()) + qAbs(px.green() - c.green())
                        + qAbs(px.blue() - c.blue()) <= 12)
                    return true;
            }
        }
        return false;
    };

    // Every item, in EITHER picker, is drawn as the whole pair — the letter in the
    // rule's text colour on a tile of its background — so what an item shows is what a
    // matching record will look like if it is chosen. Only which half the item varies
    // differs between the two pickers.
    const QColor fg = HighlightPalette::color(textSlot, dark);
    const QColor bg = HighlightPalette::color(backSlot, dark);
    QVERIFY2(carries(iconOf(text, textSlot), bg), "the text picker ignores the background");
    QVERIFY2(carries(iconOf(text, textSlot), fg), "the text picker does not show its colour");
    QVERIFY2(carries(iconOf(back, backSlot), fg), "the background picker shows no text colour");
    QVERIFY2(carries(iconOf(back, backSlot), bg), "the background picker does not show its colour");

    // Which means a choice in one picker invalidates the OTHER's entire list: it is the
    // ground (or the ink) every one of those items is previewed against. Repainting on
    // the way in is not enough — the two are edited in either order.
    const QImage textBefore = iconOf(text, HighlightPalette::kInk);
    back->setCurrentIndex(back->findData(HighlightPalette::kPaper));
    QVERIFY2(iconOf(text, HighlightPalette::kInk) != textBefore,
             "choosing a background left the text picker previewing the old one");

    const QImage backBefore = iconOf(back, 9); // Vivid Red
    text->setCurrentIndex(text->findData(HighlightPalette::kInk));
    QVERIFY2(iconOf(back, 9) != backBefore,
             "choosing a text colour left the background picker previewing the old one");
}

// Both pickers preview the same pair — that is what makes an entry show what a matching
// record will look like — so whatever tells the two apart cannot be a colour. It is the
// tile's GEOMETRY, which the preview leaves free: the text picker fills its icon edge to
// edge (there the field is context, and the answer is the letter and the bar under it),
// while the background picker draws the colour as an inset chip, which is the discrete
// block of colour it is what chooses. Painted, never lettered, and neither mark depends
// on the theme.
//
// The claim is quantitative because the complaint was: a bar 2 px tall moved about a
// tenth of a 14 px icon, and a mark that small is one a reader has to go looking for.
void TestHighlighterPane::theTwoPickersAreTellableApartAtTheSizeTheyAreDrawn()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    QComboBox *text = swatch(pane, 0, HighlighterPane::ColourRole::Foreground);
    QComboBox *back = swatch(pane, 0, HighlighterPane::ColourRole::Background);
    QVERIFY(text && back);
    text->setCurrentIndex(text->findData(HighlightPalette::kPaper));
    back->setCurrentIndex(back->findData(0)); // Deep Red

    const QImage textIcon = swatchShown(pane, 0, HighlighterPane::ColourRole::Foreground);
    const QImage backIcon = swatchShown(pane, 0, HighlighterPane::ColourRole::Background);
    QCOMPARE(textIcon.size(), text->iconSize());
    QCOMPARE(backIcon.size(), back->iconSize());

    const int total = textIcon.width() * textIcon.height();
    const int differ = differingPixels(textIcon, backIcon);
    qInfo("the two pickers differ in %d of %d pixels at %d px", differ, total,
          textIcon.width());
    // A third of the icon — the chip's whole margin, plus the bar, plus where the two
    // letters sit — at the size the row is actually drawn at.
    QVERIFY2(differ * 3 >= total,
             qPrintable(QStringLiteral("only %1 of %2 pixels tell the pickers apart")
                            .arg(differ)
                            .arg(total)));

    // And the distinction is not bought by giving up the preview: choosing a background
    // still repaints BOTH pickers, because it is the ground either one previews on.
    back->setCurrentIndex(back->findData(HighlightPalette::kPaper));
    const QImage textAfter = swatchShown(pane, 0, HighlighterPane::ColourRole::Foreground);
    const QImage backAfter = swatchShown(pane, 0, HighlighterPane::ColourRole::Background);
    QVERIFY2(differingPixels(textAfter, textIcon) > 0,
             "choosing a background left the text picker previewing the old one");
    QVERIFY2(differingPixels(backAfter, backIcon) > 0,
             "choosing a background left the background picker where it was");
    // Still tellable apart on the new pair, which is what "does not depend on the
    // colours" means: Paper on Paper is the worst case there is.
    // And this is the case that matters, because it is the one the mark has to survive:
    // Paper text on a Paper background hides the letter and the bar alike, both being
    // drawn in the text colour. That used to leave the two icons PIXEL-IDENTICAL — 0 of
    // 196 — which is the complaint in its sharpest form. The chip's margin is a shape and
    // owes the pair nothing.
    const int flat = differingPixels(textAfter, backAfter);
    qInfo("with no contrast in the pair they differ in %d of %d", flat, total);
    QVERIFY2(flat * 3 >= total,
             qPrintable(QStringLiteral("only %1 of %2 pixels tell the pickers apart once "
                                       "the pair stops contrasting")
                            .arg(flat)
                            .arg(total)));
}

// *Default* is the one entry that is NOT previewed: previewed, the theme's own text on
// the theme's own base is what Ink-on-Paper already looks like, and the one thing this
// entry has to say is that it names no colour at all. The role mark applies to it like
// any other entry — the outline is the role's own shape — so the two pickers stay
// tellable apart at the entry where they have the least to draw.
void TestHighlighterPane::aDefaultEntryStillNamesNoColourInEitherPicker()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    QComboBox *text = swatch(pane, 0, HighlighterPane::ColourRole::Foreground);
    QComboBox *back = swatch(pane, 0, HighlighterPane::ColourRole::Background);
    QVERIFY(text && back);
    text->setCurrentIndex(text->findData(HighlightPalette::kPaper));
    back->setCurrentIndex(back->findData(0)); // Deep Red

    const bool dark = pane.palette().base().color().lightness()
                      < pane.palette().text().color().lightness();
    const QColor fg = HighlightPalette::color(HighlightPalette::kPaper, dark);
    const QColor bg = HighlightPalette::color(0, dark);

    for (HighlighterPane::ColourRole role : {HighlighterPane::ColourRole::Foreground,
                                             HighlighterPane::ColourRole::Background}) {
        const QImage empty = swatchEntry(pane, 0, role, HighlightPalette::kDefault);
        QVERIFY(!empty.isNull());
        // Nothing in it is painted solid: it is an outline with a stroke through it, at
        // partial alpha, so no part of it can be read as a colour the record would get.
        int solid = 0;
        for (int y = 0; y < empty.height(); ++y)
            for (int x = 0; x < empty.width(); ++x)
                if (empty.pixelColor(x, y).alpha() > 200)
                    ++solid;
        QCOMPARE(solid, 0);
        QVERIFY2(!carriesColour(empty, bg, 100), "the *default* entry shows a colour");
        QVERIFY2(!carriesColour(empty, fg, 100), "the *default* entry shows a colour");
    }

    // Which leaves the role mark as the only thing in them, and it still has to say
    // which picker this is.
    const QImage textEmpty =
        swatchEntry(pane, 0, HighlighterPane::ColourRole::Foreground, HighlightPalette::kDefault);
    const QImage backEmpty =
        swatchEntry(pane, 0, HighlighterPane::ColourRole::Background, HighlightPalette::kDefault);
    const int differ = differingPixels(textEmpty, backEmpty);
    qInfo("the two *default* entries differ in %d pixels", differ);
    QVERIFY2(differ >= 24,
             qPrintable(QStringLiteral("the two roles' *default* entries differ in %1 pixels")
                            .arg(differ)));
}

void TestHighlighterPane::oneClickRuleSetsBothColours()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);

    // The record menu's "Highlight This Subsystem" path. It must set the TEXT colour
    // too: *default* text is unreadable on roughly half the palette now that a
    // background may be Deep or Soft by choice, and which half flips with the theme.
    MatchCriteria c;
    c.loggerEnabled = true;
    c.loggerNames = QStringList{QStringLiteral("db.pool")};
    c.loggerCoversAll = false;
    c.loggerRestrictive = true;
    pane.addRule(c);
    pane.addRule(c);

    QCOMPARE(doc.highlighters().rules.size(), 2);
    QVector<int> backgrounds;
    for (const HighlightRule &r : doc.highlighters().rules) {
        QVERIFY(HighlightPalette::isSlot(r.background));
        // Never a neutral: Ink, Gray and Paper are text colours, not highlights.
        QVERIFY(!HighlightPalette::isNeutral(r.background));
        QCOMPARE(r.foreground, HighlightPalette::readableTextSlot(r.background));
        backgrounds.append(r.background);
    }
    // And two one-click rules are told apart at a glance.
    QVERIFY(backgrounds.at(0) != backgrounds.at(1));
}

void TestHighlighterPane::reloadingTheListKeepsRulesEnabled()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);

    // Adding a SECOND rule used to switch every rule off, silently: the rebuild drops
    // the rows, which drops the current one, which ran loadEditorFor() — and that used
    // to end by forcing m_updating to false, unguarding the rest of the rebuild. The
    // builder's own setCheckState was then read back as a user edit and wrote
    // enabled=false through the reference the next line reads. One rule was fine
    // (clearing an empty list emits nothing), which is what hid it. The rules are a
    // table now and the builder has exactly the same shape, so the case still holds.
    MatchCriteria c;
    c.priorityEnabled = true;
    c.minPriority = Priority::Error;
    for (int n = 0; n < 3; ++n) {
        pane.addRule(c);
        QCOMPARE(doc.highlighters().rules.size(), n + 1);
        for (const HighlightRule &r : doc.highlighters().rules)
            QVERIFY2(r.enabled, qPrintable(QStringLiteral("rule off after %1 adds").arg(n + 1)));
        QVERIFY(doc.highlighters().anyEnabled());
    }

    // A rule the user turned OFF still survives a rebuild as off — the guard must
    // suppress the rebuild's own signals, not freeze the flag.
    setEnabled(pane, 1, false);
    QVERIFY(!doc.highlighters().rules.at(1).enabled);
    pane.addRule(c);
    QVERIFY(doc.highlighters().rules.at(0).enabled);
    QVERIFY(!doc.highlighters().rules.at(1).enabled);
    QVERIFY(doc.highlighters().rules.at(3).enabled);
}

// --- A rule's four actions, edited in the rule table --------------------------

void TestHighlighterPane::everyActionIsAColumnAndOnlyColourStartsOn()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);

    // Six columns, in the order a rule is read: is it on, what does it match, and then
    // what it does about it — both colour roles in ONE cell, since they are one answer
    // in two halves, then a button per remaining action.
    QTableWidget *table = ruleTable(pane);
    QVERIFY(table);
    QCOMPARE(table->columnCount(), int(HighlighterPane::kColumnCount));
    // And NO header row. Five of the six columns were named by a glyph in it, which is a
    // row of the pane's scarcest resource spent on a legend for icons that are now in the
    // cells themselves.
    QVERIFY2(table->horizontalHeader()->isHidden(), "the header row is back");
    // Only the rule's own summary takes the spare width; every other column is fixed to
    // the widget it holds, since a cell widget contributes nothing to ResizeToContents
    // and there is no header item left to measure instead.
    QHeaderView *head = table->horizontalHeader();
    QCOMPARE(head->sectionResizeMode(HighlighterPane::kColRule), QHeaderView::Stretch);
    for (int column : {int(HighlighterPane::kColEnabled), int(HighlighterPane::kColColours),
                       int(HighlighterPane::kColDigest), int(HighlighterPane::kColNotify),
                       int(HighlighterPane::kColTab)}) {
        QCOMPARE(head->sectionResizeMode(column), QHeaderView::Fixed);
        QVERIFY(table->columnWidth(column) > 0);
    }

    button(pane, QStringLiteral("ruleNew"))->click();

    // A new rule colours and nothing else — what a rule has always done, and the only
    // action with a self-evident meaning before it has been configured.
    QCOMPARE(doc.highlighters().rules.size(), 1);
    QCOMPARE(doc.highlighters().rules.first().actions,
             HighlightActions(HighlightAction::Color));

    // Colour is not a button: the two swatch pickers ARE the control, so the row shows
    // the colours it paints with and the three action buttons start up.
    QVERIFY(swatch(pane, 0, HighlighterPane::ColourRole::Foreground));
    QVERIFY(swatch(pane, 0, HighlighterPane::ColourRole::Background));
    QVERIFY(ruleIsEnabled(pane, 0));
    QVERIFY(!actionIsOn(pane, 0, HighlighterPane::kColDigest));
    QVERIFY(!actionIsOn(pane, 0, HighlighterPane::kColTab));
    QVERIFY(!actionIsOn(pane, 0, HighlighterPane::kColNotify));

    // Each action is a checkable icon button that says what it is where it is asked —
    // there is no header left to name the column, so the glyph, the tooltip and the
    // accessible name are the whole of it.
    for (HighlighterPane::Column column : {HighlighterPane::kColDigest,
                                           HighlighterPane::kColNotify,
                                           HighlighterPane::kColTab}) {
        QToolButton *b = actionButton(pane, 0, column);
        QVERIFY(b);
        QVERIFY(b->isCheckable());
        QVERIFY(!b->icon().isNull());
        QVERIFY(!b->toolTip().isEmpty());
        QVERIFY(!b->accessibleName().isEmpty());
        // Framed, never auto-raised: the frame is what says "press this", and a checked
        // state only tells someone who has already worked out that it is a button.
        QVERIFY(!b->autoRaise());
        // No item under the widget — an action is not a tick any more.
        QVERIFY(!table->item(0, column));
    }
    // The rule's own on/off is the exception, and stays a checkbox: it says whether the
    // rule runs at all, and a fourth pressed-in button would read as a fourth action.
    QVERIFY(table->item(0, HighlighterPane::kColEnabled)
                ->flags()
                .testFlag(Qt::ItemIsUserCheckable));
    QVERIFY(!actionButton(pane, 0, HighlighterPane::kColEnabled));
}

// An action button that is ON must not look like one that is off, and the only place
// that can be said is the glyph. A checked QToolButton is a panel a few percent darker
// than an unchecked one — invisible at 14 px in a dock, and worse than invisible in this
// table, because a selected row already tints every button in it: the cue for "this
// action is in force" was the same cue as "this is the rule being edited". So OFF is a
// thin outline in muted ink and ON is the shape filled at full ink, which is a contrast
// no style can take away.
//
// Asserted as INK — the summed alpha of the two renderings — rather than as "the two
// images differ", which a one-pixel change would satisfy while the buttons went on
// looking identical to a reader.
void TestHighlighterPane::anActionThatIsOnLooksIt()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    const auto ink = [](const QImage &image) {
        qint64 total = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                total += qAlpha(image.pixel(x, y));
        return total;
    };

    for (HighlighterPane::Column column : {HighlighterPane::kColDigest,
                                           HighlighterPane::kColNotify,
                                           HighlighterPane::kColTab}) {
        QToolButton *b = actionButton(pane, 0, column);
        QVERIFY(b);
        const QSize size = b->iconSize();
        // Straight off the icon, in both states, without touching the button: Qt asks
        // for QIcon::On while a checkable button is checked and QIcon::Off while it is
        // not, so the swap costs no signal and a rule restored from a session is drawn
        // right without a refresh.
        const QImage off = b->icon().pixmap(size, QIcon::Normal, QIcon::Off).toImage();
        const QImage on = b->icon().pixmap(size, QIcon::Normal, QIcon::On).toImage();
        QVERIFY(!off.isNull() && !on.isNull());
        QVERIFY2(ink(on) > ink(off) * 3 / 2,
                 qPrintable(QStringLiteral("column %1: on=%2 off=%3 ink")
                                .arg(int(column))
                                .arg(ink(on))
                                .arg(ink(off))));
    }
}

void TestHighlighterPane::togglingAnActionReachesTheDocument()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    setAction(pane, 0, HighlighterPane::kColDigest, true);
    QVERIFY(doc.highlighters().rules.first().actions.testFlag(HighlightAction::Digest));

    setAction(pane, 0, HighlighterPane::kColTab, true);
    QVERIFY(doc.highlighters().rules.first().actions.testFlag(HighlightAction::Tab));

    setAction(pane, 0, HighlighterPane::kColDigest, false);
    QVERIFY(!doc.highlighters().rules.first().actions.testFlag(HighlightAction::Digest));
    QVERIFY(doc.highlighters().rules.first().actions.testFlag(HighlightAction::Tab));

    // A second rule's buttons are its own: each carries the row it was built for, so it
    // acts on that rule and not on whichever one the editor below happens to be showing.
    button(pane, QStringLiteral("ruleNew"))->click();
    QCOMPARE(ruleTable(pane)->currentRow(), 1);
    setAction(pane, 0, HighlighterPane::kColDigest, true);
    QVERIFY(doc.highlighters().rules.at(0).actions.testFlag(HighlightAction::Digest));
    QVERIFY(!doc.highlighters().rules.at(1).actions.testFlag(HighlightAction::Digest));
}

// The enable column is a delegate, and both halves of it are invisible to a test that
// sets the check state on the item: the indicator is CENTRED in its cell rather than
// pinned to the cell's left edge, and the whole cell is the hit area rather than the
// 13 px indicator. In a dock column that width, a tick that has to be hit rather than
// aimed at is one the user misses — so the click, not the item, is what this drives.
//
// The three actions beside it need none of that, which is half of why they are buttons:
// a button is its own hit area and centres its own icon. What they need instead is that
// PRESSING one reaches the rule, which is what the second half of this checks.
void TestHighlighterPane::clickingAnywhereInACheckCellTogglesIt()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.resize(360, 700);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    QTableWidget *table = ruleTable(pane);
    QVERIFY(QTest::qWaitFor([table] { return table->width() > 0; }));

    QTableWidgetItem *tick = table->item(0, HighlighterPane::kColEnabled);
    const QRect cell = table->visualItemRect(tick);
    QVERIFY(cell.width() > 0);
    const auto clickAt = [table, cell](int dx) {
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {},
                          QPoint(cell.center().x() + dx, cell.center().y()));
    };

    // Dead centre, then hard against the cell's left edge: both are the same click, and
    // neither is on the indicator Qt would have drawn there.
    clickAt(0);
    QVERIFY(!doc.highlighters().rules.first().enabled);
    clickAt(-cell.width() / 2 + 1);
    QVERIFY(doc.highlighters().rules.first().enabled);

    // The indicator is drawn centred, not against the cell's left border where Qt lays a
    // view item out. What a test can hold is that the column is wider than the indicator
    // by enough for the difference to be visible.
    QStyleOptionViewItem opt;
    opt.initFrom(table);
    opt.rect = cell;
    const int side = table->style()->pixelMetric(QStyle::PM_IndicatorWidth, &opt, table);
    QVERIFY2(cell.width() >= side + 4,
             qPrintable(QStringLiteral("a %1 px column for a %2 px indicator")
                            .arg(cell.width())
                            .arg(side)));

    // And an action button reaches its rule when it is CLICKED, not only when a test
    // sets its checked state: the two go through different Qt paths, and only the click
    // is what the user does.
    QToolButton *digest = actionButton(pane, 0, HighlighterPane::kColDigest);
    QVERIFY(digest);
    QTest::mouseClick(digest, Qt::LeftButton);
    QVERIFY(digest->isChecked());
    QVERIFY(doc.highlighters().rules.first().actions.testFlag(HighlightAction::Digest));
    QTest::mouseClick(digest, Qt::LeftButton);
    QVERIFY(!doc.highlighters().rules.first().actions.testFlag(HighlightAction::Digest));
}

void TestHighlighterPane::aRuleWithNoColourDoesNotColour()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();
    priorityEnable(pane)->setChecked(true); // give it something to match

    QVERIFY(doc.highlighters().rules.first().match.anyActive());

    // *Default* on both roles is what unticking Highlight used to be. With the Action
    // box gone, the swatches are the colour control, so a rule naming no palette entry
    // does not carry HighlightAction::Color — which also stops it shadowing the rule
    // below it for an action it does not perform.
    clearColours(pane, 0);
    const HighlightRule &r = doc.highlighters().rules.first();

    // The rule still MATCHES; it just does nothing about it. That is a legitimate,
    // deliberate state — it is how a rule is parked without being deleted — and it
    // must survive a round trip rather than being read back as "colour, as of old".
    QVERIFY(r.match.anyActive());
    QVERIFY(!r.actions.testFlag(HighlightAction::Color));
    QCOMPARE(HighlightRule::fromJson(r.toJson()).actions, HighlightActions());
}

void TestHighlighterPane::pickingAColourMakesTheRuleColourAgain()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);

    // A rule that arrives claiming to colour while naming no colour is normalised on
    // the way in — otherwise the table shows two empty swatches on a rule that is
    // silently winning the Colour action for every record it matches.
    HighlightRule parked;
    parked.match.priorityEnabled = true;
    parked.match.minPriority = Priority::Error;
    parked.actions = HighlightAction::Color;
    QJsonObject state;
    QJsonArray rules;
    rules.append(parked.toJson());
    state.insert(QStringLiteral("rules"), rules);
    pane.restoreState(state);
    QCOMPARE(doc.highlighters().rules.size(), 1);
    QVERIFY(!doc.highlighters().rules.first().actions.testFlag(HighlightAction::Color));

    // And picking either role puts the action back, with no separate tick to remember.
    QComboBox *bg = swatch(pane, 0, HighlighterPane::ColourRole::Background);
    QVERIFY(bg);
    bg->setCurrentIndex(bg->findData(3));
    QCOMPARE(doc.highlighters().rules.first().background, 3);
    QVERIFY(doc.highlighters().rules.first().actions.testFlag(HighlightAction::Color));

    // Its other actions are untouched by any of this.
    setAction(pane, 0, HighlighterPane::kColDigest, true);
    clearColours(pane, 0);
    QCOMPARE(doc.highlighters().rules.first().actions,
             HighlightActions(HighlightAction::Digest));
}

void TestHighlighterPane::newCopiesTheSelectedRulesActions()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();
    setAction(pane, 0, HighlighterPane::kColDigest, true);
    clearColours(pane, 0);

    // New starts from the SELECTED rule (SPEC.md §7), and its actions are part of what
    // "a variant of the one in front of you" means — including the colours, which are
    // now what says whether it carries the Colour action at all.
    button(pane, QStringLiteral("ruleNew"))->click();
    QCOMPARE(doc.highlighters().rules.size(), 2);
    QCOMPARE(doc.highlighters().rules.at(1).actions,
             HighlightActions(HighlightAction::Digest));
    QVERIFY(doc.highlighters().rules.at(1).enabled); // ...and enabled, as always
}

void TestHighlighterPane::theOneClickRuleColoursOnly()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);

    MatchCriteria c;
    c.priorityEnabled = true;
    c.minPriority = Priority::Error;
    pane.addRule(c);

    // The record menu's one-click rule is a HIGHLIGHT: colour, and nothing the user
    // did not ask for. Digest, a tab marker or a notification arriving from a single
    // menu click would be the application deciding to interrupt on its own.
    QCOMPARE(doc.highlighters().rules.size(), 1);
    QCOMPARE(doc.highlighters().rules.first().actions,
             HighlightActions(HighlightAction::Color));
}

void TestHighlighterPane::reloadingTheListKeepsActions()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);

    // Four rules with distinct action sets, then a rebuild of the table — the companion
    // to reloadingTheListKeepsRulesEnabled(). loadEditorFor() is re-entered by
    // reloadRuleTable() without either saying so, and every action is now set through
    // that rebuild: three check states per row and two swatch pickers, all written by
    // the builder and all firing the same signals a user edit does.
    for (int i = 0; i < 4; ++i)
        button(pane, QStringLiteral("ruleNew"))->click();
    QCOMPARE(doc.highlighters().rules.size(), 4);

    setAction(pane, 0, HighlighterPane::kColDigest, true);
    selectRule(pane, 1);
    setAction(pane, 1, HighlighterPane::kColTab, true);
    clearColours(pane, 1);

    const QVector<HighlightRule> before = doc.highlighters().rules;

    // Force the rebuild the way the application does: a move reloads the whole list.
    button(pane, QStringLiteral("ruleUp"))->click();
    button(pane, QStringLiteral("ruleDown"))->click();

    const QVector<HighlightRule> after = doc.highlighters().rules;
    QCOMPARE(after.size(), before.size());
    for (int i = 0; i < after.size(); ++i) {
        QVERIFY2(after.at(i).actions == before.at(i).actions,
                 qPrintable(QStringLiteral("rule %1 lost its actions").arg(i)));
        QVERIFY2(after.at(i).enabled, qPrintable(QStringLiteral("rule %1 switched off").arg(i)));
    }
}

void TestHighlighterPane::notifySaysWhyWhenTheDesktopOffersNoNotifications()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    // Offscreen — and on a stock GNOME/Wayland session, which is the reference desktop
    // — there is no notification service, so the button is DISABLED and SAYS SO rather
    // than accepting a press that would silently do nothing. Said before it can be
    // pressed, the same habit as naming hosts.json before offering to remember a
    // password. Where a service does exist the button is simply live.
    const bool available = QSystemTrayIcon::isSystemTrayAvailable()
                           && QSystemTrayIcon::supportsMessages();
    QToolButton *notify = actionButton(pane, 0, HighlighterPane::kColNotify);
    QVERIFY(notify);
    QCOMPARE(notify->isEnabled(), available);
    QVERIFY(!notify->toolTip().isEmpty());
    if (!available)
        QVERIFY(notify->toolTip().contains(QStringLiteral("tab")));
    // The other two are untouched by this: one desktop service missing does not disable
    // the actions that never needed it.
    QVERIFY(actionButton(pane, 0, HighlighterPane::kColDigest)->isEnabled());
    QVERIFY(actionButton(pane, 0, HighlighterPane::kColTab)->isEnabled());

    // A rule that arrived from a preset or another machine carrying Notify still SHOWS
    // it here rather than being quietly rewritten by a desktop that cannot deliver it.
    HighlightRule loud;
    loud.match.priorityEnabled = true;
    loud.actions = HighlightAction::Notify;
    doc.highlighters().rules = {loud};
    pane.setDocument(&doc);
    QVERIFY(actionIsOn(pane, 0, HighlighterPane::kColNotify));
    QCOMPARE(doc.highlighters().rules.first().actions,
             HighlightActions(HighlightAction::Notify));
}

// A switched-off axis keeps its controls on screen, greyed — the pane used to collapse
// each one to its title row to save height, and height was the wrong thing to buy here
// for the same reason it was wrong in the Filters pane: an axis whose controls appear
// only once it is ticked cannot be read, only explored.
void TestHighlighterPane::switchedOffAxesStayVisibleAndGreyed()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click(); // the editor is live only with a rule

    QGroupBox *message = axis(pane, "messageGroup");
    QVERIFY(message && !message->isChecked());
    // The body is everything under the title row; the title row is the check control.
    const auto *field = patternEdit(pane);
    QVERIFY(field);
    QVERIFY(field->isVisibleTo(message));
    // Qt greys a checkable group box's contents while it is unchecked, which is what
    // carries "not in force" now that nothing is hidden.
    QVERIFY(!field->isEnabled());

    message->setChecked(true);
    QVERIFY(field->isVisibleTo(message));
    QVERIFY(field->isEnabled());

    // And the subsystem list, whose axis is off too and whose body is the tallest of
    // the five — the one collapsing was really buying.
    const auto *list = pane.findChild<QListWidget *>(QStringLiteral("subsystemList"));
    QVERIFY(list);
    QVERIFY(!axis(pane, "subsystemGroup")->isChecked());
    QVERIFY(list->isVisibleTo(axis(pane, "subsystemGroup")));
}

// A format with no %t leaves the thread axis out of the rule editor entirely. It is the
// AxisEditor's own behaviour and not a per-pane flag any more — the Filters pane showed
// such an axis greyed with the reason in its title until it stopped, so the two panes can
// no longer drift on it (AxisEditor::updateAxisState, tst_filterpane has the twin).
void TestHighlighterPane::anAxisTheFormatLacksIsNotShownAtAll()
{
    Document withThread;
    QTemporaryFile threaded;
    QVERIFY(openLog(withThread, threaded));

    Document noThread;
    QTemporaryFile plain;
    QVERIFY(plain.open());
    plain.write("2026-07-21 12:00:00,000 INFO  net.socket - a\n");
    plain.flush();
    QVERIFY2(noThread.open(plain.fileName(),
                           QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} %-5p %c - %m%n"),
                           Encoding::Utf8, QTimeZone::utc()),
             qPrintable(noThread.lastError()));

    HighlighterPane pane;
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    pane.setDocument(&withThread);
    QGroupBox *thread = axis(pane, "threadGroup");
    QVERIFY(thread);
    QVERIFY(thread->isVisible());
    // The axes the format DOES carry are untouched by this.
    QVERIFY(axis(pane, "subsystemGroup")->isVisible());
    QVERIFY(axis(pane, "timeGroup")->isVisible());

    // Rebinding to a log whose pattern has no %t takes the axis off screen — and puts
    // it back when a log that has one is selected again.
    pane.setDocument(&noThread);
    QVERIFY(!thread->isVisible());
    QVERIFY(axis(pane, "subsystemGroup")->isVisible());

    pane.setDocument(&withThread);
    QVERIFY(thread->isVisible());
}

// The two colour pickers belong to the ROW they colour, and share ONE cell: the colours
// are a per-rule answer like every other, and read down the list they say what the log
// will look like without selecting a rule at a time. Each is icon-only, which is what
// makes two of them affordable in a dock — a picker showing "Vivid Amber" twice over
// would take the width the rule's own summary needs. Their swatches differ by SHAPE, a
// letter against a filled tile, because with no header row nothing else says which role
// a picker sets.
void TestHighlighterPane::theTwoColourPickersAreCellsOfTheirOwnRow()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.resize(360, 700);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();
    button(pane, QStringLiteral("ruleNew"))->click();

    QTableWidget *table = ruleTable(pane);
    QVERIFY(QTest::qWaitFor([table] { return table->width() > 0; }));

    for (int row = 0; row < 2; ++row) {
        QComboBox *fg = swatch(pane, row, HighlighterPane::ColourRole::Foreground);
        QComboBox *bg = swatch(pane, row, HighlighterPane::ColourRole::Background);
        QVERIFY(fg && bg);
        // Both in ONE cell — one answer in two halves, and a column each spent that
        // answer's width twice over — text before background, because a record is read
        // as text on a background.
        QCOMPARE(fg->parentWidget(), bg->parentWidget());
        QCOMPARE(fg->parentWidget(), table->cellWidget(row, HighlighterPane::kColColours));
        QCOMPARE(fg->geometry().center().y(), bg->geometry().center().y());
        QVERIFY(fg->x() < bg->x());
        QCOMPARE(bg->width(), fg->width());
        // Icon-only: no room is spent on a palette name that the popup and the tooltip
        // both give. The bound is generous; a combo sized from "Vivid Amber" clears it.
        QVERIFY2(fg->width() < 60, qPrintable(QStringLiteral("picker is %1 px").arg(fg->width())));
        // Which is not to say it is unlabelled — it says what it is where it is asked.
        QVERIFY(!fg->accessibleName().isEmpty());
        QVERIFY(!bg->toolTip().isEmpty());
    }

    // Two rules, four pickers: each row carries its own pair rather than the pane
    // holding one pair that follows the selection.
    QVERIFY(swatch(pane, 0, HighlighterPane::ColourRole::Foreground)
            != swatch(pane, 1, HighlighterPane::ColourRole::Foreground));
    // The summary column is what absorbs the pane's width, so the pickers stay put as
    // the dock is resized.
    QVERIFY(table->columnWidth(HighlighterPane::kColRule)
            > table->columnWidth(HighlighterPane::kColColours));
}

// What is left below the table is the CONDITION, and nothing captions it. The editor used
// to be two labelled halves — Condition round the five axes, Action round the four
// actions — and with the actions in the table there is no second half to tell the first
// one apart from, so both captions are gone rather than one left naming everything there
// is.
void TestHighlighterPane::theEditorIsTheConditionAlone()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.resize(360, 900);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    QVERIFY2(!pane.findChild<SectionBox *>(QStringLiteral("conditionSection")),
             "the editor has grown a caption back");
    QVERIFY2(!pane.findChild<SectionBox *>(QStringLiteral("actionSection")),
             "the actions are in the rule table now");
    // And the editor as a whole carries no caption either: the table above it already
    // says which rule is being edited. So the widget the scroll area hangs off must be a
    // plain one — an empty-titled QGroupBox would still spend a frame and a title row.
    auto *scroll = pane.findChild<QScrollArea *>();
    QVERIFY(scroll);
    QVERIFY2(!qobject_cast<QGroupBox *>(scroll->parentWidget()),
             "the rule editor has grown a caption back");

    // Nothing in it is framed. Every axis is a flat SectionBox drawing a hairline along
    // its own title row instead — a frame round each of five stacked sections inside a
    // dock that is a frame already is where a border stops meaning "these belong
    // together".
    //
    // The line is PAINTED, not a QFrame in the layout, because a group box's title row is
    // drawn by the style and has no cell beside the title to put one in — a QFrame lands
    // under the title instead of alongside it. So what a test can hold is that each
    // section is the kind of box that draws it and has been asked to.
    QList<SectionBox *> rows;
    for (const char *name : {"priorityGroup", "messageGroup", "subsystemGroup",
                             "threadGroup", "timeGroup"}) {
        auto *box = pane.findChild<SectionBox *>(QString::fromLatin1(name));
        QVERIFY2(box, name);
        QVERIFY2(box->isFlat(), name);
        QVERIFY2(box->hasTitleDivider(), name);
        QVERIFY2(!box->isHeading(), name); // a control, not a caption
        // Still a checkable group box, which is the reason it is a subclass and not a
        // hand-built header: the title row is the enable control and Qt greys the body.
        QVERIFY2(box->isCheckable(), name);
        rows << box;
    }

    // One column: same left edge, same width, so five axes read as one stack.
    QVERIFY(QTest::qWaitFor([&rows] { return rows.first()->width() > 0; }));
    for (SectionBox *box : rows) {
        QCOMPARE(box->mapTo(&pane, QPoint(0, 0)).x(),
                 rows.first()->mapTo(&pane, QPoint(0, 0)).x());
        QCOMPARE(box->width(), rows.first()->width());
    }
}

// A section must be at least as tall as the title row it draws. Sounds free; it is not.
// The three body-less actions have no layout, and a QGroupBox with no layout has an
// INVALID sizeHint(), so a layout falls back to minimumSizeHint() — which under Breeze,
// with the title style sheet in play, came back at 20 px for a title running to y=24. The
// three rows silently cut their own descenders, and only on that style: Fusion answered
// 37 px and looked perfect, which is why it took a photograph of a KDE desktop to see.
// The subsystem and thread lists take the pane's spare height, exactly as they do in the
// Filters pane — it is one widget with the stretch already in it (AxisEditor), so what
// this really pins is that nothing here takes that height away from it. The editor sat in
// a container with a trailing addStretch() under it, which is the very thing AxisEditor
// deleted from its own layout: a stretch below the axes claims every spare pixel, and the
// two lists stay at their floors while an empty gap grows under Time range.
void TestHighlighterPane::theValueListsTakeTheSpareHeight()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    // A rule, because the editor is disabled without one — and a disabled widget is
    // still laid out, but the pane it lives in is not worth measuring in a state the
    // user never edits in.
    button(pane, QStringLiteral("ruleNew"))->click();
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    auto settle = [&pane](int h) {
        pane.resize(424, h);
        for (int i = 0; i < 8; ++i)
            QApplication::processEvents();
    };
    auto listHeight = [&pane](const char *name) {
        auto *l = pane.findChild<QListWidget *>(QString::fromLatin1(name));
        return l ? l->height() : -1;
    };
    auto boxHeight = [&pane](const char *name) {
        auto *g = pane.findChild<QGroupBox *>(QString::fromLatin1(name));
        return g ? g->height() : -1;
    };

    settle(760);
    const int sub0 = listHeight("subsystemList"), thr0 = listHeight("threadList");
    const int msg0 = boxHeight("messageGroup"), pri0 = boxHeight("priorityGroup");
    QVERIFY(sub0 > 0 && thr0 > 0);

    settle(1360);
    const int sub1 = listHeight("subsystemList"), thr1 = listHeight("threadList");

    // 600 px more pane means 600 px more list, split between the two.
    QVERIFY2(sub1 > sub0 + 200, qPrintable(QString("subsystem %1 -> %2").arg(sub0).arg(sub1)));
    QVERIFY2(thr1 > thr0 + 200, qPrintable(QString("thread %1 -> %2").arg(thr0).arg(thr1)));

    // Evenly, and by the SAME factor rather than in proportion to how many values each
    // list happens to hold — a split that would otherwise shift under the reader every
    // time the scan turned up another subsystem. Asserted on where the two END UP rather
    // than on how far each moved: the short pane above has both of them on their FLOORS,
    // and a floor is a row taller for the list holding one more name, so the two
    // distances differ by that row while the split itself is exact.
    QVERIFY2(qAbs(sub1 - thr1) <= 4,
             qPrintable(QString("uneven: subsystem %1, thread %2").arg(sub1).arg(thr1)));

    // And nothing else grew: the three fixed axes are the same height in a pane twice as
    // tall, which is what makes the growth the LISTS' rather than everyone's.
    QCOMPARE(boxHeight("messageGroup"), msg0);
    QCOMPARE(boxHeight("priorityGroup"), pri0);

    // A pane too short for both floors scrolls rather than starving either list.
    settle(300);
    auto *scroll = pane.findChild<QScrollArea *>(QStringLiteral("highlighterScroll"));
    QVERIFY(scroll);
    QVERIFY(scroll->verticalScrollBar()->isVisible());
    QVERIFY(listHeight("subsystemList") >= 70);
    QVERIFY(listHeight("threadList") >= 70);
}

void TestHighlighterPane::noSectionClipsItsOwnTitle()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.resize(320, 900);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    const QList<SectionBox *> sections = pane.findChildren<SectionBox *>();
    QVERIFY(!sections.isEmpty());
    QVERIFY(QTest::qWaitFor([&sections] { return sections.first()->height() > 0; }));
    for (SectionBox *box : sections) {
        if (!box->isVisible())
            continue; // an axis this format cannot fill is left out (updateAxisState)
        const QSize title = box->titleRowHint();
        QVERIFY2(box->height() >= title.height(),
                 qPrintable(QStringLiteral("%1 is %2 px tall for a %3 px title row")
                                .arg(box->objectName())
                                .arg(box->height())
                                .arg(title.height())));
    }
}

// The two panes show the SAME five axes, so they must not inset them differently. This
// costs nothing to get wrong and is invisible until someone puts the two docks side by
// side: `AxisEditor` supplies its own kSideMargin, a layout margin in the pane is ADDED
// to it, and this pane's root layout carried the style's default — which put a rule's
// Subsystem box 17 px from the dock edge against a filter's 6.
//
// Asserted against the Filters pane itself rather than against the number, because the
// claim is that they agree, not that either is 6.
void TestHighlighterPane::theAxesSitWhereTheFiltersPanesDo()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane highlighters;
    highlighters.resize(460, 820);
    highlighters.show();
    QVERIFY(QTest::qWaitForWindowExposed(&highlighters));
    highlighters.setDocument(&doc);
    button(highlighters, QStringLiteral("ruleNew"))->click();

    FilterPane filters;
    filters.resize(460, 820);
    filters.show();
    QVERIFY(QTest::qWaitForWindowExposed(&filters));
    filters.setDocument(&doc);

    const auto inset = [](QWidget &pane, const char *name) {
        auto *w = pane.findChild<QWidget *>(QString::fromLatin1(name));
        if (!w)
            return QPair<int, int>(-1, -1);
        const int left = w->mapTo(&pane, QPoint(0, 0)).x();
        return QPair<int, int>(left, pane.width() - (left + w->width()));
    };

    for (const char *name : {"subsystemGroup", "messageGroup", "timeGroup"}) {
        const QPair<int, int> here = inset(highlighters, name);
        const QPair<int, int> there = inset(filters, name);
        QVERIFY2(here.first > 0, name); // and not flush against the dock edge either
        QVERIFY2(here == there,
                 qPrintable(QStringLiteral("%1: highlighters %2/%3, filters %4/%5")
                                .arg(QLatin1String(name))
                                .arg(here.first).arg(here.second)
                                .arg(there.first).arg(there.second)));
    }

    // And the table above the axes shares their left edge, so the pane reads as one
    // column rather than as a table with a differently indented editor under it.
    QCOMPARE(ruleTable(highlighters)->mapTo(&highlighters, QPoint(0, 0)).x(),
             inset(highlighters, "subsystemGroup").first);
}

// --- The empty table, which used to say nothing at all -----------------------
//
// The main window's centre explains itself when it has no file ("No file open. Open a
// log file to begin."); the rule table got an unexplained framed void, with the axis
// editor below it correctly greyed and equally silent. There are TWO empty states and
// they do not want the same words, which is the whole of what these three pin.

void TestHighlighterPane::theEmptyTableSaysThereIsNoFileOpen()
{
    HighlighterPane pane;
    pane.resize(320, 600);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    QLabel *note = placeholder(pane);
    QVERIFY(note);
    QVERIFY(note->isVisible());
    QVERIFY(!note->text().isEmpty());

    // Muted, and from the PALETTE rather than from a constant: the pane is read on a
    // light theme and a dark one, and a grey picked for either is invisible on the other.
    QCOMPARE(note->palette().color(QPalette::WindowText),
             pane.palette().placeholderText().color());

    // It is a label over the viewport, NEVER a row: a row would be a rule to everything
    // that walks rows — the reorder buttons, the per-row "ruleRow" property, saveState().
    QTableWidget *table = ruleTable(pane);
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(pane.saveState().value(QStringLiteral("rules")).toArray().size(), 0);
    QVERIFY(table->viewport()->rect().contains(note->geometry()));
}

void TestHighlighterPane::theEmptyTableAsksForARuleWhileAFileIsOpen()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.resize(320, 600);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));

    QLabel *note = placeholder(pane);
    QVERIFY(note);
    const QString withNoFile = note->text();

    pane.setDocument(&doc);
    QVERIFY(note->isVisible());
    QVERIFY(!note->text().isEmpty());
    // A file IS open here, so "open a log file" would be an instruction the user has
    // already carried out. What is missing is a rule, and the words have to differ.
    QVERIFY2(note->text() != withNoFile, qPrintable(note->text()));
}

void TestHighlighterPane::thePlaceholderGoesAsSoonAsThereIsARuleAndComesBackWithTheLast()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.resize(320, 600);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.setDocument(&doc);

    QLabel *note = placeholder(pane);
    QVERIFY(note);
    QVERIFY(note->isVisible());

    button(pane, QStringLiteral("ruleNew"))->click();
    QCOMPARE(ruleTable(pane)->rowCount(), 1);
    QVERIFY2(!note->isVisible(), "the message is still over a table that has a rule in it");

    // And back when the last one goes — the state a user reaches by emptying the table,
    // which is now the only way to see it with a file open (a fresh log arrives with
    // rules of its own).
    button(pane, QStringLiteral("ruleRemove"))->click();
    QCOMPARE(ruleTable(pane)->rowCount(), 0);
    QVERIFY(note->isVisible());

    // Clear takes the same route, several rules at a time.
    button(pane, QStringLiteral("ruleNew"))->click();
    button(pane, QStringLiteral("ruleNew"))->click();
    QVERIFY(!note->isVisible());
    button(pane, QStringLiteral("ruleClear"))->click();
    QVERIFY(note->isVisible());
}

// Every button under the rule table needs something to act on, and for a long time only
// Clear said so: with no rules, three live buttons sat beside one correctly greyed and
// pressing any of them did nothing. Up and Down have the sharper version of the same
// problem — they are live for a selected rule, but not at the end of the list they would
// move it past. Found by object name, never by their labels, which are translated prose.

// refreshTimeBounds() is called whenever the timestamp column's terms move — a display
// mode change, a source zone change, and a RUN SELECTION, which moves the baseline the
// two seconds modes count from. It used to answer all three by reading the whole of
// AxisEditor::criteria() back into the selected rule, and criteria() is not the inverse
// of setCriteria(): a QDateTimeEdit always holds a datetime, so a rule with no bounds
// came back holding 2000-01-01, and a value axis that is switched off came back claiming
// to cover nothing. Both are compared by MatchCriteria::operator==, so merely clicking a
// run row rewrote this log's seeded rules, lit the Highlighters marker and — through
// commit() → the Document → the session — persisted a state no gesture could undo.
//
// The assertion is on the RULES and not on the marker: a patch that fixed only the
// bounds would leave the rules rewritten and still pass a marker-only test, because
// coversAll would carry the difference on its own.
void TestHighlighterPane::aRerenderThatMovesNoZoneLeavesTheSeededRulesAlone()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));
    doc.highlighters().rules = HighlighterSet::defaults().rules; // what MainWindow seeds

    HighlighterPane pane;
    pane.setDocument(&doc);
    QCOMPARE(ruleTable(pane)->rowCount(), HighlighterSet::defaults().rules.size());
    // A rule IS selected, which is the state the write-back happened in — with none
    // selected the old code wrote nothing and this would prove nothing.
    selectRule(pane, 0);
    QVERIFY(!pane.hasCustomRules());

    // Seconds, then back: neither moves the display zone (Document::recomputeDisplayZone
    // leaves it at the source zone for both), so nothing about a stored bound has become
    // wrong and nothing may be written.
    doc.setTimeDisplay(TimeDisplay::EpochSeconds);
    pane.refreshTimeBounds();
    QCOMPARE(doc.highlighters().rules, HighlighterSet::defaults().rules);
    QVERIFY(!pane.hasCustomRules());

    doc.setTimeDisplay(TimeDisplay::AsWritten);
    pane.refreshTimeBounds();
    QCOMPARE(doc.highlighters().rules, HighlighterSet::defaults().rules);
    QVERIFY(!pane.hasCustomRules());

    // "Seconds from run start" counts from the SELECTED run, so picking a run re-renders
    // the editors through this very function — the gesture the defect was found by.
    doc.setTimeDisplay(TimeDisplay::RunSeconds);
    pane.refreshTimeBounds();
    doc.selectRun(0);
    pane.refreshTimeBounds();
    QCOMPARE(doc.highlighters().rules, HighlighterSet::defaults().rules);
    QVERIFY(!pane.hasCustomRules());
}

// The other half of the same rule: what a zone change DOES invalidate is every rule's
// wall clock, not merely the selected one's. MatchCriteria stores display-zone digits
// and resolve() reads them in whatever zone is current, so a rule left alone comes to
// name a different instant with nothing on screen changing.
//
// Deterministic zones, never the machine's: the file is written in UTC+3 and the column
// is switched to UTC, so the shift is three hours wherever this runs.
void TestHighlighterPane::aDisplayZoneChangeMovesEveryRulesBoundsAndNothingElse()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));
    doc.highlighters().rules = HighlighterSet::defaults().rules;
    doc.reparseTimestamps(QTimeZone(3 * 3600)); // as written == UTC+3

    HighlighterPane pane;
    pane.setDocument(&doc);

    const QDateTime bound(QDate(2026, 7, 21), QTime(10, 0, 0));
    MatchCriteria c;
    c.timeEnabled = true;
    c.start = bound;
    c.end = bound.addSecs(60);
    pane.addRule(c); // rule 3
    pane.addRule(c); // rule 4, and the one the editor is showing

    const int seeded = HighlighterSet::defaults().rules.size();
    QCOMPARE(doc.highlighters().rules.size(), seeded + 2);
    QCOMPARE(ruleTable(pane)->currentRow(), seeded + 1);

    doc.setTimeDisplay(TimeDisplay::Utc);
    pane.refreshTimeBounds();

    const QVector<HighlightRule> &rules = doc.highlighters().rules;
    for (int row = seeded; row < rules.size(); ++row) {
        // Both rules, not just the selected one: 10:00 in UTC+3 is 07:00 UTC, which is
        // the same instant asked for in the terms the column now reads in.
        QCOMPARE(rules.at(row).match.start, QDateTime(QDate(2026, 7, 21), QTime(7, 0, 0)));
        QCOMPARE(rules.at(row).match.end, QDateTime(QDate(2026, 7, 21), QTime(7, 1, 0)));
        QVERIFY(rules.at(row).match.timeEnabled);
    }
    // ...and the three seeded rules, which name no bound, are untouched.
    QCOMPARE(rules.mid(0, seeded), HighlighterSet::defaults().rules);

    // Back again, and the digits come back: the bound is the instant, and a round trip
    // through two zones has to leave it where it was.
    doc.setTimeDisplay(TimeDisplay::AsWritten);
    pane.refreshTimeBounds();
    for (int row = seeded; row < rules.size(); ++row)
        QCOMPARE(rules.at(row).match.start, bound);
}

void TestHighlighterPane::theRuleButtonsAreDeadWhileThereIsNothingToActOn()
{
    HighlighterPane pane;

    QPushButton *newBtn = button(pane, QStringLiteral("ruleNew"));
    QPushButton *remove = button(pane, QStringLiteral("ruleRemove"));
    QPushButton *clear = button(pane, QStringLiteral("ruleClear"));
    QPushButton *up = button(pane, QStringLiteral("ruleUp"));
    QPushButton *down = button(pane, QStringLiteral("ruleDown"));
    QVERIFY(newBtn && remove && clear && up && down);

    // No document: the pane greys itself entire, which is what covers New. It is not
    // tracked with the other four because a document is all it needs.
    QVERIFY(!pane.isEnabled());
    QVERIFY(!remove->isEnabled());
    QVERIFY(!clear->isEnabled());
    QVERIFY(!up->isEnabled());
    QVERIFY(!down->isEnabled());

    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));
    pane.setDocument(&doc);

    // A file with no rules. New is the one live button in the row — it is the way out
    // of an empty table, and the placeholder over the table names it.
    QVERIFY(pane.isEnabled());
    QVERIFY(newBtn->isEnabled());
    QVERIFY2(!remove->isEnabled(), "Remove is live with no rule to remove");
    QVERIFY2(!clear->isEnabled(), "Clear is live with nothing to clear");
    QVERIFY2(!up->isEnabled(), "Up is live with no rule to move");
    QVERIFY2(!down->isEnabled(), "Down is live with no rule to move");

    // One rule, selected by the New button itself: Remove and Clear have something to
    // do, and Up and Down still do not — a single rule is both first and last.
    newBtn->click();
    QCOMPARE(ruleTable(pane)->currentRow(), 0);
    QVERIFY(remove->isEnabled());
    QVERIFY(clear->isEnabled());
    QVERIFY2(!up->isEnabled(), "Up is live on the only rule there is");
    QVERIFY2(!down->isEnabled(), "Down is live on the only rule there is");

    // ...and back, when the last rule goes.
    remove->click();
    QCOMPARE(ruleTable(pane)->rowCount(), 0);
    QVERIFY(!remove->isEnabled());
    QVERIFY(!clear->isEnabled());
    QVERIFY(newBtn->isEnabled());
}

void TestHighlighterPane::upAndDownAreDeadAtTheEndsOfTheList()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    for (int i = 0; i < 3; ++i)
        button(pane, QStringLiteral("ruleNew"))->click();
    QCOMPARE(ruleTable(pane)->rowCount(), 3);

    QPushButton *remove = button(pane, QStringLiteral("ruleRemove"));
    QPushButton *up = button(pane, QStringLiteral("ruleUp"));
    QPushButton *down = button(pane, QStringLiteral("ruleDown"));

    // First rule: nowhere above it to go. Rules are first-match-wins, so the ends of
    // the list are meaningful positions and not merely where the scrolling stops.
    selectRule(pane, 0);
    QVERIFY(remove->isEnabled());
    QVERIFY2(!up->isEnabled(), "Up is live on the first rule");
    QVERIFY(down->isEnabled());

    // In the middle, both.
    selectRule(pane, 1);
    QVERIFY(up->isEnabled());
    QVERIFY(down->isEnabled());

    // Last rule: nowhere below it.
    selectRule(pane, 2);
    QVERIFY(up->isEnabled());
    QVERIFY2(!down->isEnabled(), "Down is live on the last rule");
    QVERIFY(remove->isEnabled());
}

void TestHighlighterPane::theButtonsFollowTheSelectionAcrossAReorder()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    for (int i = 0; i < 3; ++i)
        button(pane, QStringLiteral("ruleNew"))->click();

    QPushButton *up = button(pane, QStringLiteral("ruleUp"));
    QPushButton *down = button(pane, QStringLiteral("ruleDown"));

    // A move rebuilds the whole table and puts the selection on the rule's NEW row, so
    // the states have to be right the moment the click returns — the press that moves a
    // rule to row 0 is the same press that must leave Up dead.
    selectRule(pane, 1);
    up->click();
    QCOMPARE(ruleTable(pane)->currentRow(), 0);
    QVERIFY2(!up->isEnabled(), "Up stayed live after moving its rule to the top");
    QVERIFY(down->isEnabled());

    down->click();
    QCOMPARE(ruleTable(pane)->currentRow(), 1);
    QVERIFY(up->isEnabled());
    QVERIFY(down->isEnabled());

    down->click();
    QCOMPARE(ruleTable(pane)->currentRow(), 2);
    QVERIFY(up->isEnabled());
    QVERIFY2(!down->isEnabled(), "Down stayed live after moving its rule to the bottom");
}

QTEST_MAIN(TestHighlighterPane)
#include "tst_highlighterpane.moc"
