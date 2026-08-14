#include "MappedLogSource.h"

#include <QByteArray>
#include <QFile>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

namespace loftail {

std::unique_ptr<MappedLogSource> MappedLogSource::open(const QString &path)
{
    // O_RDONLY only; we never write and never lock. On POSIX a shared read-only
    // mmap places no restriction on the writer appending/renaming/unlinking, so
    // there is nothing extra to do for append-safety here (invariant #5).
    const QByteArray local = QFile::encodeName(path);
    const int fd = ::open(local.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;

    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return nullptr;
    }

    auto src = std::unique_ptr<MappedLogSource>(new MappedLogSource());
    src->m_fd = fd;
    src->m_path = path;
    src->m_identity = (static_cast<quint64>(st.st_dev) << 32) ^ static_cast<quint64>(st.st_ino);

    if (!src->remap(static_cast<qint64>(st.st_size))) {
        // A zero-length file has nothing to map yet; that is legal (an empty log
        // that has not been written to). Keep the source with a null mapping.
        if (st.st_size != 0)
            return nullptr;
    }
    // The baseline for the rewrite check. Taken here rather than lazily on the first
    // refresh, because a rewrite that lands between the open and the first watch tick is
    // exactly as invisible as any other (HeadWitness.h). An empty log takes nothing and
    // says so — its first bytes, whenever they arrive, are an append.
    src->m_head.take(src->bytes(0, HeadWitness::kBytes));
    return src;
}

MappedLogSource::~MappedLogSource()
{
    unmap();
    if (m_fd >= 0)
        ::close(m_fd);
}

void MappedLogSource::unmap()
{
    if (m_map && m_mappedSize > 0)
        ::munmap(m_map, static_cast<size_t>(m_mappedSize));
    m_map = nullptr;
    m_mappedSize = 0;
}

bool MappedLogSource::remap(qint64 newSize)
{
    unmap();
    if (newSize <= 0)
        return false;
    void *m = ::mmap(nullptr, static_cast<size_t>(newSize), PROT_READ, MAP_SHARED, m_fd, 0);
    if (m == MAP_FAILED) {
        m_map = nullptr;
        return false;
    }
    m_map = m;
    m_mappedSize = newSize;
    return true;
}

qint64 MappedLogSource::refreshSize()
{
    struct stat st{};
    if (::fstat(m_fd, &st) != 0)
        return m_mappedSize;

    const qint64 current = static_cast<qint64>(st.st_size);
    const quint64 id = (static_cast<quint64>(st.st_dev) << 32) ^ static_cast<quint64>(st.st_ino);

    // Shrink or identity change => the file was rotated/truncated (invariant #5,
    // §6). Re-map to the new (smaller or larger) extent so no read can run past the
    // live EOF (SIGBUS guard).
    if (current < m_mappedSize || id != m_identity)
        m_truncated = true;

    if (current != m_mappedSize)
        remap(current);

    // The third way the bytes we indexed can stop being the bytes in the file, and the
    // only one with nothing in the stat to give it away: the file was rewritten in place
    // and is now the same size or bigger. Neither the inode nor the size moved, so
    // without this the growth reads as an append and the pre-rewrite records stay on
    // screen for ever (HeadWitness.h). Cheap enough for every tick — the prefix is
    // already mapped, so this is a memcmp of at most a kilobyte.
    //
    // AFTER the remap, never before: on a file that grew, the mapping in hand still ends
    // at the old extent, and on one that shrank it would run past the live EOF.
    if (!m_truncated) {
        const QByteArrayView head = bytes(0, HeadWitness::kBytes);
        if (m_head.contradicts(head))
            m_truncated = true;
        else if (m_head.wantsMore())
            m_head.take(head); // a log shorter than kBytes when it opened has grown
    }
    return m_mappedSize;
}

bool MappedLogSource::wasReplaced() const
{
    // Rotation-by-replace: the file now AT THE PATH is a different inode than the one
    // this mapping holds (rename + recreate). Our fd still follows the old inode — it
    // sees no change at all — so the path itself must be re-stat'd to notice
    // (invariant #5, §6). A path that cannot be stat'd yields 0, i.e. "unknown, not
    // replaced": that is the gap between a rotate's rename and recreate, and the next
    // tick resolves it rather than triggering a rescan against a file that isn't there.
    const quint64 current = pathIdentity(m_path);
    return current != 0 && m_identity != 0 && current != m_identity;
}

bool MappedLogSource::originVanished() const
{
    // Nothing is at the path any more. This is the OTHER reading of the stat that
    // wasReplaced() above discards as "unknown": between the two of them every outcome
    // of stat'ing the path now has a meaning — a different inode is a rotation, no
    // inode at all is a deletion, the same inode is business as usual (§6.5).
    //
    // Our fd still holds the unlinked inode and can still be read, which is exactly why
    // this cannot be inferred from size() or refreshSize(): they go on answering
    // correctly about a file that is no longer reachable by name.
    return pathIdentity(m_path) == 0;
}

QByteArrayView MappedLogSource::bytes(qint64 offset, qint64 length)
{
    if (offset < 0 || length <= 0 || !m_map)
        return {};
    if (offset >= m_mappedSize)
        return {};
    const qint64 clamped = qMin(length, m_mappedSize - offset);
    return QByteArrayView(static_cast<const char *>(m_map) + offset, clamped);
}

} // namespace loftail
