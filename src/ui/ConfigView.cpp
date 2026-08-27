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

#include "ConfigView.h"

#include "Decoder.h"
#include "FindBar.h"
#include "Fonts.h"
#include "MessageLabel.h"
#include "RemoteLocation.h"
#include "UiColors.h"

#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <utility>

namespace loftail {

namespace {
// How many matches the "N of M" count will walk before it gives a floor instead. A
// config file is small, so this is never reached in practice — which is exactly why it
// is here rather than left out: an unbounded count is a promise that stops being true on
// the one enormous file somebody eventually opens, and it would break it silently.
constexpr int kMaxCounted = 100000;

// The editor's own wheel filter. QPlainTextEdit answers Ctrl+wheel by zooming ITSELF,
// which is the one thing a page in this window must not do: the log font is a single
// application-wide size, so a page that re-fonted itself would leave every other view
// behind. Reported instead — LogView::zoomStepRequested's rule, applied to the one other
// widget in the well that has an opinion about the wheel.
class WheelFilteringEdit : public QPlainTextEdit
{
public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<void(int)> onZoomStep;

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            // The remainder accumulates: a trackpad sends a stream of small deltas where
            // a mouse sends one notch, and rounding each of them to a step would zoom
            // wildly on the first and not at all on the rest.
            m_wheelRemainder += event->angleDelta().y();
            const int steps = m_wheelRemainder / 120;
            if (steps != 0) {
                m_wheelRemainder -= steps * 120;
                if (onZoomStep)
                    onZoomStep(steps);
            }
            event->accept();
            return;
        }
        QPlainTextEdit::wheelEvent(event);
    }

private:
    int m_wheelRemainder = 0;
};

// The line ending the file mostly uses, so saving does not rewrite every line of it.
QString dominantLineEnding(const QString &text)
{
    const qsizetype crlf = text.count(QLatin1String("\r\n"));
    const qsizetype lf = text.count(QLatin1Char('\n')) - crlf;
    return crlf > lf ? QStringLiteral("\r\n") : QStringLiteral("\n");
}
} // namespace

ConfigView::ConfigView(QString address, QWidget *parent)
    : QWidget(parent), m_address(std::move(address))
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    // --- The header strip: what file this is, and which grammar it is coloured in.
    auto *header = new QWidget(this);
    auto *headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(6, 4, 6, 4);
    headerRow->setSpacing(8);

    m_pathLabel = new QLabel(header);
    m_pathLabel->setObjectName(QStringLiteral("configPathLabel")); // findChild, for tests
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerRow->addWidget(m_pathLabel, 1);

    m_syntaxSource = new QLabel(header);
    m_syntaxSource->setObjectName(QStringLiteral("configSyntaxSource")); // findChild
    QPalette mutedPalette = m_syntaxSource->palette();
    mutedPalette.setColor(QPalette::WindowText, mutedColor(palette()));
    m_syntaxSource->setPalette(mutedPalette);
    headerRow->addWidget(m_syntaxSource, 0);

    m_syntaxBox = new QComboBox(header);
    m_syntaxBox->setObjectName(QStringLiteral("configSyntax")); // findChild, for tests
    // Item DATA carries the enum, never the row index, so a grammar added to the enum
    // cannot silently re-point every stored choice one row along — the trap the priority
    // combo records.
    for (ConfigSyntax s : {ConfigSyntax::PlainText, ConfigSyntax::Ini, ConfigSyntax::Json,
                           ConfigSyntax::Xml}) {
        m_syntaxBox->addItem(configSyntaxName(s), int(s));
    }
    m_syntaxBox->setToolTip(tr("How this file is coloured. loftail picks one from the "
                               "file's extension, or from its contents where the "
                               "extension says nothing; change it here if the guess is "
                               "wrong."));
    headerRow->addWidget(m_syntaxBox, 0);
    m_layout->addWidget(header, 0);

    // --- A notice that STAYS. A save failure names a directory the reader has to act
    // on, which neither the status bar's transient channel nor the per-tick status label
    // can hold — the argument the open-refusal strip already makes one level up.
    m_notice = new MessageLabel(this);
    m_notice->setObjectName(QStringLiteral("configNotice")); // findChild, for tests
    m_notice->hide();
    m_layout->addWidget(m_notice, 0);

    // --- The text itself.
    auto *edit = new WheelFilteringEdit(this);
    edit->onZoomStep = [this](int steps) { emit zoomStepRequested(steps); };
    m_edit = edit;
    m_edit->setObjectName(QStringLiteral("configEdit")); // findChild, for tests
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_edit->setFont(logTextFont());
    m_edit->setTabChangesFocus(false);
    m_layout->addWidget(m_edit, 1);

    m_highlighter = new ConfigHighlighter(m_edit->document());
    m_highlighter->setPalette(palette());

    // --- The Find bar, VERBATIM. It is pure UI — it emits findRequested() and knows
    // nothing about what is being searched — so the only thing it needs is wording that
    // is true here.
    m_findBar = new FindBar(this);
    m_findBar->setPlaceholderText(tr("Search this file..."));
    m_layout->addWidget(m_findBar, 0);
    connect(m_findBar, &FindBar::findRequested, this, &ConfigView::runFind);
    connect(m_findBar, &FindBar::closed, this, [this]() {
        // The query is off the screen, so a selection still standing from it would be a
        // claim about a search the reader can no longer see.
        QTextCursor c = m_edit->textCursor();
        c.clearSelection();
        m_edit->setTextCursor(c);
    });

    // Any signal that arrives here is a USER change, unconditionally: setSyntax() moves
    // the combo under a QSignalBlocker, so a programmatic choice never reaches this. That
    // is what makes `chosen` true with no test around it — and `chosen` is what stops a
    // later re-sniff overriding the reader, and what the session stores.
    connect(m_syntaxBox, &QComboBox::currentIndexChanged, this, [this](int) {
        setSyntax(static_cast<ConfigSyntax>(m_syntaxBox->currentData().toInt()),
                  /*chosen=*/true);
    });

    connect(m_edit->document(), &QTextDocument::modificationChanged, this,
            &ConfigView::modifiedChanged);

    // The LABEL's own resize, not the page's. ConfigView::resizeEvent fires before the
    // layout pass that actually sizes this label, so eliding from there alone measures
    // against the width the label had a moment ago — which at construction is tiny, and
    // leaves the address cut to a stub on a window with room to spare.
    m_pathLabel->installEventFilter(this);

    setFocusProxy(m_edit);
    updatePathLabel();
    updateSyntaxLabel();
}

void ConfigView::updatePathLabel()
{
    // ELIDED, with the tooltip only where it was actually cut — the rule the log table
    // and HighlighterPane's summary column both follow. A QLabel clips silently
    // otherwise, which reads as a rendering fault rather than as a path too long for the
    // window: the address simply stops, mid-segment, with nothing to say it goes on.
    const QString full = logSourceDisplayPath(m_address);
    const QString shown =
        m_pathLabel->fontMetrics().elidedText(full, Qt::ElideMiddle, m_pathLabel->width());
    m_pathLabel->setText(shown);
    m_pathLabel->setToolTip(shown == full ? QString() : full);
}

QString ConfigView::displayName() const
{
    // The shared rule, so a config tab's name carries the same three guarantees a log
    // tab's does: never empty, never a path, never a password.
    return logSourceDisplayName(m_address);
}

void ConfigView::setContents(const QByteArray &bytes, bool existed)
{
    m_existed = existed;

    // Read through the SAME detection every log goes through — BOM decisive, then the
    // NUL-frequency heuristic, then UTF-8 validation. A config file's encoding is NOT
    // its log's: a UTF-16 log beside a UTF-8 properties file is entirely ordinary, so
    // this must never be seeded from the log's resolved encoding.
    const Decoder decoder = Decoder::detect(bytes, Encoding::Auto);
    m_encoding = int(decoder.resolvedEncoding());
    m_bom = bytes.left(int(decoder.bomLength()));
    // QByteArray::mid(), not QByteArrayView::sliced(): the view's slicing family is
    // where the Qt 6.4 floor bites — left() is not there at all — and only CI checks
    // that, so the whole family is avoided rather than audited member by member. The
    // copy is one config file, once per open.
    const QString text = decoder.decode(bytes.mid(int(decoder.bomLength())));
    m_lineEnding = dominantLineEnding(text);

    // Into the widget with '\n' throughout — QPlainTextEdit works in paragraphs and
    // would show a stray CR otherwise — and back out in the file's own ending on save.
    QString normalised = text;
    normalised.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    m_edit->setPlainText(normalised);
    m_edit->document()->setModified(false);

    if (!m_syntaxChosen) {
        // Extension first; contents only where it says nothing. A known extension is
        // never second-guessed — a `.json` holding a mistake is a JSON file with a
        // mistake in it, and colouring it as something else hides it.
        const QString name = logSourceBareName(m_address);
        const int dot = int(name.lastIndexOf(u'.'));
        ConfigSyntax picked = dot >= 0
            ? syntaxForExtension(QStringView(name).mid(dot + 1))
            : ConfigSyntax::PlainText;
        m_syntaxSniffed = picked == ConfigSyntax::PlainText;
        if (m_syntaxSniffed)
            picked = sniffSyntax(bytes);
        setSyntax(picked, /*chosen=*/false);
    }
    updateSyntaxLabel();
}

QByteArray ConfigView::toBytes() const
{
    QString text = m_edit->toPlainText();
    // QPlainTextEdit uses U+2029 for a paragraph break in some paths; normalise it
    // deliberately rather than letting it reach a config parser.
    text.replace(QChar(0x2029), QLatin1Char('\n'));
    if (m_lineEnding != QLatin1String("\n"))
        text.replace(QLatin1String("\n"), m_lineEnding);

    Decoder decoder = Decoder::detect(QByteArrayView(), static_cast<Encoding>(m_encoding));
    // The BOM is REPLAYED, never re-derived: only the read knew whether the file had
    // one, and adding one unconditionally grows a mark onto a file that never had it.
    return m_bom + decoder.encode(text);
}

bool ConfigView::isModified() const
{
    return m_edit->document()->isModified();
}

void ConfigView::setModified(bool modified)
{
    m_edit->document()->setModified(modified);
}

ConfigSyntax ConfigView::syntax() const
{
    return m_highlighter->syntax();
}

void ConfigView::setSyntax(ConfigSyntax syntax, bool chosen)
{
    if (chosen)
        m_syntaxChosen = true;
    m_highlighter->setSyntax(syntax);
    const QSignalBlocker block(m_syntaxBox);
    const int row = m_syntaxBox->findData(int(syntax));
    if (row >= 0)
        m_syntaxBox->setCurrentIndex(row);
    updateSyntaxLabel();
}

void ConfigView::updateSyntaxLabel()
{
    // SAY WHERE THE CHOICE CAME FROM. The house rule is that a guessed format is never
    // applied silently, and for colouring a modal would be absurd — so the guess is
    // discharged by being visible and one click from being overridden.
    if (m_syntaxChosen)
        m_syntaxSource->setText(tr("chosen"));
    else if (m_syntaxSniffed)
        m_syntaxSource->setText(tr("guessed from the contents"));
    else
        m_syntaxSource->setText(tr("from the file name"));
}

void ConfigView::setLogFont(const QFont &font)
{
    // The TEXT only. The header strip and the combo are chrome and stay at the UI font:
    // zooming the log text is about reading the file, not about resizing its label.
    m_edit->setFont(font);
}

void ConfigView::showNotice(const QString &text)
{
    m_notice->setText(text);
    QPalette p = m_notice->palette();
    // At SHOW time, not at construction, so it follows a theme changed under a running
    // window — the rule the open-refusal strip already follows.
    p.setColor(QPalette::WindowText, errorColor(palette()));
    m_notice->setPalette(p);
    m_notice->setVisible(!text.isEmpty());
}

void ConfigView::clearNotice()
{
    m_notice->clear();
    m_notice->hide();
}

void ConfigView::setBusy(bool busy, const QString &what)
{
    m_busy = busy;
    m_edit->setReadOnly(busy);
    if (busy) {
        // The page's own notice rather than the status bar: a connect can take twenty
        // seconds, and the status bar is rewritten on every ingest tick of every log.
        m_notice->setText(what);
        QPalette p = m_notice->palette();
        // The ORDINARY text colour, not the error colour showNotice() uses — "connecting"
        // is not a failure, and painting it red would say the open had already gone wrong.
        p.setColor(QPalette::WindowText, mutedColor(palette()));
        m_notice->setPalette(p);
        m_notice->show();
    }
}

int ConfigView::revision() const
{
    return m_edit->document()->revision();
}

bool ConfigView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_pathLabel && event->type() == QEvent::Resize)
        updatePathLabel();
    return QWidget::eventFilter(watched, event);
}

void ConfigView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        // The page notices the theme itself, so it does not matter who changed it — the
        // rule LogView follows for a font change.
        m_highlighter->setPalette(palette());
        QPalette mutedPalette = m_syntaxSource->palette();
        mutedPalette.setColor(QPalette::WindowText, mutedColor(palette()));
        m_syntaxSource->setPalette(mutedPalette);
    }
}

void ConfigView::activateFind()
{
    m_findBar->activate();
}

void ConfigView::runFind(bool forward, bool fromStart)
{
    // FIRST, above every branch. Every report below goes into this bar's own label, so a
    // search asked for from the text (F3) has to open the bar before it writes or the
    // answer lands where nobody can read it — MainWindow::runFind()'s rule.
    // Land the debt a coalesced query edit leaves — MainWindow::runFind()'s rule, and
    // F3 reaches this function by the same direct route that makes it necessary there.
    m_findBar->cancelPendingSearch();
    m_findBar->reveal();
    // Cleared once at the top and set by the branches that fail — MainWindow::runFind()'s
    // rule, and the same reason: the branches that succeed are the ones that would forget.
    m_findBar->setQueryFailed(false);

    const QString pattern = m_findBar->pattern();
    if (pattern.isEmpty()) {
        // An empty query answers only a DELIBERATE navigation. `fromStart` is what tells
        // the two apart: false for F3 and the arrows, where the reader asked to go
        // somewhere; true when the query itself changed, where they have just deleted
        // their own text and a message would be a nag.
        m_findBar->setStatus(fromStart ? QString() : tr("no search text"));
        return;
    }

    QTextDocument::FindFlags flags;
    if (!forward)
        flags |= QTextDocument::FindBackward;
    if (m_findBar->caseSensitive())
        flags |= QTextDocument::FindCaseSensitively;

    QTextDocument *doc = m_edit->document();
    QRegularExpression re;
    if (m_findBar->regex()) {
        QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
        if (!m_findBar->caseSensitive())
            options |= QRegularExpression::CaseInsensitiveOption;
        re = QRegularExpression(pattern, options);
        if (!re.isValid()) {
            m_findBar->setStatus(tr("bad regex"));
            m_findBar->setQueryFailed(true);
            return;
        }
    }

    const auto findFrom = [&](int position) {
        return m_findBar->regex() ? doc->find(re, position, flags)
                                  : doc->find(pattern, position, flags);
    };

    const int fromEnd = forward ? 0 : doc->characterCount() - 1;
    const int from = fromStart ? fromEnd : m_edit->textCursor().position();
    QTextCursor hit = findFrom(from);
    bool wrapped = false;
    if (hit.isNull()) {
        // The search wraps, and a wrap that says nothing is a teleport (SPEC.md §5).
        hit = findFrom(forward ? 0 : doc->characterCount() - 1);
        wrapped = !hit.isNull();
    }
    if (hit.isNull()) {
        // Red in the field, `0 of 0` in the label — the log's bar's wording, shared so
        // the two cannot come to describe one gesture differently.
        m_findBar->setQueryFailed(true);
        m_findBar->setStatus(FindBar::describeNoMatch());
        return;
    }
    m_edit->setTextCursor(hit);

    // Where this match sits among the others. BOUNDED for the reason the log's count is:
    // a floor rendered "47+" is honest, an unbounded walk on a huge file is not.
    int total = 0;
    int index = 0;
    bool complete = true;
    // Walked FORWARDS regardless of the search direction: "3 of 47" counts from the top
    // of the file either way, which is what the number means to a reader.
    const QTextDocument::FindFlags forwardFlags = flags & ~QTextDocument::FindBackward;
    QTextCursor walk = m_findBar->regex() ? doc->find(re, 0, forwardFlags)
                                          : doc->find(pattern, 0, forwardFlags);
    while (!walk.isNull()) {
        ++total;
        if (walk.selectionStart() == hit.selectionStart())
            index = total;
        if (total >= kMaxCounted) {
            complete = false;
            break;
        }
        walk = m_findBar->regex() ? doc->find(re, walk, forwardFlags)
                                  : doc->find(pattern, walk, forwardFlags);
    }

    // The wording is FindBar's, shared with the log's bar, so the two cannot come to
    // describe one gesture in two vocabularies.
    m_findBar->setStatus(FindBar::describeMatch(index, total, complete, wrapped, forward));
}

} // namespace loftail
