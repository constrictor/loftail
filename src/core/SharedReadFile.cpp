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

#include "SharedReadFile.h"

#include "LogSource.h" // pathIdentity(), implemented at the bottom of this file

#if !defined(Q_OS_WIN)
#include <QFile>
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h> // pread()
#endif

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

void SharedReadFile::read(qint64 offset, qint64 length, QByteArray &into)
{
    into.resize(0); // keeps the capacity: this is called per painted cell
    if (!m_handle || offset < 0 || length <= 0)
        return;

    // Plain resize(), not Qt::Uninitialized: QByteArray has no uninitialized resize
    // below Qt 6.8's resizeForOverwrite() and the floor is 6.4 (ARCHITECTURE.md §1).
    // The zero-fill is a memset the read then overwrites; against the read itself it
    // costs nothing, and resizing an already-large buffer keeps its capacity, which is
    // what makes this callable per painted cell.
    into.resize(static_cast<qsizetype>(length));
    qint64 got = 0;
    while (got < length) {
        // ReadFile counts in a DWORD and is free to return fewer bytes than asked for on
        // a file that is being written; only n == 0 means there are no more.
        const DWORD want = static_cast<DWORD>(qMin<qint64>(length - got, 1 << 20));
        DWORD n = 0;
        // The OVERLAPPED is what makes this positional. On a handle opened WITHOUT
        // FILE_FLAG_OVERLAPPED the call is still synchronous, but the read starts at the
        // offset named here rather than at the handle's own file pointer — so two
        // threads reading different ranges cannot be served each other's, which
        // SetFilePointerEx-then-ReadFile allowed (SharedReadFile.h). hEvent stays null;
        // nothing waits on it, because nothing is asynchronous.
        OVERLAPPED ov{};
        const quint64 at = static_cast<quint64>(offset + got);
        ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFULL);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        if (!::ReadFile(static_cast<HANDLE>(m_handle), into.data() + got, want, &n, &ov)) {
            // Reading at or past the end reports ERROR_HANDLE_EOF through this path
            // rather than a zero-byte success, and that is not an error to the caller:
            // a short read off the end of a growing log is the ordinary case.
            if (::GetLastError() != ERROR_HANDLE_EOF)
                into.resize(0);
            else
                into.resize(static_cast<qsizetype>(got));
            return;
        }
        if (n == 0)
            break;
        got += n;
    }
    into.resize(static_cast<qsizetype>(got));
}

#else // POSIX

bool SharedReadFile::open(const QString &path)
{
    close();
    m_file.setFileName(path);
    // No sharing decision to make: a reader here holds nothing the writer needs, and an
    // unlink or rename of an open file is ordinary.
    if (!m_file.open(QIODevice::ReadOnly))
        return false;
    m_fd = m_file.handle();
    return true;
}

void SharedReadFile::close()
{
    m_fd = -1;
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

void SharedReadFile::read(qint64 offset, qint64 length, QByteArray &into)
{
    into.resize(0); // keeps the capacity: this is called per painted cell
    if (offset < 0 || length <= 0 || m_fd < 0)
        return;

    // Plain resize(), not Qt::Uninitialized: QByteArray has no uninitialized resize
    // below Qt 6.8's resizeForOverwrite() and the floor is 6.4 (ARCHITECTURE.md §1).
    // The zero-fill is a memset the read then overwrites; against the read itself it
    // costs nothing, and resizing an already-large buffer keeps its capacity, which is
    // what makes this callable per painted cell.
    into.resize(static_cast<qsizetype>(length));
    qint64 got = 0;
    while (got < length) {
        // pread(2), not seek-then-read: the offset is an argument, so the descriptor
        // carries no position for a second thread to move (SharedReadFile.h). A short
        // read is not end of file — a signal or a large request can cut one short — so
        // this loops until it has what it asked for or the file says there is no more.
        const ::ssize_t n = ::pread(m_fd, into.data() + got,
                                    static_cast<size_t>(length - got),
                                    static_cast<::off_t>(offset + got));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            into.resize(0);
            return;
        }
        if (n == 0)
            break; // end of file
        got += n;
    }
    into.resize(static_cast<qsizetype>(got));
}

#endif

QByteArray SharedReadFile::read(qint64 offset, qint64 length)
{
    QByteArray out;
    read(offset, length, out);
    return out;
}

// --- the file's identity, which is the other thing a handle is for -----------
//
// Declared in LogSource.h and used by both local sources' wasReplaced(); see there for
// what the number means and why it is re-resolved from the PATH rather than read off
// the open handle.

quint64 pathIdentity(const QString &path)
{
#if defined(Q_OS_WIN)
    // Volume serial + file index, which Windows will only tell you through a handle —
    // so this is a second open, and it carries the same discipline as the first.
    //
    // FILE_READ_ATTRIBUTES asks for no data access at all, so it succeeds on a file
    // opened for exclusive writing; and the share mode is again all three bits, which
    // matters MORE here than in open(): this runs on every watch tick, so a stingier
    // one would block the writer's roll for a moment 750 ms at a time — intermittently
    // breaking the very rotation it is being called to detect.
    const QString native = QDir::toNativeSeparators(path);
    const HANDLE h = ::CreateFileW(reinterpret_cast<const wchar_t *>(native.utf16()),
                                   FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return 0; // not there, or not answering: "unknown", never "replaced"
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = ::GetFileInformationByHandle(h, &info);
    ::CloseHandle(h);
    if (!ok)
        return 0;

    const quint64 index = (static_cast<quint64>(info.nFileIndexHigh) << 32)
                          | static_cast<quint64>(info.nFileIndexLow);
    // ReFS has a 128-bit file id that does not fit here and reports 0 through this call.
    // Answer "unknown" rather than a value every file on the volume would share, which
    // would read as "not replaced" for a genuine rotation AND as a match between two
    // different files; the HeadWitness content check is the fallback either way.
    if (index == 0)
        return 0;
    // Folded the same way as the POSIX branch below, so the two encodings are the same
    // shape: the volume distinguishes files that share an index across volumes.
    return (static_cast<quint64>(info.dwVolumeSerialNumber) << 32) ^ index;
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
