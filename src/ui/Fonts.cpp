#include "Fonts.h"

#include <QApplication>
#include <QFontDatabase>

namespace loftail {

QFont monospaceFont()
{
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    // Only the family should differ from the rest of the UI; keep the size the
    // user's desktop chose for application text, since the fixed font's own
    // point size is often smaller than the UI font's.
    const QFont ui = QApplication::font();
    if (ui.pixelSize() > 0)
        f.setPixelSize(ui.pixelSize());
    else
        f.setPointSizeF(ui.pointSizeF());

    // Belt and braces for the fallback path: if the designated family is missing,
    // these steer substitution toward another fixed-width face rather than a
    // proportional one.
    f.setStyleHint(QFont::TypeWriter);
    f.setFixedPitch(true);
    return f;
}

} // namespace loftail
