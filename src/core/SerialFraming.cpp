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

#include "SerialFraming.h"

namespace loftail {
namespace {

// The markers. Built from the run's token so that both ends of one frame carry it: an end
// marker from a previous run cannot close this one even if it is still sitting in the buffer.
QByteArray beginMarker(const QByteArray &token)
{
    return QByteArrayLiteral("LFB-") + token;
}

QByteArray endMarker(const QByteArray &token)
{
    return QByteArrayLiteral("LFE-") + token;
}

// The offset just past a line whose ENTIRE content is `marker`, or -1.
//
// Whole-line, for the reason the header gives at length: the echo of the command contains
// the marker, so a substring search finds the echo and starts the payload in the middle of
// the command line loftail just sent.
qsizetype endOfMarkerLine(const QByteArray &buffer, const QByteArray &marker, qsizetype from)
{
    qsizetype at = from;
    while (at < buffer.size()) {
        qsizetype lineEnd = buffer.indexOf('\n', at);
        const bool last = lineEnd < 0;
        if (last)
            lineEnd = buffer.size();
        // A console adds carriage returns of its own, and a shell may pad with spaces.
        const QByteArray line = buffer.mid(at, lineEnd - at).trimmed();
        // STARTS WITH, not equals, because the END marker's line carries the exit status
        // after it — and the whole-line rule this function exists for is about the LINE
        // rather than about equality: what it refuses is a marker found in the middle of
        // one, which is how the echo of the command opens the frame early.
        if (line.startsWith(marker)) {
            // Past the newline where there is one; a marker at the very end of the buffer
            // with no newline yet is NOT accepted, because more of it may still be coming.
            return last ? -1 : lineEnd + 1;
        }
        if (last)
            return -1;
        at = lineEnd + 1;
    }
    return -1;
}

} // namespace

QByteArray framedCommand(const QString &command, const SerialFrame &frame, bool keepStdErr)
{
    // `printf` rather than `echo`, whose handling of a leading `-` and of escapes differs
    // between shells — the same reason probeCommand() uses it.
    //
    // The command is NOT quoted and must not be: it is shell source, exactly as a restart
    // script is, and quoting it would make the whole feature a no-op. What IS quoted is
    // every value any command interpolates, which is shellQuote()'s job at the layer above.
    const QString redirect = keepStdErr ? QStringLiteral(" 2>&1") : QStringLiteral(" 2>/dev/null");
    return QByteArray("printf '%s\\n' ") + beginMarker(frame.token) + "; "
        + command.toUtf8() + redirect.toUtf8() + "; printf '%s %d\\n' "
        + endMarker(frame.token) + " \"$?\"\n";
}

FramedAnswer readFramedAnswer(const QByteArray &buffer, const SerialFrame &frame)
{
    FramedAnswer out;
    const QByteArray begin = beginMarker(frame.token);
    const QByteArray end = endMarker(frame.token);

    const qsizetype payloadStart = endOfMarkerLine(buffer, begin, 0);
    if (payloadStart < 0)
        return out;   // Incomplete: the run has not started answering yet.

    qsizetype payloadEnd = -1;
    qsizetype searchFrom = payloadStart;
    if (frame.expected >= 0) {
        // LENGTH-DELIMITED. The payload may be arbitrary bytes — a log — so it may contain
        // this very token, and scanning it would truncate the log at that point.
        if (buffer.size() - payloadStart < frame.expected) {
            // Fewer bytes than the command was bounded to. Two readings, and they need
            // telling apart: the rest may still be arriving, or the far end may have
            // FINISHED and sent less.
            //
            // A SHORT PAYLOAD IS THE ORDINARY END OF A FILE AND NOT AN ERROR: `head -c L`
            // gives fewer bytes whenever the file holds fewer, which is what the byte-safety
            // probe does on purpose — it asks for a 4 KB sample of a log that may be a
            // hundred bytes long. So if the end marker has ALREADY arrived, the frame is
            // complete and simply shorter than the bound; the flag says so and the caller
            // decides what that means for it.
            //
            // The scan below is over the payload region, which is the hazard the
            // length-delimiting rule exists against — but it is reached only once the
            // payload is already short, and what it produces is a short answer rather than
            // a log truncated in silence.
            if (endOfMarkerLine(buffer, end, payloadStart) < 0)
                return out;   // Incomplete: more is coming.
            // Short and finished. Leave payloadEnd unset so the marker-delimited path below
            // bounds it, and search from the start of the payload rather than from a length
            // that reaches past the end of the buffer.
            out.shortPayload = true;
        } else {
            payloadEnd = payloadStart + frame.expected;
            searchFrom = payloadEnd;
        }
    }

    // Find the end marker's line and read the status off it.
    qsizetype at = searchFrom;
    qsizetype statusLineStart = -1;
    qsizetype statusLineEnd = -1;
    while (at < buffer.size()) {
        qsizetype lineEnd = buffer.indexOf('\n', at);
        if (lineEnd < 0)
            break;   // a partial last line: more may be coming
        const QByteArray line = buffer.mid(at, lineEnd - at).trimmed();
        if (line.startsWith(end)) {
            statusLineStart = at;
            statusLineEnd = lineEnd + 1;
            break;
        }
        at = lineEnd + 1;
    }
    if (statusLineStart < 0)
        return out;   // Incomplete: the end marker has not arrived.

    if (payloadEnd < 0)
        payloadEnd = statusLineStart;

    if (payloadEnd > statusLineStart) {
        // The bound reached past the end marker, which cannot happen for a well-formed frame:
        // the short case is handled above and answers with what arrived, so this is a frame
        // whose own structure disagrees with itself.
        out.state = FramedAnswer::State::Corrupt;
        out.consumed = statusLineEnd;
        return out;
    }

    const QByteArray status = buffer.mid(statusLineStart, statusLineEnd - statusLineStart)
                                  .trimmed()
                                  .mid(end.size())
                                  .trimmed();
    bool ok = false;
    const int code = status.toInt(&ok);

    out.output = buffer.mid(payloadStart, payloadEnd - payloadStart);
    out.exitCode = ok ? code : -1;
    out.consumed = statusLineEnd;
    // A status that did not parse is a frame that arrived damaged. The OUTPUT is still handed
    // back — a caller may well have had its answer — but the state says not to trust it, so
    // a retry is the caller's choice rather than this function's.
    out.state = ok ? FramedAnswer::State::Complete : FramedAnswer::State::Corrupt;
    return out;
}

void capFrameBuffer(QByteArray *buffer, const SerialFrame &frame)
{
    if (!buffer || buffer->size() <= kFrameBufferCap)
        return;

    // TRIM ONLY WHAT IS BEFORE THE BEGIN MARKER, WHERE THERE IS ONE, or the cap destroys the
    // very frame it is protecting: dropping from the front of a buffer whose frame has
    // started cuts the marker off, readFramedAnswer() then answers Incomplete for ever, and
    // the command times out on a device that answered it perfectly. Reachable on a booting
    // board, which prints megabytes while a command is in flight — the case this exists for.
    const QByteArray begin = QByteArrayLiteral("LFB-") + frame.token;
    const qsizetype markerAt = buffer->indexOf(begin);
    if (markerAt > 0) {
        buffer->remove(0, markerAt);
        return;
    }
    if (markerAt == 0)
        return;   // nothing before it to drop; the payload's own size is the caller's bound

    // No frame in flight yet: the answer is always the newest, so the oldest noise goes.
    buffer->remove(0, buffer->size() - kFrameBufferCap);
}

} // namespace loftail
