#include "HighlighterPane.h"

#include "AxisEditor.h"
#include "Document.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"
#include "RecordIndex.h"
#include "SectionBox.h"

#include <QBrush>
#include <QComboBox>
#include <QGridLayout>
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
#include <QSystemTrayIcon>
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

// A section of the rule editor: Condition, or Action.
//
// A FRAMED box, where the bold-word-and-a-line heading these replaced was deliberately
// not one — the objection then was nesting, three borders deep by the time the eye
// reached the subsystem list, at which point a border stops meaning "these belong
// together". What paid for it is the frame that went away above: with no "Selected rule"
// box wrapping the editor, these two ARE the outer frame rather than a third one inside
// it, and everything they contain is flat.
//
// A SectionBox although it draws no hairline of its own, for the other thing that class
// settles: which of the two things a title row can be. These two are HEADINGS — centred
// and bold — while every title row inside them is a control, pinned left. Both answers
// have to come from one place, because on the style that centres every group box title
// (Breeze, see SectionBox) the difference between a caption and a control was invisible:
// the enable controls were centred too, and once they were dragged left, leaving these
// two with them made a caption in the corner look like a control that had lost its
// checkbox.
SectionBox *makeSection(const QString &text, const QString &objectName, QWidget *parent)
{
    auto *box = new SectionBox(text, parent);
    box->setObjectName(objectName); // test contract, never translated
    box->setHeading(true);
    return box;
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
    // A rule is two questions — which records it matches, and what it does to them —
    // and the editor is laid out to say so: a "Condition" box round the five axes, an
    // "Action" box round the four actions. Without them the colour group read as a
    // sixth axis, which is precisely backwards: it is the only one of the four actions
    // with configuration attached, and configuration is what an axis looks like.
    //
    // The editor itself is a bare QWidget and no longer a captioned group box. "Selected
    // rule" said what the list above it already says — a row is selected there, and the
    // controls below change as the selection moves — and it charged a frame and a title
    // row of height for it, in the pane that has the least height to spare and now has
    // two section frames of its own to fit.
    //
    // Five axes and four actions do not fit a dock, so the whole thing lives in a
    // scroll area. Deliberately NOT collapsed down to title rows while an axis is off,
    // which is what this pane used to do: an axis that shows its controls only once it
    // is ticked has to be switched on to be read, and the answer to a rule editor that
    // does not fit is a scroll bar, not a rule the user cannot see the shape of.
    m_editor = new QWidget(this);
    auto *ev = new QVBoxLayout(m_editor);
    // Flush: the group boxes inside supply the inset now, and the pane's own root layout
    // supplies the gap to the dock edge.
    ev->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(m_editor);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    ev->addWidget(scroll);

    auto *content = new QWidget(scroll);
    scroll->setWidget(content);
    auto *cv = new QVBoxLayout(content);
    // Flush at the sides, but air above and between: a section's heading lives in the box's
    // top margin, above its frame, so the space over it is the layout's to give and the
    // default leaves the caption crowded between the row above and its own frame below.
    cv->setContentsMargins(0, 4, 0, 0);
    cv->setSpacing(10);

    QGroupBox *conditionBox = makeSection(tr("Condition"),
                                          QStringLiteral("conditionSection"), content);
    // Flush at the sides: AxisEditor's own kSideMargin is the inset, exactly as it was
    // when the axes sat against the pane edge, so a new frame costs its own two pixels
    // of width and nothing more. Width in this pane is spent, not free — the message
    // axis alone asks 290 px, and the first casualty of an overrun is a horizontal
    // scrollbar over the value lists' All/None/Invert column.
    auto *conditionBody = new QVBoxLayout(conditionBox);
    conditionBody->setContentsMargins(0, 4, 0, 6);
    cv->addWidget(conditionBox);

    // Every axis is opt-in for a highlight rule: an unconfigured rule must stay inert
    // (SPEC.md §7), the opposite of the Filters pane's enabled-by-default metadata
    // axes, which exist so their controls act on the first click.
    m_axes = new AxisEditor(AxisEditor::Defaults{/*priorityOn=*/false, /*loggerOn=*/false},
                            conditionBox);
    // An axis this log's format cannot fill is left out rather than shown greyed. The
    // Filters pane keeps it and explains it, and that asymmetry is deliberate — see
    // AxisEditor::setHidesUnsupportedAxes.
    m_axes->setHidesUnsupportedAxes(true);
    // A line per axis rather than a frame per axis: the Condition box round them all is
    // the frame now, and a framed axis inside it puts three borders between the pane and
    // the subsystem list. See AxisEditor::setFlatAxes.
    m_axes->setFlatAxes(true);
    conditionBody->addWidget(m_axes);

    QGroupBox *actionBox = makeSection(tr("Action"),
                                       QStringLiteral("actionSection"), content);
    auto *actionBody = new QVBoxLayout(actionBox);
    actionBody->setContentsMargins(0, 4, 0, 6);
    cv->addWidget(actionBox);

    // --- What the rule DOES (M19, SPEC.md §7) --------------------------------
    //
    // All four actions are the same shape — a checkable SectionBox whose title row is the
    // enable control and whose hairline runs out to the right edge — stacked one per row.
    // Three of them stop there; Highlighting has a body under its line, because it is the
    // only action with configuration attached. So what tells it apart is that it HAS
    // settings, not that it is drawn in a different grammar from its peers, and unticking
    // it greys those settings the way unticking an axis greys its controls.
    //
    // They were plain checkboxes in a two-column grid, which was wrong twice: a grid put
    // "Mark tab" beside "Digest" and "Notify" under it, so the three were neither a list
    // nor a row of pairs, and a bare checkbox next to a lined title row read as a
    // different kind of thing than the action above it.
    //
    // No glyphs: four abstract actions have none that are self-evident, and Windows
    // offscreen testing resolves no fonts at all, so a glyph there is a guaranteed blank.
    auto *colorSection = new SectionBox(tr("Highlight"), actionBox);
    m_colorGroup = colorSection;
    m_colorGroup->setObjectName(QStringLiteral("actionColor")); // test contract, never translated
    m_colorGroup->setCheckable(true);
    // Flat with a hairline along its title row, for the same reason the axes are and by
    // the same means: the one action with a body would otherwise be the only framed thing
    // in either section, and a frame that appears once reads as "this one is different in
    // kind" rather than as "this one has settings".
    m_colorGroup->setFlat(true);
    colorSection->setTitleDivider(true);
    m_colorGroup->setToolTip(tr("Recolour matching records in the log."));
    // A grid, not two rows of an HBox each: the two labels differ in width, so laying
    // each row out on its own started the two combos at different x and made a pair of
    // controls that set one thing look like two unrelated ones. One grid gives column 0
    // the wider label's width — so the labels align and the combos begin together — and
    // column 1 the same stretch for both, so the swatch lists are the same size as well.
    // The same margins makeAxisBox() gives an axis body, so the two combos indent under
    // the Highlight title by exactly as much as the priority combo does under its own.
    auto *colorBody = new QGridLayout(m_colorGroup);
    colorBody->setContentsMargins(8, 4, 8, 6);
    m_bgCombo = makeSwatchCombo(m_colorGroup);
    m_bgCombo->setObjectName(QStringLiteral("backgroundColor"));
    m_fgCombo = makeSwatchCombo(m_colorGroup);
    m_fgCombo->setObjectName(QStringLiteral("textColor"));
    // Text above background: a record is read as text on a background, so the pair reads
    // top-down in the order the eye takes them, and the one that decides legibility comes
    // first rather than being the afterthought under the box that made it necessary.
    colorBody->addWidget(new QLabel(tr("Text:"), m_colorGroup), 0, 0);
    colorBody->addWidget(m_fgCombo, 0, 1);
    colorBody->addWidget(new QLabel(tr("Background:"), m_colorGroup), 1, 0);
    colorBody->addWidget(m_bgCombo, 1, 1);
    colorBody->setColumnStretch(1, 1);

    // Both action blocks inset by the same 6 px AxisEditor puts round its own group
    // boxes, so the Highlight frame's left edge lines up with the Subsystem frame's in
    // the box above rather than sitting six pixels proud of it — which reads as a
    // rendering fault in a stack whose whole job is to look like one column. That is why
    // the two section boxes are flush at the sides and the inset is applied in here.
    auto *actionsColumn = new QVBoxLayout;
    actionsColumn->setContentsMargins(AxisEditor::kSideMargin, 0, AxisEditor::kSideMargin, 0);
    actionsColumn->addWidget(m_colorGroup);

    // The three body-less actions, one per row and in the same column as Highlight above
    // them, so all four enable controls share one left edge and one hairline length. A
    // body-less SectionBox is just its title row: no layout at all, since an empty one
    // would still charge the row its margins.
    const auto makeAction = [this, actionBox, actionsColumn](const QString &title,
                                                             const QString &objectName,
                                                             const QString &hint) {
        auto *box = new SectionBox(title, actionBox);
        box->setObjectName(objectName); // test contract, never translated
        box->setCheckable(true);
        box->setChecked(false);
        box->setFlat(true);
        box->setTitleDivider(true);
        box->setToolTip(hint);
        actionsColumn->addWidget(box);
        return box;
    };

    m_digestAction =
        makeAction(tr("Digest"), QStringLiteral("actionDigest"),
                   tr("Show this rule's newest match in the strip under the log."));
    m_tabAction = makeAction(tr("Mark tab"), QStringLiteral("actionTab"),
                             tr("Mark this log's tab when a match arrives while it is not "
                                "the log on screen."));
    m_notifyAction = makeAction(tr("Notify"), QStringLiteral("actionNotify"), QString());
    // Said before the box can be ticked, not after — the same habit as naming
    // hosts.json before the "remember this password" checkbox is offered.
    if (notificationsSupported()) {
        m_notifyAction->setToolTip(tr("Raise a desktop notification on the same trigger, "
                                      "at most one every ten seconds."));
    } else {
        m_notifyAction->setEnabled(false);
        m_notifyAction->setToolTip(tr("This desktop offers no notification service, so "
                                      "this rule marks the tab instead."));
    }
    actionBody->addLayout(actionsColumn);

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
        r.actions = readActions();
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
    // The four action controls join the SAME lambda and are written back only from
    // loadEditorFor(), which already saves and restores m_updating. That reuse is the
    // entire guard — a second path with a guard of its own is how the bug documented
    // at loadEditorFor() got in the first time.
    connect(m_colorGroup, &QGroupBox::toggled, this, [editorChanged](bool) { editorChanged(); });
    connect(m_digestAction, &QGroupBox::toggled, this, [editorChanged](bool) { editorChanged(); });
    connect(m_tabAction, &QGroupBox::toggled, this, [editorChanged](bool) { editorChanged(); });
    connect(m_notifyAction, &QGroupBox::toggled, this, [editorChanged](bool) { editorChanged(); });
}

bool HighlighterPane::notificationsSupported()
{
    return QSystemTrayIcon::isSystemTrayAvailable() && QSystemTrayIcon::supportsMessages();
}

HighlightActions HighlighterPane::readActions() const
{
    HighlightActions a;
    if (m_colorGroup->isChecked())
        a |= HighlightAction::Color;
    if (m_digestAction->isChecked())
        a |= HighlightAction::Digest;
    if (m_tabAction->isChecked())
        a |= HighlightAction::Tab;
    // Read even when the control is disabled: a rule that arrived from a preset or
    // another machine carrying Notify keeps it, rather than being quietly rewritten by
    // a desktop that happens not to offer notifications. It behaves as if it carried
    // Tab there (MainWindow::handleAlerts) and colours normally.
    if (m_notifyAction->isChecked())
        a |= HighlightAction::Notify;
    return a;
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
    QString text = axes.isEmpty() ? tr("(no match set)") : axes;

    // The NON-colour actions do have to be named, because §7 promises the list previews
    // the rules and the row's own paint can no longer say what a rule does: a rule that
    // only feeds the digest is painted exactly like one that does nothing at all.
    QStringList extras;
    if (r.actions.testFlag(HighlightAction::Digest))
        extras << tr("digest");
    if (r.actions.testFlag(HighlightAction::Tab))
        extras << tr("tab");
    if (r.actions.testFlag(HighlightAction::Notify))
        extras << tr("notify");
    if (!extras.isEmpty())
        text += QStringLiteral(" · ") + extras.join(QStringLiteral(", "));
    else if (!r.actions)
        text += QStringLiteral(" · ") + tr("does nothing");
    return text;
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
        m_colorGroup->setChecked(r.actions.testFlag(HighlightAction::Color));
        m_digestAction->setChecked(r.actions.testFlag(HighlightAction::Digest));
        m_tabAction->setChecked(r.actions.testFlag(HighlightAction::Tab));
        m_notifyAction->setChecked(r.actions.testFlag(HighlightAction::Notify));
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
