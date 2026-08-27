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

#include "Encoding.h"
#include "IFormatProvider.h"

namespace loftail {

// The later-release IFormatProvider (ARCHITECTURE.md §9, PLAN.md M8): it INSPECTS
// the sample bytes and autodetects the log4cplus ConversionPattern, where
// ManualFormatProvider ignored the sample and used the user's typed pattern.
//
// It sits behind the SAME IFormatProvider seam and hands back a LogFormat produced
// by the SAME PatternCompiler, so nothing downstream of the parser can tell a
// detected format from a typed one (invariant #3). The detected pattern STRING is
// exposed via detectedPattern() so the caller can PRE-FILL the format editor
// for confirmation — detection is never applied silently (SPEC.md §4).
//
// On a confident detection formatFor() returns the LogFormat. When nothing clears
// the confidence threshold it returns a CompileError (detected() stays false), and
// the caller falls back to manual entry exactly as before.
class DetectingFormatProvider : public IFormatProvider
{
public:
    explicit DetectingFormatProvider(Encoding encoding = Encoding::Auto)
        : m_encoding(encoding)
    {
    }

    Expected<LogFormat, CompileError> formatFor(QByteArrayView sample) override;

    // Valid after formatFor(): whether detection succeeded, and if so the pattern
    // string it resolved to (empty otherwise).
    bool detected() const { return m_detected; }
    const QString &detectedPattern() const { return m_pattern; }
    double confidence() const { return m_score; }

private:
    Encoding m_encoding;
    bool     m_detected = false;
    QString  m_pattern;
    double   m_score = 0.0;
};

} // namespace loftail
