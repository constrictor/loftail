#include "DetectingFormatProvider.h"

#include "Decoder.h"
#include "FormatDetector.h"

namespace loftail {

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
                         QStringLiteral("No log format could be detected"), -1});
    }

    m_detected = true;
    m_pattern = r.pattern;
    m_score = r.score;
    return r.format;
}

} // namespace loftail
