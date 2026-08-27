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
#include <QFontMetricsF>
#include <QHash>
#include <QString>
#include <QStringView>

#include <array>

namespace loftail {

// The wrapped-height model for AlwaysOn (ARCHITECTURE.md §7.1.1). It answers the one
// question EstimatedGeometry's cache is filled from: how many visual lines a message
// occupies when laid out at `width` pixels with QTextOption::WrapAnywhere.
//
// It is a GREEDY FILL over per-codepoint metrics, not `ceil(chars / cols)`. One column
// count for the whole view assumes every glyph is drawn at the primary face's advance,
// and two kinds of character are not: one the fixed-pitch face does not carry resolves
// through a FALLBACK face — Han, Kana, Hangul and emoji advance 1.53x to 1.77x the Latin
// advance at the reference face — and one whose INK OVERHANGS its advance ('W' at most
// point sizes here) breaks a line one character earlier than any width-over-advance
// arithmetic predicts, because QTextLine's break test allows for the last glyph's right
// bearing. Both cost a record rows it needed, and the paint then clips it to the rows it
// was given with no ellipsis and no tooltip (bugs.md 21, 22).
//
// So the walk reproduces QTextLine's own test — place a character while
// `x + advance + max(0, -rightBearing) <= width`, break before it otherwise — over a
// memoized (advance, ink overhang) pair per CODEPOINT: an eagerly filled 128-entry array
// for ASCII, then a direct-mapped front cache, then a QHash. Measured per 4096-record
// block, the unit measureBlock() works in: 0.34 ms on ASCII and 0.70 ms on Han, against
// the ~1.4 ms that same block already spends decoding its messages. Laying the text out
// instead is 24 ms, and up to 800 ms on 4000-character payload records (§7.1.1) — a freeze
// on the paint path, which is why that answer was rejected.
//
// The memo is per CODEPOINT and never per record: invariant #1 stands, nothing here holds
// a record's text. It is font-dependent, which is why it lives in the view and not in
// core beside the geometry it feeds — and why setFont() drops it (§7.1.5).
class WrapMetrics
{
public:
    WrapMetrics();

    // Rebind to `font` and DROP the memo. Every entry is that font's, so a zoom that
    // did not move the message column's origin — a session whose every width was
    // restored — would otherwise leave the model measuring at the old face's advances.
    void setFont(const QFont &font);
    const QFont &font() const { return m_font; }

    // Visual lines one paragraph occupies at `width` px. Never fewer than one: an empty
    // line still occupies a row. `width` <= 0 is treated as "no wrap".
    //
    // U+0009 is measured as one blank, because layoutWrappedText() substitutes one for it
    // before laying the paragraph out — a QTextLayout would otherwise advance a tab to an
    // 80 px tab stop where the paint drew one character (§7.1.1).
    int linesForParagraph(QStringView paragraph, int width) const;

    // The same for a whole record's message text at `width`, clamped to `cap` (the shared
    // 100-line display cap). The record's own newlines are paragraph breaks: a record is
    // not a line (invariant #2), and the split is of the DECODED string (invariant #8).
    int recordLines(const QString &text, int width, int cap) const;

private:
    // Advance and ink overhang past it, in pixels, of one codepoint in this font. The
    // overhang is `max(0, -rightBearing)`, which is what QTextLine adds to the width of a
    // line's LAST glyph before deciding it does not fit.
    struct Metric
    {
        float advance = 0;
        float overhang = 0;
    };

    Metric wideMetric(char32_t cp) const;
    Metric measure(char32_t cp) const;
    static bool isContextual(char32_t cp);

    QFont m_font;
    QFontMetricsF m_fm;
    // Filled whole by setFont(); U+0009 holds the blank it is laid out as.
    std::array<Metric, 128> m_ascii{};
    // A direct-mapped front cache over m_wide, keyed cp & (kSlots - 1) with the codepoint
    // stored so a collision is a miss and not a wrong answer. A QHash lookup is ~4 ns and
    // this walk makes one per character: on a log written wholly in Han that was the whole
    // cost of the measurement, and one masked index is not.
    static constexpr int kSlots = 4096;
    struct Slot
    {
        char32_t cp = 0; // 0 is never measured, so a zeroed table is an empty one
        Metric m;
    };
    // Both mutable: each is a memo of a pure function of (font, codepoint), and the
    // queries are const because the paint path is.
    mutable std::array<Slot, kSlots> m_slots{};
    mutable QHash<char32_t, Metric> m_wide;
    // The wide neighbour a Common/Inherited codepoint is measured after, and that
    // neighbour's own advance, so the difference is that codepoint's advance in a CJK run.
    QString m_wideBase;
    qreal m_wideBaseAdvance = 0;
};

} // namespace loftail
