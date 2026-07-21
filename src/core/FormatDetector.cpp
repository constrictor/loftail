#include "FormatDetector.h"

#include "Decoder.h"
#include "FormatPreview.h"
#include "PatternCompiler.h"
#include "Priority.h"

#include <QRegularExpression>
#include <QSet>

namespace loftail {

namespace {

// Records scored per candidate (ARCHITECTURE.md §9 "over the first ~200 records").
constexpr int kScoreRecords = 200;

// Match rate a candidate must clear to be trusted without asking the user. Real
// log4cplus files score ~1.0; unstructured text scores ~0. A clearly-above-half
// bar rejects coincidental partial matches while accepting genuine multi-line logs
// (whose continuation lines fold into their record and do not count against).
constexpr double kMinConfidence = 0.6;

// How many leading lines structural inference draws candidate patterns from.
constexpr int kInferenceLines = 60;

// Decode up to `maxLines` physical lines from the sample through the Decoder
// (invariant #8 — never scan raw bytes for '\n'; the encoding may be UTF-16).
QStringList decodeLines(QByteArrayView sample, const Decoder &decoder, int maxLines)
{
    QStringList lines;
    const int unit = decoder.unitSize();
    const qsizetype size = sample.size();
    qsizetype pos = decoder.bomLength();
    while (pos < size && lines.size() < maxLines) {
        const qsizetype start = pos;
        bool hadNl = false;
        const qsizetype end = decoder.lineEnd(sample, start, &hadNl);
        const qsizetype contentLen = (end - start) - (hadNl ? unit : 0);
        lines << decoder.decodeLine(sample.sliced(start, qMax<qsizetype>(0, contentLen)));
        pos = end;
        if (pos <= start)
            break; // defensive: no forward progress
    }
    return lines;
}

// One scored candidate: its pattern, the LogFormat it compiled to, its match rate
// over the sample, and its field count (richer patterns break score ties — a
// pattern that constrains more structure and still matches is less coincidental).
struct Scored
{
    QString   pattern;
    LogFormat format;
    double    score = 0.0;
    int       fields = 0;
    bool      valid = false;
};

Scored scoreCandidate(const QString &pattern, QByteArrayView sample, const Decoder &decoder)
{
    Scored s;
    s.pattern = pattern;
    auto compiled = PatternCompiler::compile(pattern);
    if (!compiled)
        return s; // an uncompilable candidate simply cannot win
    s.format = compiled.value();
    s.fields = s.format.fields.size();
    s.valid = true;

    // Match rate == matched records / total records over the leading sample, using
    // the SAME record-start rule the indexer and preview use (invariant #2). A
    // continuation line folds into its matched record, so multi-line records are
    // not penalized; only genuinely unrecognized lines lower the score.
    const PreviewResult pv = FormatPreview::build(s.format, sample, decoder, kScoreRecords);
    s.score = pv.totalCount > 0 ? double(pv.matchedCount) / double(pv.totalCount) : 0.0;
    return s;
}

// Rank by score, then by field count (richer wins ties), so results are stable.
bool better(const Scored &a, const Scored &b)
{
    if (a.score != b.score)
        return a.score > b.score;
    return a.fields > b.fields;
}

// The closed priority vocabulary as a regex alternation, built from the Priority
// enum via priorityName() so it never diverges from the parser's list (invariant:
// reuse the vocabulary, don't duplicate it). A whole-token match (non-letter or
// string edge on both sides) keeps "ERROR" the priority field from matching the
// word "ERROR" inside a message far more often than not — and scoring rejects the
// cases where it guesses wrong.
QRegularExpression priorityTokenRe()
{
    QStringList words;
    for (const Priority p : {Priority::Trace, Priority::Debug, Priority::Info,
                             Priority::Warn, Priority::Error, Priority::Fatal}) {
        words << QString(priorityName(p));
    }
    return QRegularExpression(QStringLiteral("(?<![A-Za-z])(?:%1)(?![A-Za-z])")
                                  .arg(words.join(QLatin1Char('|'))));
}

// A separator character that can sit between the logger and the message (" - ",
// " | ", ": ", …). Deliberately excludes '.' and ',' so a dotted logger name or a
// millisecond comma is not mistaken for a separator.
bool isSeparatorPunct(QChar c)
{
    static const QString seps = QStringLiteral("-|:>/=#");
    return seps.contains(c);
}

// Escape a run of literal text for inclusion in a ConversionPattern: only '%' is
// special to PatternCompiler (it introduces a specifier), so double it.
QString escapeLiteral(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return out;
}

// Try to recognize a date/time run at the start of `s`. Returns the consumed
// length and the %d{...} specifier that reproduces it, or {0, ""} on no match.
struct DateMatch
{
    int     length = 0;
    QString spec;
};

DateMatch matchLeadingDate(const QString &s)
{
    // Ordered longest-first so the fullest date shape wins.
    // 1) yyyy-MM-dd<sep>HH:MM:SS<msep>mmm
    {
        static const QRegularExpression re(
            QStringLiteral("^(\\d{4}-\\d{2}-\\d{2})([ T])(\\d{2}:\\d{2}:\\d{2})([.,])(\\d{3})"));
        const auto m = re.match(s);
        if (m.hasMatch()) {
            const QString dsep = m.captured(2);
            const QString msep = m.captured(4);
            return {int(m.capturedLength(0)),
                    QStringLiteral("%d{%Y-%m-%d%1%H:%M:%S%2%q}").arg(dsep, msep)};
        }
    }
    // 2) yyyy-MM-dd<sep>HH:MM:SS
    {
        static const QRegularExpression re(
            QStringLiteral("^(\\d{4}-\\d{2}-\\d{2})([ T])(\\d{2}:\\d{2}:\\d{2})"));
        const auto m = re.match(s);
        if (m.hasMatch()) {
            const QString dsep = m.captured(2);
            return {int(m.capturedLength(0)),
                    QStringLiteral("%d{%Y-%m-%d%1%H:%M:%S}").arg(dsep)};
        }
    }
    // 3) HH:MM:SS<msep>mmm (time only, with millis)
    {
        static const QRegularExpression re(
            QStringLiteral("^(\\d{2}:\\d{2}:\\d{2})([.,])(\\d{3})"));
        const auto m = re.match(s);
        if (m.hasMatch()) {
            const QString msep = m.captured(2);
            return {int(m.capturedLength(0)), QStringLiteral("%d{%H:%M:%S%1%q}").arg(msep)};
        }
    }
    // 4) HH:MM:SS (time only)
    {
        static const QRegularExpression re(QStringLiteral("^(\\d{2}:\\d{2}:\\d{2})"));
        const auto m = re.match(s);
        if (m.hasMatch())
            return {int(m.capturedLength(0)), QStringLiteral("%d{%H:%M:%S}")};
    }
    return {};
}

// Build the pattern text for everything left of the priority: an optional leading
// date, then literal separators with any bracketed run turned into a %t thread.
QString buildLeft(const QString &left)
{
    const DateMatch dm = matchLeadingDate(left);
    QString out = dm.spec;
    int c = dm.length;
    while (c < left.size()) {
        if (left[c] == QLatin1Char('[')) {
            const int close = left.indexOf(QLatin1Char(']'), c);
            if (close > c) {
                out += QStringLiteral("[%t]");
                c = close + 1;
                continue;
            }
        }
        out += escapeLiteral(QString(left[c]));
        ++c;
    }
    return out;
}

// Synthesize candidate ConversionPatterns from one sample line, anchored on the
// priority token. Produces up to two variants: one with a logger field after the
// priority, one where the message follows the priority directly. Scoring decides
// between them (and against the library). Returns empty when the line carries no
// recognizable priority token.
QStringList synthesizeFromLine(const QString &line, const QRegularExpression &prioRe)
{
    const auto pm = prioRe.match(line);
    if (!pm.hasMatch())
        return {};

    const int ps = int(pm.capturedStart(0));
    const int pe = int(pm.capturedEnd(0));
    const QString left = line.left(ps);   // includes the separator before the priority
    const QString right = line.mid(pe);   // includes the separator after the priority

    const QString leftPat = buildLeft(left);
    if (leftPat.isEmpty())
        return {}; // no date and nothing before the priority — too little to anchor on

    QStringList out;

    // Variant B: message immediately after the priority (%p ... %m).
    {
        int c = 0;
        QString sep;
        while (c < right.size() && right[c].isSpace()) { sep += right[c]; ++c; }
        if (c < right.size()) // there is a message body
            out << leftPat + QStringLiteral("%p") + escapeLiteral(sep) + QStringLiteral("%m%n");
    }

    // Variant A: a logger token, then the message (%p %c <sep> %m), with an
    // optional bracketed thread right after the priority.
    {
        int c = 0;
        QString sep1;
        while (c < right.size() && right[c].isSpace()) { sep1 += right[c]; ++c; }

        QString threadPart;
        if (c < right.size() && right[c] == QLatin1Char('[')) {
            const int close = right.indexOf(QLatin1Char(']'), c);
            if (close > c) {
                threadPart = QStringLiteral("[%t]");
                c = close + 1;
            }
        }
        QString sep1b;
        while (c < right.size() && right[c].isSpace()) { sep1b += right[c]; ++c; }

        const int ls = c;
        while (c < right.size() && !right[c].isSpace())
            ++c;
        const bool haveLogger = c > ls;

        QString sep2;
        while (c < right.size() && (right[c].isSpace() || isSeparatorPunct(right[c]))) {
            sep2 += right[c];
            ++c;
        }
        const bool haveMessage = c < right.size();

        if (haveLogger && haveMessage) {
            out << leftPat + QStringLiteral("%p") + escapeLiteral(sep1) + threadPart
                       + escapeLiteral(sep1b) + QStringLiteral("%c") + escapeLiteral(sep2)
                       + QStringLiteral("%m%n");
        }
    }

    return out;
}

} // namespace

QStringList FormatDetector::candidateLibrary()
{
    // Common log4cplus ConversionPatterns: framework defaults plus widespread house
    // styles. EVERY entry leads with a date specifier — the numeric date shape is a
    // strong anchor, so a candidate that begins with %d cannot trivially match
    // unstructured text (unlike a hypothetical %p- or %c-led one). Ordered roughly
    // most-specific first so richer patterns are tried before leaner ones.
    return {
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
        QStringLiteral("%D{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"),
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} %-5p %c [%t] %m%n"),
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} %-5p [%c] %m%n"),
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} %-5p %c - %m%n"),
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S} [%t] %-5p %c - %m%n"),
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S} %-5p %c - %m%n"),
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S} %p %c - %m%n"),
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c %m%n"),
        QStringLiteral("%d{%H:%M:%S,%q} [%t] %-5p %c - %m%n"),
        QStringLiteral("%d{%H:%M:%S} %-5p %c - %m%n"),
        QStringLiteral("%d [%t] %-5p %c - %m%n"),
        QStringLiteral("%d %-5p %c - %m%n"),
    };
}

DetectionResult FormatDetector::detect(QByteArrayView sample, const Decoder &decoder)
{
    DetectionResult result;
    if (sample.isEmpty())
        return result;

    // --- Layer 1: candidate library, scored by match rate ---------------------
    Scored best;
    for (const QString &pattern : candidateLibrary()) {
        const Scored s = scoreCandidate(pattern, sample, decoder);
        if (s.valid && (!best.valid || better(s, best)))
            best = s;
    }
    if (best.valid && best.score >= kMinConfidence) {
        result.detected = true;
        result.source = DetectionResult::Source::Library;
        result.pattern = best.pattern;
        result.format = best.format;
        result.score = best.score;
        return result;
    }

    // --- Layer 2: structural inference off the priority anchor -----------------
    const QRegularExpression prioRe = priorityTokenRe();
    const QStringList lines = decodeLines(sample, decoder, kInferenceLines);

    QStringList seen;                 // preserve first-seen order for stable ties
    QSet<QString> dedupe;
    for (const QString &line : lines) {
        for (const QString &cand : synthesizeFromLine(line, prioRe)) {
            if (!dedupe.contains(cand)) {
                dedupe.insert(cand);
                seen << cand;
            }
        }
    }

    Scored bestInfer;
    for (const QString &cand : seen) {
        const Scored s = scoreCandidate(cand, sample, decoder);
        if (s.valid && (!bestInfer.valid || better(s, bestInfer)))
            bestInfer = s;
    }
    if (bestInfer.valid && bestInfer.score >= kMinConfidence) {
        result.detected = true;
        result.source = DetectionResult::Source::Inference;
        result.pattern = bestInfer.pattern;
        result.format = bestInfer.format;
        result.score = bestInfer.score;
        return result;
    }

    // --- Layer 3: give up — the caller falls back to manual entry --------------
    return result;
}

} // namespace loftail
