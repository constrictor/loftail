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

#include "CompileError.h"
#include "Expected.h"
#include "LogFormat.h"

#include <QByteArrayView>

namespace loftail {

// The seam format autodetection (M8, ARCHITECTURE.md §9) plugs into. formatFor()
// is handed a sample of the file's leading bytes and returns a compiled LogFormat
// or a structured CompileError.
//
// The first release ships ManualFormatProvider (the user's typed pattern); a later
// release adds DetectingFormatProvider behind this SAME interface, inspecting the
// sample to guess the pattern. Because the provider is the only thing that turns a
// pattern into a LogFormat, nothing downstream of it ever sees the pattern string
// (invariant #3) — which is exactly what makes detection a drop-in rather than a
// rewrite.
class IFormatProvider
{
public:
    virtual ~IFormatProvider() = default;

    // Produce the LogFormat for a file given a sample of its leading bytes. The
    // manual provider ignores the sample; a detector inspects it.
    virtual Expected<LogFormat, CompileError> formatFor(QByteArrayView sample) = 0;
};

} // namespace loftail
