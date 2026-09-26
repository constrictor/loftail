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

#include "SshExecCommands.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <optional>

namespace loftail {

// Whether a log's bytes survive the way back, and what to do when they do not
// (ARCHITECTURE.md §6.11). `ExecSizeProbe`'s twin, in the same shape and for the same
// reason.
//
// AN SSH EXEC CHANNEL NEVER HAD TO ASK. A LOGIN CONSOLE CANNOT AVOID IT. A tty runs what
// passes over it through a terminal line discipline: `\n` becomes `\r\n`, `^S` and `^Q` are
// eaten as flow control, and depending on termios the eighth bit is stripped. So a log read
// through a console arrives quietly corrupted — not refused, not short, just wrong — which
// is the worst shape a failure can take on a path whose whole job is carrying somebody
// else's bytes faithfully.
//
// SETTLED BEHAVIOURALLY, NEVER DECLARED. `stty raw -echo` exits 0 on a busybox that accepts
// `raw` and does nothing with it, so the only evidence worth having is a round trip: send
// bytes that the line discipline WOULD mangle and require them back unchanged. That is
// ExecSizeProbe's own rule — a size is believed only after a read at that offset delivers a
// byte — applied one question over.
//
// ALWAYS COMPILED, sixth time on the same argument: this is a decision, and a decision
// reachable in one build configuration is a decision tested in one build configuration.
class ExecByteSafety
{
public:
    using RunCommand = std::function<bool(const QString &command, QByteArray *out)>;

    // Bytes actually delivered for a read at `offset`, and the bytes themselves — unlike
    // ExecSizeProbe's seam, which wants only the count. Comparing the two encodings' answers
    // is the whole test, so the content is the point.
    using ReadRaw = std::function<QByteArray(qint64 offset, qint64 length)>;

    // Enough bytes for the comparison to mean something and few enough to cost nothing on a
    // line that moves ~11 KB/s: one round trip either way.
    static constexpr qint64 kSampleBytes = 4096;

    ExecByteSafety(QString path, RunCommand run, ReadRaw read);

    // Raw where raw survives, Base64 where it does not, nullopt where neither can be had —
    // in which case `refusal` names what is missing. A refusal that keeps its tab and says
    // why (M17), because a log read through a mangling line is worse than one not read.
    std::optional<ByteSafety> settle(QString *refusal);

    // Whether the far end has a base64 encoder, asked once during settle() and worth reading
    // afterwards for the diagnostic line.
    bool hasEncoder() const { return m_hasEncoder; }

    // After a settle() that answered Raw: whether the round trip was verified against the
    // ENCODER (the strong test) or against a generated pattern (the weaker one, where there
    // is no encoder to compare with). The difference is stated rather than hidden, because
    // the weaker test cannot see a NUL — see the .cpp.
    bool verifiedAgainstEncoder() const { return m_verifiedAgainstEncoder; }

private:
    bool runRawMode();
    bool encoderRoundTripAgrees(const QByteArray &raw);
    bool generatedPatternSurvives();

    QString    m_path;
    RunCommand m_run;
    ReadRaw    m_read;
    bool       m_hasEncoder = false;
    bool       m_verifiedAgainstEncoder = false;
};

} // namespace loftail
