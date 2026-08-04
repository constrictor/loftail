#pragma once

#include <QByteArrayView>
#include <QString>
#include <QtGlobal>

#include <memory>

namespace loftail {

// Abstract read access to a log file's bytes. The model and indexer bind to this
// interface and cannot tell which implementation they hold (ARCHITECTURE.md §6).
//
// APPEND-SAFETY IS BUILT IN FROM THE START (invariant #5, PLAN.md sequencing):
//   * No implementation assumes the file is immutable — `size()` is re-queried,
//     never cached as final, and there is no post-mortem vs live mode.
//   * The file is held with full sharing / a way that never blocks the writer
//     from appending, rotating, or truncating it.
//   * Rotation/truncation detection is wired now (identity()/refreshSize()) even
//     though append *ingestion* lands in M6.
class LogSource
{
public:
    virtual ~LogSource() = default;

    // A view over [offset, offset+length) of the file. Valid until the next
    // grow/close on the source. For random-access sources this is O(1); it is
    // only ever called for byte ranges already known to be indexed.
    virtual QByteArrayView bytes(qint64 offset, qint64 length) = 0;

    // The size known as of the last refreshSize(). Re-queried, never assumed final.
    virtual qint64 size() const = 0;

    // Re-stat the file. Returns the current size. This is how the M6 watch loop
    // and rotation check learn the file grew; it must be safe to call repeatedly.
    virtual qint64 refreshSize() = 0;

    // True for local seekable files; false for future gz/SSH sources (§6.2). The
    // indexer stays a single forward pass regardless (invariant #9).
    virtual bool isRandomAccess() const = 0;

    // An opaque file-identity token (device+inode on POSIX, file index on
    // Windows). A change means the file was rotated/replaced → discard and rescan
    // (invariant #5). 0 if unavailable.
    virtual quint64 identity() const = 0;

    // True if the file shrank below the last-indexed size since open — a
    // copytruncate or truncation. Detection only; ingestion is M6.
    virtual bool wasTruncated() const = 0;

    // True when the thing at this source's ORIGIN is no longer the thing we hold: a
    // rename+recreate at a local path, or a rotated remote file. It cannot be derived
    // from identity() alone — an open mmap follows the inode it mapped even after the
    // path is renamed away, which is precisely the case this reports (invariant #5).
    //
    // Non-pure on purpose: only sources with an origin to re-resolve implement it, so
    // in-memory and synthetic sources (tests/MemoryLogSource.h) need no boilerplate to
    // say the obviously-correct "no".
    virtual bool wasReplaced() const { return false; }
};

// Whether opening may connect to a remote host (and therefore block and prompt).
enum class OpenPolicy {
    // A user-initiated open. A remote path with no live spool connects, which may
    // prompt for a password and block for the connect timeout.
    Interactive,
    // Reopen only what is already connected. Used by Document::rescan(), which runs
    // from the live watch tick on the GUI thread mid-tail: a rotation must never turn
    // into a reconnect there. Fails fast for a remote path with no live spool.
    Reuse,
};

// Open the appropriate source for `path`: MappedLogSource (mmap) on POSIX,
// BufferedLogSource on Windows, or a SpooledLogSource over a local cache for an
// `ssh://` URL (§6.3). Returns nullptr on failure, filling `error` when given — the
// remote path has failure modes ("host unreachable", "SSH support is not built in")
// that a caller cannot phrase for itself. The local choice is platform-driven, not
// mode-driven (invariant #5, §6).
std::unique_ptr<LogSource> openLogSource(const QString &path,
                                         OpenPolicy policy = OpenPolicy::Interactive,
                                         QString *error = nullptr);

// The file-identity token for the file CURRENTLY at `path`, in the same encoding
// LogSource::identity() uses (device+inode on POSIX). Unlike an open source — whose
// identity() follows the inode it holds even after a rename — this re-resolves the
// path, so the M6 watch loop can detect a rotation that replaced the path with a
// NEW file (rename + recreate) by comparing this against the open source's
// identity() (invariant #5, §6). Returns 0 when the path cannot be stat'd or the
// platform has no cheap identity (then rotation-by-replace relies on size/truncation
// detection — the Windows path, deferred).
quint64 pathIdentity(const QString &path);

} // namespace loftail
