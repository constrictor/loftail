#pragma once

#include "RemoteFetcher.h"
#include "RemoteLocation.h"

#include <memory>

namespace loftail {

// Per-file fetch tuning. The Open Remote dialog sets these before opening; anything
// not set uses the defaults below.
struct SshFetchOptions
{
    // Fetch only the last N bytes of the remote file rather than all of it, for a log
    // far too large to copy down. 0 means "the whole file", which is the default so
    // that a remote log behaves exactly like a local one. When non-zero the view
    // starts mid-file, and the status bar has to say so.
    qint64 tailStartBytes = 0;

    // How often to ask the server whether the file changed. This is the NETWORK
    // cadence; the GUI-side watch tick is separate and much cheaper (§6.3).
    int pollMs = 1000;

    // Bounds the connect and every subsequent SFTP call.
    int timeoutMs = 20000;
};

// Above this, the Open Remote dialog offers to fetch only the tail rather than the
// whole file. Advisory — it changes what is suggested, never what is allowed.
constexpr qint64 kSshLargeFileThreshold = 256LL * 1024 * 1024;

// Remember options for a location, consulted when its fetcher is next built.
void setSshFetchOptions(const RemoteLocation &location, const SshFetchOptions &options);
SshFetchOptions sshFetchOptions(const RemoteLocation &location);

// Build a fetcher that reads `location` over SSH. Returns nullptr with `error` filled
// where SSH support is not compiled in. Connecting happens in start(), not here.
std::unique_ptr<RemoteFetcher> makeSshFetcher(const RemoteLocation &location, QString *error);

} // namespace loftail
