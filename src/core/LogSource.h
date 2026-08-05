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

    // True when the thing this source was opened FROM is no longer there AT ALL — the
    // local path was deleted, the remote file has gone missing, the container an
    // expansion is reading was removed.
    //
    // The distinction from wasReplaced() is the whole point, and the two are checked in
    // that order: REPLACED means something ELSE is at the origin now, which is a
    // rotation and rescans; VANISHED means NOTHING is, which is a wait. A source cannot
    // be both, and a rotation that has completed reads as replaced, never as vanished.
    //
    // This is an OBSERVATION, not the guess invariant #5 forbids. "The file is gone" is
    // a fact a stat answers; "the file is finished" is not, which is why isComplete()
    // needs loftail to have produced the bytes and this does not.
    //
    // Only the source can answer it, for the same reason wasReplaced() is here: what has
    // to be re-resolved differs per source — a local path is re-stat'd, a spooled one
    // asks its fetcher, and neither can be derived from the other (§6.5).
    //
    // Non-pure on purpose, the third arrival by that route after wasReplaced() (M11) and
    // isComplete() (M12): the fakes need no boilerplate to say the obviously-correct
    // "no", and a source that cannot tell must say "no" rather than guess.
    virtual bool originVanished() const { return false; }

    // True when this source's byte stream is provably FINISHED — every byte has been
    // delivered and there will never be another.
    //
    // NOT "the file is not growing right now", which is unknowable and which invariant
    // #5 forbids guessing at. This is only ever true where loftail PRODUCED the bytes
    // itself from a fixed input: an archive member expanded into its own cache (§6.4).
    // Every local file and every remote file says false forever, because neither can be
    // proven finished. An archive on another machine DOES report it once its container
    // has been fetched whole — a rewritten container is not re-expanded either way, so
    // there is genuinely nothing further to wait for.
    //
    // No user-facing mode follows from it. The live controller stops polling something
    // that cannot change, which is an absence of work rather than a setting; the follow
    // control is untouched and simply has nothing left to follow.
    //
    // Non-pure on purpose, exactly as wasReplaced() is: only a source that can prove it
    // implements it, and the fakes need no boilerplate to say the obviously-correct "no".
    virtual bool isComplete() const { return false; }
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

// Open `path` as RAW BYTES rather than as a log: identical to openLogSource() except
// that the archive branch is deliberately NOT taken, so an `ssh://` container still
// goes through its spool but a `.tar.gz` one is handed back as the compressed file it
// is (§6.4).
//
// This is what an archive's own container is opened with, and it is not an
// optimization — openLogSource() on `/logs/app.log.gz` means "expand it", so a fetcher
// using it to read its own input would recurse into expanding itself forever. The rule
// is simply that a container is bytes, never a log.
std::unique_ptr<LogSource> openContainerSource(const QString &path,
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
