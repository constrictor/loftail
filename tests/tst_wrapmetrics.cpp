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

#include <QtTest>

#include <QApplication>

#include "Fonts.h"
#include "WrapMetrics.h"

using namespace loftail;

// The wrapped-height memo's COST contract (ARCHITECTURE.md §7.1.1), counted rather
// than timed.
//
// CLAUDE.md states this one in milliseconds — 0.34 ms per 4096-record block on ASCII
// and 0.70 ms on Han, against the ~1.4 ms that block already spends decoding — and a
// millisecond is not something to assert on a shared runner (tst_asyncconnect's budget
// is a deliberately loose second for exactly that reason). What the figures actually
// rest on is a count: the memo is per CODEPOINT, so a walk over a record measures a
// character it has already seen NEVER, however many records and however many times it
// occurs. That is exact, it is reproducible, and it fails by the length of the text
// when the memo goes.
//
// Every glyph measurement here is one to three QFontMetricsF queries — the dear thing
// this class exists not to repeat — so the counter reads the cost directly rather than
// standing in for it. Nothing about the ANSWERS is asserted here: what the walk places
// on a line is held against a real QTextLayout in tst_logview, and this case would pass
// just as well if every advance were wrong.
class TestWrapMetrics : public QObject
{
    Q_OBJECT

private:
    // `count` distinct Han codepoints, each repeated `each` times, interleaved so no
    // run of one character can be mistaken for the memo doing its job — the walk sees
    // every one of them again and again from the first line onward.
    static QString wideText(int count, int each)
    {
        QString s;
        s.reserve(count * each);
        for (int rep = 0; rep < each; ++rep)
            for (int i = 0; i < count; ++i)
                s.append(QChar(char16_t(0x4E00 + i)));
        return s;
    }

    static QString asciiText(int chars)
    {
        QString s;
        s.reserve(chars);
        for (int i = 0; i < chars; ++i)
            s.append(QLatin1Char(char('a' + (i % 26))));
        return s;
    }

private slots:
    // ASCII is filled EAGERLY, so log text — which is overwhelmingly ASCII — measures
    // no glyph at all however much of it is walked; everything else is measured once
    // per codepoint and never again. The two halves are one claim: a record's cost is
    // the characters it introduces, not the characters it has.
    void theMemoMeasuresEachCodepointOnceHoweverOftenItOccurs()
    {
        WrapMetrics wm;
        wm.setFont(monospaceFont());

        // The eager fill is the whole of what a font change costs: one measurement per
        // ASCII codepoint (U+0000..U+007F), and no more.
        QCOMPARE(wm.costs().glyphs, 128);

        wm.resetCosts();
        const QString ascii = asciiText(20000);
        for (int record = 0; record < 200; ++record)
            wm.recordLines(ascii, 300, 100);
        QCOMPARE(wm.costs().records, 200);
        QCOMPARE(wm.costs().glyphs, 0); // four million characters, no glyph measured

        // Beyond ASCII the memo is per codepoint: 40 distinct characters, 400 records,
        // 8000 characters each — forty measurements, once, for all of it.
        constexpr int kDistinct = 40;
        const QString han = wideText(kDistinct, 200);
        wm.resetCosts();
        for (int record = 0; record < 400; ++record)
            wm.recordLines(han, 300, 100);
        QCOMPARE(wm.costs().records, 400);
        QCOMPARE(wm.costs().glyphs, kDistinct);

        // And nothing is re-measured on a later visit, which is the half that makes a
        // second pass over a block free.
        wm.resetCosts();
        for (int record = 0; record < 400; ++record)
            wm.recordLines(han, 300, 100);
        QCOMPARE(wm.costs().glyphs, 0);
    }

    // A memo of a pure function of (font, codepoint) may not outlive the font: a zoom
    // whose column origins did not move leaves setWrapWidth() a no-op, so this drop is
    // the only thing standing between the model and heights measured at the old face
    // (§7.1.5). Counted from the other side: after the new font, the same text costs
    // its measurements again.
    void aFontChangeDropsTheMemoAndPaysForTheAsciiTableAgain()
    {
        WrapMetrics wm;
        QFont font = monospaceFont();
        wm.setFont(font);

        constexpr int kDistinct = 40;
        const QString han = wideText(kDistinct, 50);
        wm.recordLines(han, 300, 100);
        wm.resetCosts();
        wm.recordLines(han, 300, 100);
        QCOMPARE(wm.costs().glyphs, 0); // still memoized at this font

        font.setPointSize(font.pointSize() + 3);
        wm.resetCosts();
        wm.setFont(font);
        QCOMPARE(wm.costs().glyphs, 128); // the ASCII table, refilled and not kept

        wm.resetCosts();
        wm.recordLines(han, 300, 100);
        QCOMPARE(wm.costs().glyphs, kDistinct);
        wm.resetCosts();
        wm.recordLines(asciiText(5000), 300, 100);
        QCOMPARE(wm.costs().glyphs, 0);
    }
};

QTEST_MAIN(TestWrapMetrics)
#include "tst_wrapmetrics.moc"
