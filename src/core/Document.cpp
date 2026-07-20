#include "Document.h"

#include "Indexer.h"
#include "LogSource.h"
#include "PatternCompiler.h"

namespace loftail {

Document::Document() = default;
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
                       QStringView pattern,
                       Encoding requestedEncoding,
                       const QTimeZone &sourceZone,
                       const QTimeZone &displayZone)
{
    m_lastError.clear();
    m_index = RecordIndex();

    auto compiled = PatternCompiler::compile(pattern);
    if (!compiled) {
        m_lastError = compiled.error().message;
        return false;
    }
    m_format = compiled.value();

    m_source = openLogSource(path);
    if (!m_source) {
        m_lastError = QStringLiteral("Cannot open file: %1").arg(path);
        return false;
    }

    m_path = path;
    m_requestedEncoding = requestedEncoding;

    // Resolve the encoding by sniffing the first ~64 KB (§6.1).
    const qint64 sampleLen = qMin<qint64>(64 * 1024, m_source->size());
    const QByteArrayView sample = sampleLen > 0 ? m_source->bytes(0, sampleLen) : QByteArrayView();
    m_decoder = Decoder::detect(sample, requestedEncoding);

    m_sourceZone = sourceZone.isValid() ? sourceZone : inferSourceZone(m_format);
    m_displayZone = displayZone.isValid() ? displayZone : m_sourceZone;
    m_index.rebuildBlockSums(); // empty index has a valid (zero) total
    return true;
}

bool Document::open(const QString &path,
                    QStringView pattern,
                    Encoding requestedEncoding,
                    const QTimeZone &sourceZone,
                    const QTimeZone &displayZone)
{
    if (!prepare(path, pattern, requestedEncoding, sourceZone, displayZone))
        return false;

    Indexer indexer(m_format, m_decoder, m_sourceZone);
    m_index = indexer.index(*m_source);
    return true;
}

} // namespace loftail
