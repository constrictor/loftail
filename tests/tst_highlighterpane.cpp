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
#include <QLineEdit>
#include <QHeaderView>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStyleOptionViewItem>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QToolButton>
#include <QTemporaryFile>

#include "Document.h"
#include "Highlight.h"
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
    void clearRemovesEveryRuleAndMarksThePane();
    void swatchMenuIsBandedAndFitsAShortScreen();
    void oneClickRuleSetsBothColours();
    void reloadingTheListKeepsRulesEnabled();

    // M19 — a rule's effect is a set of actions, and colour is one of them. All four
    // are now set in the rule's own table row: three ticks and a pair of swatch columns.
    void everyActionIsAColumnAndOnlyColourStartsOn();
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
    void noSectionClipsItsOwnTitle();
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

void TestHighlighterPane::clearRemovesEveryRuleAndMarksThePane()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    QSignalSpy activity(&pane, &HighlighterPane::activityChanged);
    pane.setDocument(&doc);

    QPushButton *clear = button(pane, QStringLiteral("ruleClear"));
    QVERIFY(clear);
    // Nothing to clear yet, and nothing to mark: the dock wears its marker only while
    // the pane holds rules, because it is usually tabbed behind three others.
    QVERIFY(!clear->isEnabled());
    QVERIFY(!pane.hasRules());

    MatchCriteria c;
    c.priorityEnabled = true;
    c.minPriority = Priority::Error;
    pane.addRule(c);
    pane.addRule(c);
    QVERIFY(pane.hasRules());
    QVERIFY(clear->isEnabled());
    QCOMPARE(activity.count(), 1);
    QCOMPARE(activity.takeFirst().at(0).toBool(), true);

    // One action back to an uncoloured log, where before it was Remove per rule.
    clear->click();
    QVERIFY(doc.highlighters().rules.isEmpty());
    QVERIFY(!pane.hasRules());
    QVERIFY(!clear->isEnabled());
    QCOMPARE(activity.count(), 1);
    QCOMPARE(activity.takeFirst().at(0).toBool(), false);

    // Edge-triggered: adding a second rule to a pane that already had one must not
    // rewrite the dock title, which is a QTabBar entry while the panes are tabbed.
    pane.addRule(c);
    pane.addRule(c);
    QCOMPARE(activity.count(), 1);
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

QTEST_MAIN(TestHighlighterPane)
#include "tst_highlighterpane.moc"
