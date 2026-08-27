#include <QtTest>

#include <QByteArray>
#include <QStringList>

#include "Decoder.h"
#include "DetectingFormatProvider.h"
#include "FormatDetector.h"
#include "FormatPreview.h"

using namespace loftail;

// M8 — format autodetection (ARCHITECTURE.md §9). The candidate library scored by
// match rate over the first ~200 records, the priority-anchored structural
// inference fallback, and the DetectingFormatProvider that wraps both behind the
// IFormatProvider seam. All core-only, no QApplication (QTEST_GUILESS_MAIN).
class TestFormatDetector : public QObject
{
    Q_OBJECT

private slots:
    void libraryPicksExactCandidate_millisThread();
    void libraryPicksExactCandidate_plainDate();
    void libraryPicksExactCandidate_timeOnly();
    void libraryPicksSlashDateWithNdc();
    void slashDateGeneralizesAcrossSeconds();
    void slashDateDayFirstIsInferred();
    void unknownDateShapeIsNotMemorized();
    void inferenceRecoversNdcAfterLogger();
    void thresholdRejectsGarbage();
    void inferenceRecoversNonLibraryLayout();
    void providerReturnsFormatForKnownPattern();
    void providerCleanNoDetectionForGarbage();
    void emptySampleIsNoDetection();
    void varLogMessagesIsDetected();
    void varLogMessagesWithoutPidsIsDetected();
    void rsyslogRfc3339IsDetected();

private:
    // Join lines with '\n' into a UTF-8 sample the Decoder resolves as UTF-8.
    static QByteArray makeSample(const QStringList &lines)
    {
        return lines.join(QLatin1Char('\n')).toUtf8() + '\n';
    }
    static QByteArrayView view(const QByteArray &b) { return QByteArrayView(b.constData(), b.size()); }

    // Match rate of a compiled format over a sample, for asserting a returned
    // format actually parses the file cleanly.
    static double matchRate(const LogFormat &f, const QByteArray &sample, const Decoder &dec)
    {
        const PreviewResult pv = FormatPreview::build(f, view(sample), dec, 200);
        return pv.totalCount > 0 ? double(pv.matchedCount) / pv.totalCount : 0.0;
    }
};

// /var/log/messages. Not a log4cplus log, but its layout IS a ConversionPattern,
// and it is the one every Linux box has — so it must come up parsed rather than as
// a wall of unrecognized text. The shape is rsyslog's traditional file format: a
// space-padded "%b %e" with NO year, the host, then "tag[pid]:".
void TestFormatDetector::varLogMessagesIsDetected()
{
    QStringList lines;
    for (int i = 0; i < 30; ++i) {
        lines << QStringLiteral("Aug  5 10:22:%1 web1 sshd[%2]: Accepted publickey for deploy")
                     .arg(i % 60, 2, 10, QLatin1Char('0'))
                     .arg(1000 + i);
    }
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY2(r.detected, "traditional syslog must be detected");
    QCOMPARE(matchRate(r.format, sample, dec), 1.0);
    QVERIFY(r.format.dateGroup > 0);
    // The pid variant is the richest one that fits, so it is the one that wins.
    QCOMPARE(r.pattern, QStringLiteral("%D{%b %e %H:%M:%S} %h %c[%i]: %m%n"));
    QCOMPARE(int(r.format.impliedZone), int(Qt::LocalTime)); // syslog stamps local time
}

// The kernel and a good many daemons write no pid, so the tag-only variant has to
// win there rather than the file falling back to plain text.
void TestFormatDetector::varLogMessagesWithoutPidsIsDetected()
{
    QStringList lines;
    for (int i = 0; i < 30; ++i) {
        lines << QStringLiteral("Dec 31 23:5%1:07 web1 kernel: eth0: link up")
                     .arg(i % 10);
    }
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY(r.detected);
    QCOMPARE(matchRate(r.format, sample, dec), 1.0);
    QCOMPARE(r.pattern, QStringLiteral("%D{%b %e %H:%M:%S} %h %c: %m%n"));
}

// What /var/log/syslog actually holds on Debian 12 and Ubuntu 24.04 and later:
// RSYSLOG_FileFormat, an RFC3339 stamp with SIX fractional digits and no separator
// before them, then a colon offset. The microseconds are read and dropped; the
// offset in the text is what fixes the instant.
void TestFormatDetector::rsyslogRfc3339IsDetected()
{
    QStringList lines;
    for (int i = 0; i < 30; ++i) {
        lines << QStringLiteral("2026-08-24T22:04:%1.341116+03:00 web1 systemd[1]: unit %2 started")
                     .arg(i % 60, 2, 10, QLatin1Char('0'))
                     .arg(i);
    }
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY2(r.detected, "rsyslog's default file format must be detected");
    QCOMPARE(matchRate(r.format, sample, dec), 1.0);
    QCOMPARE(r.pattern, QStringLiteral("%D{%Y-%m-%dT%H:%M:%S.%Q%z} %h %c[%i]: %m%n"));
    QVERIFY(r.format.impliedDateFormat.hasMillis);
}

void TestFormatDetector::libraryPicksExactCandidate_millisThread()
{
    QStringList lines;
    for (int i = 0; i < 40; ++i) {
        lines << QStringLiteral("2026-07-21 10:22:%1,123 [main] INFO  com.acme.Foo - message %2")
                     .arg(i % 60, 2, 10, QLatin1Char('0'))
                     .arg(i);
    }
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY2(r.detected, "a classic millis+thread log4cplus log must be detected");
    QCOMPARE(int(r.source), int(DetectionResult::Source::Library));
    QCOMPARE(r.pattern, QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"));
    QVERIFY(r.score > 0.99);
    // The returned LogFormat carries all the expected role groups.
    QVERIFY(r.format.dateGroup > 0);
    QVERIFY(r.format.threadGroup > 0);
    QVERIFY(r.format.prioGroup > 0);
    QVERIFY(r.format.loggerGroup > 0);
    QVERIFY(r.format.msgGroup > 0);
}

void TestFormatDetector::libraryPicksExactCandidate_plainDate()
{
    QStringList lines;
    for (int i = 0; i < 30; ++i)
        lines << QStringLiteral("2026-07-21 10:22:33 INFO com.acme.Bar - doing work %1").arg(i);
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY(r.detected);
    QCOMPARE(int(r.source), int(DetectionResult::Source::Library));
    QVERIFY(r.score > 0.99);
    QCOMPARE(matchRate(r.format, sample, dec), 1.0);
    // No thread bracket and no millis in the data → the winning candidate must be
    // the plain-date, no-thread one.
    QVERIFY(FormatDetector::candidateLibrary().contains(r.pattern));
    QVERIFY(!r.pattern.contains(QStringLiteral("[%t]")));
    QVERIFY(!r.pattern.contains(QStringLiteral(",%q")));
}

void TestFormatDetector::libraryPicksExactCandidate_timeOnly()
{
    QStringList lines;
    for (int i = 0; i < 25; ++i)
        lines << QStringLiteral("10:22:%1 WARN com.acme.Baz - tick %2")
                     .arg(i % 60, 2, 10, QLatin1Char('0'))
                     .arg(i);
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY(r.detected);
    QCOMPARE(r.pattern, QStringLiteral("%d{%H:%M:%S} %-5p %c - %m%n"));
    QCOMPARE(matchRate(r.format, sample, dec), 1.0);
}

// The shape log4cplus emits from its %D{%m/%d/%y %H:%M:%S} default, NDC and all —
// the layout of every real sample this was developed against.
void TestFormatDetector::libraryPicksSlashDateWithNdc()
{
    QStringList lines;
    for (int i = 0; i < 40; ++i) {
        lines << QStringLiteral("07/27/26 21:59:%1 DEBUG Vms::App [] - service %2 started")
                     .arg(i % 60, 2, 10, QLatin1Char('0'))
                     .arg(i);
    }
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY2(r.detected, "a stock log4cplus %D{%m/%d/%y} log must be detected");
    QCOMPARE(int(r.source), int(DetectionResult::Source::Library));
    QCOMPARE(r.pattern, QStringLiteral("%D{%m/%d/%y %H:%M:%S} %-5p %c [%x] - %m%n"));
    QCOMPARE(matchRate(r.format, sample, dec), 1.0);
    // %D is log4cplus's LOCAL-time specifier (ARCHITECTURE.md §5.1).
    QCOMPARE(int(r.format.impliedZone), int(Qt::LocalTime));
    // The NDC is a field of its own, not the head of the message.
    QVERIFY(r.pattern.contains(QStringLiteral("[%x]")));
}

// The regression this suite exists for: a slash date must compile to a %d{...}
// SPECIFIER, never to the sample's own digits as literal text. A memorized pattern
// scores a perfect 1.0 over a head-of-file sample whose records share one second,
// so match rate alone cannot catch it — assert the generalization directly, and
// assert it indexes every record rather than only that first second's worth.
void TestFormatDetector::slashDateGeneralizesAcrossSeconds()
{
    // The first 12 records share a second, exactly as a real startup burst does.
    QStringList lines;
    for (int i = 0; i < 12; ++i)
        lines << QStringLiteral("03/12/26 11:50:47 DEBUG Vms::App [] - startup step %1").arg(i);
    for (int i = 0; i < 48; ++i) {
        lines << QStringLiteral("03/12/26 11:5%1:%2 INFO  Vms::Media [] - frame %3")
                     .arg(1 + i / 10).arg(i % 60, 2, 10, QLatin1Char('0')).arg(i);
    }
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY(r.detected);
    QVERIFY2(!r.pattern.contains(QStringLiteral("11:50:47")),
             qPrintable(QStringLiteral("pattern memorized a sample timestamp: ") + r.pattern));
    QVERIFY(r.pattern.contains(QStringLiteral("%H:%M:%S")));
    // Every record parses — not just the twelve sharing the leading second.
    const PreviewResult pv = FormatPreview::build(r.format, view(sample), dec, 200);
    QCOMPARE(pv.totalCount, 60);
    QCOMPARE(pv.matchedCount, 60);
}

// 27/07/26 cannot be month-first, and nothing but the data says so.
void TestFormatDetector::slashDateDayFirstIsInferred()
{
    QStringList lines;
    for (int i = 0; i < 30; ++i) {
        lines << QStringLiteral("27/07/26 08:%1:00 WARN  Vms::Net [] - retry %2")
                     .arg(i % 60, 2, 10, QLatin1Char('0'))
                     .arg(i);
    }
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY(r.detected);
    QVERIFY2(r.pattern.contains(QStringLiteral("%d/%m/%y")),
             qPrintable(QStringLiteral("expected a day-first date in: ") + r.pattern));
    QCOMPARE(matchRate(r.format, sample, dec), 1.0);
    // Sanity: the same layout with an unambiguous month-first date stays month-first.
    QStringList us;
    for (int i = 0; i < 30; ++i) {
        us << QStringLiteral("07/27/26 08:%1:00 WARN  Vms::Net [] - retry %2")
                  .arg(i % 60, 2, 10, QLatin1Char('0'))
                  .arg(i);
    }
    const QByteArray usSample = makeSample(us);
    const Decoder usDec = Decoder::detect(view(usSample), Encoding::Auto);
    const DetectionResult usr = FormatDetector::detect(view(usSample), usDec);
    QVERIFY(usr.detected);
    QVERIFY(usr.pattern.contains(QStringLiteral("%m/%d/%y")));
}

// A date shape no rule recognizes must NOT be copied through as literal digits.
// Falling through to manual entry is the correct outcome; a confidently wrong
// pattern that indexes one second of the file is not.
void TestFormatDetector::unknownDateShapeIsNotMemorized()
{
    QStringList lines;
    for (int i = 0; i < 40; ++i)
        lines << QStringLiteral("2026.07.21_10:22:33 INFO Vms::App - message %1").arg(i);
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY2(!r.detected, qPrintable(QStringLiteral("memorized an unknown date shape as: ")
                                     + r.pattern));
    QVERIFY(r.pattern.isEmpty());
}

// A bracketed run between the logger and the message is an NDC, and inference must
// offer it — including for a layout the library does not carry.
void TestFormatDetector::inferenceRecoversNdcAfterLogger()
{
    QStringList lines;
    for (int i = 0; i < 30; ++i) {
        lines << QStringLiteral("2026-07-21 10:22:%1 WARN com.acme.Widget [ctx-%2] | event %3")
                     .arg(i % 60, 2, 10, QLatin1Char('0'))
                     .arg(i % 4)
                     .arg(i);
    }
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY(r.detected);
    QCOMPARE(int(r.source), int(DetectionResult::Source::Inference));
    QVERIFY2(r.pattern.contains(QStringLiteral("[%x]")),
             qPrintable(QStringLiteral("NDC not recovered from: ") + r.pattern));
    QCOMPARE(matchRate(r.format, sample, dec), 1.0);
}

void TestFormatDetector::thresholdRejectsGarbage()
{
    // Unstructured prose: no date shape, no priority vocabulary. The confidence
    // threshold must reject every candidate and every inferred pattern.
    QStringList lines;
    for (int i = 0; i < 50; ++i) {
        lines << QStringLiteral("the quick brown fox jumps over the lazy dog %1").arg(i)
              << QStringLiteral("lorem ipsum dolor sit amet consectetur %1").arg(i);
    }
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY2(!r.detected, "garbage must fall through to manual entry");
    QCOMPARE(int(r.source), int(DetectionResult::Source::None));
    QVERIFY(r.pattern.isEmpty());
}

void TestFormatDetector::inferenceRecoversNonLibraryLayout()
{
    // A priority-anchored layout with a '|' separator that is NOT in the candidate
    // library. Structural inference should recover a working pattern from it.
    QStringList lines;
    for (int i = 0; i < 30; ++i)
        lines << QStringLiteral("2026-07-21 10:22:33 WARN com.acme.Widget | event number %1").arg(i);
    const QByteArray sample = makeSample(lines);
    const Decoder dec = Decoder::detect(view(sample), Encoding::Auto);

    // Sanity: the exact pattern really is absent from the library.
    QVERIFY(!FormatDetector::candidateLibrary().contains(
        QStringLiteral("%d{%Y-%m-%d %H:%M:%S} %p %c | %m%n")));

    const DetectionResult r = FormatDetector::detect(view(sample), dec);
    QVERIFY2(r.detected, "the priority anchor must let inference recover the layout");
    QCOMPARE(int(r.source), int(DetectionResult::Source::Inference));
    // A best-effort but STRUCTURALLY complete guess: date, priority, logger, message.
    QVERIFY(r.format.dateGroup > 0);
    QVERIFY(r.format.prioGroup > 0);
    QVERIFY(r.format.loggerGroup > 0);
    QVERIFY(r.format.msgGroup > 0);
    // And it parses the whole sample.
    QCOMPARE(matchRate(r.format, sample, dec), 1.0);
    QVERIFY(r.pattern.contains(QLatin1Char('|')));
}

void TestFormatDetector::providerReturnsFormatForKnownPattern()
{
    QStringList lines;
    for (int i = 0; i < 20; ++i)
        lines << QStringLiteral("2026-07-21 10:22:33,%1 [worker] ERROR com.acme.Db - boom %2")
                     .arg(i % 1000, 3, 10, QLatin1Char('0'))
                     .arg(i);
    const QByteArray sample = makeSample(lines);

    DetectingFormatProvider provider(Encoding::Auto);
    auto result = provider.formatFor(view(sample));
    QVERIFY2(bool(result), "a known-pattern sample must yield a LogFormat");
    QVERIFY(provider.detected());
    QCOMPARE(provider.detectedPattern(),
             QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"));

    const LogFormat &f = result.value();
    QVERIFY(f.dateGroup > 0 && f.prioGroup > 0 && f.loggerGroup > 0 && f.threadGroup > 0
            && f.msgGroup > 0);
}

void TestFormatDetector::providerCleanNoDetectionForGarbage()
{
    QStringList lines;
    for (int i = 0; i < 30; ++i)
        lines << QStringLiteral("just some free-form notes without structure %1").arg(i);
    const QByteArray sample = makeSample(lines);

    DetectingFormatProvider provider(Encoding::Auto);
    auto result = provider.formatFor(view(sample));
    QVERIFY2(!result, "garbage must produce a CompileError, not a bogus format");
    QVERIFY(!provider.detected());
    QVERIFY(provider.detectedPattern().isEmpty());
}

void TestFormatDetector::emptySampleIsNoDetection()
{
    const QByteArray empty;
    const Decoder dec = Decoder::detect(view(empty), Encoding::Auto);
    const DetectionResult r = FormatDetector::detect(view(empty), dec);
    QVERIFY(!r.detected);
}

QTEST_GUILESS_MAIN(TestFormatDetector)
#include "tst_formatdetector.moc"
