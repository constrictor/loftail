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
