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

namespace loftail {

// The application-wide style. It changes exactly one thing about whatever style the
// desktop already chose: dialog buttons carry no icons.
//
// Qt puts a themed icon on every standard dialog button — a folder on Open, a red X on
// Cancel, a floppy disk on Save — whenever the style answers yes to
// SH_DialogButtonBox_ButtonsHaveIcons, which the GTK and Fusion styles do. The result
// is a row of small, loud, mutually unrelated pictures next to words that already say
// what the buttons do. Every platform's own guidelines have said no to this for years,
// and the buttons read as period decoration rather than as information.
//
// Done as a style hint rather than by clearing icons on each button, because the hint
// is where the icons come from. QDialogButtonBox consults it when it BUILDS a standard
// button, so this also covers the ones loftail never gets a pointer to: the OK on a
// QMessageBox::warning(), the Yes/No on a QMessageBox::question(), and any button added
// by a dialog written later.
//
// Deliberately NOT merged into PaneTitleStyle, which is the opposite scope: that one is
// installed on the pane docks only and must stay off everything else (PaneTitleStyle.h).
// This one has no business anywhere but the application.
class AppStyle : public QProxyStyle
{
public:
    explicit AppStyle(QObject *parent = nullptr);

    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                  QStyleHintReturn *returnData) const override;
};

} // namespace loftail
