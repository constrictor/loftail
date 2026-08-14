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
    src->m_pathIdentity = pathIdentity(path);
    src->m_head.take(src->readHead());
    return src;
}

// The first bytes of the file, read fresh. Deliberately NOT through bytes(), whose
// QByteArrayView is backed by m_buffer: this runs from refreshSize() on the watch tick,
// and clobbering the buffer a caller may still be reading from would be a use-after-free
// in everything but name.
QByteArray BufferedLogSource::readHead()
{
    if (!m_file.isOpen() || !m_file.seek(0))
        return QByteArray();
    return m_file.read(HeadWitness::kBytes);
}

bool BufferedLogSource::wasReplaced() const
{
    // Compare the path's identity NOW against the one captured at open — not against
    // identity(), which is the size+mtime stand-in below and moves on every append.
    // On Windows pathIdentity() is still a stub returning 0, so this is false there
    // and rotation-by-replace falls back to the size/truncation checks, exactly as
    // before (LogSourceFactory.cpp, M6 Windows work).
    const quint64 current = pathIdentity(m_file.fileName());
    return current != 0 && m_pathIdentity != 0 && current != m_pathIdentity;
}

bool BufferedLogSource::originVanished() const
{
    // Deliberately NOT pathIdentity() == 0, which is how MappedLogSource answers this.
    // On Windows pathIdentity() is a stub that returns 0 unconditionally, so routing
    // through it would report every open file as vanished the instant the watch ticked —
    // and this is the Windows source. Ask the filesystem the question directly instead;
    // it is the same stat either way and it is honest on both platforms (§6.5).
    return !QFileInfo::exists(m_file.fileName());
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

    // A rewrite in place that did not shrink the file moves neither the size nor the
    // path identity, so the growth would otherwise read as an append and the pre-rewrite
    // records would stay on screen for ever (HeadWitness.h). AFTER m_size is updated,
    // because readHead() reads through the same handle the size bounds.
    if (!m_truncated) {
        const QByteArray head = readHead();
        if (m_head.contradicts(head))
            m_truncated = true;
        else if (m_head.wantsMore())
            m_head.take(head); // a log shorter than kBytes when it opened has grown
    }
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
