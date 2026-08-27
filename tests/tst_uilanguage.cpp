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

#include <QtTest>

#include <QApplication>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTranslator>

#include "UiLanguage.h"

using namespace loftail;

// A translator standing in for the one something else installs for the system locale —
// Qt itself in some builds, the platform theme plugin in others (KDE's does). What it
// is does not matter to the test; that it is already installed and answering before
// loftail gets a say is the entire situation being defended against.
class PretendSystemCatalogue final : public QTranslator
{
public:
    QString translate(const char *context, const char *sourceText, const char *disambiguation,
                      int n) const override
    {
        Q_UNUSED(context);
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (!sourceText || !*sourceText)
            return QString();
        return QStringLiteral("<translated:%1>").arg(QString::fromUtf8(sourceText));
    }
    bool isEmpty() const override { return false; }
};

// Which language the interface is in (UiLanguage.h).
//
// loftail ships no catalogue, so every one of its own strings is already in the source
// language whatever happens here. The thing that is NOT automatic, and the reason the
// file exists, is Qt's own strings: a QDialogButtonBox's Open and Cancel come out of
// qtbase_<lang>, loaded for the system locale by a layer loftail neither controls nor
// can portably unwind. On a Ukrainian desktop that produced a dialog with ten English
// labels and two Ukrainian buttons.
class TestUiLanguage : public QObject
{
    Q_OBJECT

private:
    QList<QTranslator *> m_installed;
    PretendSystemCatalogue m_system;

private slots:
    void cleanup()
    {
        for (QTranslator *t : m_installed)
            QCoreApplication::removeTranslator(t);
        m_installed.clear();
        QCoreApplication::removeTranslator(&m_system);
    }

    void withNoCatalogueTheSourceLanguageIsUsed();
    void qtsOwnStringsAreOverriddenNotMerelyDeferredTo();
    void standardDialogButtonsComeOutInTheSourceLanguage();
};

void TestUiLanguage::withNoCatalogueTheSourceLanguageIsUsed()
{
    m_installed = installUiLanguage(this);
    QCOMPARE(uiLanguage(), QStringLiteral("en"));
    QVERIFY2(!m_installed.isEmpty(), "the pin is a translator, and it has to be installed");

    // loftail's own contexts pass through unchanged, exactly as with no translator.
    QCOMPARE(QCoreApplication::translate("OpenRemoteDialog", "Open Remote Log"),
             QStringLiteral("Open Remote Log"));
}

void TestUiLanguage::qtsOwnStringsAreOverriddenNotMerelyDeferredTo()
{
    // The competing catalogue goes in FIRST, which is the real ordering: it is installed
    // during QApplication construction, long before main() reaches installUiLanguage().
    QCoreApplication::installTranslator(&m_system);
    QCOMPARE(QCoreApplication::translate("QPlatformTheme", "Open"),
             QStringLiteral("<translated:Open>"));

    m_installed = installUiLanguage(this);

    // installTranslator() prepends and translate() stops at the first non-null answer,
    // so the last one installed is consulted first. That ordering IS the mechanism —
    // nothing is removed, because a translator installed by a plugin cannot be reached.
    QCOMPARE(QCoreApplication::translate("QPlatformTheme", "Open"), QStringLiteral("Open"));
    QCOMPARE(QCoreApplication::translate("QPlatformTheme", "Cancel"), QStringLiteral("Cancel"));
    QCOMPARE(QCoreApplication::translate("QShortcut", "Ctrl"), QStringLiteral("Ctrl"));
}

// End to end through the widget that surfaced the bug: the buttons loftail never names.
void TestUiLanguage::standardDialogButtonsComeOutInTheSourceLanguage()
{
    QCoreApplication::installTranslator(&m_system);
    m_installed = installUiLanguage(this);

    QDialogButtonBox box(QDialogButtonBox::Open | QDialogButtonBox::Cancel);
    // The mnemonic is Qt's, not ours, so compare on the letters.
    QCOMPARE(box.button(QDialogButtonBox::Open)->text().remove(QLatin1Char('&')),
             QStringLiteral("Open"));
    QCOMPARE(box.button(QDialogButtonBox::Cancel)->text().remove(QLatin1Char('&')),
             QStringLiteral("Cancel"));
}

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestUiLanguage test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_uilanguage.moc"
