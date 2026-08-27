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

#include <QAction>
#include <QApplication>

#include "MainWindow.h"
#include "Version.h"

using namespace loftail;

// Help ▸ About (SPEC.md §1 "Which build this is"): the only place a RUNNING loftail
// says which build it is. `--version` answers the same question, but an installed
// package launched from a desktop menu has no command line to ask it on — which was
// the whole gap.
//
// The text is tested rather than the dialog: aboutText() is public precisely so that
// this needs no modal window (the same split buildRecordMenu() has). What the dialog
// adds beyond it — plain-text format, selectable text — is Qt's behaviour, not
// loftail's.
class TestAbout : public QObject
{
    Q_OBJECT

private slots:
    void theMenuOffersAbout();
    void theMenuOffersAboutQt();
    void theTextNamesTheReleaseAndTheBuild();
    void theTextNamesTheLicence();
};

void TestAbout::theMenuOffersAbout()
{
    MainWindow w;
    auto *about = w.findChild<QAction *>(QStringLiteral("aboutAction"));
    QVERIFY(about);
    // Always available. A window that has opened nothing — because the log is on a
    // host that is not answering — is exactly when the build id is asked for.
    QVERIFY(about->isEnabled());
    // AboutRole is what puts it in the application menu on macOS instead of leaving a
    // Help menu that duplicates the platform's own.
    QCOMPARE(about->menuRole(), QAction::AboutRole);
}

// Qt is LGPL and loftail redistributes it in three of its four artifacts, so the
// shipped binary has to be able to name it. Qt's dialog is the answer; what is
// loftail's to get right is that the item exists and is reachable.
void TestAbout::theMenuOffersAboutQt()
{
    MainWindow w;
    auto *aboutQt = w.findChild<QAction *>(QStringLiteral("aboutQtAction"));
    QVERIFY(aboutQt);
    QVERIFY(aboutQt->isEnabled());
    // AboutQtRole, not AboutRole: on macOS the two land in the application menu
    // separately, and giving both the same role hides one of them.
    QCOMPARE(aboutQt->menuRole(), QAction::AboutQtRole);
}

// Both configurations at once, as tst_scaffold does for the version string itself:
// which one this binary is depends on how it was configured, and neither is the odd
// case — a local build carries no build id and a CI build does.
void TestAbout::theTextNamesTheReleaseAndTheBuild()
{
    const QString text = MainWindow::aboutText();

    QVERIFY(text.contains(QStringLiteral("loftail")));
    QVERIFY(text.contains(applicationVersion()));

    const QString build = applicationBuildId();
    if (build.isEmpty()) {
        // Says it is a local build rather than showing an empty field, which would
        // read as "loftail does not know" instead of "there is no CI run behind this".
        QVERIFY(!text.contains(QStringLiteral("Build: \n")));
        QVERIFY(!text.endsWith(QStringLiteral("Build: ")));
    } else {
        // Verbatim, so what is pasted into a bug report is what the workflow stamped
        // and what release.yml checks the promoted artifacts against.
        QVERIFY(text.contains(build));
    }
}

// loftail is GPL-3.0-or-later and this dialog is the only place a running copy says so.
// The identifier is asserted VERBATIM and deliberately: it is the SPDX spelling, which
// is what makes it greppable by a packager and by licence-scanning tooling, and it is
// the reason that string is not wrapped in tr() where the sentence beside it is.
void TestAbout::theTextNamesTheLicence()
{
    const QString text = MainWindow::aboutText();

    QVERIFY(text.contains(QStringLiteral("GPL-3.0-or-later")));
    // The disclaimer of warranty is half of what the licence obliges the program to
    // state; a version number on its own does not discharge it.
    QVERIFY(text.contains(QStringLiteral("NO WARRANTY")));
    // And it points at the copy that ships beside the binary, which is where the terms
    // actually are — the dialog quotes none of them.
    QVERIFY(text.contains(QStringLiteral("LICENSE")));
}

QTEST_MAIN(TestAbout)
#include "tst_about.moc"
