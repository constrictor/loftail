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
    s.fields = int(s.format.fields.size());
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

// Which way round a slash-separated numeric date reads. Text alone cannot tell
// whether "03/12/26" is 12 March or 3 December, so the order is inferred once over
// the whole sample (a component above 12 can only be a day) and defaults to
// month-first — the convention log4cplus's own %D{%m/%d/%y} produces.
enum class DateOrder
{
    MonthFirst,
    DayFirst,
};

DateOrder inferDateOrder(const QStringList &lines)
{
    static const QRegularExpression re(QStringLiteral("^(\\d{2})/(\\d{2})/\\d{2,4}[ T]"));
    for (const QString &line : lines) {
        const auto m = re.match(line);
        if (!m.hasMatch())
            continue;
        if (m.capturedView(1).toInt() > 12)
            return DateOrder::DayFirst;   // a first component past 12 can only be a day
        if (m.capturedView(2).toInt() > 12)
            return DateOrder::MonthFirst; // ...and likewise a second one
    }
    return DateOrder::MonthFirst;
}

// Rewrite a candidate's slash-date specifier to the order inferred from the file.
// Only a %d{...}/%D{...} body can contain "%m/%d/", so the replacement cannot
// disturb literal text.
QString applyDateOrder(QString pattern, DateOrder order)
{
    if (order == DateOrder::DayFirst)
        pattern.replace(QStringLiteral("%m/%d/"), QStringLiteral("%d/%m/"));
    return pattern;
}

// Try to recognize a date/time run at the start of `s`. Returns the consumed
// length and the %d{...} specifier that reproduces it, or {0, ""} on no match.
struct DateMatch
{
    int     length = 0;
    QString spec;
};

DateMatch matchLeadingDate(const QString &s, DateOrder order)
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
    // 3) MM/dd/yy(yy)<sep>HH:MM:SS[<msep>mmm] — the shape log4cplus's %D{%m/%d/%y}
    //    default emits. Year width and the optional millis are captured rather than
    //    spelled out as four near-identical alternatives; (?!\d) keeps the 2-digit
    //    year from swallowing the first half of a 4-digit one.
    {
        static const QRegularExpression re(QStringLiteral(
            "^\\d{2}/\\d{2}/(\\d{4}|\\d{2})(?!\\d)([ T])\\d{2}:\\d{2}:\\d{2}(?:([.,])\\d{3})?"));
        const auto m = re.match(s);
        if (m.hasMatch()) {
            const QString year = m.capturedLength(1) == 4 ? QStringLiteral("%Y")
                                                          : QStringLiteral("%y");
            const QString dmy = order == DateOrder::DayFirst
                                    ? QStringLiteral("%d/%m/") + year
                                    : QStringLiteral("%m/%d/") + year;
            const QString dsep = m.captured(2);
            QString spec = QStringLiteral("%d{%1%2%H:%M:%S").arg(dmy, dsep);
            if (m.hasCaptured(3))
                spec += QStringLiteral("%1%q").arg(m.captured(3));
            return {int(m.capturedLength(0)), spec + QStringLiteral("}")};
        }
    }
    // 4) HH:MM:SS<msep>mmm (time only, with millis)
    {
        static const QRegularExpression re(
            QStringLiteral("^(\\d{2}:\\d{2}:\\d{2})([.,])(\\d{3})"));
        const auto m = re.match(s);
        if (m.hasMatch()) {
            const QString msep = m.captured(2);
            return {int(m.capturedLength(0)), QStringLiteral("%d{%H:%M:%S%1%q}").arg(msep)};
        }
    }
    // 5) HH:MM:SS (time only)
    {
        static const QRegularExpression re(QStringLiteral("^(\\d{2}:\\d{2}:\\d{2})"));
        const auto m = re.match(s);
        if (m.hasMatch())
            return {int(m.capturedLength(0)), QStringLiteral("%d{%H:%M:%S}")};
    }
    return {};
}

// Everything left of the priority: an optional leading date, then literal
// separators with any bracketed run turned into a %t thread. `memorizedDigits`
// reports that a multi-digit run reached the pattern as LITERAL text, which means
// the date shape went unrecognized and the sample's own digits got baked in.
struct LeftPart
{
    QString pattern;
    bool    memorizedDigits = false;
};

LeftPart buildLeft(const QString &left, DateOrder order)
{
    const DateMatch dm = matchLeadingDate(left, order);
    LeftPart out;
    out.pattern = dm.spec;
    int digitRun = 0;
    int c = dm.length;
    while (c < left.size()) {
        if (left[c] == QLatin1Char('[')) {
            const int close = int(left.indexOf(QLatin1Char(']'), c));
            if (close > c) {
                out.pattern += QStringLiteral("[%t]");
                digitRun = 0;
                c = close + 1;
                continue;
            }
        }
        digitRun = left[c].isDigit() ? digitRun + 1 : 0;
        if (digitRun >= 2)
            out.memorizedDigits = true;
        out.pattern += escapeLiteral(QString(left[c]));
        ++c;
    }
    return out;
}

// Synthesize candidate ConversionPatterns from one sample line, anchored on the
// priority token. Produces up to two variants: one with a logger field after the
// priority, one where the message follows the priority directly. Scoring decides
// between them (and against the library). Returns empty when the line carries no
// recognizable priority token.
QStringList synthesizeFromLine(const QString &line, const QRegularExpression &prioRe,
                               DateOrder order)
{
    const auto pm = prioRe.match(line);
    if (!pm.hasMatch())
        return {};

    const int ps = int(pm.capturedStart(0));
    const int pe = int(pm.capturedEnd(0));
    const QString left = line.left(ps);   // includes the separator before the priority
    const QString right = line.mid(pe);   // includes the separator after the priority

    const LeftPart leftPart = buildLeft(left, order);
    if (leftPart.pattern.isEmpty())
        return {}; // no date and nothing before the priority — too little to anchor on

    // A pattern that carries the sample's own digits is not a format, it is a
    // memory of one line: it matches only the records sharing that timestamp. It
    // cannot be caught downstream, because scoring runs over the HEAD of the file
    // where every record may share one second — a memorized pattern scores a
    // perfect 1.0 there and then indexes almost nothing. Refuse to synthesize it;
    // an unrecognized date shape must fall through to manual entry instead.
    if (leftPart.memorizedDigits)
        return {};
    const QString leftPat = leftPart.pattern;

    // Consume a run of whitespace from `right` starting at `c`, returning it.
    const auto takeSpace = [&right](int &c) {
        QString s;
        while (c < right.size() && right[c].isSpace()) { s += right[c]; ++c; }
        return s;
    };
    // Consume a bracketed run at `c` (if any), returning the specifier to emit.
    const auto takeBracketed = [&right](int &c, QLatin1String spec) {
        if (c < right.size() && right[c] == QLatin1Char('[')) {
            const int close = int(right.indexOf(QLatin1Char(']'), c));
            if (close > c) {
                c = close + 1;
                return QStringLiteral("[") + spec + QStringLiteral("]");
            }
        }
        return QString();
    };

    QStringList out;

    // Variant B: message immediately after the priority (%p ... %m).
    {
        int c = 0;
        const QString sep = takeSpace(c);
        if (c < right.size()) // there is a message body
            out << leftPat + QStringLiteral("%p") + escapeLiteral(sep) + QStringLiteral("%m%n");
    }

    // Variant A: a logger token, then the message (%p %c <sep> %m), with an
    // optional bracketed thread right after the priority. Emitted twice — with and
    // without an NDC between the logger and the message — because a bracketed run
    // there is ambiguous: it is %x in a log4cplus "%c [%x] - %m" layout and plain
    // message text in a log that happens to open its messages with a bracket.
    // Scoring separates them, and on a tie the NDC variant wins for having more
    // fields, which is the right call when both parse the file.
    for (const bool withNdc : {false, true}) {
        int c = 0;
        const QString sep1 = takeSpace(c);
        const QString threadPart = takeBracketed(c, QLatin1String("%t"));
        const QString sep1b = takeSpace(c);

        const int ls = c;
        while (c < right.size() && !right[c].isSpace())
            ++c;
        if (c == ls)
            continue; // no logger token

        QString ndcPart;
        QString sepNdc;
        if (withNdc) {
            sepNdc = takeSpace(c);
            ndcPart = takeBracketed(c, QLatin1String("%x"));
            // Nothing bracketed here — this variant duplicates the other. No need to
            // put `c` back: it is declared inside this loop and starts again at 0.
            if (ndcPart.isEmpty())
                continue;
        }

        QString sep2;
        while (c < right.size() && (right[c].isSpace() || isSeparatorPunct(right[c]))) {
            sep2 += right[c];
            ++c;
        }
        if (c >= right.size())
            continue; // no message body

        out << leftPat + QStringLiteral("%p") + escapeLiteral(sep1) + threadPart
                   + escapeLiteral(sep1b) + QStringLiteral("%c") + escapeLiteral(sepNdc)
                   + ndcPart + escapeLiteral(sep2) + QStringLiteral("%m%n");
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
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} %-5p %c [%x] - %m%n"),
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S} %-5p %c [%x] - %m%n"),
        // Slash-dated styles. log4cplus's own %D{%m/%d/%y %H:%M:%S} is the usual
        // source of these, so they lead with %D — which, counter-intuitively, is
        // the LOCAL-time specifier (%d is UTC; see PatternCompiler). The %m/%d
        // order here is only a default: detect() rewrites it per file when the
        // sample proves the dates are day-first.
        QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%x] - %m%n"),
        QStringLiteral("%D{%m/%d/%y %H:%M:%S} [%t] %-5p %c - %m%n"),
        QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c - %m%n"),
        QStringLiteral("%D{%m/%d/%y %H:%M:%S,%q} %-5p %c [%x] - %m%n"),
        QStringLiteral("%D{%m/%d/%Y %H:%M:%S} %-5p %c [%x] - %m%n"),
        QStringLiteral("%D{%m/%d/%Y %H:%M:%S} %-5p %c - %m%n"),
        QStringLiteral("%d{%m/%d/%y %H:%M:%S} %-5p %c - %m%n"),
        QStringLiteral("%d{%H:%M:%S,%q} [%t] %-5p %c - %m%n"),
        QStringLiteral("%d{%H:%M:%S} %-5p %c - %m%n"),
        QStringLiteral("%d [%t] %-5p %c - %m%n"),
        QStringLiteral("%d %-5p %c - %m%n"),
        // Traditional syslog — /var/log/messages, /var/log/syslog, and anything
        // else rsyslog writes in RSYSLOG_TraditionalFileFormat. Not a log4cplus
        // layout, but it IS a PatternLayout one can express, which is the only
        // thing loftail cares about: "%b %e" is the space-padded month-and-day
        // strftime writes in the C locale, and the timestamp carries no year (see
        // TimestampParser, which infers it). %D because syslog stamps local time.
        // Three variants because the tag is written three ways — with a PID, with
        // none, and (for the kernel) with no tag shape worth splitting at all —
        // and scoring is what picks between them per file.
        QStringLiteral("%D{%b %e %H:%M:%S} %h %c[%i]: %m%n"),
        QStringLiteral("%D{%b %e %H:%M:%S} %h %c: %m%n"),
        QStringLiteral("%D{%b %e %H:%M:%S} %h %m%n"),
        // The same three in rsyslog's RSYSLOG_FileFormat, which is the DEFAULT on
        // Debian 12 and Ubuntu 24.04 and later: an RFC3339 stamp with microsecond
        // precision and an explicit offset. %Q takes the "341116" spelling as well
        // as log4cplus's own "341.116" (see PatternCompiler), and the offset in the
        // text is what settles the instant, so the %D here decides nothing.
        QStringLiteral("%D{%Y-%m-%dT%H:%M:%S.%Q%z} %h %c[%i]: %m%n"),
        QStringLiteral("%D{%Y-%m-%dT%H:%M:%S.%Q%z} %h %c: %m%n"),
        QStringLiteral("%D{%Y-%m-%dT%H:%M:%S.%Q%z} %h %m%n"),
    };
}

DetectionResult FormatDetector::detect(QByteArrayView sample, const Decoder &decoder)
{
    DetectionResult result;
    if (sample.isEmpty())
        return result;

    // Both layers need the decoded head of the file: layer 1 to settle whether the
    // file's slash dates read month-first or day-first, layer 2 to infer from.
    const QStringList lines = decodeLines(sample, decoder, kInferenceLines);
    const DateOrder order = inferDateOrder(lines);

    // --- Layer 1: candidate library, scored by match rate ---------------------
    Scored best;
    for (const QString &entry : candidateLibrary()) {
        const Scored s = scoreCandidate(applyDateOrder(entry, order), sample, decoder);
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

    QStringList seen;                 // preserve first-seen order for stable ties
    QSet<QString> dedupe;
    for (const QString &line : lines) {
        for (const QString &cand : synthesizeFromLine(line, prioRe, order)) {
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
