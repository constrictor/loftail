#pragma once

#include "LogSource.h"

#include <QByteArray>

// A LogSource backed by an in-memory buffer, for indexer/model tests that must
// run without touching the filesystem or a QApplication. Exercises the same
// LogSource interface the real sources implement.
class MemoryLogSource final : public loftail::LogSource
{
public:
    explicit MemoryLogSource(QByteArray data) : m_data(std::move(data)) {}

    QByteArrayView bytes(qint64 offset, qint64 length) override
    {
        if (offset < 0 || length <= 0 || offset >= m_data.size())
            return {};
        const qint64 clamped = qMin<qint64>(length, m_data.size() - offset);
        return QByteArrayView(m_data.constData() + offset, clamped);
    }
    qint64 size() const override { return m_data.size(); }
    qint64 refreshSize() override { return m_data.size(); }
    bool isRandomAccess() const override { return true; }
    quint64 identity() const override { return 1; }
    bool wasTruncated() const override { return false; }

private:
    QByteArray m_data;
};
