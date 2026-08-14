#pragma once

#include "HeadWitness.h"
#include "LogSource.h"

#include <QByteArray>
#include <QFile>

namespace loftail {

// Buffered read strategy: the Windows source, and the portable fallback
// everywhere (ARCHITECTURE.md §6). On Windows it is PREFERRED over a mapping
// because a held file mapping can block the writer from rotating or truncating —
// exactly what a logging framework does — and under the always-watched model
// that risk would apply to every open file. The Windows open uses full sharing
// (FILE_SHARE_READ | WRITE | DELETE) so loftail never locks the writer out.
//
// NOTE (M2a, Linux dev host): this is compiled and unit-tested on POSIX via
// QFile, which is portable, so the seam is real and exercised. The Windows-only
// non-blocking share-mode open (CreateFile) is NOT reachable to build/test on
// this machine and lands with the M6 Windows work; QFile already opens for
// shared read on Windows, which is enough for the M2a read path.
class BufferedLogSource final : public LogSource
{
public:
    static std::unique_ptr<BufferedLogSource> open(const QString &path);

    QByteArrayView bytes(qint64 offset, qint64 length) override;
    qint64 size() const override { return m_size; }
    qint64 refreshSize() override;
    bool isRandomAccess() const override { return true; }
    quint64 identity() const override { return m_identity; }
    bool wasTruncated() const override { return m_truncated; }
    bool wasReplaced() const override;
    bool originVanished() const override;

private:
    BufferedLogSource() = default;
    quint64 computeIdentity() const;
    QByteArray readHead();

    QFile      m_file;
    qint64     m_size = 0;
    quint64    m_identity = 0;
    // The platform identity OF THE PATH as it stood at open, which is what
    // wasReplaced() compares against. Distinct from m_identity: that one is the
    // portable size+mtime stand-in below, which changes on every append and so
    // cannot answer "was this file replaced".
    quint64    m_pathIdentity = 0;
    bool       m_truncated = false;
    // The file's first bytes, to catch a rewrite in place (HeadWitness.h). It carries
    // more weight here than in the mapped source: on Windows pathIdentity() is still a
    // stub, so wasReplaced() is always false there and this is the only thing that can
    // tell a rotation apart from an append at all.
    HeadWitness m_head;
    QByteArray m_buffer;      // backs the QByteArrayView returned by bytes()
};

} // namespace loftail
