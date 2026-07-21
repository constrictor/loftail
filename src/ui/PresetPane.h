#pragma once

#include "PresetStore.h"

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QListWidget;
QT_END_NAMESPACE

namespace loftail {

class FilterPane;
class HighlighterPane;

// M5 — the Presets side pane (SPEC.md §9, §8). Manages two independent collections
// of named presets — filters and highlighters — each created from the current pane
// state, applied in one click (replacing, not merging, that axis), renamed, deleted,
// and exported/imported to a JSON file for sharing. Presets are GLOBAL, independent
// of any file, and persist across sessions (stored under AppConfigLocation, written
// atomically). Content is captured from / applied to the two editor panes, which
// already work in portable names and palette indices (theme- and reindex-portable).
class PresetPane : public QWidget
{
    Q_OBJECT

public:
    PresetPane(FilterPane *filters, HighlighterPane *highlighters, QWidget *parent = nullptr);

private:
    void buildUi();
    void refresh(PresetStore::Kind kind);
    QListWidget *listFor(PresetStore::Kind kind) const;

    void createPreset(PresetStore::Kind kind);
    void applyPreset(PresetStore::Kind kind);
    void renamePreset(PresetStore::Kind kind);
    void deletePreset(PresetStore::Kind kind);
    void exportPreset(PresetStore::Kind kind);
    void importPreset(PresetStore::Kind kind);

    QString selectedName(PresetStore::Kind kind) const;

    FilterPane      *m_filters = nullptr;
    HighlighterPane *m_highlighters = nullptr;
    std::unique_ptr<PresetStore> m_store;

    QListWidget *m_filterList = nullptr;
    QListWidget *m_highlighterList = nullptr;
};

} // namespace loftail
