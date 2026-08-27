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

#include "IFormatProvider.h"

#include <QString>

#include <utility>

namespace loftail {

// The first-release IFormatProvider (ARCHITECTURE.md §9): it holds the
// ConversionPattern the user entered and compiles it via PatternCompiler, ignoring
// the sample bytes. This is where the pattern STRING is confined — Document and
// everything below it hold only the compiled LogFormat, never the pattern
// (invariant #3, ARCHITECTURE.md §3).
class ManualFormatProvider : public IFormatProvider
{
public:
    explicit ManualFormatProvider(QString pattern) : m_pattern(std::move(pattern)) {}

    Expected<LogFormat, CompileError> formatFor(QByteArrayView sample) override;

    const QString &pattern() const { return m_pattern; }

private:
    QString m_pattern;
};

} // namespace loftail
