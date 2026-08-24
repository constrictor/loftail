#include "ArchiveReader.h"

#include "ArchiveLocation.h"
#include "LogSource.h"
#include "SpooledLogSource.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QFileInfo>
#include <QThread>

#include <cerrno>
#include <cstring>

#include <archive.h>
#include <archive_entry.h>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::ArchiveReader)
};
} // namespace


namespace {

// Handed to libarchive one block at a time. Big enough that the decompressor is not
// dominated by callback overhead, small enough that a growing input is re-checked
// often while a remote container is still arriving.
constexpr qint64 kInputChunk = 256LL * 1024;

// A member path as recorded in an archive may be written "./var/log/app.log" or
// "/var/log/app.log" or "var/log/app.log" for the same thing. One spelling, so a
// member picked from the list matches the member sought on reopen.
QString normalizeMember(QString path)
{
    while (path.startsWith(QLatin1String("./")))
        path.remove(0, 2);
    while (path.startsWith(u'/'))
        path.remove(0, 1);
    return path;
}

QString lastError(struct archive *a, const QString &fallback)
{
    if (a) {
        if (const char *text = archive_error_string(a)) {
            const QString message = QString::fromUtf8(text);
            if (!message.isEmpty())
                return message;
        }
    }
    return fallback;
}

} // namespace

struct ArchiveStream::Impl
{
    struct archive *handle = nullptr;
    LogSource      *input = nullptr;
    AwaitInput      await;
    qint64          offset = 0;
    QByteArray      block;   // libarchive holds this until the next callback
    qint64          entrySize = -1;
    bool            atEntry = false;

    // Set instead of `input` when this stream decompresses another stream's member
    // (openNested). Owned, because the inner stream is this one's whole input and
    // outliving it is the point.
    std::unique_ptr<ArchiveStream> inner;
};

namespace {

// The read callback. Copies into the stream's own buffer rather than handing out a
// view into the input: refreshSize() below may re-map a MappedLogSource, and a caller
// holding a pointer into the old mapping would be reading freed address space.
la_ssize_t readBlock(struct archive *a, void *client, const void **buffer)
{
    auto *d = static_cast<ArchiveStream::Impl *>(client);
    Q_UNUSED(a);

    for (;;) {
        qint64 available = d->input->size() - d->offset;
        if (available <= 0) {
            d->input->refreshSize();
            available = d->input->size() - d->offset;
        }
        if (available > 0) {
            const qint64 want = qMin(available, kInputChunk);
            const QByteArrayView view = d->input->bytes(d->offset, want);
            if (view.isEmpty())
                return 0;
            // resize() keeps the capacity, so this is a memcpy per block and not an
            // allocation per block. (QByteArray::assign would say it better but is
            // Qt 6.6; the floor is 6.4 — ARCHITECTURE.md §1.)
            d->block.resize(view.size());
            std::memcpy(d->block.data(), view.data(), static_cast<size_t>(view.size()));
            d->offset += d->block.size();
            *buffer = d->block.constData();
            return static_cast<la_ssize_t>(d->block.size());
        }
        // Nothing committed beyond what we have read. Either more is coming — a
        // container still being fetched — or this really is the end.
        if (!d->await || !d->await())
            return 0;
    }
}

// Only installed for a complete input. Seeking is what lets libarchive read a zip
// through its central directory rather than streaming it, which is where entry sizes
// come from.
la_int64_t seekBlock(struct archive *a, void *client, la_int64_t offset, int whence)
{
    auto *d = static_cast<ArchiveStream::Impl *>(client);
    Q_UNUSED(a);

    qint64 base = 0;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = d->offset;
        break;
    case SEEK_END:
        base = d->input->refreshSize();
        break;
    default:
        return ARCHIVE_FATAL;
    }
    d->offset = qBound<qint64>(0, base + offset, d->input->size());
    return d->offset;
}

// The read callback for a nested stream: its input is not a LogSource at all but the
// bytes of another stream's current member, pulled a block at a time. No seek callback
// goes with it — those bytes exist only as they are produced.
la_ssize_t readInner(struct archive *a, void *client, const void **buffer)
{
    auto *d = static_cast<ArchiveStream::Impl *>(client);

    d->block.resize(kInputChunk);
    QString readError;
    const qint64 got = d->inner->read(d->block.data(), kInputChunk, &readError);
    if (got < 0) {
        // EIO rather than libarchive's own ARCHIVE_ERRNO_MISC, which lives in a
        // private header: the number only reaches archive_errno(), which nothing here
        // reads, while the string is what lastError() puts in front of the user.
        archive_set_error(a, EIO, "%s", readError.toUtf8().constData());
        return -1;
    }
    d->block.resize(got);
    *buffer = d->block.constData();
    return static_cast<la_ssize_t>(got);
}

} // namespace

ArchiveStream::ArchiveStream() : d(std::make_unique<Impl>()) {}

ArchiveStream::~ArchiveStream()
{
    if (d && d->handle)
        archive_read_free(d->handle);
}

std::unique_ptr<ArchiveStream> ArchiveStream::open(LogSource *input, AwaitInput await,
                                                   bool allowRaw, QString *error)
{
    if (!input) {
        if (error)
            *error = Tr::tr("No archive to read.");
        return nullptr;
    }

    std::unique_ptr<ArchiveStream> stream(new ArchiveStream);
    stream->d->input = input;
    stream->d->await = std::move(await);

    struct archive *a = archive_read_new();
    if (!a) {
        if (error)
            *error = Tr::tr("Cannot start reading the archive.");
        return nullptr;
    }
    stream->d->handle = a;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (allowRaw) {
        // AFTER format_all, and it is what makes a bare .gz a one-member archive: raw
        // is the fallback format, so a gzipped tar is still recognised as a tar and
        // both kinds share one code path from here on. Off for a multi-member
        // container, where raw would swallow a corrupt file as one member of garbage.
        archive_read_support_format_raw(a);
    }

    const bool seekable = !stream->d->await;
    if (seekable)
        archive_read_set_seek_callback(a, seekBlock);
    archive_read_set_read_callback(a, readBlock);
    archive_read_set_callback_data(a, stream->d.get());

    if (archive_read_open1(a) != ARCHIVE_OK) {
        if (error)
            *error = lastError(a, Tr::tr("Cannot read the archive."));
        return nullptr;
    }
    return stream;
}

std::unique_ptr<ArchiveStream> ArchiveStream::openNested(std::unique_ptr<ArchiveStream> inner,
                                                         QString *error)
{
    if (!inner) {
        if (error)
            *error = Tr::tr("No archive to read.");
        return nullptr;
    }

    std::unique_ptr<ArchiveStream> stream(new ArchiveStream);
    stream->d->inner = std::move(inner);

    struct archive *a = archive_read_new();
    if (!a) {
        if (error)
            *error = Tr::tr("Cannot start reading the archive.");
        return nullptr;
    }
    stream->d->handle = a;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    // Raw LAST and always, exactly as open() does it: the compression filter is what
    // this stream exists for, and raw is the format that hands the filtered bytes back
    // as one unnamed member. A `.tar.gz` member is still recognised as a tar by the
    // format probe above, which is why the ordering is not merely tidiness.
    archive_read_support_format_raw(a);

    archive_read_set_read_callback(a, readInner);
    archive_read_set_callback_data(a, stream->d.get());

    if (archive_read_open1(a) != ARCHIVE_OK) {
        if (error)
            *error = lastError(a, Tr::tr("Cannot read the archive."));
        return nullptr;
    }
    return stream;
}

bool ArchiveStream::nextEntry(ArchiveEntry *out, QString *error)
{
    d->atEntry = false;
    d->entrySize = -1;

    struct archive_entry *entry = nullptr;
    const int rc = archive_read_next_header(d->handle, &entry);
    if (rc == ARCHIVE_EOF)
        return false;
    if (rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
        if (error)
            *error = lastError(d->handle, Tr::tr("Cannot read the archive."));
        return false;
    }

    if (out) {
        const char *name = archive_entry_pathname_utf8(entry);
        if (!name)
            name = archive_entry_pathname(entry);
        out->path = normalizeMember(name ? QString::fromUtf8(name) : QString());
        out->size = archive_entry_size_is_set(entry) ? archive_entry_size(entry) : -1;
        out->mtime = archive_entry_mtime_is_set(entry) ? archive_entry_mtime(entry) : 0;
    }
    d->entrySize = archive_entry_size_is_set(entry) ? archive_entry_size(entry) : -1;
    d->atEntry = archive_entry_filetype(entry) == AE_IFREG
        || archive_entry_filetype(entry) == 0; // raw format reports no type
    return true;
}

bool ArchiveStream::seekToMember(const QString &member, QString *error)
{
    const QString wanted = normalizeMember(member);
    ArchiveEntry entry;
    QString readError;

    while (nextEntry(&entry, &readError)) {
        if (!d->atEntry)
            continue; // a directory or a link is not a log
        if (wanted.isEmpty() || entry.path == wanted)
            return true;
    }
    if (error) {
        *error = readError.isEmpty()
            ? Tr::tr("The archive holds no member named %1.").arg(member)
            : readError;
    }
    return false;
}

qint64 ArchiveStream::read(char *buffer, qint64 length, QString *error)
{
    const la_ssize_t got = archive_read_data(d->handle, buffer, static_cast<size_t>(length));
    if (got < 0) {
        if (error)
            *error = lastError(d->handle, Tr::tr("Cannot read the archive."));
        return -1;
    }
    return got;
}

qint64 ArchiveStream::currentSize() const
{
    return d->entrySize;
}

namespace {

// Wait for a spooled container to deliver more bytes, or to prove it will not.
//
// The listing counterpart of ArchiveFetcher::awaitInput(), and the same reasoning: a
// remote container arrives progressively, an SSH fetcher tails forever and so never
// reaches a terminal state, and libarchive always reads past the end looking for more.
// Simpler than the fetcher's because there is nothing to publish and no generation to
// worry about — only "are there more bytes, and if not will there ever be".
// How long to sleep between checks while a remote container is still arriving.
constexpr unsigned long kAwaitSliceMs = 20;

bool awaitMoreOf(SpooledLogSource *spooled, const std::function<bool()> &cancel)
{
    const qint64 before = spooled->size();
    for (;;) {
        if (cancel && cancel())
            return false;

        if (spooled->refreshSize() > before)
            return true;

        const FetchStatus upstream = spooled->fetchStatus();
        switch (upstream.state) {
        case FetchStatus::State::Idle:
        case FetchStatus::State::Error:
        case FetchStatus::State::Disconnected:
        case FetchStatus::State::Complete:
            // Refresh once more before believing it: the fetcher may have committed its
            // last chunk and then stopped, so the bytes are there while the state says
            // otherwise.
            return spooled->refreshSize() > before;
        case FetchStatus::State::Waiting:
            break; // the host is down and still trying; this is a wait, not an end
        case FetchStatus::State::Connecting:
        case FetchStatus::State::Priming:
        case FetchStatus::State::Live:
            if (upstream.totalSize > 0 && upstream.committedSize >= upstream.totalSize)
                return spooled->refreshSize() > before; // everything has arrived
            break;
        }
        QThread::msleep(kAwaitSliceMs);
    }
}

} // namespace

QVector<ArchiveEntry> listArchiveMembers(const QString &container, QString *error,
                                         const std::function<bool()> &cancel)
{
    QVector<ArchiveEntry> entries;

    // A bare compressed stream holds exactly one member and does not name it. Answer
    // without decompressing a byte: the name is the container's, minus the suffix.
    if (ArchiveLocation::isSingleStreamName(container)) {
        ArchiveLocation loc;
        loc.container = container;
        entries.append(ArchiveEntry{loc.displayMember(), -1, 0});
        return entries;
    }

    QString openError;
    std::unique_ptr<LogSource> input =
        openContainerSource(container, OpenPolicy::Interactive, &openError);
    if (!input) {
        if (error) {
            *error = openError.isEmpty()
                ? Tr::tr("Cannot open %1.").arg(container)
                : openError;
        }
        return entries;
    }

    // A LOCAL container is whole the moment it is opened, so it needs no await — which
    // also makes it seekable, and a zip is then read through its central directory
    // rather than streamed.
    //
    // A REMOTE one is not, and assuming otherwise was a bug that predates M17: the
    // comment here used to say "fully available by the time it is listed", but nothing
    // ever made that true. It enumerated whatever the SSH fetcher had committed at that
    // instant — the 128 KB prime — so listing a remote multi-member tar quietly returned
    // the first few members and called that the archive. Since M17 it would have found
    // zero bytes and reported the container empty.
    //
    // allowRaw is false: everything reaching here is a multi-member container, the
    // single-stream case having returned above.
    AwaitInput await;
    if (auto *spooled = dynamic_cast<SpooledLogSource *>(input.get()))
        await = [spooled, cancel]() { return awaitMoreOf(spooled, cancel); };

    auto stream = ArchiveStream::open(input.get(), std::move(await), false, error);
    if (!stream)
        return entries;

    ArchiveEntry entry;
    QString readError;
    while (stream->nextEntry(&entry, &readError)) {
        // Only regular files, and only ones with something in them: an empty entry is
        // a placeholder, and a directory or a symlink is not a log.
        if (!stream->currentSize())
            continue;
        if (entry.path.isEmpty())
            continue;
        entries.append(entry);
    }
    if (!readError.isEmpty() && error)
        *error = readError;
    return entries;
}

} // namespace loftail
