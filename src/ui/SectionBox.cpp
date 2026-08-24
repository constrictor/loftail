#include "SectionBox.h"

#include "UiColors.h"

#include <QPainter>
#include <QStyle>
#include <QStyleOptionGroupBox>

namespace loftail {

namespace {

// The gap between the end of the title and the start of the line. Wide enough that the
// two read as a heading and its rule rather than as a strikethrough that missed.
constexpr int kTitleGap = 6;

} // namespace

SectionBox::SectionBox(const QString &title, QWidget *parent) : QGroupBox(title, parent)
{
    // A title row LEFT, on every style — and this is the only lever that does it.
    //
    // Breeze, the style on the reference KDE desktop, centres a group box title and
    // ignores QGroupBox::alignment() while doing it (measured: the label rect is
    // identical with and without setAlignment(Qt::AlignLeft), at x=152 of a 380 px box).
    // Since a checkable box's title row IS the axis's enable control, that put every
    // enable control in the middle of the pane while every control it governs starts at
    // the left edge — and it left this class's own hairline running from mid-pane out,
    // because the line starts where the style says the label ended.
    //
    // A style sheet rule for the title subcontrol overrides it. What it does NOT do,
    // checked on a render under Breeze and Fusion both: it does not lose the frame of a
    // framed box, does not change how the check indicator itself is drawn, and does not
    // reach the body — no rule here matches a child, so children keep drawing through
    // the desktop's style. The rest of the application's group boxes are left alone and
    // go on looking like the desktop; this is the one place where the title row is a
    // control rather than a caption.
    //
    // Not translated: a style sheet is not prose (ARCHITECTURE.md §9.1).
    applyTitleStyle();
}

void SectionBox::applyTitleStyle()
{
    // One rule, both cases, so the two cannot be set from different places and disagree.
    // A heading is centred and bold; a control is hard left. Bold belongs HERE rather than
    // in setFont() because a widget's font reaches its children and this one must not.
    // The heading's padding is load-bearing, not tidiness. Once a style sheet places the
    // title, the frame starts at the widget's own top edge and the title is laid INSIDE it
    // — measured: frame top 0, label rect y=0 — so with no padding the caption's ascent
    // begins on the border line and reads as text stuck to the frame rather than as a
    // heading of the block. TOP padding is what lifts it clear (bottom padding moves the
    // body down and leaves the text where it was), and the side padding widens the break
    // the frame line takes around it. 5 px leaves 5 px above the words and 6 below, which
    // is the balance measured across 0/4/6/8.
    //
    // The alternative a style sheet suggests does not work: a taller `margin-top` on the
    // box moves the frame down but takes the title with it, so the caption ends up ON the
    // line and the body is squeezed. Checked on a render.
    //
    // Padding applies to a heading ONLY. The same on a control's title row would push the
    // check indicator off the left edge, which is the one thing that must not move.
    setStyleSheet(m_heading
                      ? QStringLiteral("QGroupBox::title {"
                                       " subcontrol-origin: margin;"
                                       " subcontrol-position: top center;"
                                       " font-weight: bold;"
                                       " padding: 5px 6px 5px 6px; }")
                      : QStringLiteral("QGroupBox::title {"
                                       " subcontrol-origin: margin;"
                                       " subcontrol-position: top left;"
                                       " left: 0px; }"));
}

void SectionBox::setHeading(bool on)
{
    if (m_heading == on)
        return;
    m_heading = on;
    applyTitleStyle();
}

void SectionBox::setTitleDivider(bool on)
{
    if (m_titleDivider == on)
        return;
    m_titleDivider = on;
    update();
}

QSize SectionBox::titleRowHint() const
{
    QStyleOptionGroupBox option;
    initStyleOption(&option);
    const QRect label =
        style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxLabel, this);
    // Only when there IS one: the rect a style returns for an indicator a box does not
    // have is not meaningful, and QRect()'s bottom() of -1 keeps it out of the maxima.
    const QRect indicator =
        isCheckable() ? style()->subControlRect(QStyle::CC_GroupBox, &option,
                                               QStyle::SC_GroupBoxCheckBox, this)
                      : QRect();
    // Bottom edges, not heights: the title row starts wherever the style put it, and what
    // has to fit inside the widget is where it ENDS.
    const int bottom = qMax(label.bottom(), indicator.bottom());
    return {indicator.width() + kTitleGap + label.width(), bottom + 1};
}

QSize SectionBox::sizeHint() const
{
    const QSize box = QGroupBox::sizeHint();
    const QSize title = titleRowHint();
    // Invalid means "no layout, so nothing asked for a size" — for a body-less section the
    // title row is the whole answer rather than a floor under one.
    if (!box.isValid())
        return title;
    return {qMax(box.width(), title.width()), qMax(box.height(), title.height())};
}

QSize SectionBox::minimumSizeHint() const
{
    const QSize box = QGroupBox::minimumSizeHint();
    const QSize title = titleRowHint();
    return {qMax(box.width(), title.width()), qMax(box.height(), title.height())};
}

void SectionBox::paintEvent(QPaintEvent *event)
{
    QGroupBox::paintEvent(event);
    if (!m_titleDivider)
        return;

    // Where the title actually ended, asked of the style rather than measured from the
    // font: the label rect already accounts for the check indicator, the style's own
    // spacing and the text's alignment, and a hand-rolled fontMetrics() sum would be
    // wrong by a different amount on every style.
    QStyleOptionGroupBox option;
    initStyleOption(&option);
    const QRect label =
        style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxLabel, this);
    if (label.isEmpty())
        return;

    const int x0 = label.right() + kTitleGap;
    const int x1 = width() - 1;
    if (x0 >= x1)
        return; // a title too long to leave room; no line rather than a stub

    // Per paint, and out of the group the box is actually in: a switched-off axis is
    // drawn from QPalette::Disabled, and its divider has to dim with the title beside it
    // rather than stay at full strength over greyed controls.
    const QPalette::ColorGroup group = isEnabled() ? QPalette::Active : QPalette::Disabled;
    QPainter painter(this);
    painter.setPen(dividerColor(palette(), group));
    // Vertically centred on the title text. A group box's frame — when it has one — is
    // drawn through the middle of this row, so this is also exactly where the frame's top
    // edge would run, which is what makes the line read as a continuation of the heading.
    const int y = label.center().y();
    painter.drawLine(x0, y, x1, y);
}

} // namespace loftail
