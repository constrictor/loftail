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

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Shared scaffolding for the libFuzzer targets in this directory, and for the
// corpus-replay binaries built from the same sources in an ordinary build.
//
// FUZZ_CHECK rather than assert(): a fuzz target is built RelWithDebInfo (that is
// what -DLOFTAIL_SANITIZE=address and the fuzz option both do), where NDEBUG
// compiles assert() out entirely — a property test that vanishes in the only
// configuration it is ever run in. abort() is also what libFuzzer and the
// sanitizers understand as "this input is the bug"; a returned error code is not.
#define FUZZ_CHECK(cond, what)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "loftail fuzz property violated: %s\n  at %s:%d\n  (%s)\n",       \
                         (what), __FILE__, __LINE__, #cond);                                       \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

// Every target caps the input it accepts. Not for speed: an unbounded input turns
// a property test into a memory-growth test, and libFuzzer's own -max_len already
// bounds what it generates — this keeps the corpus-replay binary, which is handed
// whatever files are on disk, bounded by the same number.
inline constexpr std::size_t kMaxFuzzInput = 64 * 1024;

// Qt's own warnings are SILENCED for the duration of a fuzz or replay run, and
// this is not tidiness. A path carrying an embedded NUL makes QFileInfo print
// "Broken filename passed to function" once per input — ArchiveLocation::split()
// stats the address it is given, by design (rule 0: a real directory named
// bundle.zip must keep working) — and a write to an unbuffered stderr per
// execution took the address target from thousands of inputs a second to four.
// The warnings say nothing about the property under test: what a target asserts
// is the value that came back, and a target that WANTED to assert something
// about a warning would have to capture it rather than read it off a console.
//
// An inline variable, so one definition serves every target and the handler is
// installed before main() — libFuzzer's main is not ours to edit.
inline const bool loftailFuzzSilenced = [] {
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &) {});
    return true;
}();

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);
