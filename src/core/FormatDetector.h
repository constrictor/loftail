#pragma once

#include "LogFormat.h"

#include <QByteArrayView>
#include <QString>
#include <QStringList>

namespace loftail {

class Decoder;

// The outcome of an attempt to autodetect a file's log4cplus ConversionPattern
// (M8, ARCHITECTURE.md §9). `detected` is the confidence gate: when false, the
// caller falls back to manual entry (the format editor opens as it does
// today). When true, `pattern` is the ConversionPattern string and `format` is
// the LogFormat it compiled to — both handed on exactly as if the user had typed
// the pattern, so nothing downstream ever learns detection happened (invariant #3).
struct DetectionResult
{
    enum class Source {
        None,       // nothing cleared the confidence threshold — fall back to manual
        Library,    // a curated candidate pattern matched
        Inference,  // structural inference off the priority anchor
    };

    bool      detected = false;
    Source    source = Source::None;
    QString   pattern;      // the winning ConversionPattern string (empty when !detected)
    LogFormat format;       // the compiled form of `pattern` (default when !detected)
    double    score = 0.0;  // match rate over the sample, 0..1
};

// Format autodetection (ARCHITECTURE.md §9). Given the leading bytes of a file and
// a Decoder, it guesses the log4cplus ConversionPattern in two layers, cheapest
// first:
//
//   1. Candidate scoring. A curated library of common log4cplus patterns is
//      compiled via PatternCompiler and each is scored by MATCH RATE over the
//      first ~200 records of the sample (fraction of records the pattern parses
//      cleanly, computed by FormatPreview). The best candidate above the
//      confidence threshold wins. Resolves the common case with no inference.
//
//   2. Structural inference. When no library candidate clears the bar, the closed
//      priority vocabulary (TRACE|DEBUG|INFO|WARN|ERROR|FATAL — Priority.h) is used
//      as an anchor: a token drawn from that six-word set is almost certainly %p.
//      A leading date-shaped run is %d, a token after the priority is %c, a
//      bracketed run between the two is %x, the remainder is %m; separators are
//      reconstructed verbatim from the sample. The synthesized pattern is scored
//      the same way and accepted only if it too clears the threshold.
//
//      Inference NEVER copies a multi-digit run into the pattern as literal text.
//      Such a pattern is a memory of one line, not a format: it matches only the
//      records sharing that timestamp, and match rate cannot catch it, because the
//      sample is the head of the file — where a startup burst may put every record
//      in the same second and score it a perfect 1.0. An unrecognized date shape
//      therefore falls through to layer 3, which is the honest answer.
//
//      Slash-separated dates are ambiguous (03/12/26 is either 12 March or 3
//      December) and text alone cannot settle it, so the order is inferred once
//      over the sample — a component above 12 can only be a day — and defaults to
//      the month-first convention log4cplus's own %D{%m/%d/%y} emits.
//
// Detection always produces a PATTERN STRING fed back through PatternCompiler —
// never a bespoke parser (ARCHITECTURE.md §9). It pre-fills the existing dialog for
// confirmation and is never applied silently. Pure: no LogSource, no file handle,
// no QApplication — unit-testable on its own.
class FormatDetector
{
public:
    // Detect over `sample` (the file's leading bytes, ~64 KB) decoded through
    // `decoder`. Returns a non-detected result rather than a guess when nothing
    // clears the confidence threshold.
    static DetectionResult detect(QByteArrayView sample, const Decoder &decoder);

    // The curated candidate library, exposed for tests. Every entry leads with a
    // date specifier, whose numeric shape is a strong anchor against false matches.
    static QStringList candidateLibrary();

private:
    FormatDetector() = delete;
};

} // namespace loftail
