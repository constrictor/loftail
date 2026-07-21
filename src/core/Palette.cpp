#include "Palette.h"

namespace loftail {

namespace {
// 12 slots, each { name, light-theme color, dark-theme color }. Light-theme colors
// are saturated mid-darks that read on a near-white base (as text) or tint a row
// (as background); dark-theme colors are the softer pastel counterparts that read
// on a near-black base. Reds/ambers/greens for severity plus neutral hues (§7).
const PaletteSlot kSlots[HighlightPalette::kSlotCount] = {
    { QLatin1StringView("Red"),    QColor(0xC6, 0x28, 0x28), QColor(0xEF, 0x9A, 0x9A) },
    { QLatin1StringView("Orange"), QColor(0xE6, 0x51, 0x00), QColor(0xFF, 0xCC, 0x80) },
    { QLatin1StringView("Amber"),  QColor(0xF9, 0xA8, 0x25), QColor(0xFF, 0xE0, 0x82) },
    { QLatin1StringView("Green"),  QColor(0x2E, 0x7D, 0x32), QColor(0xA5, 0xD6, 0xA7) },
    { QLatin1StringView("Teal"),   QColor(0x00, 0x69, 0x5C), QColor(0x80, 0xCB, 0xC4) },
    { QLatin1StringView("Blue"),   QColor(0x15, 0x65, 0xC0), QColor(0x90, 0xCA, 0xF9) },
    { QLatin1StringView("Indigo"), QColor(0x28, 0x35, 0x93), QColor(0x9F, 0xA8, 0xDA) },
    { QLatin1StringView("Purple"), QColor(0x6A, 0x1B, 0x9A), QColor(0xCE, 0x93, 0xD8) },
    { QLatin1StringView("Pink"),   QColor(0xAD, 0x14, 0x57), QColor(0xF4, 0x8F, 0xB1) },
    { QLatin1StringView("Brown"),  QColor(0x4E, 0x34, 0x2E), QColor(0xBC, 0xAA, 0xA4) },
    { QLatin1StringView("Slate"),  QColor(0x37, 0x47, 0x4F), QColor(0xB0, 0xBE, 0xC5) },
    { QLatin1StringView("Gray"),   QColor(0x61, 0x61, 0x61), QColor(0xE0, 0xE0, 0xE0) },
};
} // namespace

const PaletteSlot &HighlightPalette::slot(int index)
{
    // Clamp defensively; a corrupt persisted index must not read out of bounds.
    if (index < 0)
        index = 0;
    else if (index >= kSlotCount)
        index = kSlotCount - 1;
    return kSlots[index];
}

QColor HighlightPalette::color(int index, bool dark)
{
    if (!isSlot(index))
        return QColor(); // kDefault (or corrupt): invalid == "use the theme color"
    const PaletteSlot &s = kSlots[index];
    return dark ? s.dark : s.light;
}

} // namespace loftail
