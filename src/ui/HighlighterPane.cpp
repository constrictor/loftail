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
#include <QLabel>
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
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>

namespace loftail {

namespace {

// Re-express one display-zone wall clock in a new display zone, keeping the INSTANT it
// names (§5.1: the bound is the moment, the digits are only how it is asked for).
// Returns whether the digits actually moved.
//
// The stored form carries no zone — it is what a QDateTimeEdit produced, or what an ISO
// string with no offset parsed to — so the answer is rebuilt from the converted date and
// time rather than handed back with a zone attached, which criteria() and resolve() would
// then read a second time.
bool reexpressBound(QDateTime &wallClock, const QTimeZone &from, const QTimeZone &to)
{
    if (!wallClock.isValid())
        return false;
    QDateTime at = wallClock;
    at.setTimeZone(from);
    const QDateTime moved = at.toTimeZone(to);
    if (!moved.isValid() || (moved.date() == wallClock.date() && moved.time() == wallClock.time()))
        return false;
    wallClock = QDateTime(moved.date(), moved.time());
    return true;
}

constexpr int kGlyphPx = 14;
// Inside a cell that holds a widget: enough that a framed button does not touch the row
// above and below, and no more — the table is in a dock.
constexpr int kCellMargin = 2;
// Between the two swatch pickers, which share one cell. Small, because they are one
// answer in two halves.
constexpr int kColourGap = 2;
// How far the background picker's tile is held off the edge of its icon, and how far its
// corners are rounded. This margin is the whole of what tells the two pickers apart, so
// it is a measurement rather than a taste: 2 px a side takes a 14 px swatch down to a
// 10 px chip, which is about half its area — and unlike the bar under the text picker's
// letter it is there whatever the rule's two colours are.
constexpr qreal kChipInset = 2.0;
constexpr qreal kChipRadius = 2.0;
// How far the empty-table message is held off the table's frame, so a sentence that
// wraps does not run into it.
constexpr int kPlaceholderInset = 12;

// The letter a text-colour swatch is drawn as, in three strokes inside `box`. Not a
// character from a font: the offscreen QPA plugin on Windows resolves no font at all
// (CLAUDE.md), so a glyph taken from one is a guaranteed blank there.
void drawLetterA(QPainter &p, const QColor &c, qreal weight, const QRectF &box)
{
    const qreal midX = box.center().x();
    p.setPen(QPen(c, weight, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(box.left(), box.bottom()), QPointF(midX, box.top()));
    p.drawLine(QPointF(midX, box.top()), QPointF(box.right(), box.bottom()));
    const qreal crossY = box.top() + box.height() * 0.65;
    const qreal inset = box.width() * 0.22;
    p.drawLine(QPointF(box.left() + inset, crossY), QPointF(box.right() - inset, crossY));
}

// One picker's swatch. It previews the PAIR, not the one colour it sets: the tile is the
// rule's background and the letter on it is the rule's text colour, so every item in
// either picker shows what a matching record would actually look like if that item were
// chosen. Only which half the item varies differs — the text picker runs the LETTER
// through the palette over the background the rule already has, the background picker
// runs the TILE through it under the text colour the rule already has. `counterpart` is
// the other role's colour, already resolved (see HighlighterPane::roleColour), because
// what the preview must show is what the record gets, not what the rule stores.
//
// Which leaves both pickers previewing the same pair, so what says which role a picker
// sets cannot be a colour: it is the tile's GEOMETRY, the one thing the preview leaves
// free.
//
//   Text colour       the tile fills the icon edge to edge — there the field is only
//                     context — and the answer is the letter with a BAR under it, the
//                     long-standing mark for a text-colour control.
//   Background        the tile is an inset, rounded CHIP with a margin round it: the
//                     discrete block of colour that is what this picker chooses.
//
// The margin is what makes the cue independent of the rule: the bar alone is drawn in the
// text colour, so on a rule whose text and background are the same tone it disappeared
// and the two icons came out pixel-identical (0 of 196 differing at 14 px, measured).
// A shape cue survives that, and `tst_highlighterpane` pins it at the same size the row
// draws at. The `kDefault` entry takes the same two shapes, so a picker says which role
// it sets even at the one entry that has no colour to show.
QIcon swatchIcon(const QColor &c, HighlighterPane::ColourRole role, const QColor &ink,
                 const QColor &counterpart)
{
    const bool foreground = (role == HighlighterPane::ColourRole::Foreground);
    QPixmap pm(kGlyphPx, kGlyphPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor line = ink;
    // The role's shape, in two rects: what is filled, and where the hairline round it is
    // stroked (half a pixel in, so a 1 px pen lands on whole pixels).
    const QRectF chip = foreground
                            ? QRectF(0, 0, kGlyphPx, kGlyphPx)
                            : QRectF(kChipInset, kChipInset, kGlyphPx - 2 * kChipInset,
                                     kGlyphPx - 2 * kChipInset);
    const QRectF edge = chip.adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = foreground ? 0.0 : kChipRadius;
    const auto outline = [&p, &edge, radius] {
        if (radius > 0.0)
            p.drawRoundedRect(edge, radius, radius);
        else
            p.drawRect(edge);
    };

    if (!c.isValid()) {
        // *default* is a choice like any other and has to be visible as one, in either
        // role. A blank 14 px square reads as a missing icon, and in an icon-only picker
        // there is no word beside it to say otherwise — so the empty slot is drawn as an
        // empty slot: the role's own outline with a stroke through it. Deliberately NOT
        // previewed like the rest: previewed, the theme's own text on the theme's own
        // base is what Ink-on-Paper already looks like, and the one thing this item has
        // to say is that it names no colour at all. Nothing here is painted solid, which
        // is what keeps "no colour" true whatever the theme is.
        line.setAlpha(140);
        p.setPen(QPen(line, 1.0));
        p.setBrush(Qt::NoBrush);
        outline();
        const qreal in = foreground ? 2.0 : 1.5;
        p.drawLine(QPointF(edge.left() + in, edge.bottom() - in),
                   QPointF(edge.right() - in, edge.top() + in));
        return QIcon(pm);
    }

    const QColor text = foreground ? c : counterpart;
    if (foreground) {
        p.fillRect(chip, counterpart);
        // The letter sits in what is left above the bar, so the two together are centred
        // rather than the letter alone being pushed off-centre by it.
        drawLetterA(p, text, 1.6, QRectF(3.0, 2.0, 8.0, 6.6));
        p.fillRect(QRectF(2.4, 9.8, kGlyphPx - 4.8, 2.4), text);
    } else {
        QPainterPath fill;
        fill.addRoundedRect(chip, kChipRadius, kChipRadius);
        p.fillPath(fill, c);
        // Smaller, because the chip is: the letter is what makes the entry a preview of
        // the pair rather than a colour on its own, and the chip is what makes it this
        // picker's.
        drawLetterA(p, text, 1.4, QRectF(3.9, 3.2, 6.2, 6.6));
    }
    // ...inside an outline, which is not decoration: Paper is a near-white and Ink a
    // near-black, so on the theme that matches one of them a swatch with no edge is an
    // empty cell — and the *default* swatch above is exactly what an empty cell means.
    line.setAlpha(90);
    p.setPen(QPen(line, 1.0));
    p.setBrush(Qt::NoBrush);
    outline();
    return QIcon(pm);
}

// The three action glyphs, drawn for the same reason the letter above is.
enum class Glyph { Digest, Notify, Tab };

// One glyph in one state. OFF is a thin outline in muted ink; ON is the same shape
// FILLED, at full ink and a heavier stroke.
//
// The state has to be in the glyph because it is nowhere else. A checked QToolButton is
// a slightly darker panel — on Fusion, a gradient a few percent down from the unchecked
// one — which at 14 px inside a dock is invisible, and worse than invisible in this
// table: a selected row already tints every button in it, so the only cue distinguishing
// on from off was ALSO the cue distinguishing the current rule from the rest. Rendered
// side by side, "digest on" and "digest off" were the same button. Outline-versus-filled
// is the one contrast that survives at this size, in either theme and under any style,
// because it does not depend on the style drawing anything at all.
//
// Deliberately monochrome. Colour is what the two swatches in the very next cell mean,
// and an action button that went coloured when switched on would read as a third colour
// choice on the same row.
QPixmap actionPixmap(Glyph g, const QColor &ink, bool on)
{
    QPixmap pm(kGlyphPx, kGlyphPx);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    QColor stroke = ink;
    if (!on)
        stroke.setAlpha(115); // present, plainly not in force
    p.setPen(QPen(stroke, on ? 1.6 : 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(on ? QBrush(stroke) : QBrush(Qt::NoBrush));

    switch (g) {
    case Glyph::Digest: {
        // Three lines of a strip, the last one short — a list, which is what the digest
        // under the log is. Its "fill" is the weight of the rules themselves: there is
        // no interior to a line, so ON draws them as bars.
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(stroke, on ? 2.2 : 1.1, Qt::SolidLine, Qt::RoundCap));
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
        p.drawPath(mark);
        break;
    }
    }
    return pm;
}

// Both states in one icon, which is what makes a checkable QToolButton swap them for
// free: Qt asks for QIcon::On while the button is checked and QIcon::Off while it is
// not. Nothing has to hear about a toggle, so no state can be missed — a rule loaded
// from a session draws itself correctly without a refresh.
QIcon actionGlyph(Glyph g, const QColor &ink)
{
    QIcon icon;
    icon.addPixmap(actionPixmap(g, ink, false), QIcon::Normal, QIcon::Off);
    icon.addPixmap(actionPixmap(g, ink, true), QIcon::Normal, QIcon::On);
    return icon;
}

// The enable column: the indicator CENTRED in its cell, and the whole cell toggling it.
//
// Both halves are the delegate's because neither is available otherwise. Qt lays a view
// item out check-then-icon-then-text from the left edge and ignores the item's alignment
// doing it, so a column holding nothing but a tick draws that tick hard against its left
// border, a column's width away from the middle of the space it is answering for. And the
// default hit area is the indicator itself, perhaps 13 px wide in a column that is barely
// wider: in a dock, a tick that has to be hit rather than merely aimed at is one the user
// misses.
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

QComboBox *HighlighterPane::makeSwatchCombo(int row, ColourRole role, QWidget *parent)
{
    const bool foreground = (role == ColourRole::Foreground);
    auto *combo = new SwatchCombo(parent);
    // Kept even though every row has a pair: it is how a test reaches a picker inside a
    // cell, and an object name is the contract in this codebase (§9.1).
    combo->setObjectName(foreground ? QStringLiteral("textColor")
                                    : QStringLiteral("backgroundColor"));
    combo->setProperty("ruleRow", row);
    // Item 0 is the *default* sentinel: leave this role at the theme's normal color.
    // NO ICONS HERE: every swatch previews this rule's pair, which needs the OTHER
    // picker's value, so all of them are painted by updateColourPreviews() once both
    // pickers exist — and repainted there whenever either value moves.
    combo->addItem(tr("Default"), HighlightPalette::kDefault);
    for (int i = 0; i < HighlightPalette::count(); ++i) {
        // A rule separating the three tone bands, so a list this long reads as
        // "pick a loudness, then a hue" rather than as one run of swatches. A
        // separator is not selectable and carries no data, so findData() and
        // currentData() below are unaffected.
        if (i > 0 && i % HighlightPalette::kSlotsPerBand == 0)
            combo->insertSeparator(combo->count());
        const PaletteSlot &s = HighlightPalette::slot(i);
        combo->addItem(QString(s.name), i);
    }
    // Twenty-seven slots plus the default and two separators is a taller popup than
    // a style will always fit on a short screen, so cap it and let Qt scroll rather
    // than let the list run off the top or bottom.
    combo->setMaxVisibleItems(HighlightPalette::kSlotsPerBand * 2 + 1);

    const QString name = foreground ? tr("Text colour") : tr("Background colour");
    combo->setAccessibleName(name);
    const auto describe = [combo, name] {
        // The name the closed picker does not show, said where it is asked for. With no
        // header row, this and the swatch's own shape are the whole of what says which
        // role a picker sets.
        combo->setToolTip(QStringLiteral("%1: %2").arg(name, combo->currentText()));
    };
    describe();

    connect(combo, &QComboBox::currentIndexChanged, this,
            [this, combo, foreground, describe](int) {
                describe();
                if (m_updating)
                    return;
                const int r = combo->property("ruleRow").toInt();
                if (r < 0 || r >= m_rules.size())
                    return;
                HighlightRule &rule = m_rules[r];
                (foreground ? rule.foreground : rule.background) = swatchValue(combo);
                applyColourAction(rule);
                commit();
                refreshRow(r);
                // The colour just chosen is the ground the OTHER picker previews its
                // items on, so its whole list is now out of date. Repainting both is
                // twenty-eight 14 px pixmaps and keeps the two halves of one answer
                // from ever being painted by different rules.
                updateColourPreviews(r);
            });
    return combo;
}

QColor HighlighterPane::roleColour(int paletteIndex, ColourRole role) const
{
    // What a role actually PAINTS with, which is what the other picker has to preview
    // against: the palette entry, or — for *default* — the theme's own colour, the same
    // fallback LogView applies when it draws the record (base under, text over).
    const QColor c = HighlightPalette::color(paletteIndex, isDark());
    if (c.isValid())
        return c;
    return role == ColourRole::Foreground ? palette().text().color()
                                          : palette().base().color();
}

void HighlighterPane::updateColourPreviews(int row)
{
    if (row < 0 || row >= m_rules.size() || row >= m_ruleTable->rowCount())
        return;
    QWidget *cell = m_ruleTable->cellWidget(row, kColColours);
    if (!cell)
        return;
    const HighlightRule &r = m_rules.at(row);
    const bool dark = isDark();
    const QColor ink = palette().text().color();
    const QColor fg = roleColour(r.foreground, ColourRole::Foreground);
    const QColor bg = roleColour(r.background, ColourRole::Background);
    for (ColourRole role : {ColourRole::Foreground, ColourRole::Background}) {
        const bool foreground = (role == ColourRole::Foreground);
        auto *combo = cell->findChild<QComboBox *>(foreground ? QStringLiteral("textColor")
                                                              : QStringLiteral("backgroundColor"));
        if (!combo)
            continue;
        for (int i = 0; i < combo->count(); ++i) {
            const QVariant v = combo->itemData(i);
            if (!v.isValid())
                continue; // a band separator, which carries no slot
            // kDefault yields an invalid colour, which is the *default* swatch.
            combo->setItemIcon(i, swatchIcon(HighlightPalette::color(v.toInt(), dark), role, ink,
                                             foreground ? bg : fg));
        }
    }
}

QToolButton *HighlighterPane::makeActionButton(int row, HighlightAction action, QWidget *parent)
{
    // A pressed-in icon button, not a tick. Three ticks in a row say only "three things
    // are set"; a glyph says WHICH three without a header to name the columns, and a
    // button that stays down is how a toggle says it is on. Framed, never auto-raised —
    // the frame is what says "press this" (the rule AxisEditor's buttons follow).
    auto *button = new QToolButton(parent);
    button->setCheckable(true);
    button->setAutoRaise(false);
    button->setFocusPolicy(Qt::TabFocus);
    button->setIconSize(QSize(kGlyphPx, kGlyphPx));
    button->setProperty("ruleRow", row);

    const QColor ink = palette().buttonText().color();
    switch (action) {
    case HighlightAction::Digest:
        button->setObjectName(QStringLiteral("actionDigest")); // test contract, never translated
        button->setIcon(actionGlyph(Glyph::Digest, ink));
        button->setAccessibleName(tr("Digest"));
        button->setToolTip(tr("Show this rule's newest match in the strip under the log."));
        break;
    case HighlightAction::Notify:
        button->setObjectName(QStringLiteral("actionNotify"));
        button->setIcon(actionGlyph(Glyph::Notify, ink));
        button->setAccessibleName(tr("Notify"));
        // Said before the button can be pressed, not after — the same habit as naming
        // hosts.json before the "remember this password" checkbox is offered.
        if (notificationsSupported()) {
            button->setToolTip(tr("Raise a desktop notification on the same trigger, at "
                                  "most one every ten seconds."));
        } else {
            // Disabled, not hidden, and it still SHOWS a rule that carries Notify: a rule
            // from a preset or another machine keeps the action rather than being quietly
            // rewritten by a desktop that cannot deliver it — it behaves as if it carried
            // Tab there (MainWindow::handleAlerts) and colours normally.
            button->setEnabled(false);
            button->setToolTip(tr("This desktop offers no notification service, so this "
                                  "rule marks the tab instead."));
        }
        break;
    case HighlightAction::Tab:
        button->setObjectName(QStringLiteral("actionTab"));
        button->setIcon(actionGlyph(Glyph::Tab, ink));
        button->setAccessibleName(tr("Mark tab"));
        button->setToolTip(tr("Mark this log's tab when a match arrives while it is not "
                              "the log on screen."));
        break;
    case HighlightAction::Color:
        break; // not a button: the two swatches are the colour control
    }

    connect(button, &QToolButton::toggled, this, [this, button, action](bool on) {
        if (m_updating)
            return;
        const int r = button->property("ruleRow").toInt();
        if (r < 0 || r >= m_rules.size())
            return;
        if (on)
            m_rules[r].actions |= action;
        else
            m_rules[r].actions &= ~HighlightActions(action);
        commit();
    });
    return button;
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
    // FLUSH, exactly as FilterPane's outer layout is, because the two panes must inset
    // the same axes by the same amount: `AxisEditor` supplies its own kSideMargin and a
    // layout margin here is added to it, which put a rule's Subsystem box 17 px from the
    // dock edge against a filter's 6 — visible the moment the two are compared, and the
    // sort of difference that reads as one of them being wrong. Everything above the
    // editor is then inset by hand, to the same 6.
    root->setContentsMargins(0, 0, 0, 0);

    // The table and the buttons under it, indented to where the axes below them start.
    auto *top = new QVBoxLayout;
    top->setContentsMargins(AxisEditor::kSideMargin, AxisEditor::kSideMargin,
                            AxisEditor::kSideMargin, 0);

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
    m_ruleTable->setIconSize(QSize(kGlyphPx, kGlyphPx));
    m_ruleTable->setItemDelegateForColumn(kColEnabled, new CheckCellDelegate(m_ruleTable));

    // NO HEADER ROW. It named five columns with glyphs, which is a row of the pane's
    // scarcest resource spent on a legend for icons that are now in the cells themselves:
    // an action is an icon button wearing its own glyph, and the two swatches say which
    // role they set by their shape. What is left to name is one column of prose, and a
    // column of prose in a table of six needs no caption to be found.
    QHeaderView *head = m_ruleTable->horizontalHeader();
    head->setVisible(false);
    head->setStretchLastSection(false);

    // The rule's summary takes what is left; every other column is as wide as the widget
    // it holds, measured ONCE from a prototype. Fixed rather than ResizeToContents,
    // because a cell *widget* contributes nothing to that mode — with no header item
    // either, there would be nothing left to measure and the columns would collapse.
    {
        SwatchCombo swatchProbe;
        QToolButton buttonProbe;
        buttonProbe.setCheckable(true);
        buttonProbe.setIconSize(QSize(kGlyphPx, kGlyphPx));
        buttonProbe.setIcon(actionGlyph(Glyph::Digest, palette().buttonText().color()));
        m_colourColumnWidth = 2 * swatchProbe.sizeHint().width() + kColourGap + 2 * kCellMargin;
        m_actionColumnWidth = buttonProbe.sizeHint().width() + 2 * kCellMargin;
        // And the row is as tall as the tallest of them: a cell widget is resized to its
        // cell, so a row sized from the text in the summary column would squash both.
        m_rowHeight = qMax(swatchProbe.sizeHint().height(), buttonProbe.sizeHint().height())
                      + 2 * kCellMargin;
    }
    head->setSectionResizeMode(kColRule, QHeaderView::Stretch);
    head->setSectionResizeMode(kColEnabled, QHeaderView::Fixed);
    m_ruleTable->setColumnWidth(kColEnabled, m_actionColumnWidth);
    head->setSectionResizeMode(kColColours, QHeaderView::Fixed);
    m_ruleTable->setColumnWidth(kColColours, m_colourColumnWidth);
    for (int column : {kColDigest, kColNotify, kColTab}) {
        head->setSectionResizeMode(column, QHeaderView::Fixed);
        m_ruleTable->setColumnWidth(column, m_actionColumnWidth);
    }
    m_ruleTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_ruleTable->verticalHeader()->setDefaultSectionSize(m_rowHeight);

    // What the table says when it has nothing to show. The main window's centre gets
    // "No file open. Open a log file to begin." when there is no document; this table
    // got nothing at all, so the one state in which the pane has least to say was the
    // one it explained least — an empty framed void under five buttons, with the axis
    // editor below it correctly greyed and equally unexplained.
    //
    // A label laid OVER the viewport, never a row in the table: a row would be a rule
    // to everything that walks rows (the reorder buttons, the per-row "ruleRow"
    // property, saveState()), and nothing in the table has a way to be uncountable.
    // Transparent to the mouse so it cannot swallow a click on a table that will grow
    // rows under it, and it carries an object name because that is the test contract
    // (ARCHITECTURE.md §9.1) — never its wording, which is translated prose.
    m_tablePlaceholder = new QLabel(m_ruleTable->viewport());
    m_tablePlaceholder->setObjectName(QStringLiteral("ruleTablePlaceholder"));
    m_tablePlaceholder->setAlignment(Qt::AlignCenter);
    m_tablePlaceholder->setWordWrap(true);
    m_tablePlaceholder->setTextInteractionFlags(Qt::NoTextInteraction);
    m_tablePlaceholder->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_tablePlaceholder->hide();
    applyPlaceholderColour();
    // The viewport is not a layout, so nothing else would ever resize a child of it.
    m_ruleTable->viewport()->installEventFilter(this);

    top->addWidget(m_ruleTable);

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
    top->addLayout(btnRow);
    root->addLayout(top);

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
    scroll->setObjectName(QStringLiteral("highlighterScroll")); // test contract
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    ev->addWidget(scroll);

    // Every axis is opt-in for a highlight rule: an unconfigured rule must stay inert
    // (SPEC.md §7), the opposite of the Filters pane's enabled-by-default metadata
    // axes, which exist so their controls act on the first click.
    m_axes = new AxisEditor(AxisEditor::Defaults{/*priorityOn=*/false, /*loggerOn=*/false},
                            scroll);
    // An axis this log's format cannot fill is left out rather than shown greyed. That is
    // the AxisEditor's own behaviour now and no longer something this pane asks for: the
    // Filters pane used to keep such an axis and explain it in its title, and that
    // asymmetry is gone (AxisEditor::updateAxisState).
    //
    // The editor is the scroll area's OWN WIDGET, exactly as it is in the Filters pane,
    // and NOT a widget in a container's layout with a stretch under it. That container
    // was the trailing addStretch() `AxisEditor` deleted from its own root layout and
    // says so at length: a stretch below the axes claims every spare pixel, so the
    // subsystem and thread lists could only ever be as tall as their floors while an
    // empty gap grew under Time range. With `widgetResizable`, the spare height reaches
    // the editor and its two value axes — which carry a stretch each — divide it.
    scroll->setWidget(m_axes);
    root->addWidget(m_editor, 1);

    // --- Wiring -------------------------------------------------------------
    connect(m_ruleTable, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
                loadEditorFor(row);
                // Remove, Up and Down are all statements about the current row, so this
                // is where three of the four are decided; the fourth rides along rather
                // than acquiring a call site of its own.
                updateRuleButtons();
            });
    // The rule's own tick is the one thing in the table that is an ITEM rather than a
    // widget, because it is not an action: it says whether the rule runs at all, and a
    // press-and-stay button beside three of them would read as a fourth action.
    connect(m_ruleTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_updating || item->column() != kColEnabled)
            return;
        const int row = item->row();
        if (row < 0 || row >= m_rules.size())
            return;
        // A rule's checkbox toggles enable/disable in one click (SPEC.md §8).
        m_rules[row].enabled = (item->checkState() == Qt::Checked);
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
    // Only the summary cell is painted. The enable column must keep the theme's own
    // background or the tick in it stops being legible on a Deep fill, and every other
    // cell carries a widget — an icon button, or a picker that would sit on the very
    // colour it is offering.
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

    // Both pickers in ONE cell, text before background: they are one answer — how a
    // matching record is drawn — and a column each spent that answer's width twice over
    // in a table that also has to hold a line of prose.
    auto *colours = new QWidget(m_ruleTable);
    auto *colourRow = new QHBoxLayout(colours);
    colourRow->setContentsMargins(kCellMargin, kCellMargin, kCellMargin, kCellMargin);
    colourRow->setSpacing(kColourGap);
    for (ColourRole role : {ColourRole::Foreground, ColourRole::Background}) {
        QComboBox *combo = makeSwatchCombo(row, role, colours);
        setSwatchCombo(combo, role == ColourRole::Foreground ? r.foreground : r.background);
        colourRow->addWidget(combo);
    }
    m_ruleTable->setCellWidget(row, kColColours, colours);
    // Both pickers exist now, which is exactly what their swatches need: each one
    // previews its items against the colour the other one holds.
    updateColourPreviews(row);

    // One icon button per action, centred in its cell rather than filling it: stretched
    // to a cell a button stops looking like a button and starts looking like a panel.
    const auto actionCell = [&](Column column, HighlightAction action) {
        auto *holder = new QWidget(m_ruleTable);
        auto *box = new QHBoxLayout(holder);
        box->setContentsMargins(kCellMargin, kCellMargin, kCellMargin, kCellMargin);
        QToolButton *button = makeActionButton(row, action, holder);
        button->setChecked(r.actions.testFlag(action));
        box->addWidget(button, 0, Qt::AlignCenter);
        m_ruleTable->setCellWidget(row, column, holder);
    };
    actionCell(kColDigest, HighlightAction::Digest);
    actionCell(kColNotify, HighlightAction::Notify);
    actionCell(kColTab, HighlightAction::Tab);

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
    if (has)
        setCurrentRow(0);
    else
        loadEditorFor(-1);
    // After the selection has settled, never before: three of the four buttons are
    // about the current row, and the row this rebuild ends on is not the one it
    // started with.
    updateRuleButtons();
    updatePlaceholder();
    updateActivity();
}

void HighlighterPane::updatePlaceholder()
{
    if (!m_tablePlaceholder)
        return;
    const bool empty = m_rules.isEmpty();
    if (empty) {
        // TWO empty states, and they want different words. With no document there is
        // nothing a rule could be about, so the answer is the main window's own — open
        // a log. With a document, the table is empty because the user emptied it, so the
        // answer is how to put something back, and the record menu is named because it
        // is the discoverable route in: nobody finds "New" and then five axes on their
        // own, but everybody right-clicks a line they want to see again.
        m_tablePlaceholder->setText(
            m_document ? tr("No highlight rules. Press New, or right-click a record in "
                            "the log and choose a Highlight command, to colour every "
                            "record that matches.")
                       : tr("No file open. Open a log file to add highlight rules."));
        // The geometry a resize would have given it, in case none has arrived yet — the
        // table may never have been laid out when the first document arrives.
        m_tablePlaceholder->setGeometry(m_ruleTable->viewport()->rect().adjusted(
            kPlaceholderInset, kPlaceholderInset, -kPlaceholderInset, -kPlaceholderInset));
    }
    m_tablePlaceholder->setVisible(empty);
}

void HighlighterPane::applyPlaceholderColour()
{
    if (!m_tablePlaceholder)
        return;
    // From the palette, never a constant: this pane is read on a light theme and a dark
    // one, and a grey chosen for either is invisible on the other. PlaceholderText is
    // the role Qt keeps for text that is not content, and a theme that does not define
    // it still gets a muted default derived from its own text colour.
    QPalette pal = m_tablePlaceholder->palette();
    pal.setColor(QPalette::WindowText, palette().placeholderText().color());
    m_tablePlaceholder->setPalette(pal);
}

bool HighlighterPane::eventFilter(QObject *watched, QEvent *event)
{
    if (m_tablePlaceholder && m_ruleTable && watched == m_ruleTable->viewport()
        && event->type() == QEvent::Resize) {
        m_tablePlaceholder->setGeometry(m_ruleTable->viewport()->rect().adjusted(
            kPlaceholderInset, kPlaceholderInset, -kPlaceholderInset, -kPlaceholderInset));
    }
    return QWidget::eventFilter(watched, event);
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
    // The empty-table message is muted by a colour taken from the OLD palette, so it
    // has to be re-taken here or it stays legible only on the theme it was built under.
    applyPlaceholderColour();
    reloadRuleTable();
    setCurrentRow(row);
}

bool HighlighterPane::hasCustomRules() const
{
    if (!m_document)
        return false;
    // The seed is rebuilt rather than cached: it is three rules, this runs on an edit
    // and not per record, and a cached copy is one more thing that can be stale after
    // the defaults are changed. The comparison is the whole list in order, which is
    // QList's own == over HighlightRule::operator== — so every field a rule carries is
    // covered by construction, and a field added to the rule without being added to
    // that operator is the one way this goes quietly wrong.
    return m_rules != HighlighterSet::defaults().rules;
}

void HighlighterPane::updateActivity()
{
    // Only on a CHANGE, for the reason FilterPane::updateActivity() spells out: the
    // marker rides the dock's window title, which is a QTabBar entry while the panes
    // are tabbed, and re-setting it relays out the whole bar.
    const bool active = hasCustomRules();
    if (m_activeState.has_value() && *m_activeState == active)
        return;
    m_activeState = active;
    emit activityChanged(active);
}

void HighlighterPane::updateRuleButtons()
{
    // Every button under the table needs something to act on, and only Clear used to
    // say so: with no rules, three live buttons sat beside one correctly greyed, and
    // pressing any of them did nothing at all. A disabled button is the honest answer —
    // it says the command exists and what it is waiting for, where a live one that
    // silently declines says the press was missed.
    //
    // ONE writer for all four. Called from exactly two places, which are the only two
    // that can move either input: reloadRuleTable(), where the rule count changes, and
    // the table's currentCellChanged, where the row does. Nothing here emits, so it is
    // safe under reloadRuleTable()'s re-entrancy through loadEditorFor().
    //
    // New is deliberately NOT tracked here: what it needs is a document, and the pane
    // greys itself entire without one (setDocument). With a document and an empty table
    // it is the one live button in the row, which is exactly right — it is the way out
    // of that state, and the placeholder over the table names it.
    const int row = currentRow();
    const bool selected = row >= 0 && row < m_rules.size();
    m_removeBtn->setEnabled(selected);
    m_upBtn->setEnabled(selected && row > 0);
    m_downBtn->setEnabled(selected && row < m_rules.size() - 1);
    m_clearBtn->setEnabled(!m_rules.isEmpty());
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
    // Every edit, not only the ones that add or remove a rule. While the marker was
    // "are there any rules" the rebuild was the only place the answer could move;
    // now that it is "are these still the seeded rules", unticking one, recolouring
    // one or retyping its axes moves it too — and commit() is the single funnel all
    // of those already go through.
    updateActivity();
    emit highlightersChanged();
}

void HighlighterPane::setDocument(Document *document)
{
    m_document = document;
    setEnabled(document != nullptr);
    // The zone the rules about to be taken from this document are written in. Rebinding
    // is not a zone change, so nothing is re-expressed here — refreshTimeBounds() is
    // what moves it, and it moves the bounds with it.
    m_boundZone = document ? document->displayZone() : QTimeZone();
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
    // The editors first: they show the selected rule's bounds in whatever terms the
    // timestamp column now reads in, which is a question about digits and not about
    // rules (AxisEditor::refreshTimeBounds).
    m_axes->refreshTimeBounds();

    // Then the rules themselves — and ONLY their time bounds, ONLY the valid ones, and
    // only when the DISPLAY ZONE moved. Three things this must not become:
    //
    //  - It must not read the whole of criteria() back into the selected rule. That
    //    function is not the inverse of setCriteria(): the date editors always hold a
    //    datetime, so a rule with no bounds comes back holding 2000-01-01. Writing
    //    that back is what made clicking a run row — or a live log simply restarting,
    //    through followLastRunIfMoved() — rewrite this log's seeded rules, light the
    //    Highlighters marker and persist both, with no gesture and no way back.
    //  - It must not fix up the selected rule alone. Every rule's digits are read in
    //    the current display zone by MatchCriteria::resolve(), so a zone change
    //    re-points all of them; leaving the rest alone silently moves what they match
    //    while the pane shows no change at all.
    //  - It must not key on the display MODE or on the seconds baseline. A run
    //    selection and an As Written ↔ Epoch Seconds switch leave Document's display
    //    zone exactly where it was (recomputeDisplayZone), so they legitimately write
    //    nothing here.
    //
    // The conversion is done on the STORED value, old zone → instant → new zone, so a
    // bound survives a zone change intact whether or not its rule is the one on screen.
    const QTimeZone was = m_boundZone;
    const QTimeZone now = m_document ? m_document->displayZone() : QTimeZone();
    m_boundZone = now;
    if (!was.isValid() || !now.isValid() || was == now)
        return;

    bool moved = false;
    for (int row = 0; row < m_rules.size(); ++row) {
        MatchCriteria &m = m_rules[row].match;
        // Both bounds, never short-circuited: `||` would leave `end` in the old zone
        // whenever `start` had already moved.
        const bool startMoved = reexpressBound(m.start, was, now);
        const bool endMoved = reexpressBound(m.end, was, now);
        if (!startMoved && !endMoved)
            continue;
        moved = true;
        refreshRow(row); // the summary names the bounds
    }
    // commit() is what emits highlightersChanged() and re-tests the marker, and this
    // function is on the ingest path via followLastRunIfMoved(): call it only when a
    // rule actually changed, or every tick of a log that restarts pays for a re-resolve
    // and a tab-bar relayout.
    if (moved)
        commit();
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
