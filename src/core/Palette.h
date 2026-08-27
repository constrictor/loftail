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

#include <QColor>
#include <QLatin1StringView>

namespace loftail {

// The curated dual-theme highlight palette (SPEC.md §7, ARCHITECTURE.md §8).
// Highlight rules reference a slot by INDEX or the `kDefault` sentinel — never a raw
// RGB value — so switching between the light and dark theme remaps every existing
// rule automatically, and an exported preset is portable across themes because the
// importing user's palette supplies the concrete colors.
//
// The palette is a grid: eight hues in each of three TONE BANDS, each band closed by
// a neutral. The band is the loudness, and it is the user's choice:
//
//   Deep   — dark, saturated. A strong fill under light text, or text on a light row.
//   Vivid  — maximum chroma. The screaming one; this is what highlighting is for.
//   Soft   — pale. A quiet tint under dark text, or text on a dark row.
//
//   0..7   Deep Red … Deep Pink     8  Ink    (near-black)
//   9..16  Vivid Red … Vivid Pink  17  Gray   (mid neutral)
//   18..25 Soft Red … Soft Pink    26  Paper  (near-white)
//
// **The two theme variants are a nudge, not a flip.** A slot keeps its tone in both
// themes and only shifts enough to sit correctly against the surrounding base. The
// palette used to do the opposite — one hue per slot, deep on light themes and pastel
// on dark — which meant a theme only ever offered ONE tone, so a rule that set a
// background had nothing readable to put on it: on a dark theme the best of all 144
// slot-on-slot pairs measured 1.85:1, and every background against the theme's own
// text measured between 1.13:1 and 2.09:1. Making tone a user axis rather than a
// consequence of the theme is what fixes that.
//
// Every slot therefore names the neutral that reads on it — `textOn`, Ink or Paper —
// and that pairing clears 4.5:1 in *both* themes for every slot, which is what
// `readableTextSlot()` hands out and `tst_highlight` pins.
//
// This lives in core (no QApplication needed — QColor is a plain value type), so
// the highlight evaluation in LogModel::data() and its tests stay UI-free.
struct PaletteSlot
{
    QLatin1StringView name;
    QColor            light;
    QColor            dark;
    // The slot index of the neutral that reads on this one, in either theme.
    int               textOn;
};

class HighlightPalette
{
public:
    // Three tone bands of eight hues plus one neutral each.
    static constexpr int kSlotsPerBand = 9;
    static constexpr int kBandCount = 3;
    static constexpr int kSlotCount = kSlotsPerBand * kBandCount;

    // The two neutrals, which double as the only two foregrounds guaranteed to read
    // on an arbitrary slot. Their positions are the last entry of a band, so the
    // eight hues of each band are `band * kSlotsPerBand + 0..7`.
    static constexpr int kInk = 8;
    static constexpr int kPaper = 26;

    // The sentinel a rule stores for a role left at the theme's normal color
    // ("default"): the record's un-highlighted foreground/background (SPEC.md §7).
    static constexpr int kDefault = -1;

    static int count() { return kSlotCount; }

    // A slot's definition, for the swatch picker UI. `index` must be 0..kSlotCount-1.
    static const PaletteSlot &slot(int index);

    // The concrete color of `index` in the active theme, or an INVALID QColor for
    // `kDefault` (or an out-of-range index) so the caller falls back to the theme's
    // normal color. `dark` selects the dark-theme variant.
    static QColor color(int index, bool dark);

    // The slot to paint text in when `index` is the background: Ink or Paper,
    // whichever clears 4.5:1 against it in BOTH themes. Theme-stable on purpose — a
    // rule built from this pairing stays readable across a theme switch, which a
    // *default* foreground cannot promise once tone is the user's choice.
    static int readableTextSlot(int index);

    // True when `index` is one of the three neutrals (Ink, Gray, Paper) rather than
    // one of the twenty-four hues.
    static bool isNeutral(int index)
    {
        return isSlot(index) && index % kSlotsPerBand == kSlotsPerBand - 1;
    }

    // True when `index` names a real slot rather than the default sentinel.
    static bool isSlot(int index) { return index >= 0 && index < kSlotCount; }
};

} // namespace loftail
