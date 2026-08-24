#pragma once

#include "ConfigSyntax.h"

#include <QByteArray>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QVBoxLayout;
QT_END_NAMESPACE

namespace loftail {

class FindBar;
class MessageLabel;

// One config file, open for editing, as a page in the document well (SPEC.md §4).
//
// The PEER of DocumentView, and named for that: `*View` in this tree is a page you look
// at, while `*Editor` means a block of controls inside a dialog (FormatEditor,
// LogProfileEditor, AxisEditor), which is what a reader would expect `ConfigEditor` to
// be. It is the second kind of page the well has ever held — see MainWindow's
// viewsInTabOrder(), which exists because of it.
//
// It holds NO Document and no DocumentContext. A config file is not a log: nothing
// indexes it, nothing tails it, no filter or highlight rule applies to it, and the side
// panes have nothing to say about it. What it shares with a log tab is the Find bar,
// verbatim, and the log font.
class ConfigView : public QWidget
{
    Q_OBJECT

public:
    // `address` is a resolved config address (ConfigLocation.h) — already normalized and
    // never carrying a password.
    ConfigView(QString address, QWidget *parent = nullptr);

    QString address() const { return m_address; }

    // The file's own name, for the tab. A SEGMENT, never a path.
    QString displayName() const;

    QPlainTextEdit *editor() const { return m_edit; }
    FindBar *findBar() const { return m_findBar; }

    // Fill the page from bytes read off the file. `existed` false is the supported
    // "not there yet" case: the buffer opens empty, the page says so, and Save creates
    // the file. The ENCODING, the byte-order mark and the dominant line ending are all
    // recorded here and replayed on save — see toBytes().
    void setContents(const QByteArray &bytes, bool existed);

    // The buffer as bytes to write, in the encoding it was read in, with the BOM it had
    // and the line ending it used. Never a blind UTF-8 re-encode: that silently rewrites
    // every byte of a UTF-16 config, and log4cplus built for wchar_t on Windows is
    // exactly the population that writes one.
    QByteArray toBytes() const;

    bool isModified() const;
    void setModified(bool modified);
    bool fileExisted() const { return m_existed; }

    ConfigSyntax syntax() const;
    // Choose the grammar and say where the choice came from. `chosen` marks a syntax the
    // USER picked, which is what the session stores and what stops a later re-sniff
    // overriding them.
    void setSyntax(ConfigSyntax syntax, bool chosen);
    bool syntaxWasChosen() const { return m_syntaxChosen; }

    // The log text font, so the editor reads in the same face and size as the log.
    void setLogFont(const QFont &font);

    // A message that stays until the next successful save or a dismissal — a save
    // failure names a directory the reader has to act on, which neither the status bar's
    // transient channel nor the per-tick status label can hold.
    void showNotice(const QString &text);
    void clearNotice();

    // While a remote read or write is in flight. The text is not editable during a read
    // — there is nothing in it yet to edit, and letting somebody type into a buffer that
    // is about to be replaced would throw their work away — and Save is unavailable
    // during a write, which is what stops two writes racing for one file.
    void setBusy(bool busy, const QString &what);
    bool isBusy() const { return m_busy; }

    // The document's revision when a write was started. Clearing the modified flag on a
    // reply is only honest if nothing was typed in the meantime, so the reply compares
    // against this rather than assuming it is still true.
    int revision() const;

    // Put the Find bar on screen and focus it, and run a search over the buffer. The
    // reveal comes FIRST, above every branch, for the reason MainWindow::runFind()
    // records: every report goes into this bar's own label, so a search asked for from
    // the table has to open the bar before it writes or the answer lands where nobody
    // can read it.
    void activateFind();
    void runFind(bool forward, bool fromStart);

signals:
    void modifiedChanged(bool modified);
    // Ctrl+wheel over the text. REPORTED, never acted on: the log font is one
    // application-wide size, so a page that re-fonted itself would leave every other
    // view behind — the rule LogView::zoomStepRequested already follows.
    void zoomStepRequested(int steps);

protected:
    void changeEvent(QEvent *event) override;
    // Watches the path label so the address re-elides into the width it actually gets.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updatePathLabel();
    void updateSyntaxLabel();

    QString            m_address;
    QPlainTextEdit    *m_edit = nullptr;
    FindBar           *m_findBar = nullptr;
    QComboBox         *m_syntaxBox = nullptr;
    QLabel            *m_pathLabel = nullptr;
    QLabel            *m_syntaxSource = nullptr;
    MessageLabel      *m_notice = nullptr;
    ConfigHighlighter *m_highlighter = nullptr;
    QVBoxLayout       *m_layout = nullptr;

    bool         m_busy = false;
    bool         m_existed = false;
    bool         m_syntaxChosen = false;
    bool         m_syntaxSniffed = false;
    // Replayed on save, never re-derived: see toBytes().
    QByteArray   m_bom;
    QString      m_lineEnding = QStringLiteral("\n");
    int          m_encoding = 0; // loftail::Encoding, as an int to keep the header light
};

} // namespace loftail
