#pragma once

#include "ArchiveLocation.h"
#include "SourceFetcher.h"

#include <memory>

namespace loftail {

// Build a fetcher that expands `location`'s member into a spool. Returns nullptr with
// `error` filled where archive support is not compiled in. Opening the container — and
// connecting to another machine, when the container lives there — happens in start(),
// not here.
//
// Declared in an always-compiled header, like makeSshFetcher(), so the dispatch in
// SourceSpool.cpp needs no include-path condition of its own.
std::unique_ptr<SourceFetcher> makeArchiveFetcher(const ArchiveLocation &location,
                                                  QString *error);

} // namespace loftail
