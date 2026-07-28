#include <QtTest>

#include <QSettings>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include "FormatCache.h"
#include "FormatSettings.h"
#include "ManualFormatProvider.h"

using namespace loftail;

// M3 — the format-provider seam and per-file cache. ManualFormatProvider is the
// only thing that turns a pattern string into a LogFormat (invariant #3, §9); the
// FormatCache remembers the choice per canonical file path (§4, §8). Core-only.
class TestFormatProvider : public QObject
{
    Q_OBJECT

private slots:
    void manualProviderCompilesGoodPattern();
    void manualProviderSurfacesCompileError();
    void cacheRoundTrip();
    void cacheReplacesEntry();
    void cacheMissReturnsNullopt();
    void cacheKeyedPerFileNoDirectoryFallback();
    void zoneChoiceStringRoundTrip();
    void cacheLegacyDisplayZoneKeyMigrates();
};

void TestFormatProvider::manualProviderCompilesGoodPattern()
{
    ManualFormatProvider provider(QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"));
    // The provider ignores the sample — the manual path needs no file content.
    auto result = provider.formatFor(QByteArrayView());
    QVERIFY2(bool(result), "a valid pattern must compile through the provider");

    const LogFormat &f = result.value();
    QVERIFY(f.dateGroup > 0);
    QVERIFY(f.threadGroup > 0);
    QVERIFY(f.prioGroup > 0);
    QVERIFY(f.loggerGroup > 0);
    QVERIFY(f.msgGroup > 0);
    QCOMPARE(provider.pattern(), QStringLiteral("%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"));
}

void TestFormatProvider::manualProviderSurfacesCompileError()
{
    ManualFormatProvider provider(QStringLiteral("%p %z %m")); // %z is unknown
    auto result = provider.formatFor(QByteArrayView());
    QVERIFY2(!result, "an invalid pattern must yield a CompileError, not throw");
    QCOMPARE(int(result.error().code), int(CompileError::Code::UnknownSpecifier));
    QCOMPARE(result.error().offset, 4); // points at the offending 'z'
}

void TestFormatProvider::cacheRoundTrip()
{
    QTemporaryFile logFile; // a real file so canonicalKey() resolves
    QVERIFY(logFile.open());
    QTemporaryDir settingsDir;
    QSettings store(settingsDir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);

    FormatSettings in;
    in.pattern = QStringLiteral("%d{%Y-%m-%d %H:%M:%S} %-5p %c - %m%n");
    in.encoding = Encoding::Utf16LE;
    in.sourceZone.kind = ZoneChoice::Kind::FixedOffset;
    in.sourceZone.offsetSeconds = 2 * 3600;
    in.timeDisplay = TimeDisplay::RunSeconds;

    FormatCache::save(store, logFile.fileName(), in);

    const auto out = FormatCache::load(store, logFile.fileName());
    QVERIFY(out.has_value());
    QCOMPARE(out->pattern, in.pattern);
    QCOMPARE(int(out->encoding), int(Encoding::Utf16LE));
    QCOMPARE(int(out->sourceZone.kind), int(ZoneChoice::Kind::FixedOffset));
    QCOMPARE(out->sourceZone.offsetSeconds, 2 * 3600);
    QCOMPARE(int(out->timeDisplay), int(TimeDisplay::RunSeconds));
    QCOMPARE(*out, in);
}

void TestFormatProvider::cacheLegacyDisplayZoneKeyMigrates()
{
    // A cache entry written before the timestamp header menu subsumed the display
    // zone stores a ZoneChoice string under "displayZone" and no "timeDisplay".
    // Reading it must preserve the user's choice, not reset it to the default.
    QTemporaryFile logFile;
    QVERIFY(logFile.open());
    QTemporaryDir settingsDir;
    const QString ini = settingsDir.filePath(QStringLiteral("s.ini"));
    const QString key = FormatCache::canonicalKey(logFile.fileName());
    {
        QSettings store(ini, QSettings::IniFormat);
        store.beginWriteArray(QStringLiteral("formatCache"), 1);
        store.setArrayIndex(0);
        store.setValue(QStringLiteral("path"), key);
        store.setValue(QStringLiteral("pattern"), QStringLiteral("%d %m%n"));
        store.setValue(QStringLiteral("displayZone"), QStringLiteral("local"));
        store.endArray();
        store.sync();
    }

    QSettings store(ini, QSettings::IniFormat);
    const auto out = FormatCache::load(store, logFile.fileName());
    QVERIFY(out.has_value());
    QCOMPARE(int(out->timeDisplay), int(TimeDisplay::LocalTime));
}

void TestFormatProvider::cacheReplacesEntry()
{
    QTemporaryFile logFile;
    QVERIFY(logFile.open());
    QTemporaryDir settingsDir;
    QSettings store(settingsDir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);

    FormatSettings a;
    a.pattern = QStringLiteral("%p %m%n");
    FormatCache::save(store, logFile.fileName(), a);

    FormatSettings b;
    b.pattern = QStringLiteral("%c %m%n");
    b.encoding = Encoding::System;
    FormatCache::save(store, logFile.fileName(), b);

    const auto out = FormatCache::load(store, logFile.fileName());
    QVERIFY(out.has_value());
    QCOMPARE(out->pattern, QStringLiteral("%c %m%n")); // the second save wins
    QCOMPARE(int(out->encoding), int(Encoding::System));
}

void TestFormatProvider::cacheMissReturnsNullopt()
{
    QTemporaryDir settingsDir;
    QSettings store(settingsDir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
    const auto out = FormatCache::load(store, QStringLiteral("/no/such/file.log"));
    QVERIFY(!out.has_value());
}

void TestFormatProvider::cacheKeyedPerFileNoDirectoryFallback()
{
    // Two files in the same directory must not share a format — per file only (§4).
    QTemporaryDir dir;
    QFile a(dir.filePath(QStringLiteral("a.log")));
    QFile b(dir.filePath(QStringLiteral("b.log")));
    QVERIFY(a.open(QIODevice::WriteOnly));
    QVERIFY(b.open(QIODevice::WriteOnly));
    a.close();
    b.close();

    QTemporaryDir settingsDir;
    QSettings store(settingsDir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);

    FormatSettings sa;
    sa.pattern = QStringLiteral("%p - a%n");
    FormatCache::save(store, a.fileName(), sa);

    // b was never configured: no fallback to a sibling's format.
    QVERIFY(!FormatCache::load(store, b.fileName()).has_value());
    QVERIFY(FormatCache::load(store, a.fileName()).has_value());
}

void TestFormatProvider::zoneChoiceStringRoundTrip()
{
    for (const ZoneChoice z : {
             ZoneChoice{ZoneChoice::Kind::Default, 0},
             ZoneChoice{ZoneChoice::Kind::Local, 0},
             ZoneChoice{ZoneChoice::Kind::Utc, 0},
             ZoneChoice{ZoneChoice::Kind::FixedOffset, -5 * 3600},
         }) {
        const ZoneChoice back = ZoneChoice::fromString(z.toString());
        QCOMPARE(back, z);
    }
}

QTEST_GUILESS_MAIN(TestFormatProvider)
#include "tst_formatprovider.moc"
