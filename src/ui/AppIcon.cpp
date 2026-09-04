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

#include "AppIcon.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>

namespace loftail {

namespace {

// The SVG's viewBox. Every number below is one of its coordinates, unaltered, so the
// transcription can be read against packaging/linux/loftail.svg line for line.
constexpr qreal kDesign = 256.0;

// The five colours of that file, in the order it paints them.
const QColor kTile      = QColor(0x1e, 0x27, 0x33); // the rounded plate
const QColor kPanel     = QColor(0x26, 0x34, 0x45); // the inset panel it sits on
const QColor kLine      = QColor(0x8a, 0xa0, 0xb8); // three log lines
const QColor kNewest    = QColor(0x4f, 0xc0, 0x8d); // the newest record
const QColor kFollowing = QColor(0xf0, 0xb4, 0x29); // the tail arrow

} // namespace

void paintAppMark(QPainter *painter, const QRectF &bounds)
{
    const qreal side = qMin(bounds.width(), bounds.height());
    if (side <= 0)
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    // Square and centred in whatever it was given, then scaled out of the design space,
    // so the caller states a size and nothing here states a size at all.
    painter->translate(bounds.left() + (bounds.width() - side) / 2.0,
                       bounds.top() + (bounds.height() - side) / 2.0);
    painter->scale(side / kDesign, side / kDesign);
    painter->setPen(Qt::NoPen);

    painter->setBrush(kTile);
    painter->drawRoundedRect(QRectF(0, 0, 256, 256), 48, 48);
    painter->setBrush(kPanel);
    painter->drawRoundedRect(QRectF(40, 40, 176, 176), 20, 20);

    painter->setBrush(kLine);
    painter->drawRoundedRect(QRectF(64, 72, 96, 12), 6, 6);
    painter->drawRoundedRect(QRectF(64, 100, 128, 12), 6, 6);
    painter->drawRoundedRect(QRectF(64, 128, 80, 12), 6, 6);

    painter->setBrush(kNewest);
    painter->drawRoundedRect(QRectF(64, 156, 128, 16), 8, 8);

    // The arrow, transcribed from the SVG's two paths — a bar and the head it runs into.
    // Kept as two subpaths of one filled shape for the reason the file keeps them apart:
    // they overlap, and a single outline would have to state the join.
    painter->setBrush(kFollowing);
    QPainterPath arrow;
    arrow.moveTo(64, 190);
    arrow.lineTo(160, 190);
    arrow.lineTo(146, 178);
    arrow.lineTo(146, 186);
    arrow.lineTo(64, 186);
    arrow.closeSubpath();
    arrow.moveTo(150, 178);
    arrow.lineTo(174, 190);
    arrow.lineTo(150, 202);
    arrow.lineTo(150, 194);
    arrow.lineTo(136, 194);
    arrow.lineTo(136, 186);
    arrow.lineTo(150, 186);
    arrow.closeSubpath();
    // WindingFill, not the default OddEvenFill: the two subpaths overlap, and under
    // odd-even the overlap would be punched back out to the panel colour — a notch
    // through the middle of the arrow, at every size.
    arrow.setFillRule(Qt::WindingFill);
    painter->drawPath(arrow);

    painter->restore();
}

} // namespace loftail
