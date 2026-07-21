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
