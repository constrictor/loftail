#include "BufferedLogSource.h"

#include <QFileInfo>

namespace loftail {

std::unique_ptr<BufferedLogSource> BufferedLogSource::open(const QString &path)
{
    auto src = std::unique_ptr<BufferedLogSource>(new BufferedLogSource());
    src->m_file.setFileName(path);
    // Binary read-only; QFile does not lock the writer out on any platform.
    if (!src->m_file.open(QIODevice::ReadOnly))
        return nullptr;
    src->m_size = src->m_file.size();
    src->m_identity = src->computeIdentity();
    return src;
}

quint64 BufferedLogSource::computeIdentity() const
{
    // Portable stand-in for the platform file-identity token. On POSIX this is a
    // best-effort proxy (size+mtime) since QFile does not expose the inode; the
    // real device+inode identity lives in MappedLogSource. Sufficient to wire the
    // rotation-detection seam here.
    const QFileInfo info(m_file);
    const quint64 mtime = static_cast<quint64>(info.lastModified().toMSecsSinceEpoch());
    return (mtime << 20) ^ static_cast<quint64>(info.size());
}

qint64 BufferedLogSource::refreshSize()
{
    const qint64 current = m_file.size();
    if (current < m_size)
        m_truncated = true;
    m_size = current;
    return m_size;
}

QByteArrayView BufferedLogSource::bytes(qint64 offset, qint64 length)
{
    if (offset < 0 || length <= 0 || offset >= m_size)
        return {};
    const qint64 clamped = qMin(length, m_size - offset);
    if (!m_file.seek(offset))
        return {};
    m_buffer = m_file.read(clamped);
    return QByteArrayView(m_buffer.constData(), m_buffer.size());
}

} // namespace loftail
