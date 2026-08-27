// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

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

    // True when the bytes already handed out are NO LONGER THE BYTES IN THE FILE, so
    // everything indexed from this source has to be thrown away and read again.
    //
    // Two ways in, and the second is the one that is easy to forget. The file shrank
    // below the last-indexed size (a truncation, or logrotate's copytruncate); OR its
    // already-read extent was REWRITTEN IN PLACE — `cp new.log app.log`, an editor
    // saving over it, a service restarting onto the same path. A rewrite moves neither
    // the inode nor, once the new content reaches the old length, the size, so it is
    // invisible to wasReplaced() and to a shrink check alike; only the content gives it
    // away (HeadWitness.h). Miss it and the tick reads the growth as an append, resumes
    // indexing from the old tail offset, and the pre-rewrite records stay on screen for
    // the rest of the session.
    //
    // Distinct from wasReplaced() only in what has to be re-resolved, not in what the
    // caller does about it: both mean rescan, and LiveController treats them as one.
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

    // True when this source is perfectly legal, perfectly open, and has NOTHING TO READ
    // YET — a spool whose fetcher is still connecting, still fetching its first chunk,
    // or has been refused (M17, §6.3.3). The document waits, exactly as it waits for a
    // log that has not been written; the reason comes from sourceStatusText(), which
    // already phrases each of those states differently.
    //
    // Distinct from originVanished(), and NOT foldable into it. That one is an
    // observation that the origin is GONE, and the live controller measures a two-second
    // grace period against it to cover the gap in a rotation that has renamed but not yet
    // recreated. "I have not asked yet" is a different question and must not inherit a
    // rotation-shaped hysteresis, nor teach the next reader that the two are the same.
    //
    // CONSULTED IN EXACTLY TWO PLACES — Document::prepare() and
    // LiveController::checkWhileWaiting() — and this is a rule, not an accident. In
    // particular it must NOT join checkNow()'s vanish branch: after a rotation on a slow
    // link, wasReplaced() rescans onto the new generation, and the very next tick sees
    // Priming with nothing committed yet. Fold the two together and the grace period
    // expires two seconds later and blanks the view of a log that is fine and merely
    // rotating. The two-call-site rule is what makes that unreachable, and it is
    // sufficient: a document that opened with bytes never needs this predicate again,
    // and one that opened without them stays waiting until there are some.
    //
    // ON BYTES, never on a state change, because Document::resume() is a one-way door —
    // it clears the waiting flag unconditionally, and opening a spool never fails, so a
    // resume with nothing to read settles the format against an empty sample, leaves it
    // unsettled forever, and takes the document out of the only state from which the
    // settling path can be reached again. Silently.
    //
    // Non-pure on purpose, the fourth arrival by the route wasReplaced() took.
    virtual bool notReadyYet() const { return false; }

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
// identity() (invariant #5, §6). Implemented for both platforms in SharedReadFile.cpp
// — device+inode from a stat on POSIX, volume serial + file index from a handle on
// Windows. Returns 0 for "unknown", which is every way of not getting an answer: the
// path is not there, the file will not open, or the volume has no index that fits (ReFS).
// Every caller reads 0 as "not replaced" and falls back to size/truncation and the
// HeadWitness content check, so an unknown never triggers a rescan on its own.
quint64 pathIdentity(const QString &path);

} // namespace loftail
