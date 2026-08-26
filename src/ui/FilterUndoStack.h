#pragma once

#include <QJsonObject>
#include <QList>

namespace loftail {

// One log's filter history (SPEC.md §6). Esc walks back through it, Shift+Esc walks
// forward, and it lives exactly as long as the tab: nothing here is persisted, no
// store gained a field and no schema version moved.
//
// An entry is a FilterPane::saveState() object — the five match axes plus the two
// context spinners — which is already the pane's portable snapshot, already what the
// window stashes per file (DocumentContext::filterState) and already what is written
// under the log's own key (LogFileSettings::filters). So an undo is a restoreState()
// of a state this log genuinely had, applied by the pane's ordinary path, which is
// what makes the reader keep their place for free: MainWindow::applyActiveFilters()
// brackets it in LogView::beginFilterUpdate()/endFilterUpdate() like any other edit.
//
// Kept UI-side and free of widgets so the rule below can be driven directly by a test
// with no window at all, the way ContextEmitter.h and TabLabels.h are.
//
// This is per FILE and not per view. Filters belong to the file (invariant #7) and two
// views of one log share them, so they must share the history too — which is why it
// hangs off DocumentContext beside filterState rather than off a DocumentView.
class FilterUndoStack
{
public:
    // How far back Esc can walk. An entry is a small QJsonObject of implicitly-shared
    // values, so the cap is about bounding a session's growth rather than about cost.
    static constexpr int kMaxDepth = 100;

    // A filter state has landed. `continuous` says it came from a control the user is
    // TYPING or HOLDING — the message query box, a time editor, a context spinner —
    // which is what merges a typed word into ONE entry instead of one per character.
    //
    // The debounce in FilterPane is not that rule and cannot be: it engages only once a
    // pass has measured itself slow (FilterPane.cpp, kApplyDebounceThresholdMs), so on
    // an ordinary log every keystroke arrives here separately and Esc would otherwise
    // walk back one letter at a time.
    void record(const QJsonObject &state, bool continuous)
    {
        // No baseline yet: this is what the log's filters ARE, not a change to them.
        // Establishing it here rather than from an explicit seed is deliberate — a
        // context's first hydration is what supplies it, and there is no separate site
        // anyone can forget to call.
        if (!m_haveBaseline) {
            m_baseline = state;
            m_haveBaseline = true;
            m_lastWasContinuous = continuous;
            return;
        }
        // Nothing moved. Reached often: refreshDiscoveredLists() and every route that
        // re-resolves the same criteria come through the pane's one apply path.
        if (state == m_baseline)
            return;

        // The merge. `m_undo` must be non-empty for it: the first keystroke of a run
        // has to push, or there is nothing to come back to.
        const bool merge = continuous && m_lastWasContinuous && !m_undo.isEmpty();
        if (!merge) {
            m_undo.append(m_baseline);
            if (m_undo.size() > kMaxDepth)
                m_undo.removeFirst();
        }
        // Cleared either way. A fresh edit invalidates the forward path whether or not
        // it merged into the entry before it — a redo after one would put back a state
        // that no longer follows from what is on screen.
        m_redo.clear();
        m_baseline = state;
        m_lastWasContinuous = continuous;
    }

    // End a run of continuous edits. The focus predicate alone cannot: clicking out of
    // the query box and back moves focus twice without producing a filter change, so
    // the next keystroke would still look like a continuation of the last word typed.
    // MainWindow drives this from QApplication::focusChanged.
    void breakRun() { m_lastWasContinuous = false; }

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }

    // The state to restore, having moved the current one onto the opposite stack.
    // Caller must check canUndo()/canRedo() first.
    QJsonObject undo()
    {
        m_redo.append(m_baseline);
        m_baseline = m_undo.takeLast();
        m_lastWasContinuous = false; // an undo ends any run it interrupted
        return m_baseline;
    }

    QJsonObject redo()
    {
        m_undo.append(m_baseline);
        m_baseline = m_redo.takeLast();
        m_lastWasContinuous = false;
        return m_baseline;
    }

    // What the pane ACTUALLY holds now, made the baseline without touching either
    // stack. Two callers: the window after hydrating a tab, which is what gives a
    // context its starting point, and the window after applying an undo or a redo.
    //
    // The second is deliberate rather than a formality. AxisEditor::criteria() is not
    // the inverse of setCriteria() (LogFileSettings.h) — a bound the axis does not use
    // is left in the date editors, and a value list reloads under a rule chosen from
    // coversAll/restrictive rather than as a literal list — so what comes back out of
    // the pane need not be what went in. The baseline is meant to BE what is on screen,
    // and asking costs nothing next to assuming.
    void settle(const QJsonObject &actual)
    {
        m_baseline = actual;
        m_haveBaseline = true;
        m_lastWasContinuous = false;
    }

private:
    QJsonObject        m_baseline;
    bool               m_haveBaseline = false;
    bool               m_lastWasContinuous = false;
    QList<QJsonObject> m_undo;
    QList<QJsonObject> m_redo;
};

} // namespace loftail
