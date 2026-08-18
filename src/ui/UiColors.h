#pragma once

#include <QColor>

#include <QPalette>

QT_BEGIN_NAMESPACE
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

// A hairline dividing one section of a pane from the next — SectionBox paints it along
// its title row. Most of the way from the text colour toward the surface it sits on, so
// it reads as the rule after a heading and not as a border round something.
//
// Derived rather than read out of QPalette::Mid, which is the role that ought to say this
// and on a stock Fusion palette says almost nothing: a Mid line against Window is why a
// QFrame::Sunken HLine is invisible there, measured on a render rather than guessed.
//
// `group` is which set of colours to derive from, and it is a parameter because the
// answer differs while the section is switched OFF: a divider drawn from Active over a
// body Qt has greyed from Disabled is the one part of the section that ignores its own
// state.
QColor dividerColor(const QPalette &palette, QPalette::ColorGroup group = QPalette::Current);

// --- The log table's alternating record band (SPEC.md §5) -------------------------

// The fill for every other RECORD in the log view — the zebra that says where one
// record ends and the next begins, which in "line wrap: always on" is the only thing
// that does, since a record there occupies three or four lines and nothing else marks
// the seam.
//
// Derived from the palette by a fixed step from Base toward Text, for the reason the
// rest of this file exists: the band was `base.lighter(108)`, and lighter()/darker()
// SCALE the HSV value rather than moving a fixed distance. On a near-black base that
// scales almost nothing (#141618 -> #16181a, 1.02:1) and on a pure white one the value
// is already at its maximum and cannot move at all — a stripe that was dead at both
// ends of the range and visible only on the mid-tone themes nobody has.
//
// QPalette::AlternateBase is the role that ought to say this and is deliberately not
// read: nothing obliges a theme to make it differ from Base, and a theme that leaves
// them equal takes the band away again — silently, and on exactly the wrap mode that
// needs it most. Blending toward Text also gets the direction right on both themes for
// free, which a lightening factor never can.
QColor alternateRowColor(const QPalette &palette);

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
