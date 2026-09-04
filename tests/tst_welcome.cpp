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
#include <QDir>
#include <QFontDatabase>
#include <QImage>
#include <QPalette>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTemporaryDir>

#include "ConfigReset.h"
#include "HostBookmarkStore.h"
#include "MainWindow.h"
#include "TabLabels.h"
#include "WelcomeView.h"

using namespace loftail;

// The welcome screen (SPEC.md §3): what a window with no log open shows.
//
// It replaced one centred sentence naming two menus, so what is under test is that the
// two lists the menus keep are ON THE PAGE, named the way the menus name them, and that
// activating a row does what the menu entry does. Everything is found by object name; no
// case reads a visible label to decide which widget it has, which is the same contract
// tst_recentfiles works under and for the same reason.
class TestWelcome : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString       m_good; // parses with the built-in defaults, so an open never prompts

    static bool write(const QString &path, const QByteArray &bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(bytes);
        return true;
    }

    static void seedRecent(const QStringList &paths)
    {
        QSettings settings;
        settings.setValue(QStringLiteral("recentFiles"), paths);
        settings.sync();
    }

    static WelcomeView  *welcome(MainWindow &w) { return w.findChild<WelcomeView *>(QStringLiteral("welcomeView")); }
    static QListWidget  *recentList(MainWindow &w) { return w.findChild<QListWidget *>(QStringLiteral("welcomeRecentList")); }
    static QListWidget  *remoteList(MainWindow &w) { return w.findChild<QListWidget *>(QStringLiteral("welcomeRemoteList")); }
    static QLabel       *recentEmpty(MainWindow &w) { return w.findChild<QLabel *>(QStringLiteral("welcomeRecentEmpty")); }
    static QLabel       *remoteEmpty(MainWindow &w) { return w.findChild<QLabel *>(QStringLiteral("welcomeRemoteEmpty")); }
    static QLabel       *message(MainWindow &w) { return w.findChild<QLabel *>(QStringLiteral("welcomeMessage")); }
    static QWidget      *remoteSection(MainWindow &w) { return w.findChild<QWidget *>(QStringLiteral("welcomeRemoteSection")); }
    static QTabWidget   *tabs(MainWindow &w) { return w.findChild<QTabWidget *>(QStringLiteral("documentTabs")); }

    // Which page of the centre is up. The stack has no object name of its own, and there
    // is exactly one under the window.
    static QWidget *centrePage(MainWindow &w)
    {
        auto *stack = w.findChild<QStackedWidget *>();
        return stack ? stack->currentWidget() : nullptr;
    }

    static QStringList rowLabels(QListWidget *list)
    {
        QStringList out;
        for (int i = 0; i < list->count(); ++i)
            out.append(list->item(i)->text());
        return out;
    }

private slots:
    void initTestCase();
    void init();
    void anEmptyWindowShowsTheWelcomeScreenAndALogReplacesIt();
    void everyRecentLogIsARowNamedLikeItsMenuEntry();
    void anAmpersandInANameIsNotDoubledOnAListRow();
    void everyRowCarriesItsWholeAddressAsATooltip();
    void activatingARecentRowOpensThatLog();
    void returnOnASelectedRowOpensItToo();
    void anEmptyListSaysWhichKindOfEmptyWithoutAddingARow();
    void clearingTheRecentListEmptiesThePageAsWellAsTheMenu();
    void theRecentListFollowsTheMenuAfterAnOpen();
    void openingALogClearsTheCouldNotBeReopenedMessage();
    void theContentIsCentredRatherThanFillingTheWindow();
    void aThemeThatSuppliesNoBandStillGetsOne();
    void eachActionButtonSitsLevelWithItsListRatherThanItsHeading();
    void theNameWearsTheApplicationMarkAndTheTwoAreCentredTogether();
#if defined(LOFTAIL_HAVE_SSH)
    void aRememberedRemoteLogIsARowAndASavedHostWithNoneIsAnother();
#else
    void theRemotesSectionIsAbsentWithoutSsh();
#endif
};

void TestWelcome::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_good = m_dir.filePath(QStringLiteral("good.log"));
    QVERIFY(write(m_good,
        "2026-07-21 10:00:00,000 [main] INFO  net.io - starting\n"
        "2026-07-21 10:00:01,000 [work] ERROR db.pool - boom\n"));
}

void TestWelcome::init()
{
    QSettings settings;
    settings.remove(QStringLiteral("session")); // a window here must open nothing of its own
    settings.remove(QStringLiteral("recentFiles"));
    settings.sync();
    clearLogSettings();
    // The window reads the REAL hosts.json — HostBookmarkStore::defaultDir(), which the
    // isolated XDG_CONFIG_HOME in main() puts under the temporary tree — so it has to be
    // cleared here too, or a case that saves a host leaves it for every case after it.
    QFile::remove(HostBookmarkStore(HostBookmarkStore::defaultDir()).filePath());
}

void TestWelcome::anEmptyWindowShowsTheWelcomeScreenAndALogReplacesIt()
{
    MainWindow w;
    QVERIFY(welcome(w));
    QCOMPARE(centrePage(w), static_cast<QWidget *>(welcome(w)));

    QVERIFY(w.openFile(m_good));
    QCOMPARE(centrePage(w), static_cast<QWidget *>(tabs(w)));

    // And back again, because this is the state the screen exists for far more often
    // than the first launch: the well emptied by hand. Through the real File ▸ Close Tab
    // action rather than by deleting the page, since the order that path takes — out of
    // the tab widget first, destroyed second — is what updateEmptyState() is reached by.
    auto *close = w.findChild<QAction *>(QStringLiteral("closeTabAction"));
    QVERIFY(close);
    close->trigger();
    QCOMPARE(tabs(w)->count(), 0);
    QCOMPARE(centrePage(w), static_cast<QWidget *>(welcome(w)));
}

void TestWelcome::everyRecentLogIsARowNamedLikeItsMenuEntry()
{
    // One of each address shape the application accepts, and none of them is opened —
    // the page is built from the strings alone — so this holds whether or not SSH and
    // archive support were compiled in.
    const QStringList seeded{QDir::rootPath() + QStringLiteral("var/log/very/deep/tree/app.log"),
                             QStringLiteral("ssh://ops@prod-web/var/log/service.log"),
                             QDir::rootPath() + QStringLiteral("srv/bundle.tar.gz/var/log/inner.log")};
    seedRecent(seeded);

    MainWindow w;
    // The SAME rule the Open Recent menu uses, asserted against that rule's own output:
    // the two are rendered from one enumeration, so a divergence here means the page has
    // grown a naming rule of its own.
    QCOMPARE(rowLabels(recentList(w)), prefixedLabelsFor(seeded));
}

void TestWelcome::anAmpersandInANameIsNotDoubledOnAListRow()
{
    // The menu doubles an ampersand because a QMenu reads one as a mnemonic. A list does
    // not, so carrying that line over would show a literal "&&" — silently, and only for
    // logs whose name has an ampersand in it.
    const QString path = QDir::rootPath() + QStringLiteral("var/log/a&b.log");
    seedRecent({path});

    MainWindow w;
    QCOMPARE(recentList(w)->count(), 1);
    QCOMPARE(recentList(w)->item(0)->text(), QStringLiteral("a&b.log"));
}

void TestWelcome::everyRowCarriesItsWholeAddressAsATooltip()
{
    // What makes the short label safe, exactly as it does in the menu: the row is
    // shortened, the tooltip is the address verbatim, and the address is what opening
    // the row asks for.
    const QStringList seeded{QDir::rootPath() + QStringLiteral("var/log/very/deep/tree/app.log"),
                             QStringLiteral("ssh://ops@prod-web/var/log/service.log")};
    seedRecent(seeded);

    MainWindow w;
    QCOMPARE(recentList(w)->count(), seeded.size());
    for (int i = 0; i < seeded.size(); ++i)
        QCOMPARE(recentList(w)->item(i)->toolTip(), seeded.at(i));
}

void TestWelcome::activatingARecentRowOpensThatLog()
{
    seedRecent({m_good});
    MainWindow w;
    QCOMPARE(recentList(w)->count(), 1);

    emit recentList(w)->itemActivated(recentList(w)->item(0));
    QCOMPARE(tabs(w)->count(), 1);
    QCOMPARE(centrePage(w), static_cast<QWidget *>(tabs(w)));
}

void TestWelcome::returnOnASelectedRowOpensItToo()
{
    // The case that fails against an itemDoubleClicked wiring, and the only one that
    // does: a list reachable only by double-click is not reachable from a keyboard at
    // all, and nothing on screen would say so.
    seedRecent({m_good});
    MainWindow w;
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    QListWidget *list = recentList(w);
    list->setCurrentRow(0);
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Return);

    QCOMPARE(tabs(w)->count(), 1);
}

void TestWelcome::anEmptyListSaysWhichKindOfEmptyWithoutAddingARow()
{
    MainWindow w;
    // A LABEL over the viewport and never a row: a row is an entry to everything that
    // walks rows, and the count is what activation, selection and every future feature
    // read.
    QCOMPARE(recentList(w)->count(), 0);
    QVERIFY(recentEmpty(w));
    QVERIFY(!recentEmpty(w)->text().isEmpty());
    QVERIFY(recentEmpty(w)->isVisibleTo(recentList(w)));

    // And the two empties do not want the same words.
    if (remoteEmpty(w))
        QVERIFY(remoteEmpty(w)->text() != recentEmpty(w)->text());
}

void TestWelcome::clearingTheRecentListEmptiesThePageAsWellAsTheMenu()
{
    seedRecent({m_good});
    MainWindow w;
    QCOMPARE(recentList(w)->count(), 1);

    auto *clear = w.findChild<QPushButton *>(QStringLiteral("welcomeClearRecent"));
    QVERIFY(clear);
    QVERIFY(clear->isEnabled());
    clear->click();

    QCOMPARE(recentList(w)->count(), 0);
    QVERIFY(!clear->isEnabled()); // nothing left to forget
    QVERIFY(QSettings().value(QStringLiteral("recentFiles")).toStringList().isEmpty());
}

void TestWelcome::theRecentListFollowsTheMenuAfterAnOpen()
{
    // The page is refreshed by the menu's own funnel and has no schedule of its own, so
    // an open that moved the list moves the page with it. Without that, the screen shown
    // after closing the last tab still lists what was remembered when the window opened.
    MainWindow w;
    QCOMPARE(recentList(w)->count(), 0);

    QVERIFY(w.openFile(m_good));
    QCOMPARE(recentList(w)->count(), 1);
    QCOMPARE(recentList(w)->item(0)->toolTip(), m_good);
}

void TestWelcome::openingALogClearsTheCouldNotBeReopenedMessage()
{
    MainWindow w;
    QVERIFY(message(w));
    QVERIFY(!message(w)->isVisibleTo(welcome(w))); // nothing to say to begin with

    welcome(w)->setMessage(QStringLiteral("These files could not be reopened:"));
    QVERIFY(message(w)->isVisibleTo(welcome(w)));

    // Spent the moment anything opens. It matters more here than it did for the label
    // this replaced: that one was covered for the life of the window, while the welcome
    // screen comes back every time the last tab closes.
    QVERIFY(w.openFile(m_good));
    QVERIFY(message(w)->text().isEmpty());
    QVERIFY(!message(w)->isVisibleTo(welcome(w)));
}

void TestWelcome::theContentIsCentredRatherThanFillingTheWindow()
{
    // The lists are bounded so the page has something to CENTRE. Letting them stretch is
    // what made the first version of this screen two tall empty boxes with a caption over
    // them: the content WAS the viewport, so there was no spare height, nothing to centre
    // it in, and the whole page grew emptier the larger the window got.
    MainWindow w;
    w.resize(1200, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto *view = welcome(w);
    auto *title = w.findChild<QLabel *>(QStringLiteral("welcomeTitle"));
    QListWidget *last = remoteList(w);
    QVERIFY(view && title && last);

    const int above = title->mapTo(view, QPoint(0, 0)).y();
    const int below = view->height() - (last->mapTo(view, QPoint(0, 0)).y() + last->height());

    // Stated as RELATIONS against the viewport and never as pixel counts, which would pass
    // under one style and font and fail under the next.
    //
    // The slack has to be SUBSTANTIAL and not merely non-zero, which is the whole of what
    // makes this case discriminating: a stretching layout leaves a layout margin at each
    // end, so "there is a gap" and "the two gaps are equal" are both true of it and prove
    // nothing. A fifth of the viewport is far below what centring actually leaves at the
    // height derived above (about half of it) and far above a margin.
    QVERIFY2(above + below >= view->height() / 5,
             qPrintable(QStringLiteral("above %1, below %2, viewport %3")
                            .arg(above).arg(below).arg(view->height())));
    // And it is spread over both ends rather than piled at one, which is what centred
    // means as against top- or bottom-aligned.
    QVERIFY2(qAbs(above - below) <= (above + below) / 2,
             qPrintable(QStringLiteral("above %1, below %2").arg(above).arg(below)));
}

void TestWelcome::aThemeThatSuppliesNoBandStillGetsOne()
{
    // Banding is asked for AND its colour is supplied, which is the log table's own rule
    // (UiColors::alternateRowColor, ARCHITECTURE.md §8.3). Nothing obliges a theme to make
    // AlternateBase differ from Base, and one that leaves them equal takes the band away
    // in silence — banded on the developer's desktop and flat on the user's.
    //
    // So the theme under test is one that leaves them equal, which is the only way this
    // case can tell a supplied colour from a lucky one: under Fusion the roles already
    // differ and setAlternatingRowColors() alone would pass.
    const QPalette original = QApplication::palette();
    QPalette flat = original;
    flat.setColor(QPalette::AlternateBase, flat.color(QPalette::Base));
    QApplication::setPalette(flat);

    {
        MainWindow w;
        for (QListWidget *list : {recentList(w), remoteList(w)}) {
            QVERIFY(list);
            QVERIFY(list->alternatingRowColors());
            QCOMPARE_NE(list->palette().color(QPalette::AlternateBase),
                        list->palette().color(QPalette::Base));
        }
    }

    QApplication::setPalette(original);
}

void TestWelcome::eachActionButtonSitsLevelWithItsListRatherThanItsHeading()
{
    // Kate's arrangement, and the reason it reads as a page: the button is beside the
    // thing it acts on. Level with the heading instead, it reads as part of the caption —
    // and the taller of the two heading rows (only one carries Clear) then drags its
    // button out of line with the other section's.
    if (QFontDatabase::families().isEmpty())
        QSKIP("no font database: every widget resolves to the same fallback geometry");

    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto *view = welcome(w);
    const auto topIn = [view](QWidget *child) {
        return child->mapTo(view, QPoint(0, 0)).y();
    };

    struct Pair { const char *button; QListWidget *list; };
    const Pair pairs[] = {{"welcomeOpen", recentList(w)}, {"welcomeOpenRemote", remoteList(w)}};
    for (const Pair &p : pairs) {
        auto *button = w.findChild<QPushButton *>(QLatin1String(p.button));
        QVERIFY(button);
        QVERIFY(p.list);
        // AN EQUALITY, and it has to be one: the offset that puts the button there is
        // the heading row's height plus the section layout's own spacing, so a tolerance
        // of a button's height passes just as happily when that spacing is measured as
        // some other number than the one the layout lays out with — which is exactly how
        // both buttons shipped seven pixels above their lists. It is still a relation and
        // not a pixel count, so it holds under Fusion, Breeze and Windows alike.
        QVERIFY2(topIn(button) == topIn(p.list),
                 qPrintable(QStringLiteral("%1 top %2 against list top %3")
                                .arg(QLatin1String(p.button))
                                .arg(topIn(button))
                                .arg(topIn(p.list))));
    }
}

void TestWelcome::theNameWearsTheApplicationMarkAndTheTwoAreCentredTogether()
{
    // The mark beside the name (SPEC.md §3), which is where Kate's welcome page puts its
    // own. Three separate claims, and the third is the one that would go silently.
    if (QFontDatabase::families().isEmpty())
        QSKIP("no font database: the mark is sized from the title's metrics");

    MainWindow w;
    w.resize(1200, 800);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto *view = welcome(w);
    auto *column = w.findChild<QWidget *>(QStringLiteral("welcomeColumn"));
    auto *title = w.findChild<QLabel *>(QStringLiteral("welcomeTitle"));
    auto *mark = w.findChild<QWidget *>(QStringLiteral("welcomeIcon"));
    QVERIFY(view && column && title && mark);
    QVERIFY(mark->isVisibleTo(view));

    // (1) Square, sized from the title rather than written down, and BESIDE the name
    // rather than over or under it.
    QCOMPARE(mark->width(), mark->height());
    QVERIFY(mark->height() >= title->fontMetrics().height());
    const QRect markRect(mark->mapTo(view, QPoint(0, 0)), mark->size());
    const QRect titleRect(title->mapTo(view, QPoint(0, 0)), title->size());
    QVERIFY2(markRect.right() <= titleRect.left(),
             qPrintable(QStringLiteral("mark right %1, title left %2")
                            .arg(markRect.right()).arg(titleRect.left())));
    // Level with it: the two overlap vertically rather than merely being near each other.
    QVERIFY(markRect.top() < titleRect.bottom() && titleRect.top() < markRect.bottom());

    // (2) The PAIR is centred, not the name. This is the half a change that simply
    // prepended the mark to the existing centred label would fail: the title alone would
    // stay in the middle and the whole heading would sit off to the right of it.
    const int pairCentre = (markRect.left() + titleRect.right()) / 2;
    const int columnCentre = column->mapTo(view, QPoint(0, 0)).x() + column->width() / 2;
    QVERIFY2(qAbs(pairCentre - columnCentre) <= mark->width() / 2,
             qPrintable(QStringLiteral("pair centre %1, column centre %2")
                            .arg(pairCentre).arg(columnCentre)));

    // (3) IT INKS, which is the whole of "drawn and never loaded" (AppIcon.h). The SVG in
    // packaging/ is an icon-theme file and not a Qt resource, so a mark reached through
    // QIcon::fromTheme() is a picture on an installed Linux desktop and an empty widget
    // on Windows, on macOS and in every uninstalled build — including this one. Nothing
    // about the geometry above can see that; only the pixels can.
    const QImage shot = mark->grab().toImage();
    QVERIFY(!shot.isNull());
    QSet<QRgb> colours;
    for (int y = 0; y < shot.height(); ++y)
        for (int x = 0; x < shot.width(); ++x)
            colours.insert(shot.pixel(x, y));
    QVERIFY2(colours.size() >= 3,
             qPrintable(QStringLiteral("the mark drew %1 distinct colours")
                            .arg(colours.size())));
}

#if defined(LOFTAIL_HAVE_SSH)
void TestWelcome::aRememberedRemoteLogIsARowAndASavedHostWithNoneIsAnother()
{
    const HostBookmarkStore store(HostBookmarkStore::defaultDir());
    HostBookmark withLog;
    withLog.label = QStringLiteral("prod");
    withLog.user = QStringLiteral("ops");
    withLog.host = QStringLiteral("prod-web");
    withLog.paths = {QStringLiteral("/var/log/app.log")};
    HostBookmark bare;
    bare.label = QStringLiteral("staging");
    bare.host = QStringLiteral("staging-box");
    QVERIFY(store.replaceAll({withLog, bare}));

    MainWindow w;
    QListWidget *list = remoteList(w);
    QVERIFY(list);
    QCOMPARE(list->count(), 2);

    // A remembered log: named host-and-path, and its tooltip is the address it opens.
    QCOMPARE(list->item(0)->text(), QStringLiteral("prod: /var/log/app.log"));
    QCOMPARE(list->item(0)->toolTip(), withLog.locationFor(withLog.paths.first()).toString());

    // A host with none: the trailing "..." is what says it opens the dialog instead, and
    // the empty path is what the window branches on.
    QCOMPARE(list->item(1)->text(), QStringLiteral("staging..."));
    QVERIFY(list->item(1)->data(Qt::UserRole + 2).toString().isEmpty());
}
#else
void TestWelcome::theRemotesSectionIsAbsentWithoutSsh()
{
    // Hidden entire rather than greyed, which is the opposite of what the File menu does
    // with the same two items — the menu is where the "built without SSH" sentence lives
    // and it keeps them for that, while an empty disabled column on a landing page
    // explains nothing the menu is not about to explain.
    MainWindow w;
    QVERIFY(remoteSection(w));
    QVERIFY(!remoteSection(w)->isVisibleTo(welcome(w)));
}
#endif

int main(int argc, char *argv[])
{
    // Isolate persistent state: the recent list, the saved hosts, the settings tree and
    // the session all live under the config directory, and a real one would make these
    // cases depend on what the developer last opened.
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-welcome"));

    TestWelcome tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_welcome.moc"
