#include "SharedReadFile.h"

#if defined(Q_OS_WIN)
// The build already defines both PUBLIC on loftail_core (src/core/CMakeLists.txt); they
// are repeated here so that this file is correct on its own terms, since it is the one
// file of ours that reaches for <windows.h> deliberately rather than through libarchive.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <QDir>
#include <windows.h>
#endif

namespace loftail {

SharedReadFile::~SharedReadFile()
{
    close();
}

#if defined(Q_OS_WIN)

bool SharedReadFile::open(const QString &path)
{
    close();
    // Native separators: CreateFileW takes forward slashes for an ordinary path, but not
    // once a path is long enough to need the \\?\ prefix, and a UNC path reads wrong
    // either way in a message the user might see.
    const QString native = QDir::toNativeSeparators(path);
    const HANDLE h = ::CreateFileW(reinterpret_cast<const wchar_t *>(native.utf16()),
                                   GENERIC_READ,
                                   // All three share bits, and the third is the entire
                                   // reason this function is hand-written: without
                                   // FILE_SHARE_DELETE, holding a log open stops its
                                   // writer from rolling or deleting it.
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    m_handle = h;
    return true;
}

void SharedReadFile::close()
{
    if (m_handle) {
        ::CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
}

bool SharedReadFile::isOpen() const
{
    return m_handle != nullptr;
}

qint64 SharedReadFile::size() const
{
    if (!m_handle)
        return 0;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(static_cast<HANDLE>(m_handle), &size))
        return 0;
    return static_cast<qint64>(size.QuadPart);
}

QByteArray SharedReadFile::read(qint64 offset, qint64 length)
{
    if (!m_handle || offset < 0 || length <= 0)
        return QByteArray();

    LARGE_INTEGER pos{};
    pos.QuadPart = offset;
    if (!::SetFilePointerEx(static_cast<HANDLE>(m_handle), pos, nullptr, FILE_BEGIN))
        return QByteArray();

    QByteArray out(static_cast<qsizetype>(length), Qt::Uninitialized);
    qint64 got = 0;
    while (got < length) {
        // ReadFile counts in a DWORD and is free to return fewer bytes than asked for on
        // a file that is being written; only n == 0 means there are no more.
        const DWORD want = static_cast<DWORD>(qMin<qint64>(length - got, 1 << 20));
        DWORD n = 0;
        if (!::ReadFile(static_cast<HANDLE>(m_handle), out.data() + got, want, &n, nullptr))
            return QByteArray();
        if (n == 0)
            break;
        got += n;
    }
    out.resize(static_cast<qsizetype>(got));
    return out;
}

#else // POSIX

bool SharedReadFile::open(const QString &path)
{
    close();
    m_file.setFileName(path);
    // No sharing decision to make: a reader here holds nothing the writer needs, and an
    // unlink or rename of an open file is ordinary.
    return m_file.open(QIODevice::ReadOnly);
}

void SharedReadFile::close()
{
    m_file.close();
}

bool SharedReadFile::isOpen() const
{
    return m_file.isOpen();
}

qint64 SharedReadFile::size() const
{
    return m_file.size();
}

QByteArray SharedReadFile::read(qint64 offset, qint64 length)
{
    if (offset < 0 || length <= 0 || !m_file.isOpen() || !m_file.seek(offset))
        return QByteArray();
    return m_file.read(length);
}

#endif

} // namespace loftail
