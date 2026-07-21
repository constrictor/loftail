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
