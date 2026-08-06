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
// it is deliberately NOT part of HighlightPalette: those twelve slots are a user-facing
// palette that rules reference by index and presets round-trip through, while these
// four are internal and nothing persists them.
//
// Everything here is a pure function of a QPalette, so it needs no QApplication and
// tracks whatever theme the widget is actually in.

// Whether `palette` reads as a dark theme — the base darker than the text on it. The
// same cue LogModel and HighlighterPane already use for the highlight palette.
bool isDarkPalette(const QPalette &palette);

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
