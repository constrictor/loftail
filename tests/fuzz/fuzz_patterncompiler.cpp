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

// PatternCompiler over the fuzzer's bytes read as a log4cplus ConversionPattern.
//
// The highest-value target in the tree: 621 lines, the most complex grammar here,
// and the string is TYPED BY A PERSON into Preferences — so every byte sequence a
// keyboard can produce reaches it. The %d{...} date vocabulary inside it recently
// went from ten codes to the whole C/POSIX strftime set, which is the sort of
// growth that leaves a hole nobody enumerated.
//
// What is asserted is the contract the rest of the tree binds to, not merely
// "it did not crash":
//   * a refusal carries an offset inside the pattern it refused, or the
//     documented -1 (the format editor shows a caret there);
//   * a success yields two VALID regexes — an invalid QRegularExpression matches
//     nothing silently, which is a log rendered wholly in the message column;
//   * every group index the field map hands out is a group the record regex
//     actually has, which is the shape of bugs.md 24 (a group numbered against
//     one regex and read out of another gives a null string, never an error);
//   * and both regexes are then RUN, because a compile that produces a pattern
//     nothing can match is not a compile that worked.

#include "FuzzSupport.h"

#include "LogFormat.h"
#include "PatternCompiler.h"

#include <QRegularExpression>
#include <QString>

using namespace loftail;

namespace {

// A short, ordinary record line. Short deliberately: the regex the compiler
// emits is attacker-shaped here, and match time on a long subject is what turns a
// finding about the grammar into a libFuzzer timeout about backtracking.
const QString &sampleLine()
{
    static const QString s = QStringLiteral("2026-08-27 10:15:01,123 [main] INFO  svc.a - up");
    return s;
}

void checkGroup(const LogFormat &f, int group, const char *what)
{
    if (group == -1)
        return;
    FUZZ_CHECK(group >= 1, what);
    FUZZ_CHECK(group <= f.recordRe.captureCount(), what);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    if (size > kMaxFuzzInput)
        return 0;

    const QString pattern =
        QString::fromUtf8(reinterpret_cast<const char *>(data), qsizetype(size));

    auto result = PatternCompiler::compile(pattern);
    if (!result) {
        const CompileError err = result.error();
        // -1 is CompileError's documented "unknown"; anything else has to point
        // at a character of THIS pattern, or the format editor's caret lands
        // somewhere the reader did not type.
        FUZZ_CHECK(err.offset >= -1, "a refusal's offset is inside the pattern");
        FUZZ_CHECK(err.offset <= pattern.size(), "a refusal's offset is inside the pattern");
        FUZZ_CHECK(!err.message.isEmpty(), "a refusal says why");
        return 0;
    }

    const LogFormat f = result.value();
    FUZZ_CHECK(f.recordRe.isValid(), "a compiled record regex is valid");
    FUZZ_CHECK(f.recordStartRe.isValid(), "a compiled record-start regex is valid");

    checkGroup(f, f.dateGroup, "dateGroup names a group recordRe has");
    checkGroup(f, f.prioGroup, "prioGroup names a group recordRe has");
    checkGroup(f, f.loggerGroup, "loggerGroup names a group recordRe has");
    checkGroup(f, f.threadGroup, "threadGroup names a group recordRe has");
    checkGroup(f, f.msgGroup, "msgGroup names a group recordRe has");
    for (const Field &field : f.fields)
        checkGroup(f, field.group, "a field's group is a group recordRe has");

    // A date format that compiled must be readable back by the parser that reads
    // it: tokens, not qtFormat, is what TimestampParser is driven by (LogFormat.h),
    // so a %d that produced neither is a field nothing can parse.
    if (f.dateGroup != -1) {
        FUZZ_CHECK(f.impliedDateFormat.isValid, "a %d that compiled has a date format");
        FUZZ_CHECK(!f.impliedDateFormat.tokens.isEmpty() || !f.impliedDateFormat.qtFormat.isEmpty(),
                   "a compiled date format is readable by tokens or by Qt");
    }

    // Run them. A regex that compiles and then dies on a subject is the half a
    // structural assertion cannot see.
    (void)f.recordRe.match(sampleLine());
    (void)f.recordStartRe.match(sampleLine());
    return 0;
}
