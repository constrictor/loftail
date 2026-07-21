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
    void thresholdRejectsGarbage();
    void inferenceRecoversNonLibraryLayout();
    void providerReturnsFormatForKnownPattern();
    void providerCleanNoDetectionForGarbage();
    void emptySampleIsNoDetection();

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
