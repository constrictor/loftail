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

#include "ManualFormatProvider.h"

#include "PatternCompiler.h"

namespace loftail {

Expected<LogFormat, CompileError> ManualFormatProvider::formatFor(QByteArrayView sample)
{
    // The manual provider needs no file content — the user already told us the
    // layout. A later DetectingFormatProvider is what will consume `sample`.
    Q_UNUSED(sample);
    return PatternCompiler::compile(m_pattern);
}

} // namespace loftail
