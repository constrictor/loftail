// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PaneTitleStyle.h"

#include <QAbstractButton>
#include <QDockWidget>
#include <QFontMetrics>
#include <QIcon>
#include <QMainWindow>
#include <QPainter>
#include <QIconEngine>
#include <QPixmap>
#include <QStyleOptionDockWidget>

#include <algorithm>

namespace loftail {

namespace {

// Stroke weight as a fraction of the glyph box. Thin enough to look like a control
// rather than a letter, heavy enough to survive a dark palette — which is exactly what
// the base style's hairline fails at. Floored in absolute pixels so it does not vanish
// in the ~14 px box a dock title bar actually gives a button.
constexpr qreal kStrokeRatio = 0.11;
constexpr qreal kMinStroke = 1.4;

// How much of the button the mark leaves empty on each side. The first attempt used
// 0.30, which left the glyph occupying 40% of an already-small button and reading as a
// smudge — the measured reason the first cut looked WORSE than the stock icons.
constexpr qreal kInsetRatio = 0.24;

void paintGlyph(QPainter *painter, const QRectF &bounds, QStyle::StandardPixmap which,
                const QColor &color)
{
    const qreal side = qMin(bounds.width(), bounds.height());
    if (side <= 0)
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color);
    pen.setWidthF(qMax(kMinStroke, side * kStrokeRatio));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::MiterJoin);
    painter->setPen(pen);

    const qreal inset = side * kInsetRatio;
    const QRectF box = QRectF(bounds.center().x() - side / 2, bounds.center().y() - side / 2,
                              side, side)
                           .adjusted(inset, inset, -inset, -inset);

    if (which == QStyle::SP_TitleBarCloseButton) {
        painter->drawLine(box.topLeft(), box.bottomRight());
        painter->drawLine(box.topRight(), box.bottomLeft());
    } else {
        // Float/undock: ONE window outline with a heavier top edge, not the usual pair
        // of offset rectangles. A dock title button is about 10 px of drawable area, and
        // two overlapping outlines at that size merge into a blob — which is what the
        // first version rendered. Measured, not guessed.
        painter->drawRect(box);
        painter->drawLine(QPointF(box.left(), box.top() + pen.widthF()),
                          QPointF(box.right(), box.top() + pen.widthF()));
    }
    painter->restore();
}

// Draws the glyph AT WHATEVER SIZE IT IS ASKED FOR, rather than scaling a pixmap made
// at a guessed one. That is not a refinement: a dock title button is about 14 px, every
// fixed pixmap size is therefore a downscale, and downscaling a two-stroke mark is
// exactly what made the first attempt blurrier than the icons it replaced.
class GlyphIconEngine final : public QIconEngine
{
public:
    GlyphIconEngine(QStyle::StandardPixmap which, QColor color)
        : m_which(which), m_color(color)
    {
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode, QIcon::State) override
    {
        paintGlyph(painter, rect, m_which, m_color);
    }

    // "As big as you asked": the button's sizeHint multiplies this out, so claiming the
    // full requested box is what gives the mark room inside the button.
    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override { return size; }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap pixmap(size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pixmap;
    }

    QIconEngine *clone() const override { return new GlyphIconEngine(m_which, m_color); }

private:
    QStyle::StandardPixmap m_which;
    QColor                 m_color;
};

// Whether `widget` is one of the buttons ON a dock title bar. QDockWidgetTitleButton is
// private, so this asks the only two things that are actually true of it and are
// reachable through public API.
bool isDockTitleButton(const QWidget *widget)
{
    return widget && qobject_cast<const QAbstractButton *>(widget)
        && qobject_cast<const QDockWidget *>(widget->parentWidget());
}

QColor glyphColor(const QWidget *widget)
{
    const QPalette palette = widget ? widget->palette() : QPalette();
    // WindowText rather than ButtonText: the title bar is painted as part of the dock,
    // not as a button face, so this is what the title text beside it uses.
    return palette.color(QPalette::WindowText);
}

} // namespace

PaneTitleStyle::PaneTitleStyle(QObject *parent)
{
    setParent(parent);
}

bool PaneTitleStyle::isTabbedWithAnother(const QDockWidget *dock)
{
    if (!dock || dock->isFloating())
        return false;
    auto *window = qobject_cast<QMainWindow *>(dock->parentWidget());
    if (!window)
        return false;
    // Only VISIBLE siblings count. A group whose other panes have all been closed from
    // View ▸ Panes shows no tab bar, so the remaining one is the only place its name
    // appears and the title bar has to carry it again.
    const QList<QDockWidget *> siblings =
        window->tabifiedDockWidgets(const_cast<QDockWidget *>(dock));
    return std::ranges::any_of(siblings,
                              [](const QDockWidget *sibling) { return !sibling->isHidden(); });
}

void PaneTitleStyle::drawControl(ControlElement element, const QStyleOption *option,
                                 QPainter *painter, const QWidget *widget) const
{
    if (element == CE_DockWidgetTitle) {
        if (const auto *dock = qobject_cast<const QDockWidget *>(widget);
            dock && isTabbedWithAnother(dock)) {
            if (const auto *dockOption =
                    qstyleoption_cast<const QStyleOptionDockWidget *>(option)) {
                // The tab a row above already says this. Drop the text and keep
                // everything else — the bar still paints, still hit-tests, and is still
                // what Qt starts a drag from.
                QStyleOptionDockWidget quiet(*dockOption);
                quiet.title.clear();
                QProxyStyle::drawControl(element, &quiet, painter, widget);
                return;
            }
        }
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}

int PaneTitleStyle::pixelMetric(PixelMetric metric, const QStyleOption *option,
                                const QWidget *widget) const
{
    switch (metric) {
    case PM_SmallIconSize:
        // Guarded, and the guard is the point: this style is installed on the dock, so
        // every widget INSIDE the pane inherits it too. Resizing their icons is not
        // what this is for — only the title bar's own buttons.
        if (isDockTitleButton(widget)) {
            // Tied to the title bar's font rather than fixed, so it tracks the system
            // font size the way the title text beside it does.
            const int text = QFontMetrics(widget->font()).height();
            return qBound(16, int(text * 0.9), 28);
        }
        break;
    case PM_DockWidgetTitleBarButtonMargin:
        // A larger hit target around the glyph. The base style's margin makes a button
        // barely wider than its icon, which is what puts them a pixel from the
        // scrollbar and makes them easy to miss.
        return QProxyStyle::pixelMetric(metric, option, widget) + 2;
    default:
        break;
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}

QIcon PaneTitleStyle::standardIcon(StandardPixmap standardIcon, const QStyleOption *option,
                                   const QWidget *widget) const
{
    if (standardIcon == SP_TitleBarCloseButton || standardIcon == SP_TitleBarNormalButton) {
        // Reachable only from a pane dock, since this style is installed nowhere else.
        return QIcon(new GlyphIconEngine(standardIcon, glyphColor(widget)));
    }
    return QProxyStyle::standardIcon(standardIcon, option, widget);
}

} // namespace loftail
