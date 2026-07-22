#include "Document.h"

#include "IFormatProvider.h"
#include "Indexer.h"
#include "LogSource.h"
#include "ManualFormatProvider.h"
#include "PatternCompiler.h"
#include "TimestampParser.h"

#include <QRegularExpression>

#include <limits>

namespace loftail {

Document::Document()
{
    // The FilteredIndex is a view over m_index; its address is stable for the life
    // of the Document (a member), so binding once here is enough even though
    // m_index's contents are reassigned on each prepare()/open().
    m_filtered.setSource(&m_index);
}

Document::~Document() = default;

QTimeZone Document::inferSourceZone(const LogFormat &format)
{
    // %D implies UTC, %d implies local (§5.1). Meaningful only when there is a
    // date field; without one the zone is unused.
    if (format.impliedZone == Qt::UTC)
        return QTimeZone::utc();
    return QTimeZone::systemTimeZone();
}

bool Document::prepare(const QString &path,
                       IFormatProvider &provider,
                       Encoding requestedEncoding,
                       const QTimeZone &sourceZone,
                       const QTimeZone &displayZone)
{
    m_lastError.clear();
    m_formatError = CompileError{};
    m_index = RecordIndex();
    m_filtered.clear(); // the previous subset indexed records that no longer exist
    m_format = LogFormat(); // empty == plain text until the provider succeeds

    // Run selection is per-file; a fresh prepare() starts with no runs. The caller
    // re-applies the file's remembered run-start pattern via setRunStart() after
    // this returns (and after indexing, detectRuns() rebuilds the list).
    m_runs.clear();
    m_selectedRun = -1;
    m_runStartActive = false;
    m_runStartMatcher = TextMatcher();
    recomputeViewBounds();

    m_source = openLogSource(path);
    if (!m_source) {
        m_lastError = QStringLiteral("Cannot open file: %1").arg(path);
        return false;
    }

    m_path = path;
    m_requestedEncoding = requestedEncoding;

    // Resolve the encoding by sniffing the first ~64 KB (§6.1). The same sample is
    // handed to the provider — the manual provider ignores it, a detector uses it.
    const qint64 sampleLen = qMin<qint64>(64 * 1024, m_source->size());
    const QByteArrayView sample = sampleLen > 0 ? m_source->bytes(0, sampleLen) : QByteArrayView();
    m_decoder = Decoder::detect(sample, requestedEncoding);

    auto compiled = provider.formatFor(sample);
    if (compiled) {
        m_format = compiled.value();
    } else {
        // Bad/empty/uncompilable pattern: the file STILL opens with unparsed lines
        // as plain text (SPEC.md §4). The empty LogFormat drives the indexer's
        // plain-text path (every line an Unparsed record); remember the error so
        // the Log Format dialog can point at the offending offset.
        m_formatError = compiled.error();
    }

    m_sourceZone = sourceZone.isValid() ? sourceZone : inferSourceZone(m_format);
    m_displayZone = displayZone.isValid() ? displayZone : m_sourceZone;
    m_index.rebuildBlockSums(); // empty index has a valid (zero) total
    return true;
}

bool Document::open(const QString &path,
                    IFormatProvider &provider,
                    Encoding requestedEncoding,
                    const QTimeZone &sourceZone,
                    const QTimeZone &displayZone)
{
    if (!prepare(path, provider, requestedEncoding, sourceZone, displayZone))
        return false;

    Indexer indexer(m_format, m_decoder, m_sourceZone);
    m_index = indexer.index(*m_source);
    return true;
}

bool Document::prepare(const QString &path,
                       QStringView pattern,
                       Encoding requestedEncoding,
                       const QTimeZone &sourceZone,
                       const QTimeZone &displayZone)
{
    ManualFormatProvider provider(pattern.toString());
    return prepare(path, provider, requestedEncoding, sourceZone, displayZone);
}

bool Document::open(const QString &path,
                    QStringView pattern,
                    Encoding requestedEncoding,
                    const QTimeZone &sourceZone,
                    const QTimeZone &displayZone)
{
    ManualFormatProvider provider(pattern.toString());
    return open(path, provider, requestedEncoding, sourceZone, displayZone);
}

bool Document::rescan()
{
    // Rotation/truncation (M6, SPEC.md §3): the bytes we indexed are gone or moved,
    // so drop the index and read the file at m_path again from the top — a single
    // forward pass (invariant #9), same format/decoder/zone (invariant #3). The
    // FilteredIndex is bound to &m_index (a stable member), so reassigning the
    // index's contents keeps that binding valid; we only clear its active subset.
    m_filtered.clear();
    m_source = openLogSource(m_path);
    if (!m_source) {
        m_index = RecordIndex();
        m_index.rebuildBlockSums();
        m_runs.clear();
        m_selectedRun = -1;
        recomputeViewBounds();
        m_lastError = QStringLiteral("Cannot reopen file: %1").arg(m_path);
        return false;
    }

    Indexer indexer(m_format, m_decoder, m_sourceZone);
    m_index = indexer.index(*m_source);
    // The index was rebuilt from fresh content (rotation/truncation): re-detect runs
    // and default to the newest (matches the follow-tail-on-rescan behaviour).
    detectRuns();
    selectNewestRun();
    return true;
}

QString Document::messageText(const Record &rec) const
{
    if (!m_source)
        return QString();
    const QByteArrayView bytes = m_source->bytes(rec.offset, rec.length);
    if (bytes.isEmpty())
        return QString();

    bool hadNl = false;
    const qsizetype firstEnd = m_decoder.lineEnd(bytes, 0, &hadNl);

    // No message field (plain-text / unparsed record): match against the whole
    // decoded record so a text filter still works when the pattern defines no %m
    // (SPEC.md §6). Byte range only — invariant #8 keeps this off raw newline scans.
    if (m_format.msgGroup <= 0) {
        QString whole = m_decoder.decode(bytes);
        while (whole.endsWith(QLatin1Char('\n')) || whole.endsWith(QLatin1Char('\r')))
            whole.chop(1);
        return whole;
    }

    const qsizetype firstContentLen = firstEnd - (hadNl ? m_decoder.unitSize() : 0);
    const QString firstLine =
        m_decoder.decodeLine(bytes.sliced(0, qMax<qsizetype>(0, firstContentLen)));

    // The message is the first line's tail past the record-start prefix (§4:
    // recordStartRe matches exactly up to the message field). Matching that shorter,
    // capture-free prefix — instead of the full-record recordRe with all its groups
    // — is markedly cheaper on the text-filter hot path, which runs once per record
    // that survives the integer axes (invariant #4). firstLine is an owned QString,
    // so match() shares its data (COW) with no deep copy; the QStringView-taking
    // matchView() is Qt 6.5+ only and the target minimum is 6.4 (ARCHITECTURE.md §1).
    const QRegularExpressionMatch sm = m_format.recordStartRe.match(firstLine);
    QString value = sm.hasMatch() ? firstLine.sliced(sm.capturedEnd()) : QString();

    // The message spans continuation lines (invariant #2): append the rest so a
    // multi-line record's full text is searchable, matching what data() shows.
    if (firstEnd < bytes.size()) {
        QString rest = m_decoder.decode(bytes.sliced(firstEnd));
        while (rest.endsWith(QLatin1Char('\n')) || rest.endsWith(QLatin1Char('\r')))
            rest.chop(1);
        if (!rest.isEmpty())
            value += QLatin1Char('\n') + rest;
    }
    return value;
}

void Document::applyFilters()
{
    if (!m_viewRestricted && !m_filters.anyActive()) {
        m_filtered.clear(); // identity view — nothing to materialize
        return;
    }

    const int n = m_index.records.size();
    QVector<qint32> visible;
    visible.reserve(n);
    for (int i = 0; i < n; ++i) {
        const Record &r = m_index.records.at(i);
        // Run byte-offset bound first (cheapest), then integer axes, then the message
        // decode — reached ONLY for records the earlier tests let through AND when
        // the text axis is active (invariant #4). See acceptsInView().
        if (acceptsInView(r))
            visible.append(i);
    }
    m_filtered.setVisible(std::move(visible));
}

bool Document::acceptsInView(const Record &r) const
{
    if (m_viewRestricted && (r.offset < m_viewStart || r.offset >= m_viewEnd))
        return false;
    return m_filters.accepts(r, [this, &r] { return messageText(r); });
}

QString Document::recordFirstLine(const Record &rec) const
{
    if (!m_source)
        return QString();
    const QByteArrayView bytes = m_source->bytes(rec.offset, rec.length);
    if (bytes.isEmpty())
        return QString();

    // The run-start regexp matches the WHOLE first line (timestamp, thread, priority,
    // logger, message), not the message tail — so decode the first physical line in
    // full. Byte range only; the Decoder owns line boundaries (invariant #8).
    bool hadNl = false;
    const qsizetype firstEnd = m_decoder.lineEnd(bytes, 0, &hadNl);
    const qsizetype firstContentLen = firstEnd - (hadNl ? m_decoder.unitSize() : 0);
    return m_decoder.decodeLine(bytes.sliced(0, qMax<qsizetype>(0, firstContentLen)));
}

void Document::recomputeViewBounds()
{
    m_viewRestricted = false;
    m_viewStart = std::numeric_limits<qint64>::min();
    m_viewEnd = std::numeric_limits<qint64>::max();
    if (!m_runStartActive || m_selectedRun < 0 || m_selectedRun >= m_runs.size())
        return; // no run splitting, or "all runs" selected

    m_viewStart = m_runs.at(m_selectedRun).startOffset;
    // The run ends where the NEXT run begins; the last run runs to EOF (and beyond,
    // so appended records still belong to it — the "watching the last run" case).
    m_viewEnd = (m_selectedRun + 1 < m_runs.size())
                    ? m_runs.at(m_selectedRun + 1).startOffset
                    : std::numeric_limits<qint64>::max();
    m_viewRestricted = true;
}

void Document::setRunStart(const QString &pattern, bool regex, Qt::CaseSensitivity cs)
{
    m_runStartMatcher.set(pattern, regex, cs);
    m_runStartActive = !pattern.isEmpty();
    detectRuns();
    selectNewestRun();
}

void Document::detectRuns()
{
    m_runs.clear();
    if (!m_runStartActive || m_runStartMatcher.isEmpty() || !m_runStartMatcher.isValid()) {
        recomputeViewBounds();
        return;
    }

    auto makeRun = [this](int startRecord, bool preamble) {
        const Record &r = m_index.records.at(startRecord);
        Run run;
        run.startRecord = startRecord;
        run.startOffset = r.offset;
        run.startTimestamp = r.timestamp;
        run.firstLine = recordFirstLine(r);
        run.isPreamble = preamble;
        return run;
    };

    const int n = m_index.records.size();
    int firstMarker = -1;
    for (int i = 0; i < n; ++i) {
        if (m_runStartMatcher.matches(recordFirstLine(m_index.records.at(i)))) {
            if (firstMarker < 0) {
                firstMarker = i;
                // Records before the first marker are a leading "preamble" run so no
                // content is unreachable and "all runs" == the whole file.
                if (i > 0)
                    m_runs.append(makeRun(0, /*preamble=*/true));
            }
            m_runs.append(makeRun(i, /*preamble=*/false));
        }
    }

    if (m_selectedRun >= m_runs.size())
        m_selectedRun = m_runs.isEmpty() ? -1 : int(m_runs.size()) - 1;
    recomputeViewBounds();
}

bool Document::updateRunsAfterAppend(int oldRecordCount)
{
    if (!m_runStartActive || m_runStartMatcher.isEmpty() || !m_runStartMatcher.isValid())
        return false;

    const int n = m_index.records.size();
    bool changed = false;
    for (int i = qMax(0, oldRecordCount); i < n; ++i) {
        const Record &r = m_index.records.at(i);
        if (!m_runStartMatcher.matches(recordFirstLine(r)))
            continue;
        // A first marker appearing only now: the records before it form a preamble.
        if (m_runs.isEmpty() && i > 0) {
            const Record &r0 = m_index.records.at(0);
            Run pre;
            pre.startRecord = 0;
            pre.startOffset = r0.offset;
            pre.startTimestamp = r0.timestamp;
            pre.firstLine = recordFirstLine(r0);
            pre.isPreamble = true;
            m_runs.append(pre);
        }
        Run run;
        run.startRecord = i;
        run.startOffset = r.offset;
        run.startTimestamp = r.timestamp;
        run.firstLine = recordFirstLine(r);
        m_runs.append(run);
        changed = true;
    }
    if (changed)
        recomputeViewBounds(); // the selected last run's end may now be finite
    return changed;
}

void Document::selectRun(int index)
{
    m_selectedRun = (index >= 0 && index < m_runs.size()) ? index : -1;
    recomputeViewBounds();
}

void Document::selectRunByStart(qint64 startOffset, qint64 startTimestamp)
{
    int best = -1;
    for (int i = 0; i < m_runs.size(); ++i) {
        if (m_runs.at(i).startOffset == startOffset) {
            best = i;
            break;
        }
    }
    if (best < 0 && startTimestamp != Record::kNoTimestamp) {
        for (int i = 0; i < m_runs.size(); ++i) {
            if (m_runs.at(i).startTimestamp == startTimestamp) {
                best = i;
                break;
            }
        }
    }
    selectRun(best >= 0 ? best : (m_runs.isEmpty() ? -1 : int(m_runs.size()) - 1));
}

int Document::runRecordCount(int i) const
{
    if (i < 0 || i >= m_runs.size())
        return 0;
    const int end = (i + 1 < m_runs.size()) ? m_runs.at(i + 1).startRecord
                                            : int(m_index.records.size());
    return end - m_runs.at(i).startRecord;
}

void Document::reparseTimestamps(const QTimeZone &sourceZone)
{
    m_sourceZone = sourceZone.isValid() ? sourceZone : inferSourceZone(m_format);

    // No date field, or nothing to read: the source zone is inert (§5.1).
    if (m_format.dateGroup <= 0 || !m_source)
        return;

    const TimestampParser parser(m_format.impliedDateFormat.qtFormat, m_sourceZone);
    const QRegularExpression &re = m_format.recordRe;
    const int unit = m_decoder.unitSize();

    // A pass over the existing index (invariant #10): byte offsets and record
    // boundaries are untouched, so no rescan — only the %d field is re-read and
    // re-parsed. Unparsed records carry no timestamp and are left as-is.
    for (Record &rec : m_index.records) {
        if (rec.timestamp == Record::kNoTimestamp)
            continue; // never matched a date; a zone change cannot give it one

        const QByteArrayView bytes = m_source->bytes(rec.offset, rec.length);
        if (bytes.isEmpty())
            continue;

        bool hadNl = false;
        const qsizetype end = m_decoder.lineEnd(bytes, 0, &hadNl);
        const qsizetype contentLen = end - (hadNl ? unit : 0);
        const QString firstLine =
            m_decoder.decodeLine(bytes.sliced(0, qMax<qsizetype>(0, contentLen)));

        const QRegularExpressionMatch m = re.match(firstLine);
        if (m.hasMatch())
            rec.timestamp = parser.parse(m.capturedView(m_format.dateGroup));
    }
}

} // namespace loftail
