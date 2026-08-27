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

#include "AppStyle.h"

namespace loftail {

// Base style left unset on purpose. QProxyStyle then resolves it from the DESKTOP style
// key, not from QApplication::style() — which matters precisely because this instance is
// about to become QApplication::style(), and taking the current one as a base would
// either recurse or hand ownership of a style QApplication also intends to delete.
AppStyle::AppStyle(QObject *parent)
{
    setParent(parent);
}

int AppStyle::styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                        QStyleHintReturn *returnData) const
{
    if (hint == SH_DialogButtonBox_ButtonsHaveIcons)
        return 0;
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

} // namespace loftail
