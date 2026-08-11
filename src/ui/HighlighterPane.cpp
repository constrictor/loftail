#include "HighlighterPane.h"

#include "AxisEditor.h"
#include "Document.h"
#include "MatchCriteria.h"
#include "Palette.h"
#include "Priority.h"
#include "RecordIndex.h"

#include <QApplication>
#include <QBrush>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QStyleOptionViewItem>
#include <QStylePainter>
#include <QStyledItemDelegate>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QVector>

namespace loftail {

namespace {

constexpr int kGlyphPx = 14;

QIcon swatchIcon(const QColor &c, const QColor &ink)
{
    QPixmap pm(kGlyphPx, kGlyphPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor line = ink;
    if (c.isValid()) {
        p.fillRect(pm.rect(), c);
        // ...inside an outline, which is not decoration: Paper is a near-white and Ink a
        // near-black, so on the theme that matches one of them a swatch with no edge is
        // an empty cell — and the *default* swatch below is exactly what an empty cell
        // is supposed to mean here.
        line.setAlpha(90);
        p.setPen(QPen(line, 1.0));
        p.drawRect(QRectF(0.5, 0.5, kGlyphPx - 1.5, kGlyphPx - 1.5));
    } else {
        // *default* is a choice like any other and has to be visible as one. A blank
        // 14 px square reads as a missing icon, and in an icon-only picker there is no
        // word beside it to say otherwise — so the empty slot is drawn as an empty
        // slot: an outline with a stroke through it.
        line.setAlpha(140);
        p.setPen(QPen(line, 1.0));
        p.drawRect(QRectF(0.5, 0.5, kGlyphPx - 1.5, kGlyphPx - 1.5));
        p.drawLine(QPointF(2.5, kGlyphPx - 2.5), QPointF(kGlyphPx - 2.5, 2.5));
    }
    return QIcon(pm);
}

// The header glyphs. DRAWN, never a character: the offscreen QPA plugin on Windows
// resolves no font at all (CLAUDE.md), so a glyph taken from a font is a guaranteed
// blank there — and these five columns have nothing else to name them, since a word per
// column is exactly the width the table does not have.
enum class Glyph { TextColour, Background, Digest, Notify, Tab };

QIcon headerGlyph(Glyph g, const QColor &ink)
{
    QPixmap pm(kGlyphPx, kGlyphPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    switch (g) {
    case Glyph::TextColour: {
        // An "A", as three strokes rather than as the letter: what a text colour is set
        // on is a letterform, and a letterform is legible at 14 px where a word is not.
        p.drawLine(QPointF(2.5, 11.5), QPointF(7.0, 2.5));
        p.drawLine(QPointF(7.0, 2.5), QPointF(11.5, 11.5));
        p.drawLine(QPointF(4.6, 8.2), QPointF(9.4, 8.2));
        break;
    }
    case Glyph::Background: {
        // The filled counterpart: a whole cell of colour, which is what a background is.
        QColor fill = ink;
        fill.setAlpha(90);
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(2.0, 2.0, 10.0, 10.0), 2.0, 2.0);
        break;
    }
    case Glyph::Digest: {
        // Three lines of a strip, the last one short — a list, which is what the digest
        // under the log is.
        p.drawLine(QPointF(2.5, 4.0), QPointF(11.5, 4.0));
        p.drawLine(QPointF(2.5, 7.0), QPointF(11.5, 7.0));
        p.drawLine(QPointF(2.5, 10.0), QPointF(8.0, 10.0));
        break;
    }
    case Glyph::Notify: {
        QPainterPath bell;
        bell.moveTo(2.6, 10.2);
        bell.cubicTo(4.6, 10.2, 4.0, 3.2, 7.0, 3.2);
        bell.cubicTo(10.0, 3.2, 9.4, 10.2, 11.4, 10.2);
        bell.closeSubpath();
        p.drawPath(bell);
        p.drawLine(QPointF(5.6, 12.0), QPointF(8.4, 12.0));
        break;
    }
    case Glyph::Tab: {
        // A bookmark: the one shape that means "marked" without a word, and the tab
        // marker is a mark on a tab.
        QPainterPath mark;
        mark.moveTo(4.0, 2.2);
        mark.lineTo(10.0, 2.2);
        mark.lineTo(10.0, 12.0);
        mark.lineTo(7.0, 9.4);
        mark.lineTo(4.0, 12.0);
        mark.closeSubpath();
        QColor fill = ink;
        fill.setAlpha(90);
        p.setBrush(fill);
        p.drawPath(mark);
        break;
    }
    }
    return QIcon(pm);
}

// A check column: the indicator CENTRED in its cell, and the whole cell toggling it.
//
// Both halves are the delegate's because neither is available otherwise. Qt lays a view
// item out check-then-icon-then-text from the left edge and ignores the item's alignment
// doing it, so a column holding nothing but a tick draws that tick hard against its left
// border, under a header glyph centred over the column — which reads as the tick
// belonging to the column to its left. And the default hit area is the indicator itself,
// perhaps 13 px wide in a column that is barely wider: in a dock, a tick that has to be
// hit rather than merely aimed at is one the user misses.
class CheckCellDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        // Draw the row's own background (selection, hover, alternating base) and NOTHING
        // else through the item delegate: text and icon are cleared so the style cannot
        // put a second, left-aligned indicator beside the one drawn below.
        opt.text.clear();
        opt.icon = QIcon();
        opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        const QWidget *w = opt.widget;
        QStyle *style = w ? w->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, w);

        QStyleOptionButton box;
        box.rect = indicatorRect(opt, w);
        box.state = QStyle::State_None;
        const bool enabled = index.flags().testFlag(Qt::ItemIsEnabled)
                             && index.flags().testFlag(Qt::ItemIsUserCheckable);
        if (enabled)
            box.state |= QStyle::State_Enabled;
        box.state |= (index.data(Qt::CheckStateRole).toInt() == Qt::Checked)
                         ? QStyle::State_On
                         : QStyle::State_Off;
        style->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &box, painter, w);
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        Q_UNUSED(index);
        const QWidget *w = option.widget;
        QStyle *style = w ? w->style() : QApplication::style();
        const int side = style->pixelMetric(QStyle::PM_IndicatorWidth, &option, w);
        const int pad = style->pixelMetric(QStyle::PM_FocusFrameHMargin, &option, w) + 3;
        return QSize(side + 2 * pad, side + 4);
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        const Qt::ItemFlags flags = index.flags();
        if (!flags.testFlag(Qt::ItemIsUserCheckable) || !flags.testFlag(Qt::ItemIsEnabled))
            return false;

        const auto toggle = [&] {
            const auto state = static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
            return model->setData(index,
                                  state == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                                  Qt::CheckStateRole);
        };

        switch (event->type()) {
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() != Qt::LeftButton || !option.rect.contains(me->position().toPoint()))
                return false;
            return toggle();
        }
        case QEvent::MouseButtonDblClick:
            // Swallowed, or a double click toggles twice and lands back where it began.
            return option.rect.contains(static_cast<QMouseEvent *>(event)->position().toPoint());
        case QEvent::KeyPress: {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (ke->key() != Qt::Key_Space && ke->key() != Qt::Key_Select)
                return false;
            return toggle();
        }
        default:
            return false;
        }
    }

private:
    static QRect indicatorRect(const QStyleOptionViewItem &opt, const QWidget *w)
    {
        QStyle *style = w ? w->style() : QApplication::style();
        const int side = style->pixelMetric(QStyle::PM_IndicatorWidth, &opt, w);
        const int high = style->pixelMetric(QStyle::PM_IndicatorHeight, &opt, w);
        QRect r(0, 0, side, high);
        r.moveCenter(opt.rect.center());
        return r;
    }
};

// A colour picker that shows its colour and not its name.
//
// A QComboBox always draws the current item's text, and the palette's names run to
// "Vivid Amber" — two of those in a rule row would take the width the rule's own summary
// needs. The name is not lost: it is what the popup lists, and what the cell's tooltip
// says. Only the closed state is trimmed, and it is trimmed by clearing the style
// option's text rather than by emptying the item, so findData(), currentData() and the
// popup are all untouched.
class SwatchCombo : public QComboBox
{
public:
    explicit SwatchCombo(QWidget *parent = nullptr) : QComboBox(parent)
    {
        setIconSize(QSize(kGlyphPx, kGlyphPx));
    }

    QSize sizeHint() const override { return minimumSizeHint(); }

    QSize minimumSizeHint() const override
    {
        QStyleOptionComboBox opt;
        initStyleOption(&opt);
        const QSize content(iconSize().width() + 4, iconSize().height() + 4);
        return style()->sizeFromContents(QStyle::CT_ComboBox, &opt, content, this);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter p(this);
        QStyleOptionComboBox opt;
        initStyleOption(&opt);
        opt.currentText.clear(); // the icon carries it; the popup and tooltip carry the name
        p.drawComplexControl(QStyle::CC_ComboBox, opt);
        p.drawControl(QStyle::CE_ComboBoxLabel, opt);
    }
};

// One axis's contribution to a rule's one-line summary, or an empty string when the
// axis is off. Short by necessity: this is one column of a row in a dock-width table.
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

std::optional<HighlightAction> HighlighterPane::actionForColumn(int column)
{
    switch (column) {
    case kColDigest: return HighlightAction::Digest;
    case kColNotify: return HighlightAction::Notify;
    case kColTab:    return HighlightAction::Tab;
    default:         return std::nullopt;
    }
}

void HighlighterPane::applyColourAction(HighlightRule &rule)
{
    // The Colour action has no control of its own any more: the two swatches are it. A
    // rule colours exactly when one of its roles names a palette entry, so *default* on
    // both is what unticking Highlight used to be — a rule that matches and does nothing
    // about it, which is still how a rule is parked rather than deleted.
    if (HighlightPalette::isSlot(rule.background) || HighlightPalette::isSlot(rule.foreground))
        rule.actions |= HighlightAction::Color;
    else
        rule.actions &= ~HighlightActions(HighlightAction::Color);
}

bool HighlighterPane::normaliseRules()
{
    // Rules arrive from three places that never saw this pane — a session, a preset, the
    // record menu — and one of them may predate the swatches being the colour control.
    // Normalising on the way IN rather than trusting the flag is what stops the table
    // showing two swatches on a rule the log is not colouring, or a rule with no colour
    // at all silently shadowing the rule below it for the Colour action.
    bool moved = false;
    for (HighlightRule &r : m_rules) {
        const HighlightActions before = r.actions;
        applyColourAction(r);
        moved = moved || (r.actions != before);
    }
    return moved;
}

QComboBox *HighlighterPane::makeSwatchCombo(int row, Column column)
{
    auto *combo = new SwatchCombo(m_ruleTable);
    // Kept even though every row has one: findChild() is how a test reaches the first
    // rule's pickers, and an object name is the contract in this codebase (§9.1).
    combo->setObjectName(column == kColForeground ? QStringLiteral("textColor")
                                                  : QStringLiteral("backgroundColor"));
    combo->setProperty("ruleRow", row);
    const bool dark = isDark();
    const QColor ink = palette().text().color();
    // Item 0 is the *default* sentinel: leave this role at the theme's normal color.
    combo->addItem(swatchIcon(QColor(), ink), tr("Default"), HighlightPalette::kDefault);
    for (int i = 0; i < HighlightPalette::count(); ++i) {
        // A rule separating the three tone bands, so a list this long reads as
        // "pick a loudness, then a hue" rather than as one run of swatches. A
        // separator is not selectable and carries no data, so findData() and
        // currentData() below are unaffected.
        if (i > 0 && i % HighlightPalette::kSlotsPerBand == 0)
            combo->insertSeparator(combo->count());
        const PaletteSlot &s = HighlightPalette::slot(i);
        combo->addItem(swatchIcon(HighlightPalette::color(i, dark), ink), QString(s.name), i);
    }
    // Twenty-seven slots plus the default and two separators is a taller popup than
    // a style will always fit on a short screen, so cap it and let Qt scroll rather
    // than let the list run off the top or bottom.
    combo->setMaxVisibleItems(HighlightPalette::kSlotsPerBand * 2 + 1);

    const QString role = (column == kColForeground) ? tr("Text colour") : tr("Background colour");
    combo->setAccessibleName(role);
    const auto describe = [combo, role] {
        // The name the closed picker does not show, said where it is asked for.
        combo->setToolTip(QStringLiteral("%1: %2").arg(role, combo->currentText()));
    };
    describe();

    connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, column, describe](int) {
        describe();
        if (m_updating)
            return;
        const int r = combo->property("ruleRow").toInt();
        if (r < 0 || r >= m_rules.size())
            return;
        HighlightRule &rule = m_rules[r];
        (column == kColForeground ? rule.foreground : rule.background) = swatchValue(combo);
        applyColourAction(rule);
        commit();
        refreshRow(r);
    });
    return combo;
}

QComboBox *HighlighterPane::swatchCombo(int row, Column column) const
{
    return qobject_cast<QComboBox *>(m_ruleTable->cellWidget(row, column));
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

    // --- The rules, one per row (SPEC.md §7) --------------------------------
    //
    // A TABLE, not a list: a rule is a tick, a condition and a set of actions, and every
    // one of those is a per-rule answer that the user wants to see across rules. As four
    // checkable sections under the editor they were legible one rule at a time and cost
    // the pane four rows of the height its five axes are already competing for; as
    // columns they are read down the list, set without selecting the rule first, and
    // free the editor to be about the condition alone.
    m_ruleTable = new QTableWidget(0, kColumnCount, this);
    m_ruleTable->setObjectName(QStringLiteral("ruleTable")); // test contract, never translated
    m_ruleTable->setMinimumHeight(120);
    m_ruleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ruleTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_ruleTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ruleTable->setCornerButtonEnabled(false);
    m_ruleTable->setWordWrap(false);
    m_ruleTable->setTextElideMode(Qt::ElideRight);
    // No grid: the row IS the rule, and a lattice round every cell reads as a spreadsheet
    // whose cells are independently meaningful. What separates two rules is that they are
    // painted in their own colours.
    m_ruleTable->setShowGrid(false);
    m_ruleTable->verticalHeader()->setVisible(false);
    m_ruleTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_ruleTable->setIconSize(QSize(kGlyphPx, kGlyphPx));

    auto *checks = new CheckCellDelegate(m_ruleTable);
    for (int column : {kColEnabled, kColDigest, kColNotify, kColTab})
        m_ruleTable->setItemDelegateForColumn(column, checks);

    QHeaderView *head = m_ruleTable->horizontalHeader();
    head->setIconSize(QSize(kGlyphPx, kGlyphPx));
    head->setStretchLastSection(false);
    head->setHighlightSections(false);
    head->setSectionsClickable(false);

    const QColor ink = palette().windowText().color();
    const auto headerCell = [this](int column, const QIcon &icon, const QString &text,
                                   const QString &tip) {
        auto *item = new QTableWidgetItem;
        if (icon.isNull())
            item->setText(text);
        else
            item->setIcon(icon);
        item->setToolTip(tip);
        m_ruleTable->setHorizontalHeaderItem(column, item);
    };
    // The enable column's header is deliberately blank: the tick under it is the rule's
    // own on/off, which needs no naming, and any glyph there would compete with the four
    // that do.
    headerCell(kColEnabled, QIcon(), QString(), tr("Whether this rule is in force."));
    headerCell(kColRule, QIcon(), tr("Rule"), tr("What this rule matches."));
    headerCell(kColForeground, headerGlyph(Glyph::TextColour, ink), QString(),
               tr("Text colour of matching records."));
    headerCell(kColBackground, headerGlyph(Glyph::Background, ink), QString(),
               tr("Background colour of matching records."));
    headerCell(kColDigest, headerGlyph(Glyph::Digest, ink), QString(),
               tr("Show this rule's newest match in the strip under the log."));
    headerCell(kColNotify, headerGlyph(Glyph::Notify, ink), QString(),
               notificationsSupported()
                   ? tr("Raise a desktop notification on the same trigger, at most one "
                        "every ten seconds.")
                   : tr("This desktop offers no notification service, so such a rule "
                        "marks the tab instead."));
    headerCell(kColTab, headerGlyph(Glyph::Tab, ink), QString(),
               tr("Mark this log's tab when a match arrives while it is not the log on "
                  "screen."));

    // The rule's summary takes what is left; every other column is as wide as what it
    // holds. The two colour columns are FIXED, because a cell widget contributes nothing
    // to ResizeToContents — the header glyph would be the only thing measured, and the
    // pickers would be cropped to it.
    {
        SwatchCombo probe;
        m_swatchColumnWidth = probe.sizeHint().width() + 4;
    }
    head->setSectionResizeMode(kColRule, QHeaderView::Stretch);
    for (int column : {kColEnabled, kColDigest, kColNotify, kColTab})
        head->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    for (int column : {kColForeground, kColBackground}) {
        head->setSectionResizeMode(column, QHeaderView::Fixed);
        m_ruleTable->setColumnWidth(column, m_swatchColumnWidth);
    }
    root->addWidget(m_ruleTable);

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

    // --- What the selected rule MATCHES -------------------------------------
    //
    // The five axes and nothing else. There is no "Condition" caption over them and no
    // "Action" box under them: with the actions in the table, the editor is one thing,
    // and a caption over the only half there is names it against nothing. The editor
    // itself is a bare QWidget for the same reason it always was — "Selected rule" said
    // what the table above already says.
    //
    // Five axes do not fit a dock, so this lives in a scroll area. Deliberately NOT
    // collapsed down to title rows while an axis is off, which is what this pane used to
    // do: an axis that shows its controls only once it is ticked has to be switched on to
    // be read, and the answer to a rule editor that does not fit is a scroll bar, not a
    // rule the user cannot see the shape of.
    m_editor = new QWidget(this);
    auto *ev = new QVBoxLayout(m_editor);
    ev->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(m_editor);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    ev->addWidget(scroll);

    auto *content = new QWidget(scroll);
    scroll->setWidget(content);
    auto *cv = new QVBoxLayout(content);
    cv->setContentsMargins(0, 2, 0, 0);

    // Every axis is opt-in for a highlight rule: an unconfigured rule must stay inert
    // (SPEC.md §7), the opposite of the Filters pane's enabled-by-default metadata
    // axes, which exist so their controls act on the first click.
    m_axes = new AxisEditor(AxisEditor::Defaults{/*priorityOn=*/false, /*loggerOn=*/false},
                            content);
    // An axis this log's format cannot fill is left out rather than shown greyed. That is
    // the AxisEditor's own behaviour now and no longer something this pane asks for: the
    // Filters pane used to keep such an axis and explain it in its title, and that
    // asymmetry is gone (AxisEditor::updateAxisState).
    cv->addWidget(m_axes);
    cv->addStretch(1);
    root->addWidget(m_editor, 1);

    // --- Wiring -------------------------------------------------------------
    connect(m_ruleTable, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { loadEditorFor(row); });
    // One handler for all four check columns: the rule's own tick and its three
    // remaining actions are the same kind of edit, made in the same place.
    connect(m_ruleTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_updating)
            return;
        const int row = item->row();
        if (row < 0 || row >= m_rules.size())
            return;
        const bool on = item->checkState() == Qt::Checked;
        if (item->column() == kColEnabled) {
            // A rule's checkbox toggles enable/disable in one click (SPEC.md §8).
            m_rules[row].enabled = on;
        } else if (const std::optional<HighlightAction> action = actionForColumn(item->column())) {
            if (on)
                m_rules[row].actions |= *action;
            else
                m_rules[row].actions &= ~HighlightActions(*action);
        } else {
            return;
        }
        commit();
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
            // Nothing selected: an empty rule, every axis off, so it matches nothing
            // until it is configured. It still takes a colour no other rule is using,
            // so it is visible in the table the moment it appears.
            r.background = nextFreeBackground();
            r.foreground = HighlightPalette::readableTextSlot(r.background);
        }
        applyColourAction(r);
        m_rules.append(r);
        commit();
        reloadRuleTable();
        setCurrentRow(m_rules.size() - 1);
    });
    connect(m_removeBtn, &QPushButton::clicked, this, [this] {
        const int row = currentRow();
        if (row < 0)
            return;
        m_rules.remove(row);
        commit();
        reloadRuleTable();
        setCurrentRow(qMin(row, m_rules.size() - 1));
    });
    connect(m_clearBtn, &QPushButton::clicked, this, [this] {
        // The counterpart of the Filters pane's Clear: one action back to an
        // uncoloured log, which was otherwise Remove pressed once per rule.
        if (m_rules.isEmpty())
            return;
        m_rules.clear();
        commit();
        reloadRuleTable();
    });
    auto move = [this](int delta) {
        const int row = currentRow();
        const int to = row + delta;
        if (row < 0 || to < 0 || to >= m_rules.size())
            return;
        m_rules.swapItemsAt(row, to);
        commit();
        reloadRuleTable();
        setCurrentRow(to);
    };
    connect(m_upBtn, &QPushButton::clicked, this, [move] { move(-1); });
    connect(m_downBtn, &QPushButton::clicked, this, [move] { move(1); });

    connect(m_axes, &AxisEditor::changed, this, [this] {
        if (m_updating)
            return;
        const int row = currentRow();
        if (row < 0 || row >= m_rules.size())
            return;
        m_rules[row].match = m_axes->criteria();
        commit();
        refreshRow(row);
    });
}

bool HighlighterPane::notificationsSupported()
{
    return QSystemTrayIcon::isSystemTrayAvailable() && QSystemTrayIcon::supportsMessages();
}

int HighlighterPane::currentRow() const
{
    return m_ruleTable ? m_ruleTable->currentRow() : -1;
}

void HighlighterPane::setCurrentRow(int row)
{
    if (row >= 0 && row < m_ruleTable->rowCount())
        m_ruleTable->setCurrentCell(row, kColRule);
    else
        m_ruleTable->setCurrentCell(-1, -1);
}

QString HighlighterPane::ruleSummary(const HighlightRule &r) const
{
    // What the rule MATCHES, and nothing else: its colours are what the row is painted
    // in and what its two swatches show, and its other three actions are three ticks on
    // the same row. Naming any of that here would spend the one column with prose in it
    // saying twice what the row already answers.
    const QString axes = axisSummary(r.match);
    return axes.isEmpty() ? tr("(no match set)") : axes;
}

void HighlighterPane::paintRow(int row, const HighlightRule &r) const
{
    // A rule's own colours, resolved for the current theme exactly as the view
    // resolves them, so the row is a preview rather than a description. *default*
    // leaves the role alone: an invalid QColor means "the theme's normal colour", and
    // a default-constructed QBrush is how an item says it has no override.
    //
    // The SELECTED row still paints in the theme's selection colours, hiding its own —
    // Qt's ordinary behaviour, kept deliberately. Which row is selected is now an input
    // to a command ("New" copies it), so selection has to stay unmistakable.
    //
    // Only the summary cell is painted. The check columns must keep the theme's own
    // background or the tick in them stops being legible on a Deep fill, and the two
    // swatch cells carry a widget that would sit on the colour it is offering.
    QTableWidgetItem *item = m_ruleTable->item(row, kColRule);
    if (!item)
        return;
    const bool dark = isDark();
    const QColor bg = HighlightPalette::color(r.background, dark);
    const QColor fg = HighlightPalette::color(r.foreground, dark);
    item->setBackground(bg.isValid() ? QBrush(bg) : QBrush());
    item->setForeground(fg.isValid() ? QBrush(fg) : QBrush());
}

void HighlighterPane::buildRow(int row)
{
    const HighlightRule &r = m_rules.at(row);

    auto *tick = new QTableWidgetItem;
    tick->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    tick->setCheckState(r.enabled ? Qt::Checked : Qt::Unchecked);
    tick->setToolTip(tr("Whether this rule is in force."));
    m_ruleTable->setItem(row, kColEnabled, tick);

    auto *summary = new QTableWidgetItem;
    summary->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_ruleTable->setItem(row, kColRule, summary);

    for (Column column : {kColForeground, kColBackground}) {
        QComboBox *combo = makeSwatchCombo(row, column);
        setSwatchCombo(combo, column == kColForeground ? r.foreground : r.background);
        m_ruleTable->setCellWidget(row, column, combo);
    }

    const bool notifications = notificationsSupported();
    const auto actionCell = [&](Column column, HighlightAction action, const QString &tip) {
        auto *item = new QTableWidgetItem;
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
        if (action == HighlightAction::Notify && !notifications) {
            // Not user-checkable, and it says why. A rule that arrived from a preset or
            // another machine carrying Notify still SHOWS it, rather than being quietly
            // rewritten by a desktop that happens not to offer notifications: it behaves
            // as if it carried Tab there (MainWindow::handleAlerts) and colours normally.
            flags &= ~Qt::ItemIsUserCheckable;
        }
        item->setFlags(flags);
        item->setCheckState(r.actions.testFlag(action) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(tip);
        m_ruleTable->setItem(row, column, item);
    };
    actionCell(kColDigest, HighlightAction::Digest,
               tr("Show this rule's newest match in the strip under the log."));
    // Said before the box can be ticked, not after — the same habit as naming
    // hosts.json before the "remember this password" checkbox is offered.
    actionCell(kColNotify, HighlightAction::Notify,
               notifications ? tr("Raise a desktop notification on the same trigger, at "
                                  "most one every ten seconds.")
                             : tr("This desktop offers no notification service, so this "
                                  "rule marks the tab instead."));
    actionCell(kColTab, HighlightAction::Tab,
               tr("Mark this log's tab when a match arrives while it is not the log on "
                  "screen."));

    refreshRow(row);
}

void HighlighterPane::refreshRow(int row)
{
    if (row < 0 || row >= m_rules.size() || row >= m_ruleTable->rowCount())
        return;
    const HighlightRule &r = m_rules.at(row);
    const bool wasUpdating = m_updating;
    m_updating = true; // a setText here must not re-enter the check handler
    if (QTableWidgetItem *summary = m_ruleTable->item(row, kColRule)) {
        const QString text = ruleSummary(r);
        summary->setText(text);
        summary->setToolTip(text); // the column elides; the tooltip does not
    }
    paintRow(row, r);
    m_updating = wasUpdating;
}

void HighlighterPane::reloadRuleTable()
{
    m_updating = true;
    // setRowCount(0) rather than clearContents(): removing the rows is what takes the
    // per-row swatch pickers with them, and a stale picker still carrying its old
    // "ruleRow" would write a later edit into whatever rule now sits at that index.
    m_ruleTable->setRowCount(0);
    m_ruleTable->setRowCount(m_rules.size());
    for (int row = 0; row < m_rules.size(); ++row)
        buildRow(row);
    m_updating = false;

    const bool has = !m_rules.isEmpty();
    m_editor->setEnabled(has);
    m_clearBtn->setEnabled(has);
    if (has)
        setCurrentRow(0);
    else
        loadEditorFor(-1);
    updateActivity();
}

void HighlighterPane::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::PaletteChange || !m_ruleTable)
        return;
    // Every swatch in every picker, and the header's five glyphs, are pixmaps painted
    // for the theme that was current when they were built. Rebuild, keeping the rule
    // under the cursor — the whole table's contents are derived from m_rules, so this
    // costs nothing but the widgets.
    const int row = currentRow();
    reloadRuleTable();
    setCurrentRow(row);
}

void HighlighterPane::updateActivity()
{
    // Only on a CHANGE, for the reason FilterPane::updateActivity() spells out: the
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
    // SAVED and restored, never forced false on the way out. reloadRuleTable() calls
    // this re-entrantly without meaning to — dropping the rows drops the current cell,
    // which emits currentCellChanged(-1, ...) — and an unconditional `m_updating =
    // false` here then unguards the REST of that function. What followed, while the
    // rules were a QListWidget, was silent and total: the rebuild's own setCheckState
    // calls were read back as user edits and wrote `enabled = false` into every rule.
    // The table's builder has exactly the same shape, so the discipline is the same.
    const bool wasUpdating = m_updating;
    m_updating = true;
    const bool valid = row >= 0 && row < m_rules.size();
    m_editor->setEnabled(valid);
    if (valid) {
        // setCriteria applies the rule's subsystem/thread selection EXACTLY, so moving
        // between rules shows each rule's own values rather than letting the discovery
        // rule ("a name never listed before arrives checked") leak the previous rule's
        // selection — or the whole file's — into this one.
        m_axes->setCriteria(m_rules.at(row).match);
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
    if (normaliseRules())
        syncToDocument(); // no signal: rebinding a pane is not an edit the user made
    reloadRuleTable();
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
        refreshRow(row);
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
    applyColourAction(r);
    m_rules.append(r);
    commit();
    reloadRuleTable();
    setCurrentRow(m_rules.size() - 1);
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
    normaliseRules();
    syncToDocument();
    reloadRuleTable();
    emit highlightersChanged();
}

} // namespace loftail
