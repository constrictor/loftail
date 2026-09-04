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

class QPainter;
class QRectF;

namespace loftail {

// The loftail mark, DRAWN AND NEVER LOADED.
//
// packaging/linux/loftail.svg is installed into the freedesktop icon theme and is not a
// Qt resource, so QIcon::fromTheme() answers with it on an installed Linux desktop and
// with nothing at all on Windows, on macOS and in any uninstalled build — including
// every test run. A mark that is a picture on one platform and a blank on the others is
// worse than none, which is why the welcome screen shipped without one; this is the same
// answer SshPromptDialogs gives for its reveal eye and PaneTitleStyle for its dock-title
// glyphs, one level up.
//
// The shapes below are a TRANSCRIPTION of that SVG in its own 256-unit design space, and
// the two are to be kept in step by hand: this file is the mark the application draws,
// the SVG is the mark the desktop installs, and a change to either that is not made to
// the other is how the window and its launcher come to disagree.
//
// The colours are fixed and deliberately not palette-derived. This is a logo rather than
// chrome — it stands for the application on a light theme and a dark one alike, exactly
// as a launcher icon does, and a mark that restyled itself per theme would stop being
// recognisable as the same mark.
//
// Drawn at whatever size it is asked for, so there is no pixmap to pick a size for and
// nothing to rescale on a HiDPI screen: give it the square you want and it fills it.
void paintAppMark(QPainter *painter, const QRectF &bounds);

} // namespace loftail
