#include "LogSource.h"

#include "BufferedLogSource.h"
#if defined(Q_OS_WIN)
#else
#include "MappedLogSource.h"
#include <QFile>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace loftail {

// Platform selection, not mode selection (invariant #5, §6): mmap on POSIX,
// buffered on Windows. Falls back to the buffered source if the mapping fails
// (e.g. a special file that cannot be mmapped).
std::unique_ptr<LogSource> openLogSource(const QString &path)
{
#if defined(Q_OS_WIN)
    return BufferedLogSource::open(path);
#else
    if (auto mapped = MappedLogSource::open(path))
        return mapped;
    return BufferedLogSource::open(path);
#endif
}

quint64 pathIdentity(const QString &path)
{
#if defined(Q_OS_WIN)
    // Windows file-identity via GetFileInformationByHandle lands with the M6
    // Windows work; until then rotation-by-replace there falls back to size checks.
    Q_UNUSED(path);
    return 0;
#else
    struct stat st{};
    const QByteArray local = QFile::encodeName(path);
    if (::stat(local.constData(), &st) != 0)
        return 0;
    // Same formula as MappedLogSource::identity() so the two are comparable.
    return (static_cast<quint64>(st.st_dev) << 32) ^ static_cast<quint64>(st.st_ino);
#endif
}

} // namespace loftail
