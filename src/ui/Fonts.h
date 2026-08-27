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

#include <QFont>

namespace loftail {

// The fixed-pitch font used by every table that shows log text: the record view
// (all columns, header included) and the format editor's preview.
//
// Fixed pitch is not cosmetic in LogView — every column is laid out against one
// advance, and the estimated-geometry path measures a wrapped record's height by
// walking per-codepoint advances rather than shaping the text (ARCHITECTURE.md
// §7.1.1), which is only cheap while the great majority of them are the same and
// the memo behind them is small. Asking for a family literally named "monospace" resolves through
// fontconfig on Linux but matches nothing on Windows or macOS, so take the font
// the platform designates as fixed-width instead, at the UI's own text size.
QFont monospaceFont();

// --- Zoom (SPEC.md §5, ARCHITECTURE.md §7.1.5) -------------------------------
//
// How big log text is drawn. ONE size for the whole application, not per view and not
// per log: it answers a question about the reader's eyes and their screen, which does
// not change between two tabs — and a log's own settings node (M20) holds how a log is
// READ, which a font size is no part of. So it is an application preference, remembered
// in QSettings by MainWindow and applied to every open view at once, the digest strip
// included (a strip in a different size from the table it annotates would read as a
// different kind of row).
//
// Only the SIZE moves. The family stays whatever the platform designates as fixed-width,
// because everything above depends on it (see monospaceFont).
//
// The bounds are absolute rather than relative to the platform's own size: below 6 pt
// nothing is legible and above 32 pt a viewport holds a handful of records, and both
// ends are reachable by holding a key down.
constexpr int kMinLogFontPointSize = 6;
constexpr int kMaxLogFontPointSize = 32;

// The size log text is shown at now, in points.
int logFontPointSize();
// The size it is shown at when nobody has zoomed — the platform's own text size,
// converted to points where the desktop states it in pixels.
int defaultLogFontPointSize();
// Set it, clamped into [kMin, kMax]. True when it actually moved, which is what the
// caller re-fonts its views and writes the setting on; false is a no-op, including at
// either bound, so holding the key down at 32 pt costs nothing.
bool setLogFontPointSize(int points);
// Back to the platform's own size — and to its own UNIT: a desktop that sizes its text
// in pixels gets exactly the font it got before any of this existed, rather than that
// size rounded to the nearest point. True when it moved.
bool resetLogFontPointSize();

// monospaceFont() at the current size. What every LogView is constructed with, so a
// view opened after a zoom opens zoomed with no one having to push the font into it.
QFont logTextFont();

} // namespace loftail
