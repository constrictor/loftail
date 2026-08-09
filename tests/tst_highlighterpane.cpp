#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryFile>

#include "Document.h"
#include "Highlight.h"
#include "HighlighterPane.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"

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

    // The four group axes: each is a checkable QGroupBox whose title row IS the
    // enable control. Found by OBJECT NAME, never by the title it shows — a visible
    // string is a translator's to change (CLAUDE.md), and these names are the test
    // contract precisely because they are not.
    static QGroupBox *axis(QWidget &w, const char *name)
    {
        return w.findChild<QGroupBox *>(QString::fromLatin1(name));
    }

    // Priority is the exception: one checkbox and one combo on a single row, with no
    // group box to be the enable control.
    static QCheckBox *priorityEnable(QWidget &w)
    {
        return w.findChild<QCheckBox *>(QStringLiteral("priorityEnable"));
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
    void swatchMenuIsBandedAndFitsAShortScreen();
    void oneClickRuleSetsBothColours();
    void reloadingTheListKeepsRulesEnabled();
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
    button(pane, QStringLiteral("ruleAdd"))->click();
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
    QPushButton *add = button(pane, QStringLiteral("ruleAdd"));
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
    button(pane, QStringLiteral("ruleAdd"))->click();

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
    button(pane, QStringLiteral("ruleAdd"))->click();

    // Add seeds one axis so a new rule does something visible immediately, but turning
    // that axis back off must leave the rule matching nothing at all — not matching
    // everything, which is what an "all axes inactive" filter would mean.
    QVERIFY(doc.highlighters().anyEnabled());
    priorityEnable(pane)->setChecked(false);
    QVERIFY(!doc.highlighters().anyEnabled());
    QVERIFY(!doc.highlighters().rules.first().match.anyActive());
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

QTEST_MAIN(TestHighlighterPane)
#include "tst_highlighterpane.moc"
