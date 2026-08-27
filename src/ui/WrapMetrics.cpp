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

#include "WrapMetrics.h"

#include <cmath>

namespace loftail {

WrapMetrics::WrapMetrics() : m_fm(QFont()) {}

// Whether a codepoint takes its font from the text around it rather than carrying one of
// its own — the Common and Inherited scripts, which is where the symbols, arrows, marks
// and emoji live.
bool WrapMetrics::isContextual(char32_t cp)
{
    const QChar::Script script = QChar::script(cp);
    return script == QChar::Script_Common || script == QChar::Script_Inherited;
}

void WrapMetrics::setFont(const QFont &font)
{
    m_font = font;
    m_fm = QFontMetricsF(font);
    m_wide.clear();
    m_slots.fill(Slot{});
    // One Hangul syllable, which every fallback chain draws through a wide CJK face: the
    // second context every Common/Inherited codepoint is measured in (see measure()).
    m_wideBase = QString(QChar(char16_t(0xD55C)));
    m_wideBaseAdvance = m_fm.horizontalAdvance(m_wideBase);
    // ASCII is filled EAGERLY, all 128 of it, rather than memoized on first sight. Log
    // text is overwhelmingly ASCII, so this is the whole of the hot loop: filling it up
    // front takes one branch and one flag array out of a walk that runs per character of
    // every record of every newly visited block, and costs 128 glyph measurements once
    // per zoom. U+0009 is stored as the blank layoutWrappedText() substitutes for it, so
    // the walk needs no test for it either.
    for (char32_t cp = 0; cp < m_ascii.size(); ++cp)
        m_ascii[cp] = measure(cp == U'\t' ? U' ' : cp);
}

// One codepoint's advance and ink overhang, measured through the SAME fallback chain the
// layout resolves it with — which is the whole point: QFontMetricsF::inFont() cannot be
// used to detect a fallback character at all, because it answers true for U+4E2D and
// U+1F600 alike (it resolves through the chain before answering). The advance is what
// separates them, so it is the advance that is asked for.
//
// A non-BMP codepoint is measured as its surrogate pair, so an emoji is one entry and not
// two — which is also why bugs.md 21 never bit an astral emoji: the arithmetic model
// counted a surrogate pair as TWO characters, and that more than paid for the fallback
// face's wider advance.
WrapMetrics::Metric WrapMetrics::measure(char32_t cp) const
{
    QString s;
    if (QChar::requiresSurrogates(cp)) {
        s.append(QChar(QChar::highSurrogate(cp)));
        s.append(QChar(QChar::lowSurrogate(cp)));
    } else {
        s.append(QChar(char16_t(cp)));
    }
    qreal advance = qMax(qreal(0), m_fm.horizontalAdvance(s));
    // A codepoint of the COMMON or INHERITED script has no font of its own: Qt itemizes it
    // into the run around it, so U+26A0 is 7.22 px beside Latin text and 12 px beside
    // Hangul or Kana, drawn by whichever face is carrying that run. Measured alone it is
    // always the narrow one, and a line of CJK sprinkled with ⚠ or → then measures short
    // and loses its last row — the very failure this class exists to end. So such a
    // codepoint is ALSO measured after a wide base and the WIDER answer kept. The
    // cost is an overcount on a Latin line carrying one of these symbols, which is a blank
    // strip at worst; the direction is chosen deliberately and is the direction every
    // other approximation here is rounded in. Ordinary Latin, digits and punctuation are
    // ASCII and never reach this at all.
    if (cp >= m_ascii.size() && isContextual(cp))
        advance = qMax(advance, m_fm.horizontalAdvance(m_wideBase + s) - m_wideBaseAdvance);
    Metric m;
    m.advance = float(advance);
    // A codepoint of NO advance is a combining mark: it joins the cluster in front of it,
    // it can neither begin a line nor end one, and it must contribute nothing at all. It
    // is also where the bounding-box sentinel comes from — Qt returns an unset
    // glyph_metrics_t, whose x is 100000, when the shaper placed no glyph for the string
    // — and an overhang of 100000 px would break the line at every mark of a Thai or
    // Devanagari record.
    if (advance <= 0)
        return m;
    // The overhang of a BMP codepoint is asked of rightBearing(), which is the VERY
    // number QTextLine's break test uses (both go through QFontEngine::getGlyphBearings),
    // and NOT derived from the bounding rectangle. They differ, and the difference clips:
    // the bearing is the glyph's HINTED metrics — at the reference face's 7 pt '>' bears
    // -1 px against a bounding rect that overhangs a 5.40625 px advance by 0.59 — so a
    // model built on the rect fits a twentieth character onto a 109 px line that Qt puts
    // on the next one. Only a non-BMP codepoint has no such overload, and there the rect
    // is measured against the advance ROUNDED DOWN, which is the same hinted quantity
    // approached from the other side: a pixel of conservatism costs a blank row at worst,
    // where a pixel the other way costs a line of text.
    const qreal overhang = QChar::requiresSurrogates(cp)
                               ? m_fm.boundingRect(s).right() - std::floor(advance)
                               : -qMin(qreal(0), m_fm.rightBearing(QChar(char16_t(cp))));
    m.overhang = float(qBound(qreal(0), overhang, advance));
    return m;
}

// Everything the eager ASCII table does not already hold. Deliberately out of line and
// deliberately NOT what the walk below calls per character: a QHash lookup is too big for
// the compiler to inline, and inlining is the difference between an array index and a call
// on a path that runs per character of every record of every newly visited block.
WrapMetrics::Metric WrapMetrics::wideMetric(char32_t cp) const
{
    Slot &slot = m_slots[cp & (kSlots - 1)];
    if (slot.cp == cp)
        return slot.m;
    const auto it = m_wide.constFind(cp);
    const Metric m = it != m_wide.cend() ? it.value() : *m_wide.insert(cp, measure(cp));
    slot = Slot{cp, m};
    return m;
}

int WrapMetrics::linesForParagraph(QStringView paragraph, int width) const
{
    if (width <= 0)
        return 1;
    const auto limit = qreal(width);
    int lines = 1;
    qreal x = 0;
    const QChar *const begin = paragraph.begin();
    const QChar *const end = paragraph.end();
    for (const QChar *p = begin; p != end; ++p) {
        const char16_t unit = p->unicode();
        Metric m;
        if (unit < m_ascii.size()) {
            m = m_ascii[unit]; // U+0009 holds the blank it is laid out as
        } else {
            char32_t cp = unit;
            if (QChar::isHighSurrogate(unit) && p + 1 != end && p[1].isLowSurrogate())
                cp = QChar::surrogateToUcs4(unit, (++p)->unicode());
            m = wideMetric(cp);
        }
        // QTextLine's own test, and the `x > 0` guard is Qt's too: a line always takes at
        // least one glyph, however wide, or a character wider than the column would wrap
        // for ever.
        if (x > 0 && x + qreal(m.advance) + qreal(m.overhang) > limit) {
            ++lines;
            x = 0;
        }
        x += qreal(m.advance);
    }
    return lines;
}

int WrapMetrics::recordLines(const QString &text, int width, int cap) const
{
    int lines = 0;
    qsizetype from = 0;
    while (true) {
        const qsizetype nl = text.indexOf(QLatin1Char('\n'), from);
        const qsizetype len = (nl < 0 ? text.size() : nl) - from;
        lines += linesForParagraph(QStringView(text).mid(from, len), width);
        if (lines >= cap)
            return cap;
        if (nl < 0)
            break;
        from = nl + 1;
    }
    return qMax(1, qMin(lines, cap));
}

} // namespace loftail
