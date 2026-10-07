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
#include <QComboBox>
#include <QFile>
#include <QGroupBox>
#include <QLineEdit>
#include <QProgressDialog>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include "ConfigReset.h"
#include "Decoder.h"
#include "Document.h"
#include "DocumentContext.h"
#include "DocumentView.h"
#include "FilterPane.h"
#include "LogFormat.h"
#include "LogView.h"
#include "MainWindow.h"

using namespace loftail;

// File ▸ Export Visible (SPEC.md §5): every row the active view shows, written as the
// log's ORIGINAL BYTES. What this file pins is the "original" — CRLF, a multi-line record,
// UTF-16 and its BOM all come out exactly as they went in — that what is written is the
// VISIBLE set (a filter narrows it, context widens it), and that a cancelled export
// leaves the target alone.
//
// Every comparison is on bytes read back from disk, never on decoded text: decoding is
// exactly the step that would hide a lost CR or a dropped BOM.
class TestExportVisible : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    // Four records, CRLF, the second spanning two lines, and the LAST unterminated — the
    // shape a log still being written has.
    static QByteArray rec(int i)
    {
        static const char *const recs[] = {
            "2026-07-21 10:00:00,000 [main] INFO  net.io - alpha\r\n",
            "2026-07-21 10:00:01,000 [main] WARN  db.pool - beta\r\n  continued\r\n",
            "2026-07-21 10:00:02,000 [main] INFO  net.io - gamma\r\n",
            "2026-07-21 10:00:03,000 [main] ERROR db.pool - delta",
        };
        return QByteArray(recs[i]);
    }

    QString writeLog(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write(bytes);
        return path;
    }

    static QByteArray readAll(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    static DocumentView *activeView(const MainWindow &w)
    {
        auto *t = w.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
        return t ? qobject_cast<DocumentView *>(t->currentWidget()) : nullptr;
    }

    static int visibleRecords(const MainWindow &w)
    {
        DocumentView *v = activeView(w);
        return v ? v->logView()->recordCount() : -1;
    }

    static void openAndIndex(MainWindow &w, const QString &path, int records)
    {
        w.resize(1000, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.openFile(path);
        QTRY_COMPARE(visibleRecords(w), records);
    }

    static FilterPane *filterPane(const MainWindow &w) { return w.findChild<FilterPane *>(); }

private slots:
    void init();

    void anUnfilteredExportIsTheFileByteForByte();
    void aFilterExportsOnlyWhatItShows();
    void contextRowsAreExportedToo();
    void aUtf16LogKeepsItsEncodingAndItsBom();
    void cancellingLeavesTheTargetAsItWas();
    void theActionNeedsALog();
};

void TestExportVisible::init()
{
    QVERIFY(m_dir.isValid());
    QSettings s;
    s.remove(QStringLiteral("session"));
    s.sync();
    clearLogSettings();
}

void TestExportVisible::anUnfilteredExportIsTheFileByteForByte()
{
    const QByteArray all = rec(0) + rec(1) + rec(2) + rec(3);
    const QString log = writeLog(QStringLiteral("whole.log"), all);
    MainWindow w;
    openAndIndex(w, log, 4);

    const QString out = m_dir.filePath(QStringLiteral("whole-out.log"));
    QVERIFY(w.exportVisibleTo(out));
    // CRLF kept, the continuation line kept, and nothing appended after the
    // unterminated last record.
    QCOMPARE(readAll(out), all);
}

void TestExportVisible::aFilterExportsOnlyWhatItShows()
{
    const QString log = writeLog(QStringLiteral("filtered.log"),
                                 rec(0) + rec(1) + rec(2) + rec(3));
    MainWindow w;
    openAndIndex(w, log, 4);

    FilterPane *pane = filterPane(w);
    QVERIFY(pane);
    auto *group = pane->findChild<QGroupBox *>(QStringLiteral("priorityGroup"));
    auto *combo = pane->findChild<QComboBox *>(QStringLiteral("priorityCombo"));
    QVERIFY(group && combo);
    const int row = combo->findData(int(Priority::Warn));
    QVERIFY(row >= 0);
    combo->setCurrentIndex(row);
    group->setChecked(true);
    QTRY_COMPARE(visibleRecords(w), 2);

    const QString out = m_dir.filePath(QStringLiteral("filtered-out.log"));
    QVERIFY(w.exportVisibleTo(out));
    QCOMPARE(readAll(out), rec(1) + rec(3));
}

void TestExportVisible::contextRowsAreExportedToo()
{
    const QString log = writeLog(QStringLiteral("context.log"),
                                 rec(0) + rec(1) + rec(2) + rec(3));
    MainWindow w;
    openAndIndex(w, log, 4);

    FilterPane *pane = filterPane(w);
    QVERIFY(pane);
    auto *group = pane->findChild<QGroupBox *>(QStringLiteral("messageGroup"));
    auto *text = pane->findChild<QLineEdit *>(QStringLiteral("messageText"));
    auto *before = pane->findChild<QSpinBox *>(QStringLiteral("contextBefore"));
    QVERIFY(group && text && before);
    group->setChecked(true);
    text->setText(QStringLiteral("gamma"));
    QTRY_COMPARE(visibleRecords(w), 1);
    before->setValue(1);
    QTRY_COMPARE(visibleRecords(w), 2);

    const QString out = m_dir.filePath(QStringLiteral("context-out.log"));
    QVERIFY(w.exportVisibleTo(out));
    // The dimmed record before the match is visible, so it is exported.
    QCOMPARE(readAll(out), rec(1) + rec(2));
}

void TestExportVisible::aUtf16LogKeepsItsEncodingAndItsBom()
{
    QString text;
    for (int i = 0; i < 4; ++i)
        text += QString::fromUtf8(rec(i));
    text += QStringLiteral("\r\n");
    QByteArray bytes("\xFF\xFE", 2);
    for (const QChar c : std::as_const(text)) {
        bytes.append(char(c.unicode() & 0xFF));
        bytes.append(char(c.unicode() >> 8));
    }
    const QString log = writeLog(QStringLiteral("wide.log"), bytes);
    MainWindow w;
    openAndIndex(w, log, 4);
    QCOMPARE(activeView(w)->context()->doc->decoder().resolvedEncoding(), Encoding::Utf16LE);

    const QString out = m_dir.filePath(QStringLiteral("wide-out.log"));
    QVERIFY(w.exportVisibleTo(out));
    const QByteArray written = readAll(out);
    QCOMPARE(written, bytes);
    QCOMPARE(Decoder::detect(written).resolvedEncoding(), Encoding::Utf16LE);
}

void TestExportVisible::cancellingLeavesTheTargetAsItWas()
{
    const QString log = writeLog(QStringLiteral("cancel.log"),
                                 rec(0) + rec(1) + rec(2) + rec(3));
    MainWindow w;
    openAndIndex(w, log, 4);

    const QString out = writeLog(QStringLiteral("cancel-out.log"), "untouched");
    LogView *view = activeView(w)->logView();
    view->setCopyProgressThreshold(1);

    bool sawDialog = false;
    QTimer::singleShot(0, view, [&]() {
        if (auto *dlg = view->findChild<QProgressDialog *>(QStringLiteral("exportProgress"))) {
            sawDialog = true;
            dlg->cancel();
        }
    });
    QVERIFY(!w.exportVisibleTo(out));
    QVERIFY2(sawDialog, "an export above the threshold showed no progress dialog");
    QCOMPARE(readAll(out), QByteArray("untouched"));
    QVERIFY(view->findChild<QProgressDialog *>(QStringLiteral("exportProgress")) == nullptr);
}

void TestExportVisible::theActionNeedsALog()
{
    MainWindow w;
    auto *action = w.findChild<QAction *>(QStringLiteral("exportVisibleAction"));
    QVERIFY(action);
    QVERIFY(!action->isEnabled());

    const QString log = writeLog(QStringLiteral("action.log"), rec(0));
    openAndIndex(w, log, 1);
    QVERIFY(action->isEnabled());
}

int main(int argc, char *argv[])
{
    QTemporaryDir configHome;
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("XDG_DATA_HOME", configHome.path().toUtf8());
    qputenv("HOME", configHome.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("loftail-test-exportvisible"));
    QApplication::setApplicationName(QStringLiteral("loftail-test-exportvisible"));

    TestExportVisible tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_exportvisible.moc"
