#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLineEdit;
class QLabel;
QT_END_NAMESPACE

namespace loftail {

// M4 — the Find bar (SPEC.md §5). Distinct from filtering: Find moves the cursor
// and leaves every record visible; it walks whatever is CURRENTLY visible (the
// filtered subset when a filter is active). It shares the message-matching code
// with the filter (loftail::TextMatcher) but changes no filter state.
//
// The bar is pure UI: it emits findRequested() with the query and direction, and
// MainWindow does the actual walk over the model's visible rows via Find::search().
class FindBar : public QWidget
{
    Q_OBJECT

public:
    explicit FindBar(QWidget *parent = nullptr);

    QString pattern() const;
    // The last report given to setStatus(), in full. The label itself shows an elided
    // rendering of it, so this — not the label's text — is what the report IS.
    QString status() const;
    bool regex() const;
    bool caseSensitive() const;

    // Show the bar and focus the text field (Ctrl+F).
    void activate();
    // Report the outcome of the last search in the bar's own status label (SPEC.md §5):
    // which match of how many, whether the search wrapped, or why there was nothing to
    // go to. The bar's label rather than the window's status bar, because that one is
    // rewritten on every ingest tick and tab switch. An empty string clears it.
    void setStatus(const QString &text);

signals:
    // forward=true for Find Next, false for Find Previous. `fromStart` restarts the
    // search from the top/bottom rather than the current cursor (used when the query
    // text itself changes).
    void findRequested(bool forward, bool fromStart);
    void closed();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    // Re-elides the status into the cell the bar's width gives it — see setStatus().
    void resizeEvent(QResizeEvent *event) override;
    // Watches the query field so Shift+Enter can mean "search backwards" (SPEC.md §5).
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Cuts m_statusText into the label's own width, and puts the full text on the
    // tooltip only when it did not fit. The label's width never depends on the text:
    // it is a stretch share of the bar, so the controls beside it cannot move.
    void updateStatusText();

    QString    m_statusText;
    QLineEdit *m_edit = nullptr;
    QCheckBox *m_regex = nullptr;
    QCheckBox *m_case = nullptr;
    QLabel    *m_status = nullptr;
};

} // namespace loftail
