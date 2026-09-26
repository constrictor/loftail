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

#include "ExecByteSafety.h"

#include <QCoreApplication>

#include <utility>

namespace loftail {
namespace {

struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::ExecByteSafety)
};

// A pattern of exactly the bytes a terminal line discipline interferes with, plus one that
// is neither: LF and CR (translated), XON and XOFF (eaten as flow control), 0xFF (stripped
// to 0x7F where the line is set to seven bits) and an ordinary letter as a control.
//
// EVERY ONE OF THESE EARNS ITS PLACE AND DROPPING ONE IS A SILENT BUG. Without 0x11/0x13 the
// probe passes on a line doing software flow control and every log with a `^S` in it then
// loses everything after it. Without 0xFF it passes on a seven-bit line and mangles every
// UTF-8 log. Without CR/LF it passes on a cooked tty, which is the commonest case of all.
constexpr char kPattern[] = "\x0a\x0d\x11\x13\xff\x41";

} // namespace

ExecByteSafety::ExecByteSafety(QString path, RunCommand run, ReadRaw read)
    : m_path(std::move(path))
    , m_run(std::move(run))
    , m_read(std::move(read))
{
}

bool ExecByteSafety::runRawMode()
{
    QByteArray ignored;
    // Not believed on its own — see the class comment. This only ASKS; settle() is what
    // checks whether it took.
    return m_run(QStringLiteral("stty raw -echo 2>/dev/null; echo -n"), &ignored);
}

bool ExecByteSafety::encoderRoundTripAgrees(const QByteArray &raw)
{
    // THE STRONG TEST, AND IT IS OVER THE LOG'S OWN FIRST BYTES rather than a synthetic
    // pattern. Those are the bytes that actually have to arrive intact, they need no utility
    // on the far end to generate, and whatever the log happens to contain is a better
    // sample than anything this end could invent.
    QByteArray printed;
    if (!m_run(readCommand(m_path, 0, raw.size(), ByteSafety::Base64), &printed))
        return false;
    QByteArray decoded;
    if (!decodeBase64Payload(printed, &decoded))
        return false;
    return decoded == raw;
}

bool ExecByteSafety::generatedPatternSurvives()
{
    // THE WEAKER TEST, for a far end with no encoder to compare against. `printf` of octal
    // escapes is the one way to produce arbitrary bytes with nothing but a shell.
    //
    // AND ITS LIMIT IS STATED RATHER THAN HIDDEN: a NUL cannot be carried through a shell
    // argument at all, so this cannot prove a NUL survives. That matters exactly where it
    // would hurt most — a UTF-16 log is half NULs (invariant #8's own subject) — which is
    // why settle() refuses raw mode for a sample holding one when there is no encoder.
    QByteArray printed;
    if (!m_run(QStringLiteral("printf '\\012\\015\\021\\023\\377\\101'"), &printed))
        return false;
    return printed == QByteArray(kPattern, sizeof(kPattern) - 1);
}

std::optional<ByteSafety> ExecByteSafety::settle(QString *refusal)
{
    // CLEARED AT THE TOP OF EVERY CALL, ExecSizeProbe's rule: this re-settles on every
    // login, so a stale answer would describe the previous session's line discipline.
    m_hasEncoder = false;
    m_verifiedAgainstEncoder = false;

    QByteArray probed;
    if (m_run(base64ProbeCommand(), &probed))
        m_hasEncoder = lastNonEmptyLine(probed) == QByteArrayLiteral("bG9mdGFpbA==");

    runRawMode();

    const QByteArray raw = m_read(0, kSampleBytes);

    if (m_hasEncoder) {
        // An EMPTY log is not evidence either way — there are no bytes to mangle — so the
        // comparison is skipped and raw is taken, exactly as ExecSizeProbe skips its own
        // proof read for a zero size. A log that is empty now will be read again when it
        // grows, and this re-settles per login anyway.
        if (raw.isEmpty() || encoderRoundTripAgrees(raw)) {
            m_verifiedAgainstEncoder = !raw.isEmpty();
            return ByteSafety::Raw;
        }
        // Raw does not survive, and there is an encoder. 33% of the line's throughput is
        // what correctness costs here, and a serial line is slow enough that saying so
        // matters — which is why Raw is preferred rather than Base64 used unconditionally.
        return ByteSafety::Base64;
    }

    // No encoder, so nothing to compare against and nothing to fall back to.
    if (raw.contains('\0')) {
        // THE ONE CASE THAT IS REFUSED RATHER THAN GUESSED AT. The generated pattern cannot
        // carry a NUL, so it cannot prove one survives — and a log full of NULs is a UTF-16
        // log, where getting this wrong makes the whole file unreadable rather than odd.
        if (refusal) {
            *refusal = Tr::tr("%1 is not plain text, and the far end has no `base64` to send "
                              "it through — so loftail cannot tell whether its bytes would "
                              "arrive intact. Installing `base64` there fixes this.")
                           .arg(m_path);
        }
        return std::nullopt;
    }
    if (generatedPatternSurvives())
        return ByteSafety::Raw;

    if (refusal) {
        *refusal = Tr::tr("The line to %1 changes the bytes that pass over it, and the far "
                          "end has neither a working `stty raw` nor a `base64` to work "
                          "around it. Installing either fixes this.")
                       .arg(m_path);
    }
    return std::nullopt;
}

} // namespace loftail
