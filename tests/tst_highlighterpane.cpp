#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QFrame>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSystemTrayIcon>
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

    // The rule list is the pane's own direct child; the subsystem and thread lists
    // live one level down, inside the shared AxisEditor.
    static QListWidget *ruleList(QWidget &w)
    {
        const QList<QListWidget *> direct =
            w.findChildren<QListWidget *>(Qt::FindDirectChildrenOnly);
        return direct.isEmpty() ? nullptr : direct.first();
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
    void typingARegexReachesTheDocument();
    void switchingRulesShowsThatRulesSelection();
    void invalidRegexIsFlagged();
    void addedRuleIsInertUntilConfigured();
    void newCopiesTheSelectedRule();
    void listRowsWearTheirRuleColours();
    void clearRemovesEveryRuleAndMarksThePane();
    void swatchMenuIsBandedAndFitsAShortScreen();
    void oneClickRuleSetsBothColours();
    void reloadingTheListKeepsRulesEnabled();

    // M19 — a rule's effect is a set of actions, and colour is one of them.
    void everyActionIsOfferedAndOnlyColourStartsOn();
    void togglingAnActionReachesTheDocument();
    void untickingColourLeavesTheRuleMatching();
    void newCopiesTheSelectedRulesActions();
    void theOneClickRuleColoursOnly();
    void reloadingTheListKeepsActions();
    void notifySaysWhyWhenTheDesktopOffersNoNotifications();

    // The editor's shape: conditions apart from actions, conditions always readable,
    // and an axis the format cannot fill left out.
    void switchedOffAxesStayVisibleAndGreyed();
    void anAxisTheFormatLacksIsNotShownAtAll();
    void theTwoColourCombosLineUp();
    void theSectionsAreDividedByLinesNotFrames();
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
    QListWidget *rules = ruleList(pane);
    QVERIFY(rules);
    QCOMPARE(rules->count(), 2);
    rules->setCurrentRow(0);

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
    QCOMPARE(ruleList(pane)->currentRow(), 1);

    // A copy of a rule the user switched OFF still arrives on: a rule that appeared
    // dead on the click that asked for it would read as the button having failed.
    ruleList(pane)->item(1)->setCheckState(Qt::Unchecked);
    QVERIFY(!doc.highlighters().rules.at(1).enabled);
    ruleList(pane)->setCurrentRow(1);
    newBtn->click();
    QCOMPARE(doc.highlighters().rules.size(), 3);
    QVERIFY(doc.highlighters().rules.at(2).enabled);
}

void TestHighlighterPane::listRowsWearTheirRuleColours()
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

    // The row is a preview of the rule, not a description of it: it is painted in the
    // rule's own two palette slots, resolved for the theme the pane is showing.
    QListWidget *list = ruleList(pane);
    QVERIFY(list);
    QCOMPARE(list->count(), 1);
    const HighlightRule &r = doc.highlighters().rules.first();
    const bool dark = pane.palette().base().color().lightness()
                      < pane.palette().text().color().lightness();
    QCOMPARE(list->item(0)->background().color(), HighlightPalette::color(r.background, dark));
    QCOMPARE(list->item(0)->foreground().color(), HighlightPalette::color(r.foreground, dark));

    // ...and it follows the colour combos, which are edits like any other.
    auto *bg = pane.findChild<QComboBox *>(QStringLiteral("backgroundColor"));
    QVERIFY(bg);
    const int other = (r.background + 3) % HighlightPalette::count();
    bg->setCurrentIndex(bg->findData(other));
    QCOMPARE(doc.highlighters().rules.first().background, other);
    QCOMPARE(list->item(0)->background().color(), HighlightPalette::color(other, dark));
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

    for (const char *name : {"backgroundColor", "textColor"}) {
        auto *combo = pane.findChild<QComboBox *>(QString::fromLatin1(name));
        QVERIFY2(combo, name);
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

    // Adding a SECOND rule used to switch every rule off, silently: reloadRuleList()
    // clears the list, which emits currentRowChanged(-1), which ran loadEditorFor()
    // — and that used to end by forcing m_updating to false, unguarding the rest of
    // the rebuild. The loop's setFlags then fired itemChanged before setCheckState
    // had run, so the handler read an unset check state as Unchecked and wrote
    // enabled=false back through the reference the next line reads. One rule was
    // fine (clear() on an empty list emits nothing), which is what hid it.
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
    QListWidget *list = ruleList(pane);
    QVERIFY(list);
    list->item(1)->setCheckState(Qt::Unchecked);
    QVERIFY(!doc.highlighters().rules.at(1).enabled);
    pane.addRule(c);
    QVERIFY(doc.highlighters().rules.at(0).enabled);
    QVERIFY(!doc.highlighters().rules.at(1).enabled);
    QVERIFY(doc.highlighters().rules.at(3).enabled);
}

// --- M19: the four action controls -------------------------------------------

void TestHighlighterPane::everyActionIsOfferedAndOnlyColourStartsOn()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);

    // Colour is not a peer of the other three — it is the only action with
    // configuration attached — so it is the checkable group box holding the two colour
    // combos, exactly the shape the Filters pane's axes take.
    QGroupBox *colour = axis(pane, "actionColor");
    QVERIFY(colour);
    QVERIFY(colour->isCheckable());
    QVERIFY(pane.findChild<QComboBox *>(QStringLiteral("backgroundColor")));
    QVERIFY(pane.findChild<QComboBox *>(QStringLiteral("textColor")));

    auto *digest = pane.findChild<SectionBox *>(QStringLiteral("actionDigest"));
    auto *tab = pane.findChild<SectionBox *>(QStringLiteral("actionTab"));
    auto *notify = pane.findChild<SectionBox *>(QStringLiteral("actionNotify"));
    QVERIFY(digest);
    QVERIFY(tab);
    QVERIFY(notify);

    button(pane, QStringLiteral("ruleNew"))->click();

    // A new rule colours and nothing else — what a rule has always done, and the only
    // action with a self-evident meaning before it has been configured.
    QCOMPARE(doc.highlighters().rules.size(), 1);
    QCOMPARE(doc.highlighters().rules.first().actions,
             HighlightActions(HighlightAction::Color));
    QVERIFY(colour->isChecked());
    QVERIFY(!digest->isChecked());
    QVERIFY(!tab->isChecked());
    QVERIFY(!notify->isChecked());
}

void TestHighlighterPane::togglingAnActionReachesTheDocument()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    pane.findChild<SectionBox *>(QStringLiteral("actionDigest"))->setChecked(true);
    QVERIFY(doc.highlighters().rules.first().actions.testFlag(HighlightAction::Digest));

    pane.findChild<SectionBox *>(QStringLiteral("actionTab"))->setChecked(true);
    QVERIFY(doc.highlighters().rules.first().actions.testFlag(HighlightAction::Tab));

    pane.findChild<SectionBox *>(QStringLiteral("actionDigest"))->setChecked(false);
    QVERIFY(!doc.highlighters().rules.first().actions.testFlag(HighlightAction::Digest));
    QVERIFY(doc.highlighters().rules.first().actions.testFlag(HighlightAction::Tab));
}

void TestHighlighterPane::untickingColourLeavesTheRuleMatching()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();
    priorityEnable(pane)->setChecked(true); // give it something to match

    QVERIFY(doc.highlighters().rules.first().match.anyActive());

    axis(pane, "actionColor")->setChecked(false);
    const HighlightRule &r = doc.highlighters().rules.first();

    // The rule still MATCHES; it just does nothing about it. That is a legitimate,
    // deliberate state — it is how a rule is parked without being deleted — and it
    // must survive a round trip rather than being read back as "colour, as of old".
    QVERIFY(r.match.anyActive());
    QVERIFY(!r.actions.testFlag(HighlightAction::Color));
    QCOMPARE(HighlightRule::fromJson(r.toJson()).actions, HighlightActions());
}

void TestHighlighterPane::newCopiesTheSelectedRulesActions()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();
    pane.findChild<SectionBox *>(QStringLiteral("actionDigest"))->setChecked(true);
    axis(pane, "actionColor")->setChecked(false);

    // New starts from the SELECTED rule (SPEC.md §7), and its actions are part of what
    // "a variant of the one in front of you" means.
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

    // Four rules with distinct action sets, then a rebuild of the list — the companion
    // to reloadingTheListKeepsRulesEnabled(). loadEditorFor() is re-entered by
    // reloadRuleList() without either saying so, and the four action controls are new
    // surface on exactly that path: they are set only there, and they write back only
    // through the shared editorChanged lambda.
    for (int i = 0; i < 4; ++i)
        button(pane, QStringLiteral("ruleNew"))->click();
    QCOMPARE(doc.highlighters().rules.size(), 4);

    pane.findChild<SectionBox *>(QStringLiteral("actionDigest"))->setChecked(true);
    ruleList(pane)->setCurrentRow(1);
    pane.findChild<SectionBox *>(QStringLiteral("actionTab"))->setChecked(true);
    axis(pane, "actionColor")->setChecked(false);

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
    HighlighterPane pane;
    auto *notify = pane.findChild<SectionBox *>(QStringLiteral("actionNotify"));
    QVERIFY(notify);

    // Offscreen — and on a stock GNOME/Wayland session, which is the reference desktop
    // — there is no notification service, so the control is disabled and SAYS SO rather
    // than accepting a tick that would silently do nothing. Said before the box can be
    // ticked, the same habit as naming hosts.json before offering to remember a
    // password. Where a service does exist the control is simply live.
    const bool available = QSystemTrayIcon::isSystemTrayAvailable()
                           && QSystemTrayIcon::supportsMessages();
    QCOMPARE(notify->isEnabled(), available);
    QVERIFY(!notify->toolTip().isEmpty());
    if (!available)
        QVERIFY(notify->toolTip().contains(QStringLiteral("tab")));
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

// The two swatch pickers set one thing — how a matching record is drawn — so they sit on
// ONE row, sharing a baseline and splitting the width evenly. That is also what "BG:"
// buys: two full-width labels would not fit beside two combos. The pane is short of
// height, and neither picker is worth a row of its own.
void TestHighlighterPane::theTwoColourCombosLineUp()
{
    Document doc;
    QTemporaryFile file;
    QVERIFY(openLog(doc, file));

    HighlighterPane pane;
    pane.resize(320, 700);
    pane.show();
    QVERIFY(QTest::qWaitForWindowExposed(&pane));
    pane.setDocument(&doc);
    button(pane, QStringLiteral("ruleNew"))->click();

    auto *bg = pane.findChild<QComboBox *>(QStringLiteral("backgroundColor"));
    auto *fg = pane.findChild<QComboBox *>(QStringLiteral("textColor"));
    QVERIFY(bg && fg);
    QVERIFY(QTest::qWaitFor([bg] { return bg->width() > 0; }));

    QCOMPARE(bg->y(), fg->y());
    // Even split, to the odd leftover pixel a layout has to give to one of them: what
    // would be a bug is one combo sized from its content and the other from what is
    // left, which is what happens the moment either loses the Ignored size policy.
    QVERIFY(qAbs(bg->width() - fg->width()) <= 1);
    // Text first, then background, and the pair genuinely on one line rather than
    // overlapping rows that happen to share a y.
    QVERIFY(fg->x() < bg->x());
    QCOMPARE(bg->height(), fg->height());

    // And the block they sit in lines up with the axes above it. AxisEditor insets its
    // own group boxes so a rounded frame does not run into the scroll area's edge, so
    // anything stacked with them has to use the same inset (AxisEditor::kSideMargin) —
    // otherwise the Colour frame sits six pixels proud of the Subsystem frame directly
    // above it, which reads as a rendering fault rather than as two sections.
    auto *colour = axis(pane, "actionColor");
    auto *message = axis(pane, "messageGroup");
    QVERIFY(colour && message);
    QCOMPARE(colour->mapTo(&pane, QPoint(0, 0)).x(), message->mapTo(&pane, QPoint(0, 0)).x());
    QCOMPARE(colour->width(), message->width());
}

void TestHighlighterPane::theSectionsAreDividedByLinesNotFrames()
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

    // The editor's two halves are the only frames in it: Condition round the five axes,
    // Action round the four actions.
    auto *condition = pane.findChild<SectionBox *>(QStringLiteral("conditionSection"));
    auto *action = pane.findChild<SectionBox *>(QStringLiteral("actionSection"));
    QVERIFY(condition && action);
    QVERIFY(!condition->isFlat());
    QVERIFY(!action->isFlat());
    // They are CAPTIONS, so they are headings — centred and bold. An axis's title row is
    // a control and is pinned left instead (tst_filterpane), and the two answers come from
    // the same class so the difference cannot be a per-pane accident.
    QVERIFY(condition->isHeading());
    QVERIFY(action->isHeading());

    // The editor as a whole carries no caption: the rule list above it already says
    // which rule is being edited. So the widget the scroll area hangs off must be a
    // plain one — an empty-titled QGroupBox would still spend a frame and a title row.
    auto *scroll = pane.findChild<QScrollArea *>();
    QVERIFY(scroll);
    QVERIFY2(!qobject_cast<QGroupBox *>(scroll->parentWidget()),
             "the rule editor has grown a caption back");

    // Inside them nothing is framed. Every axis, and the one action with a body, is a
    // flat SectionBox drawing a hairline along its own title row instead — three frames
    // deep at the subsystem list is where a frame stops meaning "these belong together".
    //
    // The line is PAINTED, not a QFrame in the layout, because a group box's title row is
    // drawn by the style and has no cell beside the title to put one in — a QFrame lands
    // under the title instead of alongside it. So what a test can hold is that each
    // section is the kind of box that draws it and has been asked to.
    // All five axes AND all four actions: one grammar, so an action is not a different
    // kind of thing from an axis and Highlight is not a different kind of thing from the
    // three actions that carry no settings.
    QList<SectionBox *> rows;
    for (const char *name : {"priorityGroup", "messageGroup", "subsystemGroup",
                             "threadGroup", "timeGroup", "actionColor", "actionDigest",
                             "actionTab", "actionNotify"}) {
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

    // And the four actions are ONE COLUMN: same left edge, same width, so the three
    // body-less ones read as continuations of the one above them rather than as a
    // separate block of checkboxes. They were a two-column grid of bare checkboxes, which
    // is what this replaces.
    QVERIFY(QTest::qWaitFor([&rows] { return rows.first()->width() > 0; }));
    for (const char *name : {"actionColor", "actionDigest", "actionTab", "actionNotify"}) {
        auto *box = pane.findChild<SectionBox *>(QString::fromLatin1(name));
        QCOMPARE(box->mapTo(&pane, QPoint(0, 0)).x(),
                 pane.findChild<SectionBox *>(QStringLiteral("actionColor"))
                     ->mapTo(&pane, QPoint(0, 0))
                     .x());
        QCOMPARE(box->width(),
                 pane.findChild<SectionBox *>(QStringLiteral("actionColor"))->width());
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
