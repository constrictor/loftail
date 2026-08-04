#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

// Belt and braces with the NOMINMAX the build already defines on Windows (see
// src/core/CMakeLists.txt): <archive.h> reaches <windows.h>, whose min()/max() macros
// break std::numeric_limits<...>::min() in any loftail header included after it. This
// header is the one most likely to be included first, being a fixture.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <archive.h>
#include <archive_entry.h>

// M12 — archives built at test runtime with libarchive's WRITE side.
//
// No binary fixtures are committed to the repository. Building them here is portable
// to all three platforms, needs no `gzip` or `tar` binary on the machine, and — the
// point — exercises none of loftail's own code, so a test that reads one back is
// testing the reader rather than agreeing with itself.
//
// Only compiled where LOFTAIL_HAVE_ARCHIVE is set; every test that includes this is
// gated on the same thing.

namespace loftail::fixtures {

using Member = QPair<QString, QByteArray>;

namespace detail {

inline bool writeEntries(const QString &path, int format, int (*addFilter)(struct archive *),
                         const QVector<Member> &members)
{
    struct archive *a = archive_write_new();
    if (!a)
        return false;
    bool ok = true;

    if (addFilter)
        ok = ok && addFilter(a) == ARCHIVE_OK;

    switch (format) {
    case ARCHIVE_FORMAT_RAW:
        ok = ok && archive_write_set_format_raw(a) == ARCHIVE_OK;
        break;
    case ARCHIVE_FORMAT_ZIP:
        ok = ok && archive_write_set_format_zip(a) == ARCHIVE_OK;
        break;
    default:
        ok = ok && archive_write_set_format_pax_restricted(a) == ARCHIVE_OK;
        break;
    }

    ok = ok && archive_write_open_filename(a, path.toLocal8Bit().constData()) == ARCHIVE_OK;

    for (const Member &member : members) {
        if (!ok)
            break;
        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, member.first.toUtf8().constData());
        archive_entry_set_size(entry, member.second.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        ok = ok && archive_write_header(a, entry) == ARCHIVE_OK;
        if (ok && !member.second.isEmpty()) {
            const la_ssize_t written =
                archive_write_data(a, member.second.constData(),
                                   static_cast<size_t>(member.second.size()));
            ok = written == member.second.size();
        }
        archive_entry_free(entry);
    }

    ok = archive_write_close(a) == ARCHIVE_OK && ok;
    archive_write_free(a);
    return ok;
}

} // namespace detail

// A bare gzip stream: one member, never named. What a rotated app.log.1.gz is.
inline bool writeGzip(const QString &path, const QByteArray &content)
{
    return detail::writeEntries(path, ARCHIVE_FORMAT_RAW, archive_write_add_filter_gzip,
                                {{QStringLiteral("data"), content}});
}

inline bool writeXz(const QString &path, const QByteArray &content)
{
    return detail::writeEntries(path, ARCHIVE_FORMAT_RAW, archive_write_add_filter_xz,
                                {{QStringLiteral("data"), content}});
}

inline bool writeZip(const QString &path, const QVector<Member> &members)
{
    return detail::writeEntries(path, ARCHIVE_FORMAT_ZIP, nullptr, members);
}

inline bool writeTarGz(const QString &path, const QVector<Member> &members)
{
    return detail::writeEntries(path, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED,
                                archive_write_add_filter_gzip, members);
}

inline bool writeTar(const QString &path, const QVector<Member> &members)
{
    return detail::writeEntries(path, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, nullptr, members);
}

// A log body large enough to cross the fetcher's 128 KiB prime and several 256 KiB
// expansion chunks, so the incremental paths are exercised rather than short-circuited.
inline QByteArray logBody(int records)
{
    QByteArray out;
    out.reserve(records * 64);
    for (int i = 0; i < records; ++i) {
        out += QStringLiteral("2026-08-05 10:%1:%2,%3 [main] INFO  app.core - record %4\n")
                   .arg(i / 60 % 60, 2, 10, QLatin1Char('0'))
                   .arg(i % 60, 2, 10, QLatin1Char('0'))
                   .arg(i % 1000, 3, 10, QLatin1Char('0'))
                   .arg(i)
                   .toUtf8();
    }
    return out;
}

} // namespace loftail::fixtures
