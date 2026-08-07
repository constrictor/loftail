#pragma once

#include "LogSource.h"
#include "SourceFetcher.h"

#include <memory>

namespace loftail {

class SourceSpool;

// A log that is not directly readable as a local file, read through the local spool a
// SourceFetcher is filling (ARCHITECTURE.md §6.3). One per Document; several may
// share one SourceSpool.
//
// The whole trick is that this class does no reading of its own: bytes() and size()
// delegate to an ORDINARY LOCAL SOURCE over the spool file, opened through the same
// openLogSource() everything else uses. So the paint path gets the existing mmap,
// zero-copy and latency-free, and `isRandomAccess()` is TRUE even for a remote file.
// ARCHITECTURE.md §6.2 predicted it would be false; the accommodation that actually
// paid off was invariant #9 — because the indexer only ever scans forward, the spool
// can be filled and indexed at the same time.
//
// THREADING. Everything here runs on the thread that owns this source, except that
// the fetcher thread is concurrently appending to the spool file and publishing its
// status. The two are kept apart by one rule: refreshSize() is the only method that
// reopens or re-maps the inner source, and only ONE thread ever calls it on a given
// instance — the same rule that already makes the index worker safe against a growing
// local file (LiveController.h). For a Document's source that thread is the GUI
// thread; the rule is stated per instance rather than naming the GUI thread because a
// fetcher may itself hold a private source it drives from its own thread.
class SpooledLogSource final : public LogSource
{
public:
    ~SpooledLogSource() override;

    // Bind to `spool` and adopt its current generation. Never fails: a spool that has
    // not committed a byte yet is a legal empty source, exactly as a zero-length local
    // file is (MappedLogSource::open).
    static std::unique_ptr<SpooledLogSource> open(std::shared_ptr<SourceSpool> spool);

    QByteArrayView bytes(qint64 offset, qint64 length) override;
    qint64 size() const override { return m_size; }
    qint64 refreshSize() override;
    bool isRandomAccess() const override { return true; } // the spool is a local file
    quint64 identity() const override { return m_generation; }
    bool wasTruncated() const override { return m_truncated; }
    bool wasReplaced() const override;
    bool originVanished() const override;
    bool notReadyYet() const override;
    bool isComplete() const override;

    // The fetcher's view of the world, for the status bar: connecting, priming,
    // live, or failing (and why). Not used for any correctness decision here.
    FetchStatus fetchStatus() const;

    const std::shared_ptr<SourceSpool> &spool() const { return m_spool; }

private:
    SpooledLogSource() = default;
    void adoptGeneration(quint64 generation);

    std::shared_ptr<SourceSpool> m_spool;
    std::unique_ptr<LogSource>   m_inner;      // a local source over the spool file
    qint64                       m_size = 0;   // committed bytes, never the raw file
    quint64                      m_generation = 0;
    bool                         m_truncated = false;
};

// A one-line, user-facing description of what a source is doing, or an EMPTY string
// when there is nothing worth saying — a plain local file, a healthily tailing remote
// log, a finished expansion. The status bar is for the exceptional case.
//
//   "expanding — 41.2 MB of 300 MB"   an archive member being decompressed
//   "expanding — 41.2 MB so far"      the same, where the expanded size is not recorded
//   "fetching — 2.1 MB of 40 MB"      a remote log being primed
//   "connecting…"                     before the first byte
//   "prod-web: connection refused"    a failure, in the fetcher's own words
//   "waiting for app.log to appear"   the input is not there; still trying (§6.5)
//
// `path` decides the verb, because the LogSource alone cannot tell an expansion from a
// download. NEVER contains a credential: the text comes from FetchStatus::error, which
// carries the same promise.
QString sourceStatusText(const LogSource &source, const QString &path);

} // namespace loftail
