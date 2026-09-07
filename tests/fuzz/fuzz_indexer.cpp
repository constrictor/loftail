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

// The indexer over arbitrary bytes under a fixed, known-good format.
//
// The format is held constant on purpose: what is being fuzzed here is the SCAN,
// not the grammar — fuzz_patterncompiler owns that — so the input is spent on the
// thing a log file actually is, which is bytes somebody else's process wrote and
// which loftail has to survive whatever they are. The encoding is sniffed exactly
// as an open does, from a 64 KB prefix, so a UTF-16 log and a truncated multi-byte
// sequence both reach this target through the ordinary route.
//
// What is asserted is the record space every geometry, filter and paint path in
// the tree then trusts (invariants #1, #2, #6): records are in file order, each one
// lies inside the file, and each one is at least one line. A record whose span
// runs past the end of the source is a decode of bytes that are not there.

#include "FuzzSupport.h"

#include "Decoder.h"
#include "Indexer.h"
#include "LogFormat.h"
#include "PatternCompiler.h"
#include "Record.h"
#include "RecordIndex.h"

#include "MemoryLogSource.h"

#include <QByteArray>
#include <QTimeZone>

using namespace loftail;

namespace {

// The seeded house layout — the same string tst_indexer indexes with, so a
// finding here is about the bytes and never about an exotic pattern.
const LogFormat &format()
{
    static const LogFormat f = [] {
        auto r = PatternCompiler::compile(
            QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"));
        FUZZ_CHECK(bool(r), "the seeded pattern compiles");
        return r.value();
    }();
    return f;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    if (size > kMaxFuzzInput)
        return 0;

    const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    MemoryLogSource source(bytes);

    const Decoder dec = Decoder::detect(
        source.bytesCopy(0, qMin<qint64>(source.size(), 64 * 1024)), Encoding::Auto);
    const Indexer indexer(format(), dec, QTimeZone::utc());
    const RecordIndex index = indexer.index(source);

    qint64 previous = -1;
    for (const Record &r : index.records) {
        FUZZ_CHECK(r.offset >= 0, "a record starts inside the file");
        // length is unsigned on Record, so its own floor needs no test; what has
        // to hold is that the span ends inside the file.
        FUZZ_CHECK(qint64(r.offset) + qint64(r.length) <= bytes.size(), "a record ends inside the file");
        FUZZ_CHECK(r.offset > previous, "records are in file order");
        FUZZ_CHECK(r.lineCount >= 1, "a record is at least one line (invariant #2)");
        previous = r.offset;
    }
    return 0;
}
