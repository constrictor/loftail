#include "HighlighterPane.h"

#include "AxisEditor.h"
#include "Document.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"
#include "RecordIndex.h"

#include <QBrush>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QVBoxLayout>
#include <QVector>

namespace loftail {

namespace {

QIcon swatchIcon(const QColor &c)
{
    QPixmap pm(14, 14);
    pm.fill(c.isValid() ? c : Qt::transparent);
    return QIcon(pm);
}

// One axis's contribution to a rule's one-line summary, or an empty string when the
// axis is off. Short by necessity: this is a row in a dock-width list.
//
// Not a member, so there is no tr() in scope; the context is named for the class these
// summaries are shown in. The priority glyph, the regex slashes and the joining comma
// stay literal — they are punctuation, and priorityName() is a log token that must
// round-trip against the file (invariant #4).
QString axisSummary(const MatchCriteria &c)
{
    const auto text = [](const char *s) {
        return QCoreApplication::translate("loftail::HighlighterPane", s);
    };

    QStringList parts;
    if (c.priorityEnabled)
        parts << QStringLiteral("≥%1").arg(QString(priorityName(c.minPriority)));
    if (c.loggerEnabled) {
        parts << (c.loggerNames.size() == 1
                      ? c.loggerNames.first()
                      : text(QT_TRANSLATE_NOOP("loftail::HighlighterPane", "%1 subsystems"))
                            .arg(c.loggerNames.size()));
    }
    if (c.threadEnabled) {
        parts << (c.threadNames.size() == 1
                      ? text(QT_TRANSLATE_NOOP("loftail::HighlighterPane", "thread %1"))
                            .arg(c.threadNames.first())
                      : text(QT_TRANSLATE_NOOP("loftail::HighlighterPane", "%1 threads"))
                            .arg(c.threadNames.size()));
    }
    if (c.text.active()) {
        // Slashes for a regex, quotes for a substring — the same visual shorthand the
        // Find bar's two modes have.
        const QString pat = c.text.matcher.isRegex()
                                ? QStringLiteral("/%1/").arg(c.text.matcher.pattern())
                                : QStringLiteral("\"%1\"").arg(c.text.matcher.pattern());
        parts << (c.text.negate
                      ? text(QT_TRANSLATE_NOOP("loftail::HighlighterPane", "not %1")).arg(pat)
                      : pat);
    }
    if (c.timeEnabled)
        parts << text(QT_TRANSLATE_NOOP("loftail::HighlighterPane", "in time range"));
    return parts.join(QStringLiteral(", "));
}

} // namespace

HighlighterPane::HighlighterPane(QWidget *parent) : QWidget(parent)
{
    buildUi();
    setDocument(nullptr);
}

bool HighlighterPane::isDark() const
{
    // Dark when the base is darker than the text — the same cue the view uses.
    return palette().base().color().lightness() < palette().text().color().lightness();
}

QComboBox *HighlighterPane::makeSwatchCombo(QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    // Item 0 is the *default* sentinel: leave this role at the theme's normal color.
    combo->addItem(tr("Default"), HighlightPalette::kDefault);
    const bool dark = isDark();
    for (int i = 0; i < HighlightPalette::count(); ++i) {
        // A rule separating the three tone bands, so a list this long reads as
        // "pick a loudness, then a hue" rather than as one run of swatches. A
        // separator is not selectable and carries no data, so findData() and
        // currentData() below are unaffected.
        if (i > 0 && i % HighlightPalette::kSlotsPerBand == 0)
            combo->insertSeparator(combo->count());
        const PaletteSlot &s = HighlightPalette::slot(i);
        combo->addItem(swatchIcon(HighlightPalette::color(i, dark)), QString(s.name), i);
    }
    // Twenty-seven slots plus the default and two separators is a taller popup than
    // a style will always fit on a short screen, so cap it and let Qt scroll rather
    // than let the list run off the top or bottom.
    combo->setMaxVisibleItems(HighlightPalette::kSlotsPerBand * 2 + 1);
    return combo;
}

void HighlighterPane::setSwatchCombo(QComboBox *combo, int paletteIndex)
{
    const int at = combo->findData(paletteIndex);
    combo->setCurrentIndex(at >= 0 ? at : 0); // fall back to Default
}

int HighlighterPane::swatchValue(const QComboBox *combo) const
{
    const QVariant v = combo->currentData();
    return v.isValid() ? v.toInt() : HighlightPalette::kDefault;
}

void HighlighterPane::buildUi()
{
    auto *root = new QVBoxLayout(this);

    m_ruleList = new QListWidget(this);
    m_ruleList->setMinimumHeight(120);
    root->addWidget(m_ruleList);

    auto *btnRow = new QHBoxLayout;
    // "New", not "Add": it now starts from the selected rule rather than from nothing,
    // so the word has to promise a rule to edit and not an entry appearing complete.
    m_newBtn = new QPushButton(tr("New"), this);
    m_removeBtn = new QPushButton(tr("Remove"), this);
    m_clearBtn = new QPushButton(tr("Clear"), this);
    m_upBtn = new QPushButton(tr("Up"), this);
    m_downBtn = new QPushButton(tr("Down"), this);
    m_newBtn->setToolTip(tr("Add a copy of the selected rule, or an empty rule when "
                            "nothing is selected."));
    m_clearBtn->setToolTip(tr("Remove every rule, leaving the log uncoloured."));
    // Object names, never translated: the test contract (ARCHITECTURE.md §9.1). Not
    // decoration — the pane embeds an AxisEditor, so "the button that says Add" was
    // ambiguous the moment that editor also had one, and which of them a by-text
    // lookup returned was decided by construction order.
    m_newBtn->setObjectName(QStringLiteral("ruleNew"));
    m_removeBtn->setObjectName(QStringLiteral("ruleRemove"));
    m_clearBtn->setObjectName(QStringLiteral("ruleClear"));
    m_upBtn->setObjectName(QStringLiteral("ruleUp"));
    m_downBtn->setObjectName(QStringLiteral("ruleDown"));
    btnRow->addWidget(m_newBtn);
    btnRow->addWidget(m_removeBtn);
    btnRow->addWidget(m_clearBtn);
    btnRow->addWidget(m_upBtn);
    btnRow->addWidget(m_downBtn);
    root->addLayout(btnRow);

    // --- Editor for the selected rule --------------------------------------
    //
    // Five axes plus two color pickers do not fit a dock, so the whole editor lives
    // in a scroll area and the AxisEditor collapses each axis to its title row until
    // that axis is enabled. A rule with two axes set therefore shows two open groups
    // and three one-line stubs.
    m_editor = new QGroupBox(tr("Selected rule"), this);
    auto *ev = new QVBoxLayout(m_editor);

    auto *scroll = new QScrollArea(m_editor);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    ev->addWidget(scroll);

    auto *content = new QWidget(scroll);
    scroll->setWidget(content);
    auto *cv = new QVBoxLayout(content);
    cv->setContentsMargins(0, 0, 0, 0);

    // Every axis is opt-in for a highlight rule: an unconfigured rule must stay inert
    // (SPEC.md §7), the opposite of the Filters pane's enabled-by-default metadata
    // axes, which exist so their controls act on the first click.
    m_axes = new AxisEditor(AxisEditor::Defaults{/*priorityOn=*/false, /*loggerOn=*/false},
                            content);
    m_axes->setCollapsible(true);
    cv->addWidget(m_axes);

    auto *bgRow = new QHBoxLayout;
    bgRow->addWidget(new QLabel(tr("Background:"), content));
    m_bgCombo = makeSwatchCombo(content);
    m_bgCombo->setObjectName(QStringLiteral("backgroundColor")); // test contract, never translated
    bgRow->addWidget(m_bgCombo, 1);
    cv->addLayout(bgRow);

    auto *fgRow = new QHBoxLayout;
    fgRow->addWidget(new QLabel(tr("Text:"), content));
    m_fgCombo = makeSwatchCombo(content);
    m_fgCombo->setObjectName(QStringLiteral("textColor"));
    fgRow->addWidget(m_fgCombo, 1);
    cv->addLayout(fgRow);

    cv->addStretch(1);
    root->addWidget(m_editor, 1);

    // --- Wiring -------------------------------------------------------------
    connect(m_ruleList, &QListWidget::currentRowChanged, this, [this](int row) {
        loadEditorFor(row);
    });
    // A rule's checkbox toggles enable/disable in one click (SPEC.md §8).
    connect(m_ruleList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (m_updating)
            return;
        const int row = m_ruleList->row(item);
        if (row >= 0 && row < m_rules.size()) {
            m_rules[row].enabled = (item->checkState() == Qt::Checked);
            commit();
        }
    });

    connect(m_newBtn, &QPushButton::clicked, this, [this] {
        if (!m_document)
            return;
        const int from = currentRow();
        HighlightRule r; // inert until an axis is configured
        if (from >= 0 && from < m_rules.size()) {
            // A copy of what is selected, criteria and colours alike: a second rule is
            // nearly always a variant of the one in front of the user — the same axes
            // with one value changed — and retyping it was the whole cost of the old
            // "Add", which produced a rule resembling nothing on screen.
            r = m_rules.at(from);
            // ...but enabled, whatever the source was. A rule the user asked for
            // arriving switched off would read as the button having failed.
            r.enabled = true;
        } else {
            // Nothing selected: an empty rule, every axis off, so it colours nothing
            // until it is configured. It still takes a colour no other rule is using,
            // so it is visible in the list the moment it appears.
            r.background = nextFreeBackground();
            r.foreground = HighlightPalette::readableTextSlot(r.background);
        }
        m_rules.append(r);
        commit();
        reloadRuleList();
        m_ruleList->setCurrentRow(m_rules.size() - 1);
    });
    connect(m_removeBtn, &QPushButton::clicked, this, [this] {
        const int row = currentRow();
        if (row < 0)
            return;
        m_rules.remove(row);
        commit();
        reloadRuleList();
        m_ruleList->setCurrentRow(qMin(row, m_rules.size() - 1));
    });
    connect(m_clearBtn, &QPushButton::clicked, this, [this] {
        // The counterpart of the Filters pane's Clear: one action back to an
        // uncoloured log, which was otherwise Remove pressed once per rule.
        if (m_rules.isEmpty())
            return;
        m_rules.clear();
        commit();
        reloadRuleList();
    });
    auto move = [this](int delta) {
        const int row = currentRow();
        const int to = row + delta;
        if (row < 0 || to < 0 || to >= m_rules.size())
            return;
        m_rules.swapItemsAt(row, to);
        commit();
        reloadRuleList();
        m_ruleList->setCurrentRow(to);
    };
    connect(m_upBtn, &QPushButton::clicked, this, [move] { move(-1); });
    connect(m_downBtn, &QPushButton::clicked, this, [move] { move(1); });

    auto editorChanged = [this] {
        if (m_updating)
            return;
        const int row = currentRow();
        if (row < 0 || row >= m_rules.size())
            return;
        HighlightRule &r = m_rules[row];
        r.match = m_axes->criteria();
        r.background = swatchValue(m_bgCombo);
        r.foreground = swatchValue(m_fgCombo);
        commit();
        // Refresh only this row's summary so the current selection is preserved.
        // Guard the setText so its itemChanged does not re-enter the enable handler.
        m_updating = true;
        if (QListWidgetItem *item = m_ruleList->item(row)) {
            item->setText(ruleSummary(r));
            paintItem(item, r); // the colour combos are edits like any other
        }
        m_updating = false;
    };
    connect(m_axes, &AxisEditor::changed, this, editorChanged);
    connect(m_bgCombo, &QComboBox::currentIndexChanged, this, [editorChanged](int) { editorChanged(); });
    connect(m_fgCombo, &QComboBox::currentIndexChanged, this, [editorChanged](int) { editorChanged(); });
}

int HighlighterPane::currentRow() const
{
    return m_ruleList ? m_ruleList->currentRow() : -1;
}

QString HighlighterPane::ruleSummary(const HighlightRule &r) const
{
    // What the rule MATCHES, and nothing about its colours: the row is painted in them
    // (paintItem), so naming them as well spent dock width saying twice what one look
    // answers — and a colour is recognised faster than "bg:Deep Amber" is read.
    const QString axes = axisSummary(r.match);
    return axes.isEmpty() ? tr("(no match set)") : axes;
}

void HighlighterPane::paintItem(QListWidgetItem *item, const HighlightRule &r) const
{
    // A rule's own colours, resolved for the current theme exactly as the view
    // resolves them, so the list is a preview rather than a description. *default*
    // leaves the role alone: an invalid QColor means "the theme's normal colour", and
    // a default-constructed QBrush is how an item says it has no override.
    //
    // The SELECTED row still paints in the theme's selection colours, hiding its own —
    // Qt's ordinary behaviour, kept deliberately. Which row is selected is now an input
    // to a command ("New" copies it), so selection has to stay unmistakable; and the
    // rule under the cursor is the one whose two swatch combos are on screen anyway.
    const bool dark = isDark();
    const QColor bg = HighlightPalette::color(r.background, dark);
    const QColor fg = HighlightPalette::color(r.foreground, dark);
    item->setBackground(bg.isValid() ? QBrush(bg) : QBrush());
    item->setForeground(fg.isValid() ? QBrush(fg) : QBrush());
}

void HighlighterPane::reloadRuleList()
{
    m_updating = true;
    m_ruleList->clear();
    for (const HighlightRule &r : m_rules) {
        auto *item = new QListWidgetItem(ruleSummary(r), m_ruleList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(r.enabled ? Qt::Checked : Qt::Unchecked);
        paintItem(item, r);
    }
    m_updating = false;
    const bool has = !m_rules.isEmpty();
    m_editor->setEnabled(has);
    m_clearBtn->setEnabled(has);
    if (has)
        m_ruleList->setCurrentRow(0);
    else
        loadEditorFor(-1);
    updateActivity();
}

void HighlighterPane::updateActivity()
{
    // Only on a CHANGE, for the reason FilterPane::updateSummary() spells out: the
    // marker rides the dock's window title, which is a QTabBar entry while the panes
    // are tabbed, and re-setting it relays out the whole bar.
    const bool active = hasRules();
    if (m_activeState.has_value() && *m_activeState == active)
        return;
    m_activeState = active;
    emit activityChanged(active);
}

int HighlighterPane::nextFreeBackground() const
{
    // The first palette slot no existing rule paints with, so a second rule is
    // distinguishable from the first at a glance. Once every slot is spoken for,
    // cycle rather than refuse — a repeated colour is a small annoyance, a button
    // that silently does nothing is not.
    //
    // Offered in band order — Deep, then Vivid, then Soft — and skipping the three
    // neutrals, which are foreground colours far more often than they are anyone's
    // idea of a highlight. Deep leads because it is the band that reads as a fill in
    // either theme without shouting.
    QVector<int> offered;
    for (int i = 0; i < HighlightPalette::count(); ++i) {
        if (!HighlightPalette::isNeutral(i))
            offered.append(i);
    }
    QSet<int> used;
    for (const HighlightRule &r : m_rules)
        used.insert(r.background);
    for (int i : offered) {
        if (!used.contains(i))
            return i;
    }
    return offered.at(m_rules.size() % offered.size());
}

void HighlighterPane::loadEditorFor(int row)
{
    // SAVED and restored, never forced false on the way out. reloadRuleList() calls
    // this re-entrantly without meaning to — `m_ruleList->clear()` drops the current
    // row, which emits currentRowChanged(-1) — and an unconditional `m_updating =
    // false` here then unguards the REST of that function. What followed was silent
    // and total: the loop's `setFlags` fired itemChanged before `setCheckState` had
    // run, so the handler read the item's not-yet-set check state as Unchecked and
    // wrote `enabled = false` back into `m_rules` — through the very reference the
    // next line reads (`for (const HighlightRule &r : m_rules)`), so `setCheckState`
    // was then handed the false it had just caused. Every rule switched itself off,
    // and only from the second rule onward, because clear() on an empty list emits
    // nothing. Restoring the flag is the fix; the aliasing is only how it bit.
    const bool wasUpdating = m_updating;
    m_updating = true;
    const bool valid = row >= 0 && row < m_rules.size();
    m_editor->setEnabled(valid);
    if (valid) {
        const HighlightRule &r = m_rules.at(row);
        // setCriteria applies the rule's subsystem/thread selection EXACTLY, so moving
        // between rules shows each rule's own values rather than letting the discovery
        // rule ("a name never listed before arrives checked") leak the previous rule's
        // selection — or the whole file's — into this one.
        m_axes->setCriteria(r.match);
        setSwatchCombo(m_bgCombo, r.background);
        setSwatchCombo(m_fgCombo, r.foreground);
    }
    m_updating = wasUpdating;
}

void HighlighterPane::syncToDocument()
{
    if (!m_document)
        return;
    m_document->highlighters().rules = m_rules;
    m_document->resolveHighlighters();
}

void HighlighterPane::commit()
{
    syncToDocument();
    emit highlightersChanged();
}

void HighlighterPane::setDocument(Document *document)
{
    m_document = document;
    setEnabled(document != nullptr);
    m_axes->setDocument(document);
    m_rules = document ? document->highlighters().rules : QVector<HighlightRule>();
    reloadRuleList();
}

void HighlighterPane::refreshDiscoveredLists()
{
    // Re-resolve the rules against the grown intern tables (SPEC.md §6 discovery
    // timing) and re-render the editor for the current rule, which repopulates its
    // subsystem/thread lists from the new tables.
    //
    // Deliberately NOT AxisEditor::refreshDiscoveredLists(): that applies the
    // discovery rule, which would tick every newly found subsystem into whichever
    // rule happens to be selected. A filter is a statement about the whole file and
    // should widen with it; a rule naming `net.io` must not silently grow to name
    // `db.pool` as well.
    if (m_document)
        m_document->resolveHighlighters();
    loadEditorFor(currentRow());
}

void HighlighterPane::refreshTimeBounds()
{
    m_axes->refreshTimeBounds();
    // The shown instant is unchanged, but the rule's stored wall clock was written in
    // the old zone; take the re-rendered values back so the two cannot disagree.
    const int row = currentRow();
    if (row >= 0 && row < m_rules.size()) {
        m_rules[row].match = m_axes->criteria();
        commit();
    }
}

void HighlighterPane::addRule(const MatchCriteria &criteria)
{
    if (!m_document)
        return;

    // The first palette slot no existing rule paints with, so a second "highlight
    // this thread" is distinguishable from the first at a glance (nextFreeBackground,
    // shared with the New button).
    const int slot = nextFreeBackground();

    HighlightRule r;
    r.match = criteria;
    r.background = slot;
    // ...and the text that reads on it. Leaving the foreground at *default* was safe
    // only while the palette gave a theme one tone: now that a background can be Deep
    // or Soft by the user's choice, half of them would be unreadable under the theme's
    // own text colour, and which half flips when the theme does. The pairing here is
    // theme-stable (Palette.h), so a one-click rule stays legible either way.
    r.foreground = HighlightPalette::readableTextSlot(slot);
    m_rules.append(r);
    commit();
    reloadRuleList();
    m_ruleList->setCurrentRow(m_rules.size() - 1);
}

QJsonObject HighlighterPane::saveState() const
{
    HighlighterSet set;
    set.rules = m_rules;
    QJsonObject o;
    o.insert(QStringLiteral("rules"), set.toJson());
    return o;
}

void HighlighterPane::restoreState(const QJsonObject &o)
{
    m_rules = HighlighterSet::fromJson(o.value(QStringLiteral("rules")).toArray()).rules;
    syncToDocument();
    reloadRuleList();
    emit highlightersChanged();
}

} // namespace loftail
