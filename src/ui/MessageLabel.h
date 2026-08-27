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

#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>

namespace loftail {

// A wrapping message label whose HEIGHT a layout cannot get wrong.
//
// The problem it solves, measured in Preferences at a 560 px dialog width: the format
// editor's warning was given 25 px for text needing 34, so its second line was drawn over
// the row beneath it. A word-wrapped QLabel answers sizeHint() with a guess — a width and
// height it would like, from an aspect-ratio heuristic — and a layout that sizes the row
// from that hint rather than from heightForWidth() at the width it then hands over
// reserves a height the text does not fit in. Whether the chain asks for the right one
// depends on the layouts between the label and the widget: the same message spanning a
// QFormLayout row came out correct in one editor here and clipped in the other.
//
// So the label answers for itself: both hints report heightForWidth() at the width it has
// actually been given. It converges rather than oscillating because these labels span a
// full row — the width comes from the row, never from the hint — so a resize changes the
// height hint, the row grows, and the width does not move again.
//
// It is NOT a general-purpose replacement for QLabel. Use it where a message may wrap and
// its row must grow to fit; a caption of a known one line needs none of this.
class MessageLabel : public QLabel
{
public:
    explicit MessageLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setWordWrap(true);
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    QSize sizeHint() const override { return hintAtCurrentWidth(QLabel::sizeHint()); }

    QSize minimumSizeHint() const override
    {
        return hintAtCurrentWidth(QLabel::minimumSizeHint());
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        // Only on a WIDTH change: a height change is the layout acting on the hint this
        // would recompute, and asking it to think again there is how a relayout loop
        // starts. The width is what the wrapped height is a function of.
        if (event->oldSize().width() != event->size().width())
            updateGeometry();
    }

private:
    QSize hintAtCurrentWidth(QSize base) const
    {
        // Before the first layout there is no width to ask about, and QLabel's own guess
        // is as good as anything — it is only ever used to get the first pass started.
        if (width() <= 0)
            return base;
        return {base.width(), heightForWidth(width())};
    }
};

} // namespace loftail
