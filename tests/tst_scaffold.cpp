#include <QtTest>

#include "Version.h"

// M0 smoke test: proves the CMake + Qt Test + CTest wiring works and that core
// logic is reachable without a QApplication. Real coverage begins in M1.
class TestScaffold : public QObject
{
    Q_OBJECT

private slots:
    void applicationVersionIsNonEmpty();
    void applicationIdentityIsSet();
};

void TestScaffold::applicationVersionIsNonEmpty()
{
    QVERIFY(!loftail::applicationVersion().isEmpty());
}

void TestScaffold::applicationIdentityIsSet()
{
    QCOMPARE(QString::fromLatin1(loftail::applicationName), QStringLiteral("loftail"));
    QCOMPARE(QString::fromLatin1(loftail::organizationName), QStringLiteral("loftail"));
}

QTEST_APPLESS_MAIN(TestScaffold)
#include "tst_scaffold.moc"
