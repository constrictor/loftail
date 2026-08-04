#pragma once

#include "LogSource.h"
#include "RemoteFetcher.h"

#include <memory>

namespace loftail {

class RemoteSpool;

// A remote log, read through the local spool a RemoteFetcher is filling
// (ARCHITECTURE.md §6.3). One per Document; several may share one RemoteSpool.
//
// The whole trick is that this class does no reading of its own: bytes() and size()
// delegate to an ORDINARY LOCAL SOURCE over the spool file, opened through the same
// openLogSource() everything else uses. So the paint path gets the existing mmap,
// zero-copy and latency-free, and `isRandomAccess()` is TRUE for a remote file.
// ARCHITECTURE.md §6.2 predicted it would be false; the accommodation that actually
// paid off was invariant #9 — because the indexer only ever scans forward, the spool
// can be filled and indexed at the same time.
//
// THREADING. Everything here runs on the GUI thread, except that the fetcher thread
// is concurrently appending to the spool file and publishing its status. The two are
// kept apart by one rule: refreshSize() is the only method that reopens or re-maps
// the inner source, and only the GUI thread calls it — the same rule that already
// makes the index worker safe against a growing local file (LiveController.h).
class SpooledLogSource final : public LogSource
{
public:
    ~SpooledLogSource() override;

    // Bind to `spool` and adopt its current generation. Never fails: a spool that has
    // not committed a byte yet is a legal empty source, exactly as a zero-length local
    // file is (MappedLogSource::open).
    static std::unique_ptr<SpooledLogSource> open(std::shared_ptr<RemoteSpool> spool);

    QByteArrayView bytes(qint64 offset, qint64 length) override;
    qint64 size() const override { return m_size; }
    qint64 refreshSize() override;
    bool isRandomAccess() const override { return true; } // the spool is a local file
    quint64 identity() const override { return m_generation; }
    bool wasTruncated() const override { return m_truncated; }
    bool wasReplaced() const override;

    // The fetcher's view of the world, for the status bar: connecting, priming,
    // live, or failing (and why). Not used for any correctness decision here.
    FetchStatus fetchStatus() const;

    const std::shared_ptr<RemoteSpool> &spool() const { return m_spool; }

private:
    SpooledLogSource() = default;
    void adoptGeneration(quint64 generation);

    std::shared_ptr<RemoteSpool> m_spool;
    std::unique_ptr<LogSource>   m_inner;      // a local source over the spool file
    qint64                       m_size = 0;   // committed bytes, never the raw file
    quint64                      m_generation = 0;
    bool                         m_truncated = false;
};

} // namespace loftail
