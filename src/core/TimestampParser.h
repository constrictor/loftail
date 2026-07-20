#pragma once

#include "Record.h"

#include <QDateTime>
#include <QString>
#include <QStringView>
#include <QTimeZone>
#include <QVector>

namespace loftail {

// Parses a %d timestamp field into UTC epoch milliseconds (invariant #10, §5.1).
// Zone conversion happens EXACTLY ONCE on the way in, here: the source zone is
// applied to the parsed wall-clock text. Nothing downstream is zone-aware.
//
// Built from the LogFormat's translated Qt date format. Because indexing calls
// this once per record on a hot path, it pre-tokenizes the format into
// fixed-width numeric fields and parses by direct integer extraction rather than
// QDateTime::fromString (which is far too slow at millions of records — see the
// §11 target). Formats it cannot handle fast fall back to fromString so
// correctness never depends on the fast path.
class TimestampParser
{
public:
    TimestampParser() = default;

    // `qtFormat` is LogFormat::impliedDateFormat.qtFormat; `sourceZone` is the
    // zone the log was written in (inferred from the pattern or set by the user).
    TimestampParser(const QString &qtFormat, const QTimeZone &sourceZone);

    bool isValid() const { return m_valid; }

    // Parse one timestamp field. Returns UTC epoch ms, or Record::kNoTimestamp
    // when the text does not match the format.
    qint64 parse(QStringView text) const;

private:
    enum class Tok { Year4, Year2, Month, Day, Hour24, Hour12, Minute, Second, Millis, Literal };
    struct Token { Tok kind; int width; QChar literal; };

    QVector<Token> m_tokens;
    QTimeZone      m_zone;
    QString        m_qtFormat;   // kept for the fromString fallback
    bool           m_valid = false;
    bool           m_fastPath = false;  // all tokens are fixed-width numerics/literals
};

} // namespace loftail
