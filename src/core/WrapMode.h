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

#include <QtGlobal>

namespace loftail {

// How a view wraps long records (SPEC.md §5). It lives in core, although the only
// thing that acts on it is LogView, because it is one of the settings a log's node in
// the settings tree carries (LogProfile.h) and nothing in core may see a widget.
//
// LogView aliases this as LogView::WrapMode, so `LogView::WrapMode::AlwaysOn` keeps
// meaning what it always did. The session stores it as an int and always has.
//
// It is a SEED, not a per-file property (invariant #7): the node supplies the mode a
// newly created view of that log starts in, and the view owns it thereafter.
enum class WrapMode : quint8 {
    Off,                // long lines extend horizontally
    SelectedRecordOnly, // only the focused record wraps
    AlwaysOn,           // every record wraps; estimated geometry (§7.1.1)
};

} // namespace loftail
