#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#if !defined(Q_OS_WIN)
#include <QFile>
#endif

namespace loftail {

// A read-only handle on a file somebody else is still writing to — which under the
// always-watched model is every log loftail opens (invariant #5: observing a log must
// not disturb the process producing it).
//
// It exists because the portable answer turned out not to be portable. QFile reads a
// growing file correctly on both platforms, but its Windows open passes
// FILE_SHARE_READ | FILE_SHARE_WRITE and NOT FILE_SHARE_DELETE, so while loftail holds
// the file, the writer's ROLL — the rename of the current log, or its deletion — fails
// with a sharing violation. Appends were never blocked, which is exactly why this hid
// for six milestones: the one operation the read path exercises all day is the one
// operation the missing share bit does not touch, so nothing local ever said a word.
// Windows CI eventually did, through a test that deleted a log while a window had it
// open (tst_reload::reloadingAVanishedLogWaitsForItRatherThanFailing).
//
// So on Windows the handle is opened by hand with all three share bits. Everywhere else
// QFile is already right and is what backs this: a POSIX unlink or rename needs no
// permission from a reader at all, so there is no sharing decision to make.
//
// Read-only and positioned: every read names its own offset, because the callers are
// paint-path reads into an index of byte offsets rather than a stream being consumed.
//
// pathIdentity() (declared in LogSource.h) is implemented in this class's .cpp for both
// platforms rather than in LogSourceFactory.cpp where it used to live: Windows will only
// name a file's identity through a handle, so it is a second open with the same share
// bits and the same reason for them — and it runs on every watch tick.
class SharedReadFile
{
public:
    SharedReadFile() = default;
    ~SharedReadFile();
    SharedReadFile(const SharedReadFile &) = delete;
    SharedReadFile &operator=(const SharedReadFile &) = delete;

    // Opens for shared reading. Closes whatever was open first. False if the file is
    // not there or cannot be read; the reason is the platform's and is not carried,
    // because every caller answers a failed open with the waiting state (§6.5) rather
    // than by reporting it.
    bool open(const QString &path);
    void close();
    bool isOpen() const;

    // Asked of the OS on every call, never cached: the whole point of the handle is
    // that the file is growing behind it.
    qint64 size() const;

    // Up to length bytes from offset. Short at end of file, empty on any error — and a
    // short READ is not end of file, so the Windows path loops until it has what it
    // asked for or the file says there is no more.
    QByteArray read(qint64 offset, qint64 length);

private:
#if defined(Q_OS_WIN)
    // The raw HANDLE, kept as void * so that no downstream translation unit inherits
    // <windows.h> (and its min/max macros) from this header.
    void *m_handle = nullptr;
#else
    QFile m_file;
#endif
};

} // namespace loftail
