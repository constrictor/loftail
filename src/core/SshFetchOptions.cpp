#include "SshFetcher.h"

#include <QHash>

namespace loftail {

// Per-location fetch tuning, kept in a translation unit that is ALWAYS compiled even
// though the fetcher that consumes it is not. The Open Remote dialog and the Remote
// Hosts menu set these on every build, so that the UI needs no #if of its own — the
// whole point of putting the transport behind RemoteFetcher. Without SSH support the
// values are simply stored and never read.
namespace {

QHash<QString, SshFetchOptions> &optionsStore()
{
    static QHash<QString, SshFetchOptions> store;
    return store;
}

} // namespace

void setSshFetchOptions(const RemoteLocation &location, const SshFetchOptions &options)
{
    optionsStore().insert(location.toString(), options);
}

SshFetchOptions sshFetchOptions(const RemoteLocation &location)
{
    return optionsStore().value(location.toString(), SshFetchOptions{});
}

} // namespace loftail
