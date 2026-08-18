#include "Fonts.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QScreen>

namespace loftail {

namespace {
// The chosen size in points, or 0 for "nobody has chosen one". The sentinel is not
// laziness: at 0 the font is byte-identical to the one loftail drew before zoom
// existed, pixel-sized UI fonts included, so the untouched case cannot drift.
int g_logFontPointSize = 0;

// What to answer on a platform that will not say how big its text is — the Windows
// offscreen plugin has no font database at all, and a screen may not be there yet.
constexpr int kAssumedPointSize = 10;
constexpr qreal kAssumedDpi = 96.0;
} // namespace

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

int defaultLogFontPointSize()
{
    const QFont f = monospaceFont();
    if (f.pointSizeF() > 0)
        return qBound(kMinLogFontPointSize, qRound(f.pointSizeF()), kMaxLogFontPointSize);
    if (f.pixelSize() > 0) {
        // A desktop that states its text size in PIXELS. A zoom has to be expressed in
        // one unit, and points is the one that survives being carried to another screen,
        // so convert here — at the primary screen's logical DPI, the same number Qt
        // itself resolves a point size through.
        qreal dpi = kAssumedDpi;
        if (const QScreen *s = QGuiApplication::primaryScreen(); s && s->logicalDotsPerInchY() > 0)
            dpi = s->logicalDotsPerInchY();
        return qBound(kMinLogFontPointSize, qRound(f.pixelSize() * 72.0 / dpi),
                      kMaxLogFontPointSize);
    }
    return kAssumedPointSize;
}

int logFontPointSize()
{
    return g_logFontPointSize > 0 ? g_logFontPointSize : defaultLogFontPointSize();
}

bool setLogFontPointSize(int points)
{
    const int wanted = qBound(kMinLogFontPointSize, points, kMaxLogFontPointSize);
    if (wanted == logFontPointSize())
        return false; // already there — including at either bound, where a key repeat lands
    g_logFontPointSize = wanted;
    return true;
}

bool resetLogFontPointSize()
{
    if (g_logFontPointSize == 0)
        return false;
    g_logFontPointSize = 0;
    // True even where the point size rounds to the same number: what comes back is the
    // platform's own font in the platform's own unit, which on a pixel-sized desktop is
    // not the same font as that size rounded to a point.
    return true;
}

QFont logTextFont()
{
    QFont f = monospaceFont();
    if (g_logFontPointSize > 0)
        f.setPointSizeF(g_logFontPointSize);
    return f;
}

} // namespace loftail
