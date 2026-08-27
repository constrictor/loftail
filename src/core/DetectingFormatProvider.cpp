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

#include "DetectingFormatProvider.h"

#include "Decoder.h"
#include "FormatDetector.h"

#include <QCoreApplication>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::DetectingFormatProvider)
};
} // namespace


Expected<LogFormat, CompileError> DetectingFormatProvider::formatFor(QByteArrayView sample)
{
    m_detected = false;
    m_pattern.clear();
    m_score = 0.0;

    // Resolve the encoding the same way the indexer will, then run detection over
    // the decoded sample (invariant #8 — the Decoder owns line boundaries).
    const Decoder decoder = Decoder::detect(sample, m_encoding);
    const DetectionResult r = FormatDetector::detect(sample, decoder);

    if (!r.detected) {
        // Clean no-detection: report it as a compile error so the provider is a
        // drop-in for ManualFormatProvider, while detected() stays false so the
        // caller falls back to the manual dialog (opening empty, as today).
        return Expected<LogFormat, CompileError>::makeError(
            CompileError{CompileError::Code::EmptyPattern,
                         Tr::tr("No log format could be detected"), -1});
    }

    m_detected = true;
    m_pattern = r.pattern;
    m_score = r.score;
    return r.format;
}

} // namespace loftail
