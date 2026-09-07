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

// The regression half of fuzzing, and the half that runs for everyone.
//
// A libFuzzer target is just a function taking bytes. This is a main() that walks
// the files of a corpus directory and hands each one to that function — no
// libFuzzer, no Clang, no coverage instrumentation, nothing to install. So every
// input the fuzzer ever finds interesting, and every crash it ever produces once
// somebody drops the minimised file into tests/fuzz/corpus/, becomes an ordinary
// CTest case that runs in the default GCC build in a few milliseconds and stays
// run for as long as the project exists.
//
// The corpus directory is the argument, so one source file makes both binaries:
// the fuzz build links the target with libFuzzer's main, the ordinary build links
// the same target with this one.
//
// A failing input aborts inside FUZZ_CHECK (or inside the code under test), which
// is a non-zero exit and therefore a red CTest case. The file name is printed
// BEFORE the input runs rather than after, because the run may not come back.

#include "FuzzSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <cstdio>

namespace {

bool replayFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "corpus replay: cannot read %s\n", qPrintable(path));
        return false;
    }
    const QByteArray bytes = f.readAll();
    std::fprintf(stderr, "corpus replay: %s (%lld bytes)\n", qPrintable(QFileInfo(path).fileName()),
                 static_cast<long long>(bytes.size()));
    std::fflush(stderr);
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                           std::size_t(bytes.size()));
    return true;
}

} // namespace

// A QCoreApplication is deliberately NOT created: everything these targets reach
// is src/core, which links QtCore only and is unit-testable without one. If a
// target ever needs an event loop, it has stopped being a pure parser and belongs
// in an ordinary tst_ binary instead.
int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir-or-file> [...]\n", argv[0]);
        return 2;
    }

    int replayed = 0;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        const QFileInfo info(arg);
        if (info.isDir()) {
            QDir dir(arg);
            const QStringList names = dir.entryList(QDir::Files, QDir::Name);
            for (const QString &name : names) {
                if (!replayFile(dir.filePath(name)))
                    return 1;
                ++replayed;
            }
        } else {
            if (!replayFile(arg))
                return 1;
            ++replayed;
        }
    }

    // An empty corpus is a FAILURE, not a pass. A directory that was renamed,
    // emptied or never installed would otherwise report a green case having
    // replayed nothing, which is the exact shape of a guard that guards nothing.
    if (replayed == 0) {
        std::fprintf(stderr, "corpus replay: no inputs found — the corpus is empty or missing\n");
        return 1;
    }

    // The empty input as well, always: it is the one input every parser is handed
    // by a person (an empty Preferences field, a bare Return in the open dialog)
    // and the one no corpus file can hold.
    LLVMFuzzerTestOneInput(nullptr, 0);

    std::fprintf(stderr, "corpus replay: %d inputs, no property violated\n", replayed);
    return 0;
}
