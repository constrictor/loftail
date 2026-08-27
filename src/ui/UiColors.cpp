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

#include "UiColors.h"

#include "ConfigSyntax.h"

#include <QPalette>
#include <QWidget>

#include <cmath>

namespace loftail {

namespace {

// How far a placeholder sits from the field's text colour toward its background. Enough
// to read as a hint rather than as typed content, not so far that it disappears.
constexpr qreal kPlaceholderMix = 0.45;

// How far a filter-context row recedes. Less than a placeholder: this is real log text
// that the user asked to see, not a hint — it must stay comfortably readable while
// still losing against the matches it surrounds. The fill moves further than the text
// because a highlight rule's background is a large, saturated area and half of it is
// still plainly the same hue.
constexpr qreal kContextTextMix = 0.35;
constexpr qreal kContextFillMix = 0.55;

// How far the log table's alternating band sits from the view's base toward its text.
// Small enough to read as a band rather than as a second surface — a record is not a
// row in a spreadsheet, it is a paragraph three or four lines tall in wrap-always-on,
// and a heavy stripe over that much area competes with the highlight colours that mean
// something. It lands near 1.13:1 on both a white base and a near-black one, where the
// lighter(108) it replaces measured 1.00:1 and 1.02:1 respectively.
constexpr qreal kAlternateRowMix = 0.06;

// How far a section divider sits from the text colour toward the surface behind it.
// Further than anything else here, because this is not text: it is a line whose whole
// job is to be noticed without being read, and the full text colour draws a rule across
// the pane heavier than the frames it replaced.
constexpr qreal kDividerMix = 0.72;

// The contrast a placeholder must already have against its field before this leaves the
// theme alone. Expressed as a WCAG contrast ratio; 4.5 is the bound for body text and
// 3.0 for large text, so a hint is allowed to sit below both — but Qt's unset default
// (black on a dark field) lands near 1.1, which is what this is here to catch.
constexpr qreal kMinPlaceholderContrast = 2.0;

// The two chrome hues, one variant per theme, in the same shape HighlightPalette uses.
// The light values are the ones that were previously hardcoded at each call site; the
// dark ones are lifted toward the light end of the same hue so they carry on a dark
// field instead of sinking into it.
//
// Every one of these is held to WCAG 4.5:1 against its theme's Window by
// tst_uicolors::chromeColoursCarryOnBothThemes. That bound is deliberately the BODY-TEXT
// one rather than the 3.0 allowed for large text: these colours are only ever used on
// ordinary-sized sentences, and the sentence they were introduced for is the one saying
// a password is about to be written to disk in the clear.
//
// kWarningLight was #b9770e until that assertion was tightened — a pleasant amber that
// measured 3.2:1 on a light dialog and failed. Darkening it is the whole fix; the hue is
// unchanged, and the dark variant already measured 7.6:1 and was left alone.
constexpr QRgb kErrorLight = 0xffc0392b;
constexpr QRgb kErrorDark = 0xffff7b6e;
constexpr QRgb kWarningLight = 0xff8f5c00;
constexpr QRgb kWarningDark = 0xffffb454;

// Config-file syntax colours (ConfigSyntax.h). A fixed pair per role, NOT a lighter()/
// darker() of the theme's own text: those scale the HSV value and are therefore no-ops
// at both ends of the range, which is how the log table's zebra band measured 1.00:1 on
// a white theme for eight milestones.
//
// Only three roles are coloured. Key, Tag and Attribute spend WEIGHT and italics
// instead, so they stay at the theme's own text colour and need no contrast argument at
// all — which is what keeps this table small enough to hold above 4.5:1 against Base in
// two themes. Every pair below is measured by
// tst_uicolors::everySyntaxColourReadsOnBothThemes.
// A comment gets a colour OF ITS OWN rather than the muted placeholder role, and that
// is a deliberate reversal: placeholderColor() measures 3.89:1 on white, which is fine
// for a hint in an empty field and wrong here. In an EDITOR a comment is text the reader
// is reading and editing — in a log4cplus properties file it is usually the explanation
// of what the setting below it does — so it is de-emphasised by hue, never by dropping
// below the legibility line. Caught by the contrast test on its first run.
constexpr QRgb kSyntaxCommentLight = 0xff4a7a4a; // desaturated green
constexpr QRgb kSyntaxCommentDark  = 0xff8fbf8f;
constexpr QRgb kSyntaxStringLight = 0xff0f7a3d; // green, dark enough on white
constexpr QRgb kSyntaxStringDark  = 0xff6bd18a;
constexpr QRgb kSyntaxNumberLight = 0xff8a4b00; // amber/brown
constexpr QRgb kSyntaxNumberDark  = 0xffe0a253;
constexpr QRgb kSyntaxValueLight  = 0xff0b5f96; // blue
constexpr QRgb kSyntaxValueDark   = 0xff6fb8e8;

QColor mix(const QColor &from, const QColor &to, qreal amount)
{
    // Blended in float, which is what QColor's channels and fromRgbF() are: computing
    // at double width only to narrow on the way in buys nothing and hides the cast.
    const auto t = float(amount);
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * t,
                            from.greenF() + (to.greenF() - from.greenF()) * t,
                            from.blueF() + (to.blueF() - from.blueF()) * t);
}

} // namespace

bool isDarkPalette(const QPalette &palette)
{
    return palette.color(QPalette::Base).lightness() < palette.color(QPalette::Text).lightness();
}

qreal relativeLuminance(const QColor &color)
{
    const auto channel = [](qreal value) {
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF())
        + 0.0722 * channel(color.blueF());
}

qreal contrastRatio(const QColor &a, const QColor &b)
{
    const qreal la = relativeLuminance(a);
    const qreal lb = relativeLuminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

QColor compositeOver(const QColor &over, const QColor &under)
{
    // float, not qreal: alphaF() and fromRgbF() are both float, so a double here is
    // only ever narrowed back again (mix() above, same reasoning).
    const float alpha = over.alphaF();
    if (alpha >= 1.0F)
        return over;
    return QColor::fromRgbF(over.redF() * alpha + under.redF() * (1 - alpha),
                            over.greenF() * alpha + under.greenF() * (1 - alpha),
                            over.blueF() * alpha + under.blueF() * (1 - alpha));
}

QColor errorColor(const QPalette &palette)
{
    return QColor::fromRgba(isDarkPalette(palette) ? kErrorDark : kErrorLight);
}

QColor warningColor(const QPalette &palette)
{
    return QColor::fromRgba(isDarkPalette(palette) ? kWarningDark : kWarningLight);
}

QColor mutedColor(const QPalette &palette)
{
    // Against the WINDOW, not the base: this is for labels sitting on the dialog
    // background rather than inside a field.
    return mix(palette.color(QPalette::WindowText), palette.color(QPalette::Window),
               kPlaceholderMix);
}

QColor syntaxColor(const QPalette &palette, ConfigRole role)
{
    const bool dark = isDarkPalette(palette);
    switch (role) {
    case ConfigRole::Comment:
        return QColor::fromRgba(dark ? kSyntaxCommentDark : kSyntaxCommentLight);
    case ConfigRole::String:
        return QColor::fromRgba(dark ? kSyntaxStringDark : kSyntaxStringLight);
    case ConfigRole::Number:
        return QColor::fromRgba(dark ? kSyntaxNumberDark : kSyntaxNumberLight);
    case ConfigRole::Value:
        return QColor::fromRgba(dark ? kSyntaxValueDark : kSyntaxValueLight);
    case ConfigRole::Key:
    case ConfigRole::Tag:
    case ConfigRole::Attribute:
        break;
    }
    // Weight and italics carry these, so the colour is the theme's own — which is both
    // the most readable answer available and the one that cannot fall below 4.5:1.
    return palette.color(QPalette::Text);
}

QColor placeholderColor(const QPalette &palette)
{
    return mix(palette.color(QPalette::Text), palette.color(QPalette::Base), kPlaceholderMix);
}

QColor dividerColor(const QPalette &palette, QPalette::ColorGroup group)
{
    return mix(palette.color(group, QPalette::WindowText),
               palette.color(group, QPalette::Window), kDividerMix);
}

QColor alternateRowColor(const QPalette &palette)
{
    return mix(palette.color(QPalette::Base), palette.color(QPalette::Text), kAlternateRowMix);
}

QColor contextTextColor(const QColor &fg, const QColor &bg)
{
    return mix(fg, bg, kContextTextMix);
}

QColor contextFillColor(const QColor &ruleBg, const QColor &base)
{
    return mix(ruleBg, base, kContextFillMix);
}

void ensureReadablePlaceholder(QWidget *widget)
{
    if (!widget)
        return;

    QPalette palette = widget->palette();
    const QColor base = palette.color(QPalette::Active, QPalette::Base);
    const QColor current =
        compositeOver(palette.color(QPalette::Active, QPalette::PlaceholderText), base);
    if (contrastRatio(current, base) >= kMinPlaceholderContrast)
        return; // the theme filled the role in sensibly — leave it entirely alone

    const QColor readable = placeholderColor(palette);
    // Both groups: a field in an inactive window still shows its placeholder.
    palette.setColor(QPalette::Active, QPalette::PlaceholderText, readable);
    palette.setColor(QPalette::Inactive, QPalette::PlaceholderText, readable);
    widget->setPalette(palette);
}

} // namespace loftail
