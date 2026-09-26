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

// Turning one command on a serial console into one answer (ARCHITECTURE.md §6.11).
//
// An exec channel gives a private stdout, a separate stderr and an exit status; a console
// gives one stream carrying loftail's own commands echoed back, the answer, the prompt, and
// whatever else the device printed meanwhile. Every case here is a shape that stream really
// takes, and two of them are defects that would be silent in production.

#include <QtTest>

#include "SerialFraming.h"

using namespace loftail;

namespace {

SerialFrame frameFor(qint64 expected = -1)
{
    SerialFrame f;
    f.token = QByteArrayLiteral("7f3a9c01");
    f.expected = expected;
    return f;
}

QByteArray begin(const SerialFrame &f) { return QByteArrayLiteral("LFB-") + f.token; }
QByteArray end(const SerialFrame &f) { return QByteArrayLiteral("LFE-") + f.token; }

} // namespace

class TestSerialFraming : public QObject
{
    Q_OBJECT

private slots:
    void theCommandIsNotQuotedAndItsStderrIsDiscarded();
    void aRestartScriptKeepsItsStderrByAskingForIt();
    void anAnswerIsWhatLiesBetweenTheMarkers();
    void theEchoOfTheCommandDoesNotOpenTheFrame();
    void noiseBeforeAndAfterTheFrameIsNotTheAnswer();
    void aPayloadHoldingTheTokenIsNotTruncatedByIt();
    void aFrameIsIncompleteUntilItsEndMarkerHasAWholeLine();
    void aShortPayloadIsCompleteAndSaysItWasShort();
    void aStatusThatDoesNotParseIsCorrupt();
    void theCapNeverDropsAFrameThatHasAlreadyStarted();
};

void TestSerialFraming::theCommandIsNotQuotedAndItsStderrIsDiscarded()
{
    // THE VALUES ARE QUOTED AND THE COMMAND IS NOT, and getting that backwards fails in both
    // directions silently: quote the command and the whole facility is a no-op, interpolate a
    // value bare and a file name is remote code execution. Quoting is shellQuote()'s job one
    // layer up; here the command is shell source, exactly as a restart script is.
    const QByteArray framed = framedCommand(QStringLiteral("stat -c '%s %Y' /var/log/app.log"),
                                            frameFor());
    QVERIFY(framed.contains(QByteArrayLiteral("stat -c '%s %Y' /var/log/app.log")));
    QVERIFY(!framed.contains(QByteArrayLiteral("'stat -c")));
    // runCommand()'s own rule one transport over: a far end's complaint is not the answer we
    // asked for, and on ONE stream there is no other way to keep the two apart.
    QVERIFY(framed.contains(QByteArrayLiteral("2>/dev/null")));
    // And it ends in a newline, or the far end never runs it.
    QVERIFY(framed.endsWith('\n'));
}

void TestSerialFraming::aRestartScriptKeepsItsStderrByAskingForIt()
{
    // The one command whose stderr must NOT be discarded: over a console stdout and stderr
    // cannot be told apart, so a restart script's clean-run rule falls back to its exit status
    // alone (SPEC.md §4) — and the output it shows has to be all of what the script said.
    const QByteArray framed = framedCommand(QStringLiteral("systemctl restart myapp"),
                                            frameFor(), /*keepStdErr=*/true);
    QVERIFY(framed.contains(QByteArrayLiteral("2>&1")));
    QVERIFY(!framed.contains(QByteArrayLiteral("2>/dev/null")));
}

void TestSerialFraming::anAnswerIsWhatLiesBetweenTheMarkers()
{
    const SerialFrame f = frameFor();
    const QByteArray stream = begin(f) + "\n1024 1767225600\n" + end(f) + " 0\n";
    const FramedAnswer a = readFramedAnswer(stream, f);
    QCOMPARE(a.state, FramedAnswer::State::Complete);
    QCOMPARE(a.output, QByteArrayLiteral("1024 1767225600\n"));
    QCOMPARE(a.exitCode, 0);
    QCOMPARE(a.consumed, stream.size());

    // A non-zero status rides the end marker, which is the whole reason it is there.
    const QByteArray failed = begin(f) + "\n" + end(f) + " 127\n";
    const FramedAnswer b = readFramedAnswer(failed, f);
    QCOMPARE(b.state, FramedAnswer::State::Complete);
    QCOMPARE(b.exitCode, 127);
    QVERIFY(b.output.isEmpty());
}

void TestSerialFraming::theEchoOfTheCommandDoesNotOpenTheFrame()
{
    // A DEFECT THAT WOULD BE SILENT, AND ONLY ON A DEVICE WITH ECHO ON. Before `stty -echo`
    // has taken — and on a console where it never will — the command loftail just typed comes
    // back first, and it CONTAINS the begin token. A substring match would start the payload
    // in the middle of that echoed command line and lose the first read of every command.
    const SerialFrame f = frameFor();
    const QByteArray echoed = QByteArray("printf '%s\\n' ") + begin(f)
        + "; stat -c '%s %Y' /a.log; printf '%s %d\\n' " + end(f) + " \"$?\"\n";
    const QByteArray stream = echoed + begin(f) + "\n1024 1767225600\n" + end(f) + " 0\n";

    const FramedAnswer a = readFramedAnswer(stream, f);
    QCOMPARE(a.state, FramedAnswer::State::Complete);
    // The answer, and not a slice of the echoed command line.
    QCOMPARE(a.output, QByteArrayLiteral("1024 1767225600\n"));
    QVERIFY(!a.output.contains(QByteArrayLiteral("printf")));
}

void TestSerialFraming::noiseBeforeAndAfterTheFrameIsNotTheAnswer()
{
    // Requirement 7: a device runs other things and they print to the same console. Anything
    // outside the markers is discarded, which is the whole of the tolerance.
    const SerialFrame f = frameFor();
    const QByteArray stream = QByteArrayLiteral("[   12.345678] usb 1-1: new device\r\n")
        + begin(f) + "\n" + "42\n" + end(f) + " 0\n"
        + QByteArrayLiteral("[   12.999999] eth0: link up\r\n");
    const FramedAnswer a = readFramedAnswer(stream, f);
    QCOMPARE(a.state, FramedAnswer::State::Complete);
    QCOMPARE(a.output, QByteArrayLiteral("42\n"));
    // And what follows the frame is LEFT, so the caller keeps it rather than losing a line
    // the device printed between two commands.
    QVERIFY(a.consumed < stream.size());
    QCOMPARE(stream.mid(a.consumed), QByteArrayLiteral("[   12.999999] eth0: link up\r\n"));
}

void TestSerialFraming::aPayloadHoldingTheTokenIsNotTruncatedByIt()
{
    // THE MOST IMPORTANT CASE IN THIS FILE. A log is arbitrary bytes, so it may contain this
    // run's token — and a payload found by SCANNING for the end marker would stop there,
    // truncating the log at whatever point it happens to mention it. Silently, and on exactly
    // the logs nobody thinks to test. `readCommand()` carries `| head -c L`, so the byte count
    // is known before the run starts, and that is what bounds the payload instead.
    const SerialFrame f0 = frameFor();
    QByteArray payload = QByteArrayLiteral("line one\n");
    payload += end(f0) + " 0\n";                // the log genuinely contains an end marker
    payload += QByteArrayLiteral("line three\n");

    SerialFrame f = frameFor(payload.size());
    const QByteArray stream = begin(f) + "\n" + payload + end(f) + " 0\n";

    const FramedAnswer a = readFramedAnswer(stream, f);
    QCOMPARE(a.state, FramedAnswer::State::Complete);
    QCOMPARE(a.output, payload);                // ALL of it, marker-lookalike and all
    QVERIFY(a.output.contains(QByteArrayLiteral("line three")));
    QCOMPARE(a.exitCode, 0);
}

void TestSerialFraming::aFrameIsIncompleteUntilItsEndMarkerHasAWholeLine()
{
    // A console delivers in small reads, so a partial marker is the ordinary state rather
    // than a corner. Accepting one would read the exit status off half a number.
    const SerialFrame f = frameFor();
    QVERIFY(readFramedAnswer(QByteArray(), f).state == FramedAnswer::State::Incomplete);
    QVERIFY(readFramedAnswer(begin(f), f).state == FramedAnswer::State::Incomplete);
    QVERIFY(readFramedAnswer(begin(f) + "\n42\n", f).state == FramedAnswer::State::Incomplete);
    // The end marker arrived but its line has not finished, so the status may still be
    // half-written: `1` of `127` would read as exit 1.
    QVERIFY(readFramedAnswer(begin(f) + "\n42\n" + end(f) + " 12", f).state
            == FramedAnswer::State::Incomplete);
    QVERIFY(readFramedAnswer(begin(f) + "\n42\n" + end(f) + " 127\n", f).state
            == FramedAnswer::State::Complete);
    QCOMPARE(readFramedAnswer(begin(f) + "\n42\n" + end(f) + " 127\n", f).exitCode, 127);
}

void TestSerialFraming::aShortPayloadIsCompleteAndSaysItWasShort()
{
    // A SHORT PAYLOAD IS THE ORDINARY END OF A FILE, and calling it corruption was a design
    // mistake the pty test caught: `head -c L` gives fewer bytes whenever the file holds fewer,
    // and the byte-safety probe does exactly that ON PURPOSE — it asks for a 4 KB sample of a
    // log that may be a hundred bytes long. So the frame is complete, the flag says it came
    // back short, and the CALLER decides what that means: a fetch that bounded its request by a
    // size it had just observed learns the file shrank underneath it, and a probe learns the
    // log is small.
    SerialFrame f = frameFor(100);
    const QByteArray payload = QByteArray(40, 'x') + "\n";
    const QByteArray stream = begin(f) + "\n" + payload + end(f) + " 0\n";
    const FramedAnswer a = readFramedAnswer(stream, f);
    QCOMPARE(a.state, FramedAnswer::State::Complete);
    QVERIFY(a.shortPayload);
    QCOMPARE(a.output, payload);
    QCOMPARE(a.exitCode, 0);
    QCOMPARE(a.consumed, stream.size());

    // And a payload of exactly the bound is NOT flagged, or the flag says nothing.
    SerialFrame exact = frameFor(payload.size());
    const QByteArray full = begin(exact) + "\n" + payload + end(exact) + " 0\n";
    const FramedAnswer b = readFramedAnswer(full, exact);
    QCOMPARE(b.state, FramedAnswer::State::Complete);
    QVERIFY(!b.shortPayload);
}

void TestSerialFraming::aStatusThatDoesNotParseIsCorrupt()
{
    // The status is what makes the frame an exec channel rather than a stream. A marker whose
    // number arrived mangled is a frame that cannot be believed, so the output is handed back
    // but the state says not to trust it — the retry is the caller's choice, not this
    // function's, because for a metadata command the output may well be the whole answer.
    const SerialFrame f = frameFor();
    const QByteArray stream = begin(f) + "\n42\n" + end(f) + " ??\n";
    const FramedAnswer a = readFramedAnswer(stream, f);
    QCOMPARE(a.state, FramedAnswer::State::Corrupt);
    QCOMPARE(a.output, QByteArrayLiteral("42\n"));
    QCOMPARE(a.exitCode, -1);
}

void TestSerialFraming::theCapNeverDropsAFrameThatHasAlreadyStarted()
{
    // A BOOTING BOARD PRINTS MEGABYTES WHILE A COMMAND IS IN FLIGHT, which is the input the
    // cap exists for — and a cap that trimmed from the front regardless would cut the begin
    // marker off, leaving every later read Incomplete and timing the command out against a
    // device that answered it correctly.
    const SerialFrame f = frameFor();
    QByteArray buffer = QByteArray(kFrameBufferCap, 'n');   // pre-frame noise, at the cap
    buffer += begin(f);
    buffer += "\n";
    buffer += QByteArrayLiteral("the answer\n");
    capFrameBuffer(&buffer, f);

    QVERIFY2(buffer.startsWith(begin(f)), "the cap cut the frame's own begin marker off");
    // The noise before it went, which is the other half of what the cap is for.
    QVERIFY(buffer.size() < kFrameBufferCap);

    // With no frame started, the oldest noise is what goes — the answer is always newest.
    QByteArray noise = QByteArray(kFrameBufferCap + 4096, 'n');
    noise += QByteArrayLiteral("newest");
    capFrameBuffer(&noise, f);
    QCOMPARE(noise.size(), kFrameBufferCap);
    QVERIFY(noise.endsWith(QByteArrayLiteral("newest")));
}

QTEST_APPLESS_MAIN(TestSerialFraming)
#include "tst_serialframing.moc"
