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
