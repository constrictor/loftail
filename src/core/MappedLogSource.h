#pragma once

#include "LogSource.h"

namespace loftail {

// POSIX read strategy: mmap the currently-indexed extent, re-mapping as the file
// grows (ARCHITECTURE.md §6). Chosen on POSIX because a mapping survives a
// rename/unlink (it holds the inode), giving rotation safety for free, and gives
// zero-copy random access on the paint path.
//
// Append-safe (invariant #5): the file is opened read-only and NON-BLOCKING for
// the writer — a shared mmap never prevents the logging process from appending,
// renaming, or unlinking. Truncation (copytruncate) is caught by refreshSize()'s
// shrink check so a read past the new EOF cannot SIGBUS.
class MappedLogSource final : public LogSource
{
public:
    ~MappedLogSource() override;

    // Open and map `path`. Returns nullptr on failure (missing file, mmap error).
    static std::unique_ptr<MappedLogSource> open(const QString &path);

    QByteArrayView bytes(qint64 offset, qint64 length) override;
    qint64 size() const override { return m_mappedSize; }
    qint64 refreshSize() override;
    bool isRandomAccess() const override { return true; }
    quint64 identity() const override { return m_identity; }
    bool wasTruncated() const override { return m_truncated; }

private:
    MappedLogSource() = default;
    void unmap();
    bool remap(qint64 newSize);

    int      m_fd = -1;
    void    *m_map = nullptr;   // MAP_FAILED-checked pointer to the mapping
    qint64   m_mappedSize = 0;  // bytes currently mapped (== indexed extent)
    quint64  m_identity = 0;    // dev<<32 ^ inode, for rotation detection
    bool     m_truncated = false;
};

} // namespace loftail
