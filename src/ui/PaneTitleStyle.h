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

#pragma once

#include <QProxyStyle>

QT_BEGIN_NAMESPACE
class QDockWidget;
QT_END_NAMESPACE

namespace loftail {

// How a side pane's title bar is drawn (SPEC.md §8). Installed on the pane docks only —
// never on the window — so nothing else in the application inherits any of it.
//
// It exists because Qt's default dock title bar has two problems when the panes ship
// TABBED TOGETHER, which is how loftail arranges them:
//
//   1. The name is printed TWICE, once on the tab and again on the title bar directly
//      below it, which is the tab's own label repeated a row lower.
//   2. Its close and float buttons are drawn at PM_SmallIconSize with the base style's
//      hairline glyphs — around 12 px of thin, low-contrast line art squeezed against
//      the scrollbar. They are hard to see on a dark palette and harder to hit.
//
// THE OBVIOUS FIX IS NOT AVAILABLE, and the reason is worth recording because it looks
// like an oversight otherwise. Hiding the title bar outright (setTitleBarWidget with an
// empty widget, the documented idiom) would remove both problems and reclaim a row —
// but a dock can only be DRAGGED by Qt's own title bar. A custom title bar widget never
// receives the drag, Qt exposes no public way to start one, and dragging a tab in the
// dock tab bar only reorders it. All three were measured, not assumed. Since the panes
// ship tabbed, hiding the bar would make `SPEC.md` §8's "dragging a pane moves that
// pane" unreachable in the default layout: a pane could never be pulled out again.
//
// So the real title bar stays, and this changes only how it is PAINTED.
class PaneTitleStyle : public QProxyStyle
{
public:
    explicit PaneTitleStyle(QObject *parent = nullptr);

    // Blanks the title TEXT while the dock is tabbed, leaving the bar itself — and
    // therefore the drag — intact. Deliberately done here rather than by clearing the
    // dock's windowTitle(), which is also where the tab bar gets its label: blanking
    // that would blank the tab, which is the one place the name belongs.
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget) const override;

    // Bigger title-bar buttons, and a bigger margin around them. Guarded to the dock
    // title buttons themselves: PM_SmallIconSize reaches every child of the pane too,
    // and resizing the icons inside the Filters pane is not what this is for.
    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override;

    // Crisp close and float glyphs drawn in the widget's own text colour, instead of
    // the base style's washed-out line art. Antialiased and generated at several sizes
    // so Qt can pick one rather than scale a 12 px bitmap.
    QIcon standardIcon(StandardPixmap standardIcon, const QStyleOption *option,
                       const QWidget *widget) const override;

    // Whether `dock` currently shares a tab bar with another VISIBLE pane — i.e.
    // whether its name is already on screen a row above. Asked on paint, so it is
    // always current; there is no cached state to go stale, and no "tabification
    // changed" signal to hang one off (Qt has none, and only the dock that MOVED emits
    // anything at all when a tab group changes).
    static bool isTabbedWithAnother(const QDockWidget *dock);
};

} // namespace loftail
