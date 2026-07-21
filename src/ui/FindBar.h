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
    bool regex() const;
    bool caseSensitive() const;

    // Show the bar and focus the text field (Ctrl+F).
    void activate();
    // Report the outcome of the last search in the bar's status label.
    void setStatus(const QString &text);

signals:
    // forward=true for Find Next, false for Find Previous. `fromStart` restarts the
    // search from the top/bottom rather than the current cursor (used when the query
    // text itself changes).
    void findRequested(bool forward, bool fromStart);
    void closed();

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    QLineEdit *m_edit = nullptr;
    QCheckBox *m_regex = nullptr;
    QCheckBox *m_case = nullptr;
    QLabel    *m_status = nullptr;
};

} // namespace loftail
