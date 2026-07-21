#pragma once

#include <QColor>
#include <QLatin1StringView>

namespace loftail {

// The curated 12-entry dual-theme highlight palette (SPEC.md §7, ARCHITECTURE.md
// §8). Highlight rules reference a slot by INDEX (0..11) or the `kDefault` sentinel
// — never a raw RGB value — so switching between the light and dark theme remaps
// every existing rule automatically, and an exported preset is portable across
// themes because the importing user's palette supplies the concrete colors.
//
// Each slot defines one color for light themes and one for dark, chosen to stay
// legible whether the rule uses it as a full-row background or as text. The set
// covers the usual severity associations (reds, ambers, greens) plus neutral
// distinguishing hues, per SPEC.md §7.
//
// This lives in core (no QApplication needed — QColor is a plain value type), so
// the highlight evaluation in LogModel::data() and its tests stay UI-free.
struct PaletteSlot
{
    QLatin1StringView name;
    QColor            light;
    QColor            dark;
};

class HighlightPalette
{
public:
    static constexpr int kSlotCount = 12;

    // The sentinel a rule stores for a role left at the theme's normal color
    // ("default"): the record's un-highlighted foreground/background (SPEC.md §7).
    static constexpr int kDefault = -1;

    static int count() { return kSlotCount; }

    // A slot's definition, for the swatch picker UI. `index` must be 0..11.
    static const PaletteSlot &slot(int index);

    // The concrete color of `index` in the active theme, or an INVALID QColor for
    // `kDefault` (or an out-of-range index) so the caller falls back to the theme's
    // normal color. `dark` selects the dark-theme variant.
    static QColor color(int index, bool dark);

    // True when `index` names a real slot (0..11) rather than the default sentinel.
    static bool isSlot(int index) { return index >= 0 && index < kSlotCount; }
};

} // namespace loftail
