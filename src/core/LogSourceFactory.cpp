#include "LogSource.h"

#include "BufferedLogSource.h"
#if defined(Q_OS_WIN)
#else
#include "MappedLogSource.h"
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

} // namespace loftail
