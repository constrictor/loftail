#pragma once

#include <QAbstractScrollArea>

namespace loftail {

class Document;
class LogModel;

// THROWAWAY M2a prototype of the custom record view (ARCHITECTURE.md §7.1). Its
// only job is to prove the performance scheme against a real log: EXACT geometry
// mode (wrap off), line-unit scrolling over the two-level prefix sums, and
// visible-only painting so the §5 laziness guarantee holds.
//
// Deliberately NOT here (that is M2b, the production LogView): selection,
// keyboard navigation, clipboard, column headers/reorder, and the wrap modes. The
// estimated-geometry machinery (M2c) is kept entirely out of this path.
class LogViewProto : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit LogViewProto(const Document *document, LogModel *model, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateScrollRange();
    int lineHeight() const;
    int visibleLines() const;

    const Document *m_document;
    LogModel       *m_model;
};

} // namespace loftail
