#include "LogView.h"

#include "UiColors.h"

#include "Decoder.h"
#include "Document.h"
#include "Fonts.h"
#include "LogFormat.h"
#include "LogModel.h"
#include "LogSource.h"
#include "RecordIndex.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QHeaderView>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QProgressDialog>
#include <QRegion>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionHeader>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QWheelEvent>
#include <QtMath>

#include <limits>

namespace loftail {

namespace {

// Records copied between two progress updates. Large enough that the update and the
// event pass are noise beside the decoding, small enough that Cancel answers within a
// frame on any machine (ARCHITECTURE.md §7.1.6).
constexpr int kCopyChunk = 2000;

// Reserve `chars` on a string being built record by record, bounded by what a QString
// can hold at all so an absurd estimate cannot throw before a single record is copied.
void reserveFor(QString &s, qint64 chars)
{
    s.reserve(qsizetype(qBound<qint64>(0, chars, qint64(std::numeric_limits<int>::max()))));
}

// A column's seed width in CHARACTERS of the fixed-pitch font rather than in pixels
// (SPEC.md §5, ARCHITECTURE.md §7.1). Every column renders in monospaceFont(), so a
// character count is the one unit that means the same thing at every font size — and a
// pixel count is a guess about a font nobody has resolved yet. These are what a TYPICAL
// value of the field takes; the header caption is measured separately and the wider of
// the two wins, which is what stops "Priority" arriving in a column too narrow to say so.
int seedColumnChars(FieldRole role)
{
    switch (role) {
    case FieldRole::Date:       return 23; // 2026-07-21 14:32:05,123
    case FieldRole::Thread:     return 12;
    case FieldRole::Priority:   return 5;  // the longest level word; the caption is longer
    case FieldRole::Logger:     return 20;
    case FieldRole::FileName:   return 18;
    case FieldRole::LineNumber: return 6;
    case FieldRole::Method:     return 18;
    case FieldRole::Location:   return 26;
    case FieldRole::ThreadName: return 14;
    case FieldRole::ProcessId:  return 8;
    case FieldRole::Hostname:   return 18;
    case FieldRole::Elapsed:    return 10;
    case FieldRole::Ndc:        return 18;
    case FieldRole::Mdc:        return 20;
    case FieldRole::EnvVar:     return 14;
    case FieldRole::Message:    return 200; // wide: wrap-off scrolls sideways (§5)
    }
    return 16;
}

// A gutter, so a value that fills its column does not touch the first glyph of the next.
constexpr int kColumnPadding = 10;
// Floors and ceilings for every width this file computes. The floor is what a font that
// resolves to NOTHING falls back to — Windows' offscreen plugin ships no fonts, so every
// advance there is 0 and an unfloored width would collapse the whole header.
constexpr int kMinColumnWidth = 40;
constexpr int kMaxColumnWidth = 2400;
constexpr int kFallbackCharWidth = 8;
// A seed reads the intern table but is not a fit: one 300-character logger name may not
// open a column half a window wide before the user has asked for anything.
constexpr int kSeedNameMaxChars = 40;
// What "Fit to Contents" is allowed to measure, so the menu item costs the same on a
// ten-million-record log as on a small one.
constexpr int kFitSampleRecords = 400;
constexpr int kFitNamesScanned = 20000;
// A drag that has left the viewport scrolls on a timer (SPEC.md §5). 50 ms is a rate a
// reader can steer by rather than one that overshoots the record they meant, and the
// per-tick distance grows with how far outside the viewport the pointer is, so a long
// haul does not take a minute — capped, because past a few lines a tick nobody can see
// what is being selected anyway.
constexpr int kAutoScrollIntervalMs = 50;
constexpr int kAutoScrollMaxLines = 8;

int clampColumnWidth(int w) { return qBound(kMinColumnWidth, w, kMaxColumnWidth); }

// How many runs one cell will mark. A one-character query over a hundred-thousand-
// character message has a match per character, and this is the paint path (§7.1.4).
constexpr int kMaxCellMarks = 64;

// What Find matched, and what to paint it in (SPEC.md §5, ARCHITECTURE.md §7.1.4).
// `find` is the query the search ran — never a precomputed list of positions — so a
// cell is marked from the same text the same paint just decoded, and nothing is held
// for a record off screen (invariant #1). The two colours are the RECORD's own,
// SWAPPED: whatever contrast that record already had, a rule's colours or the theme's,
// the marked run has exactly, in either theme, and no palette entry can drift from it.
struct CellMark
{
    const TextMatcher *find = nullptr;
    QColor             bg;   // the record's foreground
    QColor             fg;   // the record's background
    bool active() const { return find != nullptr; }
};

// Repaint the glyphs under `patches` in the mark's colours. The text is redrawn by the
// SAME call that drew it, merely clipped — never re-positioned by hand — so the mark
// cannot drift from the glyphs it is meant to be under.
//
// Every run of one cell is collected into ONE region and redrawn ONCE, because that
// call draws the WHOLE cell and only the clip narrows it: per run it would be
// O(runs x display lines) whole-paragraph redraws, and both multipliers are capped
// rather than small — 64 runs over 100 lines (§7.1.4). A region is what keeps
// "redrawn by the same call" true at O(1) calls per cell.
template <class DrawFn>
void paintMarks(QPainter &p, const QRegion &patches, const CellMark &mark, DrawFn &&draw)
{
    if (patches.isEmpty())
        return;
    // Every run is filled BEFORE anything is redrawn: a fill issued after a redraw
    // would erase the glyphs of any run it touched.
    for (const QRect &patch : patches)
        p.fillRect(patch, mark.bg);
    p.save();
    p.setClipRegion(patches, Qt::IntersectClip);
    p.setPen(mark.fg);
    draw();
    p.restore();
}

// What `QPainter::drawText()` lays a single line out at: with no word wrap asked for it
// gives the line an unbounded width rather than the rect's, so nothing it draws is ever
// re-broken and a left-aligned run starts at the rect's left edge whatever its width.
constexpr int kUnwrappedLineWidth = std::numeric_limits<int>::max();

// The substitutions `QPainter::drawText()` makes for `Qt::TextSingleLine` before it lays
// text out: a tab is one blank where no `Qt::TextExpandTabs` was asked for, and a line
// break is one blank because the line is single. Each is one character for one, so every
// `TextMatcher::Span` offset still means the character it meant — the same discipline
// `paragraphForLayout()` keeps for the wrapped path.
QString singleLineForLayout(const QString &text)
{
    QString s = text;
    s.replace(QLatin1Char('\t'), QLatin1Char(' '));
    s.replace(QLatin1Char('\r'), QLatin1Char(' '));
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return s;
}

// One fixed-height cell, ELIDED at the column's right edge rather than left to clip
// mid-glyph (SPEC.md §5). The ellipsis is the only thing that tells a value too wide
// for its column from one that genuinely ends there — and it is what makes the tooltip
// honest, since both ask the same question of the same width.
void drawElidedCell(QPainter &p, const QRect &rect, const QString &text,
                    const CellMark &mark = CellMark())
{
    constexpr int kFlags = Qt::AlignVCenter | Qt::TextSingleLine;
    const QString shown = p.fontMetrics().elidedText(text, Qt::ElideRight, rect.width());
    p.drawText(rect, kFlags, shown);
    if (!mark.active())
        return;

    // The runs are found in the string AS DRAWN, so what the ellipsis took away is
    // simply not marked: a match past the cut is not on screen, and the honest answer
    // to "where is it" is to say nothing rather than to point at the ellipsis.
    const QVector<TextMatcher::Span> spans = mark.find->spans(shown, kMaxCellMarks);
    if (spans.isEmpty())
        return;

    // Where each run LANDED — asked of a layout of the very string just drawn, exactly
    // as the wrapped path asks its own layout. A sum of per-character advances is not
    // the visual x of a shaped run: it is right for Latin and wrong for Arabic, Hebrew
    // and Indic, where a mark placed that way covers the wrong glyphs. It is also the
    // cheaper of the two, since a prefix advance per run end is a shaping pass per run.
    QTextLayout layout(singleLineForLayout(shown), p.font());
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid())
        line.setLineWidth(kUnwrappedLineWidth); // what QPainter::drawText lays a single line out at
    layout.endLayout();
    if (!line.isValid())
        return;

    QRegion patches;
    for (const TextMatcher::Span &span : spans) {
        const int x0 = int(line.cursorToX(span.start));
        const int x1 = int(line.cursorToX(span.start + span.length));
        const QRect patch = QRect(rect.left() + qMin(x0, x1), rect.top(),
                                  qMax(1, qAbs(x1 - x0)), rect.height())
                                .intersected(rect);
        patches += patch;
    }
    paintMarks(p, patches, mark, [&] { p.drawText(rect, kFlags, shown); });
}

// Wrapped text is laid out HERE, by the marked and the unmarked path alike, and that is
// the whole point. Qt offers no `drawText` flag pair that reaches
// `QTextOption::WrapAtWordBoundaryOrAnywhere` — `Qt::TextWordWrap | Qt::TextWrapAnywhere`
// behaves as plain `WrapAnywhere` — so for as long as one path used flags and the other a
// layout, a record re-flowed to different line breaks the moment Find was armed, and lost
// whatever text the extra lines carried (bugs.md 4). One function lays every wrapped cell
// out, `LogView::measureWrappedLines()` counts through the same one, and the wrap mode is
// the only thing that varies between the two renderings.
QTextOption wrapOptionFor(bool wordWrap)
{
    QTextOption option;
    // AlwaysOn wraps ANYWHERE and must go on doing so: §7.1.1's height model is
    // ceil(chars / cols), which only a character wrap satisfies. SelectedRecordOnly means
    // "read this one record in full", so it breaks at word boundaries and falls back to
    // anywhere for a word wider than the column.
    option.setWrapMode(wordWrap ? QTextOption::WrapAtWordBoundaryOrAnywhere
                                : QTextOption::WrapAnywhere);
    return option;
}

// One paragraph as it is laid out. A `QTextLayout` advances U+0009 to a tab stop — 80 px
// by default, about eleven columns at the reference face — where `drawText` gave it the
// width of one ordinary character and where the height model counts it as one. So a tab is
// laid out as a single blank, which renders pixel-identically to what `drawText` drew and
// keeps a TAB-indented stack trace breaking where it always broke. The substitution is one
// character for one, so every span offset still means the character it meant.
QString paragraphForLayout(QStringView paragraph)
{
    QString s = paragraph.toString();
    s.replace(QLatin1Char('\t'), QLatin1Char(' '));
    return s;
}

// Lay `text` out at `width` and hand each paragraph to `visit(layout, lines, base)`.
// Returns the display lines used. A record is not a line (invariant #2): the message's own
// newlines are paragraph breaks, which `QTextLayout` does not take for itself, and the
// string split is a DECODED one, never raw bytes (invariant #8).
template <class Visit>
int layoutWrappedText(const QString &text, const QFont &font, int width, bool wordWrap,
                      int maxLines, int lineHeight, Visit &&visit)
{
    const QTextOption option = wrapOptionFor(wordWrap);
    const QList<QStringView> paragraphs = QStringView(text).split(QLatin1Char('\n'));
    int placed = 0; // display lines used so far by this record
    int base = 0;   // character offset of this paragraph within `text`
    for (const QStringView &paragraph : paragraphs) {
        if (placed >= maxLines)
            break;
        QTextLayout layout(paragraphForLayout(paragraph), font);
        layout.setTextOption(option);
        layout.beginLayout();
        QVector<QTextLine> lines;
        while (placed + lines.size() < maxLines) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(width);
            // Positioned on the view's own line pitch rather than the layout's, so a record
            // occupies exactly the lines the geometry model gave it.
            line.setPosition(QPointF(0, (placed + lines.size()) * lineHeight));
            lines.append(line);
        }
        layout.endLayout();
        visit(layout, lines, base);
        placed += int(lines.size());
        base += int(paragraph.size()) + 1; // the newline itself
    }
    return qMax(1, placed);
}

// The wrapped message cell. The text is laid out whether or not there is anything to mark,
// because a mark has to know where each character landed — the only way to be sure of that
// is to have placed it — and because a reader must not see the record re-break when the
// search arms. The same `QTextLayout` draws the text and answers where the run is, so the
// two cannot disagree.
void drawWrappedCell(QPainter &p, const QRect &rect, const QString &text, int lineHeight,
                     bool wordWrap, const CellMark &mark)
{
    QVector<TextMatcher::Span> spans;
    if (mark.active())
        spans = mark.find->spans(text, kMaxCellMarks);

    const int lh = qMax(1, lineHeight);
    const int maxLines = qMax(1, rect.height() / lh);

    p.save();
    p.setClipRect(rect, Qt::IntersectClip); // drawText clipped to its rect; a layout does not

    layoutWrappedText(
        text, p.font(), rect.width(), wordWrap, maxLines, lh,
        [&](QTextLayout &layout, const QVector<QTextLine> &lines, int base) {
            const auto drawLayout = [&] { layout.draw(&p, rect.topLeft()); };
            drawLayout();
            if (spans.isEmpty())
                return;

            // Every run of every line of this paragraph, collected before anything is
            // redrawn: `drawLayout` draws the whole paragraph, so one call per run would
            // redraw up to 100 display lines up to 64 times over (§7.1.4).
            QRegion patches;
            for (const QTextLine &line : lines) {
                const int lineStart = line.textStart();
                const int lineEnd = lineStart + line.textLength();
                for (const TextMatcher::Span &span : spans) {
                    // Paragraph-local, and clipped to this line: a match may straddle a
                    // wrapped-line boundary, and then it is marked on BOTH lines.
                    const int from = qMax(span.start - base, lineStart);
                    const int to = qMin(span.start + span.length - base, lineEnd);
                    if (to <= from)
                        continue;
                    const int x0 = int(line.cursorToX(from));
                    const int x1 = int(line.cursorToX(to));
                    const QRect patch =
                        QRect(rect.left() + qMin(x0, x1), rect.top() + int(line.position().y()),
                              qMax(1, qAbs(x1 - x0)), lh)
                            .intersected(rect);
                    patches += patch;
                }
            }
            paintMarks(p, patches, mark, drawLayout);
        });
    p.restore();
}
} // namespace

// ---------------------------------------------------------------------------
// Pure geometry mapping (exact mode). The base RecordIndex prefix sums give the
// unwrapped line<->record mapping; here we fold in the one selected record whose
// wrapped height differs, patching a single delta rather than rebuilding (§7.1.1).
// ---------------------------------------------------------------------------

static qint64 selExtraLines(const RecordIndex &idx, int selRecord, int selWrapLines)
{
    if (selRecord < 0 || selRecord >= idx.records.size())
        return 0;
    // Wrapping never merges physical lines, so after the shared 100-line display
    // cap the wrapped height is >= the unwrapped height: the delta is non-negative.
    return qMax<qint64>(0, qint64(selWrapLines) - RecordIndex::displayLines(idx.records.at(selRecord)));
}

qint64 LogView::totalScrollLines(const RecordIndex &idx, int selRecord, int selWrapLines)
{
    return idx.totalLines() + selExtraLines(idx, selRecord, selWrapLines);
}

qint64 LogView::scrollLineOfRecord(const RecordIndex &idx, int selRecord, int selWrapLines, int r)
{
    qint64 line = idx.firstLineOfRecord(r);
    if (selRecord >= 0 && r > selRecord)
        line += selExtraLines(idx, selRecord, selWrapLines);
    return line;
}

int LogView::recordAtScrollLine(const RecordIndex &idx, int selRecord, int selWrapLines, qint64 line)
{
    const qint64 extra = selExtraLines(idx, selRecord, selWrapLines);
    if (extra == 0 || selRecord < 0)
        return idx.recordAtLine(line);

    const qint64 selStart = idx.firstLineOfRecord(selRecord);
    if (line < selStart)
        return idx.recordAtLine(line);
    if (line < selStart + selWrapLines)
        return selRecord; // inside the wrapped selected record
    return idx.recordAtLine(line - extra); // records after the selected one
}

// ---------------------------------------------------------------------------
// Pure clipboard helpers
// ---------------------------------------------------------------------------

QString LogView::flattenCell(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('\t'), QLatin1Char(' '));
    out.replace(QLatin1Char('\r'), QLatin1Char(' '));
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return out;
}

QString LogView::columnsToTsv(const QVector<QVector<QString>> &rows)
{
    QStringList lines;
    lines.reserve(rows.size());
    for (const QVector<QString> &cells : rows) {
        QStringList joined;
        joined.reserve(cells.size());
        for (const QString &c : cells)
            joined << c;
        lines << joined.join(QLatin1Char('\t'));
    }
    return lines.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LogView::LogView(const Document *document, LogModel *model, QWidget *parent, Role role)
    : QAbstractScrollArea(parent), m_document(document), m_model(model), m_role(role)
{
    // Every column, and the header, render in the same fixed-pitch font: cells
    // line up vertically, and the estimated-geometry path's character-count model
    // stays valid (invariant #6, ARCHITECTURE.md §7.1.1).
    // At the application's current log-text size (Fonts.h), so a view opened after a
    // zoom opens zoomed and nothing has to push a font into it afterwards.
    setFont(logTextFont());
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setFocusProxy(this);

    m_header = new QHeaderView(Qt::Horizontal, this);
    m_header->setModel(m_model);
    m_header->setSectionsMovable(true);
    m_header->setSectionsClickable(false);
    m_header->setSectionResizeMode(QHeaderView::Interactive);
    m_header->setStretchLastSection(false);
    m_header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // A caption reading "Priorit" says nothing about being a caption cut short, so the
    // header elides like the cells under it and names itself in full on hover. The
    // tooltip is filtered rather than answered through the model's ToolTipRole, which
    // QHeaderView would show for every section whether or not it fitted.
    m_header->setTextElideMode(Qt::ElideRight);
    m_header->viewport()->installEventFilter(this);
    // Widths that fit what is in them, measured from the font (SPEC.md §5). At this
    // point the index is empty, so this is the caption and the per-role allowance; the
    // scan-completion seed refines the Subsystem/Thread columns from the intern tables.
    seedColumnWidths();

    m_selection = new QItemSelectionModel(m_model, this);

    // Debounces width-change remeasurement in AlwaysOn (§7.1.1): a drag-resize
    // fires many events, so remeasure once when it settles rather than per frame.
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(120);
    connect(m_resizeTimer, &QTimer::timeout, this, &LogView::applyDebouncedResize);

    verticalScrollBar()->setSingleStep(1);

    connect(m_header, &QHeaderView::sectionResized, this,
            [this](int logical, int oldSize, int newSize) {
                // A width the USER chose is theirs from here on and no later seed may
                // touch it. Hiding and unhiding a section resizes it to and from zero,
                // which is not somebody dragging a divider, so both ends are excluded.
                if (!m_applyingColumnWidths && oldSize > 0 && newSize > 0)
                    markUserSized(logical);
                recomputeGeometry();
                emit columnLayoutChanged();
            });
    // Double-clicking a divider fits the column to its left (SPEC.md §5). QHeaderView
    // emits this and leaves the resizing to whoever is driving it — a QTableView would
    // resize to contents here; with no view behind the header, nothing did.
    connect(m_header, &QHeaderView::sectionHandleDoubleClicked, this,
            [this](int logical) { fitColumnToContents(logical); });
    // Moving a column across the message column moves that column's ORIGIN, which is
    // exactly what the wrapped height is measured against (§7.1.1) — so a move is a
    // width change to everything downstream of it and has to re-measure exactly as a
    // resize does. It is emphatically NOT a no-op outside AlwaysOn: in
    // SelectedRecordOnly this is what re-measures m_selWrapCache at the new origin.
    // Not markUserSized(), though: moving a column is not sizing it.
    connect(m_header, &QHeaderView::sectionMoved, this, [this](int, int, int) {
        recomputeGeometry();
        emit columnLayoutChanged();
    });
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &LogView::handleRowsInserted);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &LogView::handleRowsRemoved);
    connect(m_model, &QAbstractItemModel::modelReset, this, &LogView::handleModelReset);
    // A trailing record that grew in place (M6 live append with no new rows) arrives
    // as a dataChanged, not an insert; it changes that record's height, so refresh
    // geometry and keep following if attached.
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &, const QModelIndex &, const QList<int> &) { handleTailChanged(); });

    // Return-to-bottom control (SPEC.md §3): a small overlay shown only when follow
    // has detached; clicking it re-attaches and jumps to the newest record. A digest
    // strip does not scroll, so it has nothing to detach from and no button to offer.
    if (m_role == Role::Main) {
        m_followButton = new QToolButton(viewport());
        m_followButton->setText(tr("Follow tail ↓"));
        m_followButton->setToolTip(tr("Jump to the newest record and follow new ones"));
        m_followButton->setCursor(Qt::PointingHandCursor);
        m_followButton->setAutoRaise(false);
        m_followButton->hide();
        connect(m_followButton, &QToolButton::clicked, this, &LogView::followTail);
    }

    if (m_role == Role::Digest) {
        // The strip borrows the table's column layout rather than carrying a second
        // set of captions, and it is sized to its rows rather than scrolled — so both
        // scrollbars go, and the vertical one comes back only when the height cap bites
        // (see sizeHint). Its height is its whole contract, so it must be able to ask
        // for one and must not be stretched past it.
        m_header->hide();
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        // Click-focus, not strong: Tab must not stop on a strip of at most a few rows
        // on the way from the table to the Find bar.
        setFocusPolicy(Qt::ClickFocus);
        // Its rows are a different ordinal space from the main view's, so the size hint
        // has to be recomputed whenever the content changes rather than only on resize.
        connect(m_model, &QAbstractItemModel::modelReset, this, [this] { refreshDigestCap(); });
        connect(m_model, &QAbstractItemModel::rowsInserted, this, [this] { refreshDigestCap(); });
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, [this] { refreshDigestCap(); });
    }

    layoutHeader();
    recomputeGeometry();
}

LogView::~LogView() = default;

// ---------------------------------------------------------------------------
// Basic metrics
// ---------------------------------------------------------------------------

// The pitch a display line is drawn at, and it must be the number QT lays text out
// at — not QFontMetrics::height(), which rounds ascent and descent SEPARATELY while
// QTextLine::height() is ceil(ascentF + descentF). The two differ by a pixel at over
// half the point sizes this font is offered at, and the whole height model is
// ceil(chars / cols) lines of this size (§7.1.1): one pixel short per line clips the
// bottom of a wrapped record and, past about thirteen lines, loses the last one
// entirely. qCeil(QFontMetricsF::height()) is that number at every size measured.
int LogView::lineHeight() const
{
    return qMax(1, qCeil(QFontMetricsF(fontMetrics()).height()));
}
int LogView::visibleLines() const { return qMax(1, viewport()->height() / lineHeight()); }
// Both go through the MODEL, not the document (M19, ARCHITECTURE.md §7.5): which
// subset a view shows is the model's question now that a second model can point at a
// second FilteredIndex over the same document. With no view index set — every view but
// the digest strip — these are exactly what they were.
int LogView::recordCount() const { return m_model->rowCount(); }
const RecordIndex &LogView::geom() const { return m_model->viewGeometry(); }

int LogView::messageColumn() const
{
    const QVector<Field> &fields = m_document->format().fields;
    for (int c = 0; c < fields.size(); ++c)
        if (fields.at(c).role == FieldRole::Message)
            return c;
    return -1;
}

bool LogView::columnIsOnScreen(int logical) const
{
    // A section of ZERO WIDTH counts as off, and not only for tidiness: QHeaderView
    // resizes a section to 0 BEFORE it marks it hidden, and the sectionResized that
    // reaches recomputeGeometry() is emitted from inside that resize — so asking
    // isSectionHidden() alone answers "still visible" at the one moment the geometry
    // has to change, and every record keeps the height its now-hidden message wanted.
    return logical >= 0 && !m_header->isSectionHidden(logical)
        && m_header->sectionSize(logical) > 0;
}

int LogView::wrappedMessageColumn() const
{
    // The message column WHEN THERE IS SOMETHING TO WRAP. A column the reader has
    // switched off in the header menu is not on screen, so measuring its text is
    // measuring nothing: AlwaysOn used to go on giving every record the height its
    // hidden message wanted, and the paint then skipped the section and left three
    // blank rows under every record with the scrollbar still spanning them. A hidden
    // message column reads exactly like a format with no %m, which is what this asks.
    const int c = messageColumn();
    return columnIsOnScreen(c) ? c : -1;
}

bool LogView::messageIsLastVisibleColumn() const
{
    // Walked back from the END of the visual order past every hidden section, not
    // compared against count() - 1: a column switched off after the message leaves the
    // message last on screen, and one switched off before it moves nothing.
    const int msgCol = wrappedMessageColumn();
    if (msgCol < 0)
        return false;
    for (int vi = m_header->count() - 1; vi > m_header->visualIndex(msgCol); --vi) {
        if (columnIsOnScreen(m_header->logicalIndex(vi)))
            return false;
    }
    return true;
}

int LogView::selRecordForGeometry() const
{
    if (m_wrapMode == WrapMode::SelectedRecordOnly && m_current >= 0 && m_current < recordCount())
        return m_current;
    return -1;
}

int LogView::measureWrappedLines(const QString &text, int width) const
{
    // Counted through the very function that paints it (§7.1.1, §7.1.4). This used to
    // divide a `QFontMetrics::boundingRect()` height, and no flag that function takes
    // measures word-boundary wrapping — `TextWordWrap | TextWrapAnywhere` measures a
    // character wrap — so the height and the text disagreed by a line wherever a word had
    // to move down one.
    const int lines =
        layoutWrappedText(text, font(), qMax(minWrapWidth(), width), /*wordWrap=*/true,
                          RecordIndex::kDisplayLineCap, lineHeight(),
                          [](QTextLayout &, const QVector<QTextLine> &, int) {});
    return qMin<int>(RecordIndex::kDisplayLineCap, qMax(1, lines));
}

int LogView::selWrapLines() const
{
    const int sel = selRecordForGeometry();
    if (sel < 0)
        return 0;
    if (m_selWrapCache > 0)
        return m_selWrapCache;
    return RecordIndex::displayLines(geom().records.at(sel));
}

int LogView::recordHeightLines(int r) const
{
    if (selRecordForGeometry() == r)
        return qMax(1, selWrapLines());
    return RecordIndex::displayLines(geom().records.at(r));
}

// ---------------------------------------------------------------------------
// Mode-branching geometry wrappers (invariant #6). This is the ONLY seam where
// the exact and estimated paths meet: each wrapper branches on the mode once.
// When not estimating() the wrappers forward to the exact statics with the
// selected-record wrap folded in, so the M2b exact path is byte-identical; the
// estimated machinery (m_estimated) is reached from nowhere else.
// ---------------------------------------------------------------------------

bool LogView::estimating() const
{
    // AlwaysOn with a message column to wrap. Without a message field — or with the
    // one there is switched off in the header menu — there is nothing width-dependent
    // to estimate, so we stay on the exact path and every record is its own physical
    // height.
    return m_wrapMode == WrapMode::AlwaysOn && wrappedMessageColumn() >= 0;
}

qint64 LogView::mapTotalLines() const
{
    if (estimating())
        return m_estimated.totalLines();
    return totalScrollLines(geom(), selRecordForGeometry(), selWrapLines());
}

qint64 LogView::mapLineOfRecord(int r) const
{
    if (estimating())
        return m_estimated.firstLineOfRecord(r);
    return scrollLineOfRecord(geom(), selRecordForGeometry(), selWrapLines(), r);
}

int LogView::mapRecordAtLine(qint64 line) const
{
    if (estimating())
        return m_estimated.recordAtLine(line);
    return recordAtScrollLine(geom(), selRecordForGeometry(), selWrapLines(), line);
}

int LogView::mapRecordHeightLines(int r) const
{
    if (estimating())
        return qMax(1, m_estimated.recordHeightLines(r));
    return recordHeightLines(r);
}

// ---------------------------------------------------------------------------
// Estimated-mode support (AlwaysOn only)
// ---------------------------------------------------------------------------

qreal LogView::charAdvance() const
{
    // One character's advance in the primary fixed-pitch face, UNROUNDED. Every integer
    // advance Qt offers truncates: at the reference face's shipped 9 pt both
    // averageCharWidth() and horizontalAdvance() answer 7 where the advance is 7.21875,
    // so a 379 px column measured 54 characters where only 52 fit, the record was given
    // one row too few, and everything past the fold was drawn nowhere — no ellipsis and
    // no tooltip, because a wrapped message deliberately offers neither (bugs.md 17).
    // Rounding the other way is no better: qCeil() undercounts and hangs a blank line off
    // every wrapped record. Keep the fraction and floor the division that uses it.
    return qMax(qreal(1), QFontMetricsF(font()).horizontalAdvance(QLatin1Char('0')));
}

int LogView::minWrapWidth() const
{
    // qCeil, not a truncating multiply: this width is handed straight to viewportCols(),
    // so a floor a pixel short of kMinWrapCols characters divides back out as one column
    // too many — and the qMax() there then re-raises the count to a width that cannot
    // hold it, which is one column of clipping surviving the fix above.
    return qCeil(kMinWrapCols * charAdvance());
}

int LogView::messageWrapWidth() const
{
    // The ONE expression every wrap width comes from (§7.1.1): the message column's
    // origin to the right edge of the viewport, floored at kMinWrapCols characters.
    //
    // The floor is what stops one record filling the screen (bugs.md 11). The columns
    // before the message are seeded from the intern tables when the scan finishes, and
    // on a narrow window their sum can reach or pass the viewport's right edge — which
    // left the wrap width at a single pixel, every record measuring the 100-line
    // display cap, and a 1400 px record against a 466 px viewport. The floor is in
    // characters and not pixels because log text zooms (Fonts.h).
    const int msgCol = wrappedMessageColumn();
    // Not the last visible column: the fields after it own the pixels past its divider,
    // so the cell wraps WITHIN ITS OWN SECTION. To the right edge regardless — which is
    // what shipped — and the message is laid out over every column after it and drawn
    // under their text on the record's first line, unreadably (bugs.md 18). It needs no
    // gesture to reach: `%d [%t] %-5p %m (%c)%n` is an ordinary pattern and puts the
    // subsystem after the message.
    if (msgCol >= 0 && !messageIsLastVisibleColumn())
        return qMax(minWrapWidth(), m_header->sectionSize(msgCol));
    const int x = msgCol >= 0 ? m_header->sectionViewportPosition(msgCol) : 0;
    return qMax(minWrapWidth(), viewport()->width() - x);
}

int LogView::viewportCols() const
{
    // Characters that fit across the wrapped message column. Fixed-pitch font, so this
    // is a divide, not a shaping pass (§7.1.1) — but a FLOORED divide by a FRACTIONAL
    // advance, which is what QTextLine actually fits at every point size and width the
    // zoom offers. An integer advance is a rounded one, and rounded down it hands the
    // record more columns than the layout can place (bugs.md 17).
    return qMax(kMinWrapCols, qFloor(messageWrapWidth() / charAdvance()));
}

void LogView::ensureEstimatorBound()
{
    // Bind (or rebind) the estimator to the current index, then fold in whatever
    // the tail has grown since it last looked — never a width change, which the
    // debounced resize owns.
    //
    // The tail sync is not an optimisation. This used to rebind on the index's
    // BLOCK COUNT, which moves once every kBlockSize records, so up to 4095 appends
    // into a partly-filled block changed nothing at all: the block stayed flagged
    // measured with a per-record height vector of its old length, and every record
    // past it was read out of that vector's end (bugs.md 1). A trailing record
    // growing continuation lines in place moved the count not at all and went on
    // being drawn at its stale height, with the scroll range never growing to reach
    // the lines it had gained.
    const RecordIndex &idx = geom();
    if (m_estimated.index() != &idx) {
        m_estimated.reset(&idx, viewportCols());
        return;
    }
    m_estimated.syncTail();
}

void LogView::measureBlock(int block)
{
    if (!estimating() || block < 0 || block >= m_estimated.blockCount()
        || m_estimated.isBlockMeasured(block))
        return;

    const int msgCol = messageColumn();
    const int cols = m_estimated.columns();
    const int cap = RecordIndex::kDisplayLineCap;
    const int start = block * RecordIndex::kBlockSize;
    const int n = recordCount();
    const int end = qMin(start + RecordIndex::kBlockSize, n);
    // Records whose exact height is already cached are NOT decoded again. After a
    // tail sync a measured block is short by only the records the growth touched —
    // typically one — and re-decoding the other 4095 on every ingest tick is the
    // cost that made keeping a partial measurement worth the machinery.
    const int first = start + m_estimated.measuredRecordsInBlock(block);

    QVector<quint16> lines;
    lines.reserve(qMax(0, end - first));
    QVector<int> lineChars;
    for (int r = first; r < end; ++r) {
        // Decoded message text via the model (invariant #8: split the decoded
        // string on '\n', never scan raw bytes). Only char counts are kept — no
        // parsed text is retained per record (invariant #1); the block cache
        // stores heights, and only for visited blocks.
        const QString msg = m_model->cellText(r, msgCol);
        lineChars.clear();
        int from = 0;
        while (true) {
            const int nl = msg.indexOf(QLatin1Char('\n'), from);
            if (nl < 0) {
                lineChars.append(int(msg.size()) - from);
                break;
            }
            lineChars.append(nl - from);
            from = nl + 1;
        }
        lines.append(quint16(EstimatedGeometry::measuredRecordLines(lineChars, cols, cap)));
    }
    m_estimated.measureBlock(block, lines, first - start);
}

void LogView::measureBlockOfRecord(int record)
{
    if (!estimating())
        return;
    ensureEstimatorBound();
    if (record >= 0 && record < recordCount())
        measureBlock(m_estimated.blockOfRecord(record));
    updateScrollBars();
    viewport()->update();
}

void LogView::measureVisibleBlocks()
{
    // Measure every block the viewport touches, then re-anchor so a height
    // refinement does not make the content under the cursor jump. Called from
    // paint; the whole thing is cached, so subsequent frames measure nothing.
    ensureEstimatorBound();
    const int n = recordCount();
    if (n == 0)
        return;

    const qint64 topLine = verticalScrollBar()->value();
    const int topRec = m_estimated.recordAtLine(topLine);
    if (topRec < 0)
        return;
    // A viewport of V lines holds at most V records (each >= 1 line), so
    // [topRec, topRec+V] bounds everything paintable this frame.
    const int lastRec = qMin(n - 1, topRec + visibleLines());
    const int b0 = m_estimated.blockOfRecord(topRec);
    const int b1 = m_estimated.blockOfRecord(lastRec);

    bool measured = false;
    for (int b = b0; b <= b1; ++b) {
        if (!m_estimated.isBlockMeasured(b)) {
            measureBlock(b);
            measured = true;
        }
    }
    if (measured) {
        // Refined heights change the total and can shift records; keep topRec put
        // (snap to its top) and refresh the scrollbar range without recursing.
        const QSignalBlocker guard(verticalScrollBar());
        updateScrollBars();
        const qint64 anchor = m_estimated.firstLineOfRecord(topRec);
        verticalScrollBar()->setValue(int(qMin<qint64>(anchor, verticalScrollBar()->maximum())));
    }
}

// ---------------------------------------------------------------------------
// Geometry / scrollbars / header layout
// ---------------------------------------------------------------------------

void LogView::recomputeGeometry()
{
    if (estimating()) {
        // Estimated path: no per-record selection wrap to fold in. Ensure the
        // estimator tracks the current index, then let the scrollbar reflect the
        // (refining) estimated total. Measurement happens lazily on paint.
        ensureEstimatorBound();
        m_selWrapCache = -1;
        updateScrollBars();
        viewport()->update();
        // A column resize changes the message width (hence the wrap column count)
        // just like a viewport resize does, so re-sync the width-keyed cache on
        // the same debounce rather than remeasuring on every drag step.
        m_resizeTimer->start();
        return;
    }

    const int sel = selRecordForGeometry();
    if (sel >= 0) {
        const int msgCol = wrappedMessageColumn();
        if (msgCol >= 0) {
            // The same width the paint lays the cell out in, from the same expression.
            // These used to floor independently — 50 px here against 10 px there — so
            // below ~28 px of available width the paint wrapped narrower than the
            // measure, needed more lines than the rows it had been given, and dropped
            // the tail of the selected record with no ellipsis and no tooltip.
            m_selWrapCache = measureWrappedLines(m_model->cellText(sel, msgCol), messageWrapWidth());
        } else {
            m_selWrapCache = RecordIndex::displayLines(geom().records.at(sel));
        }
    } else {
        m_selWrapCache = -1;
    }
    updateScrollBars();
    viewport()->update();
}

void LogView::updateScrollBars()
{
    const qint64 total = mapTotalLines(); // exact or estimated, per the mode
    const int page = visibleLines();
    // Clamped into int, not cast: a record measures at most kDisplayLineCap display
    // lines, so a log of 21 million records overflows and QAbstractSlider reads the
    // negative range as no range at all — the log could not be scrolled.
    verticalScrollBar()->setRange(
        0, int(qBound<qint64>(0, total - page, qint64(std::numeric_limits<int>::max()))));
    verticalScrollBar()->setPageStep(page);
    verticalScrollBar()->setSingleStep(1);

    const int len = m_header->length();
    horizontalScrollBar()->setRange(0, qMax(0, len - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setSingleStep(qMax(1, fontMetrics().averageCharWidth() * 2));
    m_header->setOffset(horizontalScrollBar()->value());
}

void LogView::layoutHeader()
{
    // A hidden header reserves NOTHING. Asking unconditionally would leave a
    // header-tall blank band above the digest strip — which reads as a rendering fault
    // rather than a bug, in the one widget whose whole claim is that it is exactly as
    // tall as its rows.
    if (m_header->isHidden()) {
        setViewportMargins(0, 0, 0, 0);
        return;
    }
    const int h = m_header->sizeHint().height();
    setViewportMargins(0, h, 0, 0);
    m_header->setGeometry(viewport()->x(), viewport()->y() - h, viewport()->width(), h);
}

qint64 LogView::digestContentLines(bool *capped) const
{
    const qint64 lines = m_model->rowCount() > 0 ? mapTotalLines() : 0;

    // The cap. A single digest record can legitimately be a hundred-line stack trace,
    // and a strip that ate the log it sits under would be worse than no strip. A third
    // of the parent's height, or kDigestMaxLines, whichever is smaller.
    qint64 capLines = kDigestMaxLines;
    if (const QWidget *p = parentWidget(); p && p->height() > 0)
        capLines = qMin<qint64>(capLines, qMax(1, p->height() / (3 * lineHeight())));

    const qint64 shown = qMin(lines, capLines);
    if (capped)
        *capped = shown < lines;
    return shown;
}

void LogView::refreshDigestCap()
{
    if (m_role != Role::Digest)
        return;
    bool capped = false;
    digestContentLines(&capped);
    // The strip is "not scrolled" in every ordinary case; the scrollbar comes back only
    // where the cap bit, so the content past it stays reachable rather than truncated.
    //
    // Guarded on an actual change, and kept OUT of sizeHint(), which must stay a pure
    // query: QAbstractScrollArea::setVerticalScrollBarPolicy() calls layoutChildren()
    // whether or not the policy moved, and layout asks for the size hint — so setting
    // it from inside the hint is an infinite recursion. It hung tst_multidoc.
    const Qt::ScrollBarPolicy wanted =
        capped ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff;
    if (verticalScrollBarPolicy() != wanted)
        setVerticalScrollBarPolicy(wanted);
    updateGeometry();
}

QSize LogView::sizeHint() const
{
    if (m_role != Role::Digest)
        return QAbstractScrollArea::sizeHint();

    // Display lines, not rows: a digest record renders at full height exactly as it
    // does in the log, which is the point of showing it here rather than summarising it.
    return QSize(QAbstractScrollArea::sizeHint().width(),
                 frameWidth() * 2 + int(digestContentLines(nullptr)) * lineHeight());
}

void LogView::setHorizontalOffset(int value)
{
    if (horizontalScrollBar()->value() == value)
        return;
    // Past the strip's own range when the table is wider than it is; clamping keeps the
    // columns as aligned as they can be rather than snapping back to zero.
    horizontalScrollBar()->setValue(
        qBound(horizontalScrollBar()->minimum(), value, horizontalScrollBar()->maximum()));
    m_header->setOffset(horizontalScrollBar()->value());
    viewport()->update();
}

void LogView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    layoutHeader();
    positionFollowButton();
    if (estimating()) {
        // Debounce (§7.1.1): a drag-resize fires a burst of these. Keep the view
        // usable now from the existing (possibly stale-width) estimates, and
        // remeasure once when the drag settles.
        ensureEstimatorBound();
        updateScrollBars();
        viewport()->update();
        m_resizeTimer->start();
        return;
    }
    recomputeGeometry();
}

void LogView::applyDebouncedResize()
{
    if (!estimating())
        return;
    ensureEstimatorBound();
    const int topRec = m_estimated.recordAtLine(verticalScrollBar()->value());
    if (m_estimated.setColumns(viewportCols())) {
        // Width actually changed: every measurement was width-keyed and is now
        // dropped, so the total falls back to estimates until blocks are
        // remeasured on the next paint. Re-anchor on the top record so the
        // content does not jump under the new geometry.
        const QSignalBlocker guard(verticalScrollBar());
        updateScrollBars();
        if (topRec >= 0) {
            const qint64 anchor = m_estimated.firstLineOfRecord(topRec);
            verticalScrollBar()->setValue(int(qMin<qint64>(anchor, verticalScrollBar()->maximum())));
        }
    }
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Zoom (SPEC.md §5, ARCHITECTURE.md §7.1.5)
// ---------------------------------------------------------------------------

void LogView::wheelEvent(QWheelEvent *event)
{
    if (!(event->modifiers() & Qt::ControlModifier)) {
        QAbstractScrollArea::wheelEvent(event); // a plain wheel scrolls, as it always did
        return;
    }
    // angleDelta is in eighths of a degree and a mouse notch is 120 of them, but a
    // trackpad sends a stream of small deltas — so the remainder is ACCUMULATED rather
    // than rounded away, or a two-finger pinch-ish scroll would zoom either wildly or
    // not at all depending on the device.
    m_wheelZoomRemainder += event->angleDelta().y();
    constexpr int kNotch = 120;
    const int steps = m_wheelZoomRemainder / kNotch;
    if (steps != 0) {
        m_wheelZoomRemainder -= steps * kNotch;
        emit zoomStepRequested(steps);
    }
    event->accept();
}

void LogView::changeEvent(QEvent *event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange)
        applyFontChange();
}

void LogView::applyFontChange()
{
    // The constructor sets the font BEFORE it builds the header, and QAbstractScrollArea
    // is already delivering events to this object by then — the same trap m_header's
    // nullptr initializer records for eventFilter(). Nothing below survives running
    // early, and nothing below is needed: the constructor does all of it anyway.
    if (!m_header || !m_selection)
        return;

    // Where the reader is, in RECORDS, captured before anything moves. Deliberately NOT
    // the filter-anchor bracket: that expresses the anchor in SOURCE ordinals because a
    // refilter replaces the view's record space, and a font change replaces nothing —
    // the same records in the same order, drawn at a different height. The precedent it
    // does follow is applyDebouncedResize(), which re-anchors on the top record for
    // exactly this reason.
    const int n = recordCount();
    int    topRecord = -1;
    qint64 intoRecord = 0; // display lines scrolled INTO that record
    if (n > 0) {
        const qint64 top = verticalScrollBar()->value();
        topRecord = mapRecordAtLine(top);
        if (topRecord >= 0)
            intoRecord = qMax<qint64>(0, top - mapLineOfRecord(topRecord));
    }

    // The exact path's one cached height: measured in PIXELS and divided by the line
    // height, so both halves of it have just moved.
    m_selWrapCache = -1;
    // Everything below narrows or widens the scroll range, and Qt CLAMPS the old value
    // into a range that shrank. A clamp that lands at the bottom reaches
    // updateFollowFromScrollPosition() as an ordinary scroll, so a view the reader had
    // detached starts following again and the zoom throws them to the end of the log —
    // the trap endFilterUpdate() records for the filter bracket, on the one other path
    // that replaces the line space without replacing the records. A flag and not a
    // QSignalBlocker, for the reason that bracket gives: blocking the scrollbar
    // suppresses rangeChanged, whose connection to QAbstractScrollArea's own
    // show/hide handling is queued.
    const bool wasFollowScroll = m_inFollowScroll;
    m_inFollowScroll = true;
    // Columns nobody has spoken for are re-seeded in the new font's characters, so they
    // grow and shrink with the text in them; a width that was dragged, fitted or
    // restored is left exactly where it was put, because a zoom is a statement about
    // text size and not about somebody's column layout.
    seedColumnWidths();
    // The header band is as tall as the header's own font asks for.
    layoutHeader();
    positionFollowButton();
    // THE ONE THAT IS EASY TO MISS. ensureEstimatorBound() rebinds on the index's
    // ADDRESS and folds in what its TAIL has grown, and a font change moves neither —
    // so on its own it leaves the estimator holding wrapped heights measured at the old
    // character advance, i.e. at a column count that no longer holds. setColumns() is
    // what drops them, and it must come AFTER the re-seed above, which moves the message
    // column's origin and therefore the count itself.
    if (m_estimated.isBound())
        m_estimated.setColumns(viewportCols());
    recomputeGeometry();
    // Digest role only, a no-op in the others: the strip's height is a line count times
    // the line height, and both ends of that just changed.
    refreshDigestCap();

    // m_following is what it was before any of the above, because the guard is what
    // stopped the clamp from rewriting it.
    if (m_following) {
        m_inFollowScroll = wasFollowScroll;
        scrollToEnd(); // following the tail outranks the anchor (SPEC.md §3)
        return;
    }
    if (topRecord >= 0 && topRecord < recordCount()) {
        const qint64 height = qMax(1, mapRecordHeightLines(topRecord));
        const qint64 target = mapLineOfRecord(topRecord) + qMin(intoRecord, height - 1);
        verticalScrollBar()->setValue(
            int(qBound<qint64>(0, target, qint64(verticalScrollBar()->maximum()))));
    }
    m_inFollowScroll = wasFollowScroll;
}

void LogView::scrollContentsBy(int dx, int dy)
{
    if (dx != 0) {
        m_header->setOffset(horizontalScrollBar()->value());
        emit horizontalOffsetChanged(horizontalScrollBar()->value());
        // A horizontal scroll moves the message column's origin left, so the space the
        // message wraps within — its origin to the viewport's right edge — grows with
        // it, exactly as a resize or a column move changes it. The measurements are
        // width-keyed, so they are stale from here and something has to say so; in
        // AlwaysOn this restarts the debounce rather than remeasuring, which is what a
        // dragged scrollbar wants (one remeasure when it settles, not one per pixel).
        recomputeGeometry();
    }
    if (dy != 0)
        updateFollowFromScrollPosition(); // a vertical move may detach/re-attach follow
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void LogView::resolveRowColors(int row, bool selected, QColor &bg, QColor &fg) const
{
    // Selection always wins — a highlighted record is still clearly selectable.
    if (selected) {
        bg = palette().highlight().color();
        fg = palette().highlightedText().color();
        return;
    }

    // Highlight rules (M5, SPEC.md §7): first-match-wins is evaluated in
    // LogModel::data(), which hands back the matched rule's palette colors via the
    // Background/Foreground roles — or an empty variant meaning "leave this role at
    // the theme default". An invalid/absent background keeps the base fill with the
    // alternating-record band; an invalid foreground keeps the theme text color.
    // One call, not two data() lookups: highlighting is per record, and a rule may
    // now match on message text, so resolving the roles separately would run the rule
    // list — and potentially the decode — twice per painted record (SPEC.md §7).
    QColor ruleBg, ruleFg;
    m_model->rowColors(row, ruleBg, ruleFg);

    if (ruleBg.isValid()) {
        // A rule-coloured record wears its own colour, unbanded: the band is what a
        // record wears when nothing else has claimed it, and tinting every other one
        // of a rule's records would read as two rules.
        bg = ruleBg;
    } else {
        // `row` is a RECORD, not a line (invariant #2), and the caller fills the whole
        // record's rect with what comes back — so the band changes exactly at a record
        // boundary, which in wrap-always-on is the only thing marking one.
        bg = (row % 2) ? alternateRowColor(palette()) : palette().base().color();
    }

    fg = ruleFg.isValid() ? ruleFg : palette().text().color();

    // Filter context (M15, SPEC.md §6): a record shown only because a neighbour
    // matched recedes, so the matches stay findable by eye. Last, and below
    // selection: it modifies whatever the row would otherwise have been, including a
    // highlight rule's colours — which are softened rather than dropped, or a
    // rule-coloured context row would read as a match.
    if (m_model->rowIsContext(row)) {
        if (ruleBg.isValid())
            bg = contextFillColor(bg, palette().base().color());
        fg = contextTextColor(fg, bg);
    }
}

bool LogView::marking() const
{
    // An empty query matches everything, and marking everything is not a mark; a regex
    // that will not compile matched nothing to land on in the first place.
    return !m_findMatcher.isEmpty() && m_findMatcher.isValid();
}

void LogView::setFindMatcher(const TextMatcher &matcher)
{
    m_findMatcher = matcher;
    viewport()->update(); // the marks are re-derived per paint; asking for one is all it takes
}

void LogView::clearFindMatcher()
{
    if (m_findMatcher.isEmpty())
        return;
    m_findMatcher = TextMatcher();
    viewport()->update();
}

void LogView::paintEvent(QPaintEvent *event)
{
    QPainter p(viewport());
    p.fillRect(event->rect(), palette().base());

    const RecordIndex &idx = geom();
    const int n = idx.records.size();
    if (n == 0) {
        // An empty grid is indistinguishable from an empty log, so a view with nothing
        // in it says WHY when it has something to say — "app.log has not appeared yet"
        // (SPEC.md §3). Drawn here rather than as a swapped-in widget because the tab
        // is a real, live view throughout: it keeps its filters, its column layout and
        // its place in the session, and gains rows the moment the log turns up.
        if (!m_placeholderText.isEmpty()) {
            p.setPen(mutedColor(palette()));
            p.drawText(viewport()->rect(), Qt::AlignCenter | Qt::TextWordWrap,
                       m_placeholderText);
        }
        return;
    }

    const int lh = lineHeight();

    // What Find matched, marked in the record's own two colours swapped (SPEC.md §5).
    // Per record, because those two colours are: a rule's, the band's, the selection's
    // or a context row's dimmed pair — whatever contrast the record already had, the
    // marked run has exactly, in either theme.
    const bool mark = marking();
    const auto markFor = [this, mark](const QColor &bg, const QColor &fg) {
        CellMark m;
        if (mark) {
            m.find = &m_findMatcher;
            m.bg = fg;
            m.fg = bg;
        }
        return m;
    };

    // Estimated (wrap-always-on) path — kept entirely separate from the exact
    // painting below so the exact path never routes through estimation (#6).
    if (estimating()) {
        measureVisibleBlocks(); // one-time per block; cached, re-anchors if refined

        const qint64 topLine = verticalScrollBar()->value();
        int r = m_estimated.recordAtLine(topLine);
        if (r < 0)
            return;
        int y = int((m_estimated.firstLineOfRecord(r) - topLine) * lh);

        const QVector<Field> &fields = m_document->format().fields;
        const int msgCol = messageColumn();
        const int vh = viewport()->height();
        const int vw = viewport()->width();
        // Constant for the whole paint, and the number the heights below were measured
        // in: read once rather than per record on the paint path. So is whether the
        // message is the last thing on the row, which is what decides whether the width
        // it was measured in is also the width it may be drawn in.
        const int msgW = messageWrapWidth();
        const bool msgLast = messageIsLastVisibleColumn();

        while (r < n && y < vh) {
            const int hLines = qMax(1, m_estimated.recordHeightLines(r));
            const int rowH = hLines * lh;
            const bool selected = m_selection->isSelected(m_model->index(r, 0));

            QColor band, fg;
            resolveRowColors(r, selected, band, fg);
            p.fillRect(QRect(0, y, vw, rowH), band);
            p.setPen(fg);
            const CellMark mark = markFor(band, fg);

            for (int vi = 0; vi < fields.size(); ++vi) {
                const int logical = m_header->logicalIndex(vi);
                if (logical < 0 || m_header->isSectionHidden(logical))
                    continue;
                const int x = m_header->sectionViewportPosition(logical);
                const int w = m_header->sectionSize(logical);
                if (logical == msgCol) {
                    // The width the height was measured in, from the one expression.
                    const int availW = msgW;
                    // Measured at the floor, CLIPPED at the section (§7.1.1). The two are
                    // the same number whenever the message is last, and whenever its
                    // section is at least kMinWrapCols wide; where they differ the tail
                    // of each line is lost rather than drawn over the next column's text,
                    // which is the trade SPEC.md §5 states. Clipped rather than laid out
                    // narrower, because laying out at `w` would break the lines somewhere
                    // else than the height was counted for.
                    const bool clip = !msgLast && w < availW;
                    if (clip) {
                        p.save();
                        p.setClipRect(QRect(x, y, w, rowH));
                    }
                    // Character wrapping (WrapAnywhere) so the painted height
                    // matches the ceil(chars/cols) measurement model exactly.
                    drawWrappedCell(p, QRect(x, y, availW, rowH), m_model->cellText(r, logical),
                                    lh, /*wordWrap=*/false, mark);
                    if (clip)
                        p.restore();
                } else {
                    drawElidedCell(p, QRect(x, y, w, lh), m_model->cellText(r, logical), mark);
                }
            }
            y += rowH;
            ++r;
        }
        return;
    }

    const int sel = selRecordForGeometry();
    const int selW = selWrapLines();
    const qint64 topLine = verticalScrollBar()->value();

    int r = recordAtScrollLine(idx, sel, selW, topLine);
    if (r < 0)
        return;
    const qint64 rTop = scrollLineOfRecord(idx, sel, selW, r);
    int y = int((rTop - topLine) * lh); // <= 0 when scrolled into a record

    const QVector<Field> &fields = m_document->format().fields;
    const int msgCol = messageColumn();
    const int vh = viewport()->height();
    const int vw = viewport()->width();
    const int msgW = messageWrapWidth(); // what selWrapLines() measured in
    const bool msgLast = messageIsLastVisibleColumn();

    while (r < n && y < vh) {
        const int hLines = recordHeightLines(r);
        const int rowH = hLines * lh;
        const bool selected = m_selection->isSelected(m_model->index(r, 0));

        QColor band, fg;
        resolveRowColors(r, selected, band, fg);
        p.fillRect(QRect(0, y, vw, rowH), band);
        p.setPen(fg);
        const CellMark mark = markFor(band, fg);

        const bool wrapThis = (sel == r);
        for (int vi = 0; vi < fields.size(); ++vi) {
            const int logical = m_header->logicalIndex(vi);
            if (logical < 0 || m_header->isSectionHidden(logical))
                continue;
            const int x = m_header->sectionViewportPosition(logical);
            const int w = m_header->sectionSize(logical);

            if (logical == msgCol) {
                const QString msg = m_model->cellText(r, logical);
                if (wrapThis) {
                    const int availW = msgW;
                    // Measured at the floor, clipped at the section: the same rule as
                    // the estimated path above, and the reason it is not scoped to
                    // AlwaysOn is that a message column moved off the end is drawn over
                    // the columns after it in either mode (bugs.md 18).
                    const bool clip = !msgLast && w < availW;
                    if (clip) {
                        p.save();
                        p.setClipRect(QRect(x, y, w, rowH));
                    }
                    drawWrappedCell(p, QRect(x, y, availW, rowH), msg, lh,
                                    /*wordWrap=*/true, mark);
                    if (clip)
                        p.restore();
                } else {
                    // Wrap off: each physical line of the record is a clipped line of
                    // its own (invariant #2), so each elides on its own too — and each
                    // is marked on its own, for the same reason.
                    const QList<QStringView> segs = QStringView(msg).split(QLatin1Char('\n'));
                    for (int li = 0; li < segs.size() && li < hLines; ++li)
                        drawElidedCell(p, QRect(x, y + li * lh, w, lh), segs.at(li).toString(),
                                       mark);
                }
            } else {
                drawElidedCell(p, QRect(x, y, w, lh), m_model->cellText(r, logical), mark);
            }
        }

        y += rowH;
        ++r;
    }
}

// ---------------------------------------------------------------------------
// Hit-testing, selection, navigation
// ---------------------------------------------------------------------------

int LogView::recordAtViewportY(int yPix) const
{
    const RecordIndex &idx = m_document->index();
    if (idx.records.isEmpty() || yPix < 0)
        return -1;
    const qint64 line = qint64(verticalScrollBar()->value()) + yPix / lineHeight();
    return mapRecordAtLine(line);
}

int LogView::recordUnderPoint(int y) const
{
    if (y < 0 || recordCount() == 0)
        return -1;
    const qint64 line = qint64(verticalScrollBar()->value()) + y / lineHeight();
    if (line >= mapTotalLines())
        return -1;
    const int record = mapRecordAtLine(line);
    return (record < 0 || record >= recordCount()) ? -1 : record;
}

void LogView::selectRange(int anchor, int current)
{
    const int cols = m_model->columnCount();
    if (cols == 0)
        return;
    const int lo = qMin(anchor, current);
    const int hi = qMax(anchor, current);
    const QItemSelection sel(m_model->index(lo, 0), m_model->index(hi, cols - 1));
    m_selection->select(sel, QItemSelectionModel::ClearAndSelect);
    m_selection->setCurrentIndex(m_model->index(current, 0), QItemSelectionModel::NoUpdate);
}

// Ctrl+click (SPEC.md §5). Toggle, not ClearAndSelect: what makes this gesture worth
// having is that everything else in the selection survives it — which is also why it
// goes through the selection model directly rather than through selectRange(), whose
// whole job is to replace the selection with one contiguous run.
void LogView::toggleRecordSelection(int record)
{
    const int n = recordCount();
    const int cols = m_model->columnCount();
    if (n == 0 || cols == 0)
        return;
    record = qBound(0, record, n - 1);
    // An explicit pick forgets a selection a filter had hidden, exactly as a plain click
    // does (SPEC.md §6): the user is choosing records here and now.
    m_stickySource = -1;
    const QItemSelection row(m_model->index(record, 0), m_model->index(record, cols - 1));
    m_selection->select(row, QItemSelectionModel::Toggle);
    m_selection->setCurrentIndex(m_model->index(record, 0), QItemSelectionModel::NoUpdate);
    // The focus and the anchor move even when the click took the record OUT: the anchor
    // is where the pointer last was, and a Shift+click after this must extend from there.
    m_current = record;
    m_anchor = record;
    recomputeGeometry();  // the focused record's wrapped height is part of the geometry
    ensureRecordVisible(record);
}

// Select All (SPEC.md §5). "All" is what is IN VIEW: recordCount() is the filtered
// subset while a filter is active (invariant #6), so the command narrows with the
// filters rather than reaching past them into the file.
//
// The whole thing is one QItemSelection range — a selection of four million records is
// one object, not four million — and no part of it walks the index.
void LogView::selectAllRecords()
{
    const int n = recordCount();
    const int cols = m_model->columnCount();
    if (n == 0 || cols == 0)
        return;
    // An explicit pick forgets a selection a filter had hidden, exactly as a click does
    // (SPEC.md §6): everything visible has just been chosen here and now.
    m_stickySource = -1;
    const QItemSelection all(m_model->index(0, 0), m_model->index(n - 1, cols - 1));
    m_selection->select(all, QItemSelectionModel::ClearAndSelect);
    // No scrolling: every record the reader can see is now selected wherever they are,
    // and jumping them to either end of the log would be the command's only visible
    // effect on a screenful they were reading. The anchor moves to the top so a
    // Shift+click afterwards narrows from there; the focus moves only if there was none,
    // since in SelectedRecordOnly it is what wraps.
    m_anchor = 0;
    if (m_current < 0) {
        m_current = 0;
        m_selection->setCurrentIndex(m_model->index(0, 0), QItemSelectionModel::NoUpdate);
    }
    recomputeGeometry();
}

// The selection as RANGES (ARCHITECTURE.md §7.1.6). QItemSelectionModel holds ranges,
// and Select All over four million records is exactly one of them; asking it for
// selectedRows(0) instead materialised four million QModelIndexes and copied their rows
// into a vector of the same length before a single character had been decoded.
//
// Merged and ascending, because a record must be copied once however many ranges name
// it — which is what answering in rows gave for free. Every selection this view builds
// spans the whole row (selectRange, toggleRecordSelection, selectAllRecords), so a range
// is a run of records and the full-row test selectedRows() applied is kept explicitly.
QVector<QPair<int, int>> LogView::selectedRecordRanges() const
{
    const int n = m_model->rowCount();
    const int cols = m_model->columnCount();
    QVector<QPair<int, int>> ranges;
    const QItemSelection sel = m_selection->selection();
    ranges.reserve(sel.size());
    for (const QItemSelectionRange &r : sel) {
        if (!r.isValid() || cols == 0 || r.left() > 0 || r.right() < cols - 1)
            continue;
        const int top = qMax(0, r.top());
        const int bottom = qMin(n - 1, r.bottom());
        if (top <= bottom)
            ranges.push_back({top, bottom});
    }
    // Nothing selected: the focused record, exactly as the per-row form fell back.
    if (ranges.isEmpty() && m_current >= 0 && m_current < n)
        ranges.push_back({m_current, m_current});

    std::sort(ranges.begin(), ranges.end());
    QVector<QPair<int, int>> merged;
    merged.reserve(ranges.size());
    for (const QPair<int, int> &r : ranges) {
        // `<= last.second + 1` also fuses two ranges that merely touch, so a Ctrl+click
        // that rebuilt a contiguous run out of pieces still costs one range to walk.
        if (!merged.isEmpty() && r.first <= merged.last().second + 1)
            merged.last().second = qMax(merged.last().second, r.second);
        else
            merged.push_back(r);
    }
    return merged;
}

qint64 LogView::rangeTotal(const QVector<QPair<int, int>> &ranges)
{
    qint64 total = 0;
    for (const QPair<int, int> &r : ranges)
        total += qint64(r.second) - r.first + 1;
    return total;
}

// What to reserve the output string at, from the 32-byte index entries and nothing else
// (invariant #1): the alternative is decoding every record twice, once to measure and
// once to keep. Integer work over the same ranges, so it costs nanoseconds per record
// against microseconds for the decode it sizes.
qint64 LogView::selectedByteLength(const QVector<QPair<int, int>> &ranges) const
{
    const RecordIndex &idx = m_document->index();
    qint64 bytes = 0;
    for (const QPair<int, int> &range : ranges) {
        for (int row = range.first; row <= range.second; ++row) {
            const int r = m_model->sourceRow(row);
            if (r >= 0 && r < idx.records.size())
                bytes += idx.records.at(r).length;
        }
    }
    return bytes;
}

// The walk both copy commands share (ARCHITECTURE.md §7.1.6).
//
// Below the threshold it is the loop it always was: no dialog, no event processing,
// nothing that can re-enter — which is what makes an ordinary copy inert.
//
// Above it the copy is worth waiting for, so it says so and offers to stop. That means
// re-entering the event loop, and the four things that can then happen to the row space
// under the walk are answered as follows:
//   * an APPEND adds rows past the end of what was selected, and every row is resolved
//     through m_model->sourceRow() at the moment it is read, bounds-checked exactly as
//     before, so a row that has gone is skipped rather than read from stale bytes;
//   * a TAIL REMOVAL (the provisional record re-evaluated under a filter, which happens
//     on a live log every tick) is the same case, and deliberately does NOT abandon the
//     copy — the ingest handler runs to completion inside processEvents(), so what the
//     walk sees between two chunks is always a consistent index;
//   * a MODEL RESET is the one event that makes a view row mean a different record, so
//     the prefix already collected and the remainder would come from two different
//     mappings. That is abandoned, and reported, because a silently spliced copy is
//     worse than none;
//   * DESTRUCTION of the view or the model cannot happen while an APPLICATION-MODAL
//     dialog is up — Qt drops a close event on a modally blocked window and no tab can
//     be clicked shut — which is why the dialog is shown before the first
//     processEvents() rather than after QProgressDialog's own minimumDuration. The
//     QPointers are the belt to that braces, and the reason nothing below the loop
//     touches a member without checking them.
bool LogView::walkSelection(const QVector<QPair<int, int>> &ranges, qint64 total,
                            const std::function<void(int viewRow)> &emitRow) const
{
    if (ranges.isEmpty())
        return false;

    if (total <= m_copyProgressThreshold) {
        for (const QPair<int, int> &range : ranges) {
            for (int row = range.first; row <= range.second; ++row)
                emitRow(row);
        }
        return true;
    }

    const int totalRows = int(qMin<qint64>(total, std::numeric_limits<int>::max()));
    QPointer<LogView> self(const_cast<LogView *>(this));
    QPointer<LogModel> model(m_model);
    bool replaced = false;

    QPointer<QProgressDialog> dlg(new QProgressDialog(
        tr("Copying %n record(s)…", nullptr, totalRows), tr("Cancel"), 0, totalRows,
        const_cast<LogView *>(this)));
    dlg->setObjectName(QStringLiteral("copyProgress")); // findChild, for tests
    dlg->setWindowTitle(tr("Copy"));
    dlg->setWindowModality(Qt::ApplicationModal);
    dlg->setMinimumDuration(0);
    // Neither auto-close nor auto-reset: reset() clears the very flag wasCanceled()
    // answers, and reaching the maximum would therefore forget a Cancel pressed on the
    // last chunk. The dialog is closed by being deleted, below, on every path.
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->setValue(0);
    dlg->show();

    // Scoped to the dialog, so the connection cannot outlive the local it writes to.
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, dlg.data(),
            [&replaced]() { replaced = true; });

    bool ok = true;
    qint64 done = 0;
    for (const QPair<int, int> &range : ranges) {
        for (int row = range.first; row <= range.second; ++row) {
            emitRow(row);
            if (++done % kCopyChunk != 0)
                continue;
            dlg->setValue(int(done));
            QCoreApplication::processEvents();
            // self FIRST: the dialog is a child of this view, so testing it after a
            // destruction would be the dangling read this guard exists to prevent.
            if (!self || !model || replaced || !dlg || dlg->wasCanceled()) {
                ok = false;
                break;
            }
        }
        if (!ok)
            break;
    }
    if (ok) {
        // The last chance to cancel, which the final partial chunk would otherwise not
        // offer. A Cancel that arrives after the work is done still copies nothing:
        // "cancelled" means the clipboard is untouched, whenever it lands.
        dlg->setValue(totalRows);
        QCoreApplication::processEvents();
        ok = self && model && !replaced && dlg && !dlg->wasCanceled();
    }

    const bool abandoned = replaced;
    delete dlg.data();
    if (abandoned && self) {
        QMessageBox::information(self, tr("Copy"),
                                 tr("The log changed while the selection was being "
                                    "copied, so nothing was copied."));
    }
    return ok;
}

void LogView::selectRecordSilently(int record)
{
    const int n = recordCount();
    if (n == 0)
        return;
    record = qBound(0, record, n - 1);
    m_current = record;
    m_anchor = record;
    selectRange(record, record);
}

void LogView::setCurrentRecord(int record, bool extendSelection)
{
    const int n = recordCount();
    if (n == 0)
        return;
    record = qBound(0, record, n - 1);
    // An explicit pick forgets a selection a filter had hidden: the user has chosen a
    // different record, so re-selecting the old one on the next widening would fight them.
    m_stickySource = -1;
    if (extendSelection && m_anchor >= 0) {
        m_current = record;
        selectRange(m_anchor, record);
    } else {
        selectRecordSilently(record);
    }
    recomputeGeometry();     // selection can change the wrapped-record geometry
    ensureRecordVisible(record);
}

void LogView::ensureRecordVisible(int record)
{
    const qint64 recTop = mapLineOfRecord(record);
    const qint64 recBottom = recTop + mapRecordHeightLines(record);
    const qint64 top = verticalScrollBar()->value();
    const qint64 page = visibleLines();
    if (recTop < top)
        verticalScrollBar()->setValue(int(recTop));
    else if (recBottom > top + page)
        verticalScrollBar()->setValue(int(qMax<qint64>(0, recBottom - page)));
}

// A press picks a record; what the modifiers decide is what happens to the rest of the
// selection (SPEC.md §5).
//
// LEFT button only. A right press must not move the selection, because the context menu
// that follows it decides for itself whether to — and deliberately leaves a multi-record
// selection alone, which a press that had already collapsed it would make impossible.
void LogView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const int record = recordAtViewportY(int(event->position().y()));
    if (record < 0) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    setFocus();
    // Shift outranks Ctrl: "extend to here" is what Shift+click has meant since M2 and
    // goes on meaning exactly that, whatever else is held down.
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        setCurrentRecord(record, true);
    } else if (event->modifiers().testFlag(Qt::ControlModifier)) {
        // A Ctrl press deliberately does NOT arm a drag. An additive drag would have to
        // remember the whole selection to put back the rows the pointer passed over and
        // came back from, and toggling as it went would make the result depend on the
        // path the pointer took rather than on its two ends.
        toggleRecordSelection(record);
        return;
    } else {
        setCurrentRecord(record, false);
    }
    m_dragging = true;
    m_autoScrollY = int(event->position().y());
}

void LogView::mouseMoveEvent(QMouseEvent *event)
{
    // Mouse tracking is off, so a move only arrives while a button is down — there is no
    // hover cost here at all, and the guard is about the presses this view did not take
    // (a right press, or one that landed below the last record).
    if (!m_dragging) {
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    m_autoScrollY = int(event->position().y());
    updateAutoScroll(m_autoScrollY);
    extendDragTo(m_autoScrollY);
}

void LogView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }
    endDrag();
    event->accept(); // the press was this view's to take, and so is the release ending it
}

// A double-click on a cell means "do what this column is for" (SPEC.md §5). The view
// decides nothing about that: it reports the record and the column and lets the window
// reach the record menu's own item, which is what keeps the gesture and the menu one
// thing rather than two that can drift.
//
// PLAIN left button only. Shift and Ctrl already mean something on the press that begins
// the pair — extend, and take one record in or out — and a double-click carrying one is
// a gesture nobody aimed.
void LogView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || event->modifiers() != Qt::NoModifier) {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
        return;
    }
    // recordUnderPoint, not recordAtViewportY: the empty space below the last record
    // answers "nothing" here for the reason it does for the menu — a gesture aimed at a
    // record the cursor is not on acts on a record the user cannot see themselves
    // pointing at. The press that came first clamps instead, and selected the last
    // record; that is a click, and this is not.
    const int record = recordUnderPoint(int(event->position().y()));
    if (record < 0) {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
        return;
    }
    // Whatever the press before it armed, a double-click is not the beginning of a drag:
    // what follows it is a release, and the filter this is about to apply replaces the
    // record space under the pointer in between.
    endDrag();
    // Act on the record under the pointer, not on whatever the selection happens to be —
    // the press already put it here, and saying so costs nothing and cannot be wrong.
    if (m_current != record)
        setCurrentRecord(record);
    setFocus();
    emit recordDoubleClicked(record, m_header->logicalIndexAt(int(event->position().x())));
    event->accept();
}

void LogView::hideEvent(QHideEvent *event)
{
    // A view hidden mid-drag — its tab switched away, its window closed — never sees the
    // release, so the drag ends here instead of leaving a timer scrolling a widget
    // nobody is looking at.
    endDrag();
    QAbstractScrollArea::hideEvent(event);
}

void LogView::extendDragTo(int viewportY)
{
    // Clamped into the viewport: past either edge the drag is about the first or last
    // record on screen, and the scrolling is the autoscroll timer's business.
    const int h = qMax(1, viewport()->height());
    const int record = recordAtViewportY(qBound(0, viewportY, h - 1));
    if (record < 0 || record == m_current)
        return;
    // Through setCurrentRecord()'s extend path, so a drag and a Shift+click build a
    // range the same way out of the same anchor.
    setCurrentRecord(record, true);
}

void LogView::updateAutoScroll(int viewportY)
{
    if (viewportY >= 0 && viewportY < viewport()->height()) {
        stopAutoScroll();
        return;
    }
    if (!m_autoScrollTimer) {
        m_autoScrollTimer = new QTimer(this);
        m_autoScrollTimer->setInterval(kAutoScrollIntervalMs);
        connect(m_autoScrollTimer, &QTimer::timeout, this, &LogView::autoScrollTick);
    }
    if (!m_autoScrollTimer->isActive())
        m_autoScrollTimer->start();
}

void LogView::autoScrollTick()
{
    if (!m_dragging) {
        stopAutoScroll();
        return;
    }
    const int h = viewport()->height();
    const int lh = lineHeight();
    int lines = 0;
    if (m_autoScrollY < 0)
        lines = -(1 + qMin(kAutoScrollMaxLines - 1, -m_autoScrollY / lh));
    else if (m_autoScrollY >= h)
        lines = 1 + qMin(kAutoScrollMaxLines - 1, (m_autoScrollY - h) / lh);
    if (lines == 0) {
        stopAutoScroll();
        return;
    }
    // The scrollbar, not scrollContentsBy: this IS the user scrolling, so follow must
    // detach exactly as it does when they drag the bar itself (SPEC.md §3). A bar
    // already at its end simply does not move, and the extend below then re-selects the
    // record it is already on — which costs nothing and lets a still-growing log carry
    // the drag on past what was the end.
    verticalScrollBar()->setValue(verticalScrollBar()->value() + lines);
    extendDragTo(m_autoScrollY);
}

void LogView::stopAutoScroll()
{
    if (m_autoScrollTimer)
        m_autoScrollTimer->stop();
}

void LogView::endDrag()
{
    m_dragging = false;
    stopAutoScroll();
}

void LogView::contextMenuEvent(QContextMenuEvent *event)
{
    // The viewport forwards this to the scroll area the same way it forwards a mouse
    // press, so the position is in VIEWPORT coordinates — the coordinates the hit test
    // and the header's own both expect.
    //
    // Resolved through recordUnderPoint rather than recordAtViewportY because the empty
    // space BELOW the last record has to answer "nothing", and that hit test deliberately
    // clamps to the last record instead (it backs a click, which selects the nearest
    // row). A menu for a record the cursor is not on would act on a record the user
    // cannot see themselves pointing at.
    const int record = recordUnderPoint(int(event->pos().y()));
    if (record < 0) {
        QAbstractScrollArea::contextMenuEvent(event);
        return;
    }

    // Right-clicking outside the selection moves it, as every list view does: the
    // menu's copy items act on the selection, so what is under the cursor and what
    // the menu acts on must not disagree. A right-click INSIDE a multi-record
    // selection leaves it alone — that is what makes "these five records" possible.
    if (!m_selection->isSelected(m_model->index(record, 0)))
        setCurrentRecord(record);
    setFocus();

    emit recordMenuRequested(record, m_header->logicalIndexAt(int(event->pos().x())),
                             event->globalPos());
    event->accept();
}

// ---------------------------------------------------------------------------
// Tooltips for what does not fit (SPEC.md §5)
//
// The columns elide; the tooltip does not — HighlighterPane's summary column set the
// precedent. Both halves ask the same question of the same width, the section's, so the
// tooltip appears exactly where an ellipsis was painted and nowhere else: a tooltip
// repeating a value already fully on screen is noise, and "there is more here" is the
// whole of what this feature says.
//
// Everything here runs when a tooltip is ASKED FOR, never on the paint path, and holds
// nothing: the text is decoded through the model on demand, exactly as painting does
// (invariant #1).
// ---------------------------------------------------------------------------

QString LogView::truncatedCellText(const QPoint &pos) const
{
    const int record = recordUnderPoint(pos.y());
    if (record < 0)
        return {};
    const int logical = m_header->logicalIndexAt(pos.x());
    if (logical < 0 || logical >= m_model->columnCount() || m_header->isSectionHidden(logical))
        return {};

    QString text;
    if (logical == messageColumn()) {
        // A wrapped message is not elided — every character of it is on screen, in as
        // many lines as it takes — so there is nothing for a tooltip to add.
        if (estimating() || selRecordForGeometry() == record)
            return {};
        // Wrap off: each physical line of the record is drawn and clipped on its own
        // (invariant #2), so the answer is about the line under the cursor rather than
        // about the whole record.
        const qint64 line = qint64(verticalScrollBar()->value()) + pos.y() / lineHeight();
        const int within = int(line - mapLineOfRecord(record));
        const QString message = m_model->cellText(record, logical);
        const QList<QStringView> segs = QStringView(message).split(QLatin1Char('\n'));
        if (within < 0 || within >= segs.size())
            return {};
        text = segs.at(within).toString();
    } else {
        text = m_model->cellText(record, logical);
    }

    if (text.isEmpty())
        return {};
    const int w = m_header->sectionSize(logical);
    return fontMetrics().elidedText(text, Qt::ElideRight, w) == text ? QString() : text;
}

QString LogView::truncatedHeaderText(int x) const
{
    const int logical = m_header->logicalIndexAt(x);
    if (logical < 0 || logical >= m_model->columnCount() || m_header->isSectionHidden(logical))
        return {};
    const QString text =
        m_model->headerData(logical, Qt::Horizontal, Qt::DisplayRole).toString();
    if (text.isEmpty())
        return {};

    // Measured against the rect the STYLE puts the label in, not the raw section: a
    // header section spends a few pixels either side on its margin, so a caption that
    // fits the section can still be the one painted as "Priorit".
    QStyleOptionHeader opt;
    opt.initFrom(m_header);
    opt.orientation = Qt::Horizontal;
    opt.section = logical;
    opt.text = text;
    opt.rect = QRect(m_header->sectionViewportPosition(logical), 0,
                     m_header->sectionSize(logical), m_header->height());
    const QRect label = m_header->style()->subElementRect(QStyle::SE_HeaderLabel, &opt, m_header);
    return m_header->fontMetrics().elidedText(text, Qt::ElideRight, label.width()) == text
        ? QString()
        : text;
}

bool LogView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const QString text = truncatedCellText(help->pos());
        // Answered either way — hiding is as much of an answer as showing, and letting
        // an unanswered help event travel up the parents would hand the cursor whatever
        // tooltip the surrounding widget carries.
        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), text, viewport());
        return true;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

bool LogView::eventFilter(QObject *watched, QEvent *event)
{
    // The header is a QHeaderView of its own, so its tooltips arrive on ITS viewport and
    // never reach this one. QAbstractScrollArea filters its own scrollbars through this
    // same function, and does so from ITS constructor — which runs before m_header.
    if (m_header && watched == m_header->viewport() && event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const QString text = truncatedHeaderText(help->pos().x());
        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), text, m_header);
        return true;
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

void LogView::keyPressEvent(QKeyEvent *event)
{
    const int n = recordCount();
    if (n == 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const int cur = m_current < 0 ? mapRecordAtLine(verticalScrollBar()->value()) : m_current;

    if (event->matches(QKeySequence::Copy)) {
        copySelectionRaw();
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier)
        && event->modifiers().testFlag(Qt::ShiftModifier) && event->key() == Qt::Key_C) {
        copySelectionAsColumns();
        return;
    }
    // Ctrl+A, exactly as Copy above: under a MainWindow the Edit-menu action's
    // window-scoped shortcut gets there first (and resolves to the ACTIVE view), so this
    // is what a view standing on its own answers with.
    if (event->matches(QKeySequence::SelectAll)) {
        selectAllRecords();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Up:   setCurrentRecord(cur - 1, shift); return;
    case Qt::Key_Down: setCurrentRecord(cur + 1, shift); return;
    case Qt::Key_Home: setCurrentRecord(0, shift); return;
    case Qt::Key_End:  setCurrentRecord(n - 1, shift); return;
    case Qt::Key_PageUp: {
        const qint64 target = mapLineOfRecord(cur) - visibleLines();
        setCurrentRecord(mapRecordAtLine(qMax<qint64>(0, target)), shift);
        return;
    }
    case Qt::Key_PageDown: {
        const qint64 target = mapLineOfRecord(cur) + visibleLines();
        setCurrentRecord(mapRecordAtLine(target), shift);
        return;
    }
    default:
        QAbstractScrollArea::keyPressEvent(event);
    }
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

// ONE string, reserved once and appended into (ARCHITECTURE.md §7.1.6). The QStringList
// this used to build and then join() held the whole selection's text TWICE at the moment
// of the join — on a four-million-record log, gigabytes, and a real hang if the second
// allocation failed. The output is byte-for-byte what it was.
void LogView::copySelectionRaw() const
{
    const QVector<QPair<int, int>> ranges = selectedRecordRanges();
    if (ranges.isEmpty())
        return;
    const qint64 total = rangeTotal(ranges);
    const RecordIndex &idx = m_document->index();
    const Decoder &dec = m_document->decoder();

    QString out;
    // Bytes, in the encoding's own code units, plus one separator per record. An
    // over-estimate is a reserve and nothing worse; an under-estimate costs a realloc,
    // which is why the separators are counted.
    reserveFor(out, selectedByteLength(ranges) / qMax(1, dec.unitSize()) + total);

    bool first = true;
    const bool ok = walkSelection(ranges, total, [&](int viewRow) {
        // Selection rows are VIEW rows; copy must read the SOURCE record's true byte
        // range (invariant #6 mapping, and the full text regardless of display cap).
        // Through the model, so a digest strip copies the record its own row names.
        const int r = m_model->sourceRow(viewRow);
        if (r < 0 || r >= idx.records.size())
            return;
        const Record &rec = idx.records.at(r);
        // Re-read per record rather than hoisted: a rotation replaces the source, and
        // the walk above can re-enter the event loop between two records.
        LogSource *src = m_document->source();
        // The true byte range — copy yields full text regardless of display cap (§5).
        QString text = src ? dec.decode(src->bytes(rec.offset, rec.length)) : QString();
        while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char('\r')))
            text.chop(1);
        if (!first)
            out += QLatin1Char('\n');
        first = false;
        out += text;
    });
    if (!ok)
        return; // cancelled or abandoned: the clipboard is left exactly as it was
    QApplication::clipboard()->setText(out);
}

void LogView::copySelectionAsColumns() const
{
    const QVector<QPair<int, int>> ranges = selectedRecordRanges();
    if (ranges.isEmpty())
        return;
    const qint64 total = rangeTotal(ranges);

    // Visual order, skipping hidden columns, so the copy matches what is shown. Resolved
    // ONCE: the header cannot move under an application-modal dialog, and asking it per
    // record walked the whole header per selected record.
    QVector<int> columns;
    const int fieldCount = m_model->columnCount();
    columns.reserve(fieldCount);
    for (int vi = 0; vi < fieldCount; ++vi) {
        const int logical = m_header->logicalIndex(vi);
        if (logical < 0 || m_header->isSectionHidden(logical))
            continue;
        columns.push_back(logical);
    }

    QString out;
    reserveFor(out, selectedByteLength(ranges) + total);

    bool first = true;
    const bool ok = walkSelection(ranges, total, [&](int viewRow) {
        if (!first)
            out += QLatin1Char('\n');
        first = false;
        for (int i = 0; i < columns.size(); ++i) {
            if (i > 0)
                out += QLatin1Char('\t');
            out += flattenCell(m_model->cellText(viewRow, columns.at(i)));
        }
    });
    if (!ok)
        return; // as above: nothing reaches the clipboard unless the whole copy did
    QApplication::clipboard()->setText(out);
}

// ---------------------------------------------------------------------------
// Wrap mode, column state, model signals
// ---------------------------------------------------------------------------

void LogView::setPlaceholderText(const QString &text)
{
    if (m_placeholderText == text)
        return;
    m_placeholderText = text;
    viewport()->update();
}

void LogView::setWrapMode(WrapMode mode)
{
    if (m_wrapMode == mode)
        return;
    m_wrapMode = mode;
    if (mode == WrapMode::AlwaysOn) {
        // Bind the estimator and sync it to the current width. setColumns() is a
        // no-op (and preserves the cache) when the width is unchanged since the
        // last AlwaysOn stint, so a plain Off<->AlwaysOn toggle keeps measurements.
        ensureEstimatorBound();
        m_estimated.setColumns(viewportCols());
    }
    // Switching AWAY from AlwaysOn deliberately leaves m_estimated untouched: the
    // exact path never reads it, and keeping the cache means returning to AlwaysOn
    // does not re-measure from scratch.
    recomputeGeometry();
}

QByteArray LogView::saveColumnState() const
{
    return m_header->saveState();
}

bool LogView::restoreColumnState(const QByteArray &state)
{
    const bool ok = m_header->restoreState(state);
    // A restored layout speaks for every column, whatever it was that saved it: the
    // widths in it are the ones the user was last looking at, and a seed arriving after
    // the restore — which is the order session restore runs in, since indexing starts
    // last — would silently widen columns somebody had narrowed on purpose.
    m_userSizedColumns.fill(true, m_model->columnCount());
    recomputeGeometry();
    // Session restore moves every section at once and QHeaderView reports none of it,
    // so a digest strip mirroring only sectionResized/sectionMoved would sit under the
    // restored layout with the default one until the user touched a divider.
    emit columnLayoutChanged();
    return ok;
}

// ---------------------------------------------------------------------------
// Column widths (SPEC.md §5, ARCHITECTURE.md §7.1)
//
// Two questions, deliberately answered by different code. A SEED is what a column is
// worth before anyone has looked at it: the caption plus a typical value, in characters
// of the resolved font, computed at construction and once more when the scan finishes
// and the intern tables are complete. A FIT is what the user asks for by name, and it
// measures the widest value there actually is — bounded, because the file may hold ten
// million records and a menu item may not walk them.
//
// The rule joining them is that a seed never touches a column somebody has spoken for.
// ---------------------------------------------------------------------------

int LogView::charWidth() const
{
    const int w = fontMetrics().horizontalAdvance(QLatin1Char('0'));
    // 0 is what a platform with an EMPTY font database answers (Windows offscreen ships
    // no fonts at all), and every width here is a multiple of this one.
    return w > 0 ? w : kFallbackCharWidth;
}

int LogView::textWidth(const QString &text) const
{
    const int w = fontMetrics().horizontalAdvance(text);
    return w > 0 ? w : int(text.size()) * charWidth();
}

int LogView::charsWidth(int chars) const
{
    if (chars <= 0)
        return 0;
    // Measured as one run rather than as one glyph's advance multiplied out. A
    // QFontMetrics advance is an INTEGER, so `chars * charWidth()` accumulates the
    // per-glyph rounding: at some sizes that came to a whole character, and the Time
    // column opened a pixel narrower than the very timestamp its allowance names — the
    // column eliding the value it was seeded for. textWidth() keeps the empty-font-
    // database fallback, which lands on exactly the old arithmetic.
    return textWidth(QString(chars, QLatin1Char('0')));
}

int LogView::headerLabelInset(int logical) const
{
    // The width a section spends on its own margin before the caption starts —
    // PM_HeaderMargin either side, 12 px on Breeze against 4 on Fusion. Asked of the
    // style exactly as truncatedHeaderText() asks it, because the two have to agree:
    // one decides whether a caption fits, the other decides how wide to open the column
    // so that it does. Measured against a SYNTHETIC rect of known width rather than the
    // live section, so it is right at construction time, before the header has been
    // laid out and while every section is still its default width.
    constexpr int kProbeWidth = 1000; // wider than any inset a style could ask for
    constexpr int kProbeHeight = 32;
    QStyleOptionHeader opt;
    opt.initFrom(m_header);
    opt.orientation = Qt::Horizontal;
    opt.section = logical;
    opt.rect = QRect(0, 0, kProbeWidth, kProbeHeight);
    const QRect label = m_header->style()->subElementRect(QStyle::SE_HeaderLabel, &opt, m_header);
    // Bounded both ways: a style that answers nonsense may not collapse a column or
    // open one half a window wide.
    return qBound(0, kProbeWidth - label.width(), kProbeWidth / 2);
}

void LogView::markUserSized(int logical)
{
    if (logical < 0)
        return;
    if (m_userSizedColumns.size() <= logical)
        m_userSizedColumns.resize(logical + 1);
    m_userSizedColumns[logical] = true;
}

void LogView::applyColumnWidth(int logical, int width)
{
    const bool wasApplying = m_applyingColumnWidths;
    m_applyingColumnWidths = true;
    m_header->resizeSection(logical, width);
    m_applyingColumnWidths = wasApplying;
}

int LogView::widestInternedWidth(int logical, int maxChars) const
{
    const QVector<Field> &fields = m_document->format().fields;
    if (logical < 0 || logical >= fields.size())
        return 0;
    const FieldRole role = fields.at(logical).role;
    if (role != FieldRole::Logger && role != FieldRole::Thread)
        return 0;
    // The intern tables ARE the complete value set — discovery is a side effect of
    // indexing (invariant #4) — so the widest subsystem or thread name costs one walk of
    // a list the filter pane already shows, and no record decode at all.
    const RecordIndex &idx = m_document->index();
    const QVector<QString> &names =
        (role == FieldRole::Logger) ? idx.loggers.names() : idx.threads.names();
    const int n = qMin<int>(names.size(), kFitNamesScanned);
    int best = 0;
    for (int i = 0; i < n; ++i) {
        const QString &name = names.at(i);
        best = qMax(best, textWidth(maxChars > 0 && name.size() > maxChars
                                        ? name.left(maxChars)
                                        : name));
    }
    return best;
}

int LogView::sampledContentWidth(int logical) const
{
    const int rows = recordCount();
    if (rows <= 0)
        return 0;

    int best = 0;
    int budget = kFitSampleRecords;
    const auto measureRow = [&](int r) {
        --budget; // spent whether or not the cell had anything in it
        const QString text = m_model->cellText(r, logical);
        if (text.isEmpty())
            return;
        if (text.contains(QLatin1Char('\n'))) {
            // A record is not a line (invariant #2): a multi-line message is drawn one
            // physical line at a time, so it is the widest LINE that has to fit.
            for (QStringView seg : QStringView(text).split(QLatin1Char('\n')))
                best = qMax(best, textWidth(seg.toString()));
        } else {
            best = qMax(best, textWidth(text));
        }
    };

    // What is on screen first: a fit that leaves the row the user is pointing at elided
    // reads as broken, however representative the rest of the sample was.
    const int top = qBound(0, mapRecordAtLine(verticalScrollBar()->value()), rows - 1);
    for (int r = top; r < rows && budget > 0 && r <= top + visibleLines(); ++r)
        measureRow(r);
    // Then an even stride across the whole view, so the cost is the same on a log of ten
    // million records as on one of ten.
    const int step = qMax(1, rows / qMax(1, budget));
    for (int r = 0; r < rows && budget > 0; r += step)
        measureRow(r);
    return best;
}

int LogView::seedWidthOf(int logical) const
{
    const QVector<Field> &fields = m_document->format().fields;
    if (logical < 0 || logical >= fields.size())
        return kMinColumnWidth;
    const FieldRole role = fields.at(logical).role;

    // The caption always fits: a column headed "Priorit" is the very thing this replaces.
    // The style's inset is added to the CAPTION term and to nothing else — a cell is
    // painted at the raw section rect, so a value owes the header's margin nothing and
    // widening every column by it would be a gutter nobody asked for.
    int w = qMax(textWidth(m_model->headerData(logical, Qt::Horizontal, Qt::DisplayRole)
                               .toString())
                     + headerLabelInset(logical),
                 charsWidth(seedColumnChars(role)));
    // Whatever the scan has interned so far, clamped — the constructor sees an empty
    // table and answers 0, which is why the per-role allowance above is a floor and not
    // an alternative.
    w = qMax(w, widestInternedWidth(logical, kSeedNameMaxChars));
    return clampColumnWidth(w + kColumnPadding);
}

int LogView::contentWidthOf(int logical) const
{
    const QVector<Field> &fields = m_document->format().fields;
    if (logical < 0 || logical >= fields.size())
        return kMinColumnWidth;
    const FieldRole role = fields.at(logical).role;

    // Same rule as the seed, and needed here for the same reason: a fit over a column
    // whose values are all shorter than its caption — an Elapsed, a Location, a thread
    // name over a log of short ones — would otherwise open truncated on a style with a
    // wide header margin, having just been asked to show everything.
    int w = textWidth(m_model->headerData(logical, Qt::Horizontal, Qt::DisplayRole).toString())
        + headerLabelInset(logical);
    switch (role) {
    case FieldRole::Logger:
    case FieldRole::Thread:
        w = qMax(w, widestInternedWidth(logical, -1)); // unclamped: this one was asked for
        break;
    case FieldRole::Priority:
        // Six words, known at compile time. priorityName() is NOT translated (it round-
        // trips against the log text, invariant #4), so this is exactly what is painted.
        for (const Priority p : {Priority::Trace, Priority::Debug, Priority::Info,
                                 Priority::Warn, Priority::Error, Priority::Fatal})
            w = qMax(w, textWidth(QString(priorityName(p))));
        break;
    default:
        w = qMax(w, sampledContentWidth(logical));
        break;
    }
    return clampColumnWidth(w + kColumnPadding);
}

void LogView::seedColumnWidths()
{
    const int cols = m_model->columnCount();
    for (int c = 0; c < cols; ++c) {
        if (m_userSizedColumns.value(c, false))
            continue;
        applyColumnWidth(c, seedWidthOf(c));
    }
}

void LogView::resetColumnWidths()
{
    m_userSizedColumns.clear();
    seedColumnWidths();
}

void LogView::fitColumnToContents(int logical)
{
    if (logical < 0 || logical >= m_model->columnCount() || m_header->isSectionHidden(logical))
        return;
    applyColumnWidth(logical, contentWidthOf(logical));
    // A fit is as much the user's choice as a drag, so it claims the column: the seed
    // that runs when the scan finishes must not quietly undo it.
    markUserSized(logical);
}

void LogView::fitColumnsToContents()
{
    for (int c = 0; c < m_model->columnCount(); ++c)
        fitColumnToContents(c);
}

void LogView::scrollToEnd()
{
    // Programmatic jump to the end (follow, or an explicit End). Guard the follow
    // bookkeeping so this move is never misread as the user scrolling away.
    m_inFollowScroll = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    m_inFollowScroll = false;
}

void LogView::followTail()
{
    setFollowingState(true);
    scrollToEnd();
    viewport()->update();
}

void LogView::setFollowingState(bool following)
{
    if (m_followButton)
        m_followButton->setVisible(!following);
    if (following != m_following) {
        m_following = following;
        if (!following && m_followButton)
            positionFollowButton();
        emit followingChanged(m_following);
    }
}

void LogView::updateFollowFromScrollPosition()
{
    if (m_inFollowScroll)
        return; // our own scroll-to-end, not a user action
    if (m_filterAnchor.active) {
        // Inside a filter re-apply. endResetModel() narrows the scroll range and Qt
        // CLAMPS the old line value into it, which arrives here as an ordinary scroll —
        // and a clamp that lands at the bottom is exactly how a view the user had
        // detached used to start following again, in silence. endFilterUpdate() carries
        // the follow state over verbatim instead.
        return;
    }
    const bool atBottom = verticalScrollBar()->value() >= verticalScrollBar()->maximum();
    // Scrolling to the bottom re-attaches; scrolling away detaches (SPEC.md §3).
    setFollowingState(atBottom);
}

void LogView::positionFollowButton()
{
    if (!m_followButton)
        return;
    m_followButton->adjustSize();
    const int m = 12;
    const QSize s = m_followButton->size();
    m_followButton->move(viewport()->width() - s.width() - m,
                         viewport()->height() - s.height() - m);
}

void LogView::handleRowsInserted()
{
    // Stay pinned to the newest record while following (SPEC.md §3). Detached, the
    // rows still index and the view holds its position for reading history.
    if (estimating())
        ensureEstimatorBound(); // extend the estimator over the newly appended blocks
    updateScrollBars();
    if (m_following)
        scrollToEnd();
    if (m_followButton && !m_following)
        positionFollowButton();
    viewport()->update();
}

void LogView::handleRowsRemoved()
{
    // The trailing (provisional) record's view row was dropped for re-evaluation
    // under an active filter (M6). Refresh geometry; a following view re-pins after
    // the matching re-insert arrives.
    if (estimating())
        ensureEstimatorBound();
    updateScrollBars();
    if (m_following)
        scrollToEnd();
    viewport()->update();
}

void LogView::handleTailChanged()
{
    // A trailing record grew taller in place (continuation lines appended) with no
    // new rows: its height changed, so the scroll range must be recomputed.
    if (estimating())
        ensureEstimatorBound();
    updateScrollBars();
    if (m_following)
        scrollToEnd();
    viewport()->update();
}

void LogView::beginFilterUpdate()
{
    m_filterAnchor = FilterAnchor{};
    if (m_role != Role::Main)
        return; // the digest strip does not scroll and holds no selection
    m_filterAnchor.active = true;
    m_filterAnchor.following = m_following;

    const int n = recordCount();
    if (n == 0)
        return;

    const qint64 top = verticalScrollBar()->value();
    const int topRow = mapRecordAtLine(top);
    if (topRow >= 0 && topRow < n) {
        m_filterAnchor.topSource = m_model->sourceRow(topRow);
        // Lines scrolled INTO that record — which is why it carries over only where the
        // record itself survives; a different record has its own first line.
        m_filterAnchor.topOffset = qMax<qint64>(0, top - mapLineOfRecord(topRow));
    }
    if (m_current >= 0 && m_current < n) {
        m_filterAnchor.currentSource = m_model->sourceRow(m_current);
        const qint64 curTop = mapLineOfRecord(m_current);
        m_filterAnchor.currentOffset = curTop - top; // may be negative: partly above
        // Intersects the viewport, not "starts inside it": a tall record scrolled to
        // its second line is still what the reader is looking at.
        m_filterAnchor.currentOnScreen = curTop + mapRecordHeightLines(m_current) > top
                                      && curTop < top + visibleLines();
    }
}

void LogView::endFilterUpdate()
{
    if (!m_filterAnchor.active)
        return; // unpaired, or a digest strip. Must be a no-op, or the follow guard latches.
    const FilterAnchor a = m_filterAnchor;

    // (a) SELECTION FIRST. In WrapMode::SelectedRecordOnly the selection IS part of the
    //     line space (selRecordForGeometry -> the geometry statics), so a target line
    //     computed before the selection is restored is computed against a different
    //     mapping than the one it is then applied to.
    const int wanted = a.currentSource >= 0 ? a.currentSource : m_stickySource;
    const int currentRow = wanted >= 0 ? m_model->viewRowOf(wanted) : -1;
    if (currentRow >= 0) {
        selectRecordSilently(currentRow);
        m_stickySource = -1;
    } else {
        // Hidden by the new filter: nothing is selected, but the ordinal is kept so a
        // widening brings the selection back (SPEC.md §6). It is never moved to a
        // neighbour — that would silently select a record the user did not pick.
        m_stickySource = wanted;
    }

    // (b) geometry over the new subset, with that selection's wrapped height in it
    recomputeGeometry();

    // (c) the target top line
    const int n = recordCount();
    qint64 target = 0;
    if (n > 0) {
        if (currentRow >= 0 && a.currentOnScreen) {
            target = mapLineOfRecord(currentRow) - a.currentOffset;
        } else if (a.topSource >= 0) {
            const int topRow = m_model->viewRowAtOrAfter(a.topSource);
            target = topRow >= n
                   ? verticalScrollBar()->maximum() // every survivor is above the old top
                   : mapLineOfRecord(topRow)
                     + (m_model->sourceRow(topRow) == a.topSource ? a.topOffset : 0);
        }
    }

    // (d) apply, still inside the bracket so the move is not read as the user scrolling
    if (a.following) {
        scrollToEnd(); // following the tail outranks the anchor (SPEC.md §3)
    } else {
        const qint64 maxLine = verticalScrollBar()->maximum();
        verticalScrollBar()->setValue(int(qBound<qint64>(0, target, maxLine)));
    }

    m_filterAnchor = FilterAnchor{};
    setFollowingState(a.following); // carried over VERBATIM, never re-derived
    viewport()->update();
}

void LogView::handleModelReset()
{
    // Any reset that is NOT a filter re-apply replaces the record space itself, so a
    // remembered source ordinal means something different there, or nothing.
    if (!m_filterAnchor.active)
        m_stickySource = -1;
    // A drag whose record space has just been replaced is over: the anchor it extends
    // from is dropped on the next line, so the next move would extend from nothing.
    endDrag();
    m_current = -1;
    m_anchor = -1;
    m_selWrapCache = -1;
    // The index the estimator was bound to is gone; drop the cache so it rebinds
    // (with fresh measurements) on the next AlwaysOn geometry query.
    m_estimated.clear();
    recomputeGeometry();
    if (m_followButton)
        m_followButton->setVisible(!m_following);
}

} // namespace loftail
