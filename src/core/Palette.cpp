#include "Palette.h"

namespace loftail {

namespace {
constexpr int kInk = HighlightPalette::kInk;
constexpr int kPaper = HighlightPalette::kPaper;

// Three tone bands of eight hues, each closed by a neutral; { name, light-theme
// color, dark-theme color, the neutral that reads on it }. See Palette.h for what
// the bands mean and why the two variants no longer flip tone between themes.
//
// The numbers behind the table, all WCAG relative-luminance:
//
//  - The Deep band is luminance-EQUALIZED across hues (0.055 on light themes, 0.085
//    on dark) rather than equal in HSL lightness, which would make a deep amber far
//    brighter than a deep purple. That lands every Deep at ~9.4:1 under Paper on a
//    light theme and ~7.6:1 on a dark one, and ~2.1:1 against a dark base — a fill
//    that is plainly a fill without glowing.
//  - The Soft band is the mirror: fixed high HSL lightness, so the pastels look like
//    a family. All clear 8.9:1 under Ink.
//  - The Vivid band is NOT equalized — maximum chroma is the point, and a screaming
//    yellow is genuinely lighter than a screaming purple. What it is instead is kept
//    OUT OF THE DEAD BAND at luminance ~0.16–0.21, where neither Ink nor Paper
//    reaches 4.5:1, and both of a hue's variants are held on the same side of it so
//    one `textOn` answer serves the slot in both themes. That constraint is why the
//    two variants of Vivid Red, Vivid Blue and Gray sit almost on top of each other:
//    those hues live next to the dead band, so there is nowhere to nudge them to.
const PaletteSlot kSlots[HighlightPalette::kSlotCount] = {
    { QLatin1StringView("Deep Red"),      QColor(0x86, 0x0F, 0x0F), QColor(0xA3, 0x1A, 0x1A), kPaper },
    { QLatin1StringView("Deep Orange"),   QColor(0x6E, 0x30, 0x0C), QColor(0x85, 0x3E, 0x16), kPaper },
    { QLatin1StringView("Deep Amber"),    QColor(0x50, 0x41, 0x09), QColor(0x62, 0x51, 0x10), kPaper },
    { QLatin1StringView("Deep Green"),    QColor(0x09, 0x4E, 0x12), QColor(0x10, 0x5F, 0x1A), kPaper },
    { QLatin1StringView("Deep Teal"),     QColor(0x08, 0x4A, 0x46), QColor(0x0F, 0x5D, 0x57), kPaper },
    { QLatin1StringView("Deep Blue"),     QColor(0x0E, 0x43, 0x7E), QColor(0x18, 0x53, 0x95), kPaper },
    { QLatin1StringView("Deep Purple"),   QColor(0x69, 0x10, 0x94), QColor(0x7F, 0x1D, 0xAF), kPaper },
    { QLatin1StringView("Deep Pink"),     QColor(0x82, 0x0E, 0x45), QColor(0x9D, 0x19, 0x57), kPaper },
    { QLatin1StringView("Ink"),           QColor(0x0B, 0x0D, 0x0F), QColor(0x0B, 0x0D, 0x0F), kPaper },

    { QLatin1StringView("Vivid Red"),     QColor(0xDB, 0x06, 0x06), QColor(0xDF, 0x06, 0x06), kPaper },
    { QLatin1StringView("Vivid Orange"),  QColor(0xEA, 0x59, 0x06), QColor(0xF9, 0x6F, 0x1F), kInk   },
    { QLatin1StringView("Vivid Amber"),   QColor(0xF9, 0xCB, 0x15), QColor(0xFA, 0xD4, 0x3D), kInk   },
    { QLatin1StringView("Vivid Green"),   QColor(0x06, 0xE0, 0x23), QColor(0x15, 0xF9, 0x34), kInk   },
    { QLatin1StringView("Vivid Teal"),    QColor(0x06, 0xDB, 0xCD), QColor(0x10, 0xF9, 0xE9), kInk   },
    { QLatin1StringView("Vivid Blue"),    QColor(0x05, 0x67, 0xD6), QColor(0x06, 0x6A, 0xDE), kPaper },
    { QLatin1StringView("Vivid Purple"),  QColor(0x8A, 0x05, 0xCC), QColor(0xA5, 0x06, 0xF4), kPaper },
    { QLatin1StringView("Vivid Pink"),    QColor(0xF6, 0x06, 0x76), QColor(0xF9, 0x10, 0x7D), kInk   },
    { QLatin1StringView("Gray"),          QColor(0x5F, 0x64, 0x69), QColor(0x5F, 0x64, 0x69), kPaper },

    { QLatin1StringView("Soft Red"),      QColor(0xEF, 0x9A, 0x9A), QColor(0xEB, 0xAD, 0xAD), kInk   },
    { QLatin1StringView("Soft Orange"),   QColor(0xEF, 0xB9, 0x9A), QColor(0xEB, 0xC4, 0xAD), kInk   },
    { QLatin1StringView("Soft Amber"),    QColor(0xEF, 0xDE, 0x9A), QColor(0xEB, 0xDE, 0xAD), kInk   },
    { QLatin1StringView("Soft Green"),    QColor(0x9A, 0xEF, 0xA5), QColor(0xAD, 0xEB, 0xB6), kInk   },
    { QLatin1StringView("Soft Teal"),     QColor(0x9A, 0xEF, 0xE9), QColor(0xAD, 0xEB, 0xE7), kInk   },
    { QLatin1StringView("Soft Blue"),     QColor(0x9A, 0xC2, 0xEF), QColor(0xAD, 0xCA, 0xEB), kInk   },
    { QLatin1StringView("Soft Purple"),   QColor(0xD2, 0x9A, 0xEF), QColor(0xD6, 0xAD, 0xEB), kInk   },
    { QLatin1StringView("Soft Pink"),     QColor(0xEF, 0x9A, 0xC2), QColor(0xEB, 0xAD, 0xCA), kInk   },
    { QLatin1StringView("Paper"),         QColor(0xF7, 0xF8, 0xF9), QColor(0xFD, 0xFD, 0xFD), kInk   },
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

int HighlightPalette::readableTextSlot(int index)
{
    // A corrupt or default index has no background to read against; Paper is the
    // answer that is at least never invisible on the un-highlighted row of a dark
    // theme, which is where an unexpected value is most likely to land.
    return isSlot(index) ? kSlots[index].textOn : kPaper;
}

} // namespace loftail
