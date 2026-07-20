#include <QtTest>

#include "CompileError.h"
#include "LogFormat.h"
#include "PatternCompiler.h"

using namespace loftail;

// M1 coverage for PatternCompiler. Table-driven by design (PLAN.md M1,
// ARCHITECTURE.md §10): every specifier and modifier, %d{...} translation
// including a rejected code, malformed patterns, and patterns missing %p or %c.
class TestPatternCompiler : public QObject
{
    Q_OBJECT

private:
    // Compile, asserting success, and return the LogFormat. Fails the test if the
    // pattern did not compile.
    static LogFormat compileOk(const QString &pattern)
    {
        auto result = PatternCompiler::compile(pattern);
        if (!result) {
            qWarning() << "compile failed:" << result.error().message << "at" << result.error().offset;
            [&]() { QVERIFY2(false, "expected pattern to compile"); }();
        }
        return result ? result.value() : LogFormat{};
    }

private slots:
    // --- specifiers: compile a full pattern and check field extraction ---------
    void matchesFields_data();
    void matchesFields();

    // --- modifiers: padding, truncation, width+precision -----------------------
    void modifiers_data();
    void modifiers();

    // --- %d{...} strftime translation ------------------------------------------
    void dateTranslation_data();
    void dateTranslation();

    void defaultDateFormat();
    void impliedZone_data();
    void impliedZone();

    // --- malformed patterns -> structured error --------------------------------
    void errors_data();
    void errors();

    // --- missing %p / %c is a warning-not-error path ---------------------------
    void missingPriorityOrLogger_data();
    void missingPriorityOrLogger();

    // --- record-boundary detection ---------------------------------------------
    void recordStartRe();

    // --- literal %% and %n -----------------------------------------------------
    void literalPercentAndNewline();

    // --- field order, role groups, names ---------------------------------------
    void fieldOrderAndGroups();

    void emptyPattern();
};

// ---------------------------------------------------------------------------

void TestPatternCompiler::matchesFields_data()
{
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<QString>("line");
    QTest::addColumn<QString>("date");
    QTest::addColumn<QString>("thread");
    QTest::addColumn<QString>("priority");
    QTest::addColumn<QString>("logger");
    QTest::addColumn<QString>("message");

    QTest::newRow("full log4cplus line")
        << "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"
        << "2026-07-21 14:32:05,123 [main] INFO  net.socket - Connection opened"
        << "2026-07-21 14:32:05,123" << "main" << "INFO" << "net.socket" << "Connection opened";

    QTest::newRow("no thread, no padding")
        << "%d{%Y-%m-%d %H:%M:%S} %p %c - %m%n"
        << "2026-07-21 14:32:05 ERROR db.pool - pool exhausted"
        << "2026-07-21 14:32:05" << QString() << "ERROR" << "db.pool" << "pool exhausted";

    QTest::newRow("message with embedded punctuation")
        << "%p %c: %m%n"
        << "WARN app.core: retrying in 5s (attempt 2/3)"
        << QString() << QString() << "WARN" << "app.core" << "retrying in 5s (attempt 2/3)";

    QTest::newRow("empty message captures empty")
        << "%p %c - %m%n"
        << "INFO x.y - "
        << QString() << QString() << "INFO" << "x.y" << QString();
}

void TestPatternCompiler::matchesFields()
{
    QFETCH(QString, pattern);
    QFETCH(QString, line);
    QFETCH(QString, date);
    QFETCH(QString, thread);
    QFETCH(QString, priority);
    QFETCH(QString, logger);
    QFETCH(QString, message);

    const LogFormat fmt = compileOk(pattern);
    const QRegularExpressionMatch m = fmt.recordRe.match(line);
    QVERIFY2(m.hasMatch(), qPrintable("recordRe did not match: " + fmt.recordRe.pattern()));

    if (!date.isNull()) {
        QVERIFY(fmt.dateGroup > 0);
        QCOMPARE(m.captured(fmt.dateGroup), date);
    }
    if (!thread.isNull()) {
        QVERIFY(fmt.threadGroup > 0);
        QCOMPARE(m.captured(fmt.threadGroup), thread);
    } else {
        QCOMPARE(fmt.threadGroup, -1);
    }
    if (!priority.isNull()) {
        QVERIFY(fmt.prioGroup > 0);
        QCOMPARE(m.captured(fmt.prioGroup), priority);
    }
    if (!logger.isNull()) {
        QVERIFY(fmt.loggerGroup > 0);
        QCOMPARE(m.captured(fmt.loggerGroup), logger);
    }
    QVERIFY(fmt.msgGroup > 0);
    QCOMPARE(m.captured(fmt.msgGroup), message);
}

// ---------------------------------------------------------------------------

void TestPatternCompiler::modifiers_data()
{
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<QString>("line");
    QTest::addColumn<QString>("expectedPriority");
    QTest::addColumn<QString>("expectedLogger");
    QTest::addColumn<QString>("expectedMessage");

    // Left-justify + min width: "INFO" padded to "INFO " (trailing space).
    QTest::newRow("left pad %-5p")
        << "%-5p %c - %m%n" << "INFO  core - hi"
        << "INFO" << "core" << "hi";

    // Right-justify + min width, bracketed so there is no separator to absorb the
    // leading pad: "[ WARN]".
    QTest::newRow("right pad bracketed %5p")
        << "[%5p] %m%n" << "[ WARN] something"
        << "WARN" << QString() << "something";

    // Truncation modifier: the regex still matches a normal logger.
    QTest::newRow("truncate %.30c")
        << "%p %.30c - %m%n" << "DEBUG some.very.long.logger.name - x"
        << "DEBUG" << "some.very.long.logger.name" << "x";

    // Combined width + precision on the message.
    QTest::newRow("width+precision %20.30m")
        << "%p %c %20.30m%n" << "TRACE mod hello world"
        << "TRACE" << "mod" << "hello world";
}

void TestPatternCompiler::modifiers()
{
    QFETCH(QString, pattern);
    QFETCH(QString, line);
    QFETCH(QString, expectedPriority);
    QFETCH(QString, expectedLogger);
    QFETCH(QString, expectedMessage);

    const LogFormat fmt = compileOk(pattern);
    const QRegularExpressionMatch m = fmt.recordRe.match(line);
    QVERIFY2(m.hasMatch(), qPrintable("recordRe did not match: " + fmt.recordRe.pattern()));

    QCOMPARE(m.captured(fmt.prioGroup), expectedPriority);
    if (!expectedLogger.isNull())
        QCOMPARE(m.captured(fmt.loggerGroup), expectedLogger);
    QCOMPARE(m.captured(fmt.msgGroup), expectedMessage);
}

// ---------------------------------------------------------------------------

void TestPatternCompiler::dateTranslation_data()
{
    QTest::addColumn<QString>("inner");     // the strftime format inside %d{...}
    QTest::addColumn<QString>("sample");    // a date rendered by that format
    QTest::addColumn<QString>("qtFormat");  // expected translated Qt format

    QTest::newRow("iso")       << "%Y-%m-%d %H:%M:%S" << "2026-07-21 14:32:05" << "yyyy-MM-dd HH:mm:ss";
    QTest::newRow("iso ms")    << "%Y-%m-%d %H:%M:%S,%q" << "2026-07-21 14:32:05,123" << "yyyy-MM-dd HH:mm:ss,zzz";
    QTest::newRow("compact")   << "%Y%m%d" << "20260721" << "yyyyMMdd";
    QTest::newRow("time only") << "%H:%M:%S" << "14:32:05" << "HH:mm:ss";
    QTest::newRow("12h am/pm") << "%y/%m/%d %I:%M %p" << "26/07/21 02:32 PM" << "yy/MM/dd hh:mm AP";
    QTest::newRow("literal %") << "%H%%%M" << "14%32" << "HH%mm";
}

void TestPatternCompiler::dateTranslation()
{
    QFETCH(QString, inner);
    QFETCH(QString, sample);
    QFETCH(QString, qtFormat);

    const QString pattern = QStringLiteral("%d{") + inner + QStringLiteral("} %m%n");
    const LogFormat fmt = compileOk(pattern);

    QCOMPARE(fmt.impliedDateFormat.qtFormat, qtFormat);
    QCOMPARE(fmt.impliedDateFormat.strftime, inner);
    QVERIFY(fmt.impliedDateFormat.isValid);

    // The generated sub-regex must match text the format actually produces.
    const QString line = sample + QStringLiteral(" the message");
    const QRegularExpressionMatch m = fmt.recordRe.match(line);
    QVERIFY2(m.hasMatch(), qPrintable("date regex did not match sample: " + fmt.recordRe.pattern()));
    QCOMPARE(m.captured(fmt.dateGroup), sample);
    QCOMPARE(m.captured(fmt.msgGroup), QStringLiteral("the message"));
}

void TestPatternCompiler::defaultDateFormat()
{
    // Bare %d (no braces) falls back to the default format.
    const LogFormat fmt = compileOk(QStringLiteral("%d %m%n"));
    QCOMPARE(fmt.impliedDateFormat.qtFormat, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    QVERIFY(fmt.impliedDateFormat.isValid);

    const QRegularExpressionMatch m = fmt.recordRe.match(QStringLiteral("2026-07-21 14:32:05 hello"));
    QVERIFY(m.hasMatch());
    QCOMPARE(m.captured(fmt.dateGroup), QStringLiteral("2026-07-21 14:32:05"));
}

void TestPatternCompiler::impliedZone_data()
{
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<int>("zone"); // Qt::TimeSpec

    QTest::newRow("%d local")        << "%d{%H:%M:%S} %m%n" << int(Qt::LocalTime);
    QTest::newRow("%D utc")          << "%D{%H:%M:%S} %m%n" << int(Qt::UTC);
    QTest::newRow("%d bare local")   << "%d %m%n" << int(Qt::LocalTime);
    QTest::newRow("%D bare utc")     << "%D %m%n" << int(Qt::UTC);
}

void TestPatternCompiler::impliedZone()
{
    QFETCH(QString, pattern);
    QFETCH(int, zone);

    const LogFormat fmt = compileOk(pattern);
    QCOMPARE(int(fmt.impliedZone), zone);
    QVERIFY(fmt.dateGroup > 0);
}

// ---------------------------------------------------------------------------

void TestPatternCompiler::errors_data()
{
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<int>("code");    // CompileError::Code
    QTest::addColumn<int>("offset");

    QTest::newRow("empty")
        << "" << int(CompileError::Code::EmptyPattern) << 0;
    QTest::newRow("dangling percent at end")
        << "abc%" << int(CompileError::Code::DanglingPercent) << 3;
    QTest::newRow("modifier with no specifier")
        << "%-5" << int(CompileError::Code::DanglingPercent) << 0;
    QTest::newRow("unknown specifier")
        << "%p %z %m" << int(CompileError::Code::UnknownSpecifier) << 4;
    QTest::newRow("unknown specifier after modifier")
        << "%-5z" << int(CompileError::Code::UnknownSpecifier) << 3;
    QTest::newRow("unterminated date brace")
        << "%d{%Y-%m-%d" << int(CompileError::Code::UnterminatedDateBrace) << 2;
    QTest::newRow("unsupported date code %j")
        << "%d{%Y-%j} %m" << int(CompileError::Code::UnsupportedDateCode) << 6;
    QTest::newRow("unsupported date code %a")
        << "%d{%a %H:%M} %m" << int(CompileError::Code::UnsupportedDateCode) << 3;
    QTest::newRow("dangling percent in date")
        << "%d{%H:%M:%} %m" << int(CompileError::Code::DanglingPercentInDate) << 9;
}

void TestPatternCompiler::errors()
{
    QFETCH(QString, pattern);
    QFETCH(int, code);
    QFETCH(int, offset);

    auto result = PatternCompiler::compile(pattern);
    QVERIFY2(!result, "expected a compile error");
    QCOMPARE(int(result.error().code), code);
    QCOMPARE(result.error().offset, offset);
    QVERIFY(!result.error().message.isEmpty());
}

// ---------------------------------------------------------------------------

void TestPatternCompiler::missingPriorityOrLogger_data()
{
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<bool>("hasPriority");
    QTest::addColumn<bool>("hasLogger");

    QTest::newRow("missing priority")
        << "%d{%H:%M:%S} %c - %m%n" << false << true;
    QTest::newRow("missing logger")
        << "%d{%H:%M:%S} %p - %m%n" << true << false;
    QTest::newRow("missing both")
        << "%d{%H:%M:%S} %m%n" << false << false;
    QTest::newRow("message only")
        << "%m%n" << false << false;
    QTest::newRow("has both")
        << "%p %c %m%n" << true << true;
}

void TestPatternCompiler::missingPriorityOrLogger()
{
    QFETCH(QString, pattern);
    QFETCH(bool, hasPriority);
    QFETCH(bool, hasLogger);

    // Missing %p or %c must still compile — the UI warns, it does not fail (SPEC §4).
    const LogFormat fmt = compileOk(pattern);
    QCOMPARE(fmt.prioGroup > 0, hasPriority);
    QCOMPARE(fmt.loggerGroup > 0, hasLogger);
    QVERIFY(fmt.msgGroup > 0);
}

// ---------------------------------------------------------------------------

void TestPatternCompiler::recordStartRe()
{
    const LogFormat fmt = compileOk(QStringLiteral("%d{%Y-%m-%d %H:%M:%S} %p %c - %m%n"));

    // A record's first line matches the start prefix...
    QVERIFY(fmt.recordStartRe.match(
        QStringLiteral("2026-07-21 14:32:05 INFO net.x - hello")).hasMatch());

    // ...a continuation line (an embedded newline in the message) does not.
    QVERIFY(!fmt.recordStartRe.match(
        QStringLiteral("\tat com.example.Foo.bar(Foo.java:10)")).hasMatch());
    QVERIFY(!fmt.recordStartRe.match(
        QStringLiteral("plain continuation text")).hasMatch());

    // The start prefix stops before the message: it must not capture it.
    const QString startPattern = fmt.recordStartRe.pattern();
    QVERIFY(!startPattern.contains(QStringLiteral("(.*)")));
    QVERIFY(startPattern.startsWith(QLatin1Char('^')));
    QVERIFY(!startPattern.endsWith(QLatin1Char('$')));
}

// ---------------------------------------------------------------------------

void TestPatternCompiler::literalPercentAndNewline()
{
    const LogFormat fmt = compileOk(QStringLiteral("%p (%%) %m%n"));
    const QRegularExpressionMatch m = fmt.recordRe.match(QStringLiteral("INFO (%) done"));
    QVERIFY2(m.hasMatch(), qPrintable(fmt.recordRe.pattern()));
    QCOMPARE(m.captured(fmt.prioGroup), QStringLiteral("INFO"));
    QCOMPARE(m.captured(fmt.msgGroup), QStringLiteral("done"));

    // %n emits no field and no capture group.
    QCOMPARE(fmt.fields.size(), 2); // priority, message
}

// ---------------------------------------------------------------------------

void TestPatternCompiler::fieldOrderAndGroups()
{
    const LogFormat fmt = compileOk(QStringLiteral("%d{%H:%M:%S} %p %c %F:%L %M - %m%n"));

    QCOMPARE(fmt.fields.size(), 7);

    const QVector<FieldRole> expectedRoles = {
        FieldRole::Date, FieldRole::Priority, FieldRole::Logger,
        FieldRole::FileName, FieldRole::LineNumber, FieldRole::Method, FieldRole::Message
    };
    for (int i = 0; i < fmt.fields.size(); ++i) {
        QCOMPARE(int(fmt.fields[i].role), int(expectedRoles[i]));
        QCOMPARE(fmt.fields[i].group, i + 1); // groups are 1-based in field order
    }

    // Role indices point at the right groups.
    QCOMPARE(fmt.dateGroup, 1);
    QCOMPARE(fmt.prioGroup, 2);
    QCOMPARE(fmt.loggerGroup, 3);
    QCOMPARE(fmt.msgGroup, 7);
    QCOMPARE(fmt.threadGroup, -1);

    // Column names.
    QCOMPARE(fmt.fields[0].name, QStringLiteral("Time"));
    QCOMPARE(fmt.fields[1].name, QStringLiteral("Priority"));
    QCOMPARE(fmt.fields[2].name, QStringLiteral("Subsystem"));

    // And it actually parses a line laid out that way.
    const QRegularExpressionMatch m = fmt.recordRe.match(
        QStringLiteral("14:32:05 INFO db.pool Conn.cpp:42 doConnect - opened"));
    QVERIFY2(m.hasMatch(), qPrintable(fmt.recordRe.pattern()));
    QCOMPARE(m.captured(1), QStringLiteral("14:32:05"));
    QCOMPARE(m.captured(fmt.prioGroup), QStringLiteral("INFO"));
    QCOMPARE(m.captured(fmt.loggerGroup), QStringLiteral("db.pool"));
    QCOMPARE(m.captured(4), QStringLiteral("Conn.cpp")); // %F
    QCOMPARE(m.captured(5), QStringLiteral("42"));       // %L
    QCOMPARE(m.captured(6), QStringLiteral("doConnect")); // %M
    QCOMPARE(m.captured(fmt.msgGroup), QStringLiteral("opened"));
}

void TestPatternCompiler::emptyPattern()
{
    auto result = PatternCompiler::compile(QString());
    QVERIFY(!result);
    QCOMPARE(int(result.error().code), int(CompileError::Code::EmptyPattern));
}

QTEST_APPLESS_MAIN(TestPatternCompiler)
#include "tst_patterncompiler.moc"
