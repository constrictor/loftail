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

#include "RemoteLocation.h"
#include "SourceFetcher.h"

#include <memory>

namespace loftail {

// A log on a device, read over its serial console (SPEC.md §3, ARCHITECTURE.md §6.11).
//
// ALWAYS COMPILED, exactly as SshFetcher.h is: the factory is what SourceSpool.cpp's one
// address-to-fetcher dispatch calls, and a build with no serial support answers with a
// refusal naming the missing dependency rather than making that call site carry an `#if`.
std::unique_ptr<SourceFetcher> makeSerialFetcher(const RemoteLocation &location, QString *error);

} // namespace loftail
