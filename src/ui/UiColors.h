#pragma once

#include <QColor>

QT_BEGIN_NAMESPACE
class QPalette;
class QWidget;
QT_END_NAMESPACE

namespace loftail {

// Chrome colours that are not part of a log's content: an invalid pattern's red, a
// caution's amber, secondary explanatory text, and the grey of placeholder text.
//
// They exist because every one of these was originally a hardcoded hex chosen against
// a light theme — `#c0392b`, `#b9770e`, `#b04a00`, `color: gray` — which on a dark
// palette range from dim to nearly invisible. The log's OWN colours never had this
// problem: highlight rules go through HighlightPalette, which has carried a light and
// a dark variant per slot since M5. This is the same idea applied to the chrome, and
// it is deliberately NOT part of HighlightPalette: those slots are a user-facing
// palette that rules reference by index and presets round-trip through, while these
// four are internal and nothing persists them.
//
// Everything here is a pure function of a QPalette, so it needs no QApplication and
// tracks whatever theme the widget is actually in.

// Whether `palette` reads as a dark theme — the base darker than the text on it. The
// same cue LogModel and HighlighterPane already use for the highlight palette.
bool isDarkPalette(const QPalette &palette);

// --- Measuring legibility ---------------------------------------------------------
//
// Public because every colour above is only defensible against a number, and the number
// has to be available where the colour is USED as well as where it is chosen: a test
// asserting a chrome colour carries, a widget deciding whether a theme's own colour is
// good enough. They were private to this file until the light warning amber was found
// sitting at 3.2:1 — below the body-text bound — with nothing anywhere able to say so.

// WCAG relative luminance. Not QColor::lightness(): that is an HSL coordinate and calls
// a saturated blue and a saturated yellow equally light, which is exactly wrong for
// legibility.
qreal relativeLuminance(const QColor &color);

// The WCAG contrast ratio between two colours, 1.0 (identical) to 21.0 (black on white).
// The bounds worth knowing: 4.5 for body text, 3.0 for large text and for non-text
// indicators. Both colours must be opaque — composite first with compositeOver().
qreal contrastRatio(const QColor &a, const QColor &b);

// `over` composited onto `under`, honouring alpha. Measuring a partly transparent colour
// against its background without this measures a colour that is never actually drawn —
// and Qt's default PlaceholderText is exactly that, black at 50% alpha.
QColor compositeOver(const QColor &over, const QColor &under);

// An invalid regex, an uncompilable pattern: something the user must fix.
QColor errorColor(const QPalette &palette);

// A caution about something that will still work — a remembered password stored as
// plain text, a pattern that parses nothing.
QColor warningColor(const QPalette &palette);

// Secondary text: an aside, a detected value, an explanation. Derived from the palette
// rather than picked, so it lands correctly on any theme instead of on two.
QColor mutedColor(const QPalette &palette);

// The colour placeholder text SHOULD have: partway from the field's text colour toward
// its background, so it reads as a hint rather than as content.
QColor placeholderColor(const QPalette &palette);

// --- Filter context rows (M15, SPEC.md §6) ---------------------------------------
//
// A record shown only because a neighbour matched the filter reads as recessed, so a
// real match is still findable by eye in a view that now holds both. Both take the
// colours already resolved for the row rather than a palette, because the row may be
// carrying a highlight rule's colours and those must be softened too — not overridden.

// A context row's text: partway from `fg` toward the row's own `bg`.
QColor contextTextColor(const QColor &fg, const QColor &bg);

// A context row's fill WHEN a highlight rule supplied one: partway from the rule's
// colour toward the view's base. Without this a rule-coloured context row paints at
// full saturation and is indistinguishable from a match, losing the cue exactly on the
// loudest rows — while suppressing the rule colour entirely would throw away the fact
// that the lead-up record was also from `db.pool`.
QColor contextFillColor(const QColor &ruleBg, const QColor &base);

// Give `widget` a readable placeholder colour IF the theme's own is unreadable.
//
// This is the bug the whole file was written for. QPalette::PlaceholderText arrived in
// Qt 5.12, and a platform theme that predates it — or simply does not fill it in, which
// is common — leaves the role at Qt's built-in default of BLACK at 50% alpha. On a dark
// theme that is black text on a dark field: the placeholder is there, occupies space,
// and cannot be read.
//
// Deliberately conditional and measured rather than unconditional: a theme that sets
// the role sensibly is left completely alone, and the test is the actual contrast of
// the resulting colour against the field, so it also catches a theme that sets the role
// badly rather than not at all.
void ensureReadablePlaceholder(QWidget *widget);

} // namespace loftail
