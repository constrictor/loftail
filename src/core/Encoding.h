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

#include <QtGlobal>

namespace loftail {

// The user-selectable per-file encoding (SPEC.md §4, ARCHITECTURE.md §6.1).
// `Auto` is the DEFAULT and is itself the persisted choice — the stored setting
// is the user's selection, never the encoding auto-detect happened to resolve to
// (§6.1). The Decoder turns `Auto` into one of the concrete values by sniffing.
enum class Encoding : quint8 {
    Auto,     // sniff a BOM, then a NUL-frequency heuristic, then validate UTF-8
    Utf8,     // forced; a BOM is tolerated and skipped
    Utf16LE,  // forced
    Utf16BE,  // forced
    System,   // forced system 8-bit codepage (Latin-1/local on Unix)
};

} // namespace loftail
