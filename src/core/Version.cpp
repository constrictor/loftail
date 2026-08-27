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

#include "Version.h"

#ifndef LOFTAIL_VERSION
#define LOFTAIL_VERSION "0.0.0"
#endif

// Empty is the ordinary value, not a fallback: a local build carries no build id and
// prints a bare version. The guard is for a translation unit compiled outside the
// loftail_core target, which would otherwise fail to build rather than say "unknown".
#ifndef LOFTAIL_BUILD_ID
#define LOFTAIL_BUILD_ID ""
#endif

namespace loftail {

QString applicationVersion()
{
    return QStringLiteral(LOFTAIL_VERSION);
}

QString applicationBuildId()
{
    return QStringLiteral(LOFTAIL_BUILD_ID);
}

QString applicationVersionString()
{
    const QString build = applicationBuildId();
    if (build.isEmpty())
        return applicationVersion();
    return applicationVersion() + QLatin1Char('+') + build;
}

} // namespace loftail
