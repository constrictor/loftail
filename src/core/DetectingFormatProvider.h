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
// exposed via detectedPattern() so the caller can PRE-FILL the Log Format dialog
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
