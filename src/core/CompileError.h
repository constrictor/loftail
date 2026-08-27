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

#include <QString>

namespace loftail {

// A structured failure from PatternCompiler::compile. It carries an offset into
// the original pattern string so the format editor (M3) can point a caret at
// the exact character that is wrong, rather than showing a vague "bad pattern"
// (PLAN.md M1; ARCHITECTURE.md §3).
//
// `code` lets tests and UI branch on the failure kind without matching the
// human-readable `message`, which is free to change wording.
// Every field below has a default member initializer, so no CompileError can hold a
// garbage `code`. The report comes from PatternCompiler returning one out of Expected's
// std::variant, whose active alternative the analyzer cannot track — so it reads
// std::get<E> as returning nothing. NOLINTNEXTLINE has to be the LAST comment line
// before the declaration; on any earlier one it suppresses a comment and nothing else.
// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct CompileError
{
    enum class Code {
        None,
        EmptyPattern,           // the pattern string was empty
        DanglingPercent,        // a '%' with no specifier after it (possibly after a modifier)
        UnknownSpecifier,       // a '%X' where X is not a recognized conversion character
        UnterminatedDateBrace,  // '%d{' or '%D{' with no closing '}'
        UnterminatedBrace,      // '%c{', '%X{' or '%E{' with no closing '}'
        UnsupportedDateCode,    // a code inside %d{...} outside the supported strftime subset
        DanglingPercentInDate,  // a '%' at the very end of a %d{...} inner format
        InvalidRegex,           // the generated regex failed to compile (should not happen)
    };

    Code code = Code::None;
    QString message;   // human-readable, for display
    int offset = -1;   // 0-based index into the pattern where the problem is; -1 if unknown

    bool isError() const { return code != Code::None; }
};

} // namespace loftail
