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

#include <QGroupBox>

namespace loftail {

// A checkable group box that can draw a hairline along its own title row, from the end
// of the title out to its right edge — the look a bold word followed by a rule has,
// without being one.
//
// It exists because the title row of a QGroupBox is drawn by the style and is not a
// layout: there is no cell beside the title to put a QFrame in, and a QFrame added to
// the box's layout lands UNDER the title rather than beside it. So the line is painted.
//
// Everything else about the box is untouched, which is the whole point of subclassing
// rather than rebuilding the header out of a checkbox and a frame. The title row stays
// the enable control, `isChecked()`/`setChecked()`/`toggled()` stay where every caller
// already expects them (`AxisEditor`'s five axes are `QGroupBox *`), and Qt keeps greying
// the body while the box is off — a hand-written `setEnabled()` standing in for that is
// the exact special case the priority axis was rewritten to remove.
//
// Painting rather than snapshotting also means the colour is resolved per paint, so the
// line follows a theme switched mid-session and dims with the box when the axis is off,
// neither of which a palette written once at construction does.
class SectionBox : public QGroupBox
{
    Q_OBJECT

public:
    explicit SectionBox(const QString &title, QWidget *parent = nullptr);

    // Draw the hairline (or stop). Off by default: a FRAMED section has a frame to
    // separate it from the next and wants no line as well.
    void setTitleDivider(bool on);
    bool hasTitleDivider() const { return m_titleDivider; }

    // Draw the title as a HEADING — centred and bold — rather than as a control pinned to
    // the left edge.
    //
    // The distinction is what the title row IS. An axis's title row is its enable
    // control: it carries a checkbox, it is clicked, and a control belongs where the
    // controls it governs begin. "Condition" and "Action" are captions over a block that
    // holds those controls; centred and bold they read as the label of a section, and hard
    // left they read as a control that has lost its checkbox.
    //
    // Bold comes from the same style sheet rule as the position, deliberately, and NOT
    // from setFont(): a QWidget's font is inherited by its children, so bolding the box
    // bolds every label, list and combo inside it.
    void setHeading(bool on);
    bool isHeading() const { return m_heading; }

    // A section with no body is just its title row, and Qt does not size that case
    // correctly: a QGroupBox with no layout has an INVALID sizeHint() — QSize(-1, -1) — so
    // a layout falls back to minimumSizeHint(), and with a style sheet in play that came
    // back at 20 px under Breeze for a title occupying y=4..24. The row clipped its own
    // descenders, on that style only; Fusion answered 37 px and showed nothing wrong.
    //
    // Both hints therefore expand to fit the title row, and both stay PURE QUERIES — no
    // setter may run inside a size hint (LogView::sizeHint records what that costs).
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // What the title row alone needs: indicator, gap, text, asked of the style rather than
    // measured off the font, for the reason paintEvent() gives. Public because it is also
    // the question a test has to ask to see that a row is not clipping its own words —
    // which is a thing only one of the two styles here ever did.
    QSize titleRowHint() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void applyTitleStyle();

    bool m_titleDivider = false;
    bool m_heading = false;
};

} // namespace loftail
