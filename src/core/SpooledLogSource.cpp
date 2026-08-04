#include "SpooledLogSource.h"

#include "SourceSpool.h"

namespace loftail {

SpooledLogSource::~SpooledLogSource() = default;

std::unique_ptr<SpooledLogSource> SpooledLogSource::open(std::shared_ptr<SourceSpool> spool)
{
    if (!spool)
        return nullptr;

    auto src = std::unique_ptr<SpooledLogSource>(new SpooledLogSource());
    src->m_spool = std::move(spool);

    const FetchStatus status = src->m_spool->status();
    src->adoptGeneration(status.generation);
    src->m_size = qMin(src->m_inner ? src->m_inner->size() : 0, status.committedSize);
    // A fresh source starts clean even when it adopts a generation the fetcher
    // reached by rotating: this IS the rescan that rotation asked for.
    src->m_truncated = false;
    return src;
}

void SpooledLogSource::adoptGeneration(quint64 generation)
{
    m_generation = generation;
    const QString path = m_spool->spoolPath(generation);
    // Reuse the platform's local source over the spool: mmap on POSIX, buffered on
    // Windows. Nothing about the read path is remote-specific.
    m_inner = path.isEmpty() ? nullptr : openLogSource(path);
}

FetchStatus SpooledLogSource::fetchStatus() const
{
    return m_spool ? m_spool->status() : FetchStatus{};
}

bool SpooledLogSource::wasReplaced() const
{
    // The remote file was rotated or truncated: the fetcher started a new spool
    // generation rather than rewriting the one under us, so the bytes this source
    // holds are still intact — they just no longer describe the remote file.
    return m_spool && m_spool->status().generation != m_generation;
}

qint64 SpooledLogSource::refreshSize()
{
    if (!m_spool)
        return m_size;

    const FetchStatus status = m_spool->status();

    if (status.generation != m_generation) {
        // Rotation: swap to the new spool file. Latch truncation as well, so a caller
        // that ignores wasReplaced() still learns that the byte stream is
        // discontinuous rather than silently reading a different file's offsets.
        adoptGeneration(status.generation);
        m_truncated = true;
        m_size = 0;
    }

    // Clamp to what the fetcher has COMMITTED, not to the spool file's raw size. The
    // fetcher publishes committedSize only after its write lands, so this is what
    // makes a concurrent append impossible to observe half-written — and it is why
    // this class needs no lock against the fetcher thread at all.
    const qint64 spooled = m_inner ? m_inner->refreshSize() : 0;
    const qint64 visible = qMin(spooled, status.committedSize);

    if (visible < m_size)
        m_truncated = true; // defensive: within a generation, committed only grows
    m_size = visible;
    return m_size;
}

QByteArrayView SpooledLogSource::bytes(qint64 offset, qint64 length)
{
    if (!m_inner || offset < 0 || length <= 0 || offset >= m_size)
        return {};
    // Clamp to the committed extent, not to the inner source's own idea of the file:
    // the spool on disk may already be longer than what has been published.
    return m_inner->bytes(offset, qMin(length, m_size - offset));
}

} // namespace loftail
