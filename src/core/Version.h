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

#pragma once

#include <QString>

namespace loftail {

// Application identity. Kept in core (UI-free) so both the UI layer and tests
// can reach it without a QApplication. Values are also used to configure
// QSettings via QApplication's organization/application names.
inline constexpr auto organizationName = "loftail";
inline constexpr auto applicationName = "loftail";

// The RELEASE this binary belongs to, e.g. "0.1.0". Sourced from the CMake project
// version via a compile definition, and the same string the .deb, the AppImage and
// the macOS bundle carry in their names — so it must stay a bare version number.
QString applicationVersion();

// The BUILD, e.g. "1234.g1a2b3c4": the CI run number and the abbreviated commit it
// was made from. Empty for a local build, which is how a local build says so. Not a
// release identity — every build of one release shares applicationVersion() and
// differs here. Composed at configure time; see the top-level CMakeLists.txt.
QString applicationBuildId();

// What --version prints: applicationVersion(), plus '+' and the build id when there
// is one. That is SemVer build metadata, which is defined to be ignored when versions
// are compared, so the string still names release applicationVersion() to a parser.
QString applicationVersionString();

} // namespace loftail
