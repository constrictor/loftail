#pragma once

#include "Decoder.h"
#include "Encoding.h"
#include "LogFormat.h"
#include "RecordIndex.h"

#include <QString>
#include <QTimeZone>

#include <memory>

namespace loftail {

class LogSource;

// All per-file state for one open log (invariant #7, ARCHITECTURE.md §12). The
// main window holds a std::vector<std::unique_ptr<Document>> plus an active
// pointer — a vector of length one today, multi-file later — and NOTHING outside
// a Document may hold per-file state (no singletons, no "current file" global).
//
// M2a scope: Document owns the source, format, decoder, zones, and index, and can
// build the index synchronously. Moving indexing to a worker thread with batched
// model updates is M2b; the split is deliberate (PLAN.md).
class Document
{
public:
    Document();
    ~Document();

    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;

    // Open `path`, compile `pattern`, resolve the encoding, and build the index
    // synchronously. Returns false if the file cannot be opened or the pattern
    // fails to compile (the compile error text is left in lastError()).
    //
    // `requestedEncoding` defaults to Auto (the persisted default is the user's
    // choice, including Auto — §6.1). An invalid `sourceZone`/`displayZone` means
    // "infer": the source zone comes from the pattern's date specifier (§5.1) and
    // the display zone defaults to the source zone.
    bool open(const QString &path,
              QStringView pattern,
              Encoding requestedEncoding = Encoding::Auto,
              const QTimeZone &sourceZone = QTimeZone(),
              const QTimeZone &displayZone = QTimeZone());

    const QString &path() const { return m_path; }
    const QString &lastError() const { return m_lastError; }

    LogSource *source() const { return m_source.get(); }
    const LogFormat &format() const { return m_format; }
    const Decoder &decoder() const { return m_decoder; }
    const RecordIndex &index() const { return m_index; }
    RecordIndex &index() { return m_index; }

    const QTimeZone &sourceZone() const { return m_sourceZone; }
    const QTimeZone &displayZone() const { return m_displayZone; }
    Encoding requestedEncoding() const { return m_requestedEncoding; }
    Encoding resolvedEncoding() const { return m_decoder.resolvedEncoding(); }

    // Every file opens at its end, following (SPEC.md §3); watching is always on.
    // The watch/append loop itself is M6; the flag lives here from the start.
    bool following() const { return m_following; }
    void setFollowing(bool f) { m_following = f; }

    // The zone inferred from a compiled format's date specifier (§5.1): UTC for a
    // %D pattern, the system zone otherwise. A hint the user may override.
    static QTimeZone inferSourceZone(const LogFormat &format);

private:
    QString                    m_path;
    QString                    m_lastError;
    std::unique_ptr<LogSource> m_source;
    LogFormat                  m_format;
    Decoder                    m_decoder;
    RecordIndex                m_index;
    QTimeZone                  m_sourceZone;
    QTimeZone                  m_displayZone;
    Encoding                   m_requestedEncoding = Encoding::Auto;
    bool                       m_following = true;
};

} // namespace loftail
