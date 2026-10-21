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

#include "Fonts.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFontInfo>
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

// A fixed-pitch family that is really installed, for when the one the platform names is not.
// The usual faces first, in no deep order, then whatever the database calls fixed pitch.
QString installedFixedPitchFamily()
{
    static const char *const kUsual[] = {
        "Menlo", "SF Mono", "Monaco", "DejaVu Sans Mono", "Liberation Mono",
        "Noto Sans Mono", "Consolas", "Courier New", "Courier",
    };
    const QStringList families = QFontDatabase::families();
    for (const char *name : kUsual) {
        const QString family = QString::fromLatin1(name);
        if (families.contains(family, Qt::CaseInsensitive) && QFontDatabase::isFixedPitch(family))
            return family;
    }
    for (const QString &family : families) {
        // LastResort is macOS's face of last resort, which draws every character as a box
        // naming its Unicode block, and may well call itself fixed pitch.
        if (QFontDatabase::isFixedPitch(family) && !QFontDatabase::isPrivateFamily(family)
            && family.compare(QLatin1String("LastResort"), Qt::CaseInsensitive) != 0) {
            return family;
        }
    }
    return {};
}
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

    // AND BRACES THAT ARE NOT A HINT. Neither of those two is honoured by every font
    // database: CoreText, for one, answers a missing family with a proportional face
    // whatever the style hint says — which is what the macOS build gets whenever the
    // platform theme's fixed font names a family that is not there (every offscreen run,
    // where it is "Monospace"). The height model memoizes one advance per codepoint on the
    // strength of the face being fixed pitch (§7.1.1), so a proportional one is not a
    // cosmetic difference. Asked only when the resolved face is not fixed pitch, so the
    // ordinary case never enumerates the database.
    if (!QFontInfo(f).fixedPitch()) {
        if (const QString family = installedFixedPitchFamily(); !family.isEmpty())
            f.setFamilies({family});
    }
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
