#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <functional>
#include <memory>

namespace loftail {

class LogSource;

// One member of an archive, as the picker shows it.
struct ArchiveEntry
{
    QString path;         // as recorded in the archive, leading "./" and "/" removed
    qint64  size = -1;    // -1 when the archive does not record it (a raw gzip stream)
    qint64  mtime = 0;    // seconds since the epoch; 0 when not recorded
};

// Called when the reader has consumed everything its input has committed so far.
// Return true once more input has arrived (the read is retried), false when there will
// never be any more, which the reader treats as end of file.
//
// This is what lets an archive be expanded WHILE its container is still arriving: a
// remote .tar.gz is decompressed as the SSH fetcher downloads it, rather than after.
using AwaitInput = std::function<bool()>;

// A forward-only reader over an archive, sitting on an ordinary LogSource.
//
// The input being a LogSource rather than a file name is the whole reason a remote
// archive costs nothing extra: it is a MappedLogSource for a local container and a
// SpooledLogSource for one on another machine, and this cannot tell (§6.4).
//
// The ONLY translation unit that touches libarchive, along with the entry-listing
// function below — everything else in the archive path is portable and always
// compiled, so a build without the dependency differs in exactly one place.
class ArchiveStream
{
public:
    ~ArchiveStream();

    ArchiveStream(const ArchiveStream &) = delete;
    ArchiveStream &operator=(const ArchiveStream &) = delete;

    // Bind to `input`, which must outlive the stream.
    //
    // `await` null means the input is already complete, and that also buys a SEEK
    // callback: libarchive can then read a zip through its central directory, which is
    // both quicker and the only way entry sizes are known. A growing input cannot be
    // seeked — the end it would seek to has not arrived — so it stays streaming.
    //
    // `allowRaw` enables the fallback format that treats undecodable bytes as one
    // unnamed member. TRUE for a bare compressed stream, which is exactly what it is
    // for; FALSE for a multi-member container, where it would otherwise accept any
    // rubbish named .zip as a one-member archive instead of reporting it as broken.
    static std::unique_ptr<ArchiveStream> open(LogSource *input, AwaitInput await,
                                               bool allowRaw, QString *error);

    // A stream over the CURRENT MEMBER of another stream, which is what makes a
    // compressed log inside an archive readable: `logs.zip/app.log.1.gz` is the
    // ordinary shape of a rotation bundle, and libarchive's filters apply to the
    // container's own bytes, never to what comes out of a member — so without this the
    // member arrived as raw gzip, parsed to no records, and the tab sat there empty
    // with nothing on screen to say why.
    //
    // Takes ownership of `inner`, which must already be positioned on its member
    // (seekToMember). Always raw-capable and never seekable: what it reads is produced
    // a block at a time by the stream underneath, so there is no offset to seek to.
    static std::unique_ptr<ArchiveStream> openNested(std::unique_ptr<ArchiveStream> inner,
                                                     QString *error);

    // Advance to the next member. False at the end of the archive, or on error, which
    // is distinguished by `error` being non-empty.
    bool nextEntry(ArchiveEntry *out, QString *error);

    // Advance to the member at `member`, or to the sole member when `member` is empty.
    // False if there is no such member (`error` says which was wanted).
    bool seekToMember(const QString &member, QString *error);

    // Read the current member's data. Returns the byte count, 0 at its end, or -1 with
    // `error` filled.
    qint64 read(char *buffer, qint64 length, QString *error);

    // The current member's recorded size, or -1 when the archive does not carry it.
    qint64 currentSize() const;

    // Defined in the .cpp. Public only so libarchive's read callback — a free function,
    // because libarchive takes a function pointer — can reach it; nothing outside that
    // translation unit can do anything with an incomplete type.
    struct Impl;

private:
    ArchiveStream();
    std::unique_ptr<Impl> d;
};

// The regular-file members of `container`, in archive order. Directories, symlinks and
// device nodes are skipped; a bare compressed stream yields exactly one synthetic entry
// named for the container with its compression suffix removed, so the caller needs no
// special case.
//
// COST: a zip is read through its central directory and is quick at any size. A .tar.gz
// has no index and must be DECOMPRESSED to be enumerated, so listing a large one costs
// what expanding it costs — which is why the picker is never shown when there is
// nothing to choose, and why since M17 this is called from a worker rather than from the
// thread showing the picker (OpenArchiveDialog::chooseMembers).
//
// `cancel` is polled while waiting for a remote container's bytes and while walking the
// entries; returning true abandons the listing and yields what was found so far. May be
// null for a caller that will always wait.
QVector<ArchiveEntry> listArchiveMembers(const QString &container, QString *error,
                                         const std::function<bool()> &cancel = {});

} // namespace loftail
