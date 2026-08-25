#include "PresetPane.h"

#include "FilterPane.h"
#include "HighlighterPane.h"

#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace loftail {

PresetPane::PresetPane(FilterPane *filters, HighlighterPane *highlighters, QWidget *parent)
    : QWidget(parent), m_filters(filters), m_highlighters(highlighters),
      m_store(std::make_unique<PresetStore>(PresetStore::defaultDir()))
{
    buildUi();
    refresh(PresetStore::Kind::Filters);
    refresh(PresetStore::Kind::Highlighters);
}

QListWidget *PresetPane::listFor(PresetStore::Kind kind) const
{
    return kind == PresetStore::Kind::Filters ? m_filterList : m_highlighterList;
}

void PresetPane::buildUi()
{
    auto *root = new QVBoxLayout(this);

    // Build one section per kind: a list plus the create/apply/rename/delete and
    // export/import controls (SPEC.md §9). The two are independent collections.
    auto section = [this, root](const QString &title, PresetStore::Kind kind, QListWidget *&listOut) {
        auto *box = new QGroupBox(title, this);
        auto *v = new QVBoxLayout(box);

        auto *list = new QListWidget(box);
        list->setMinimumHeight(90);
        v->addWidget(list);
        listOut = list;
        // Double-click applies, matching the "one click" intent (SPEC.md §9).
        connect(list, &QListWidget::itemDoubleClicked, this, [this, kind](QListWidgetItem *) {
            applyPreset(kind);
        });

        auto *row1 = new QHBoxLayout;
        auto *saveBtn = new QPushButton(tr("Save current…"), box);
        auto *applyBtn = new QPushButton(tr("Apply"), box);
        row1->addWidget(saveBtn);
        row1->addWidget(applyBtn);
        v->addLayout(row1);

        auto *row2 = new QHBoxLayout;
        auto *renameBtn = new QPushButton(tr("Rename"), box);
        auto *deleteBtn = new QPushButton(tr("Delete"), box);
        row2->addWidget(renameBtn);
        row2->addWidget(deleteBtn);
        v->addLayout(row2);

        auto *row3 = new QHBoxLayout;
        auto *exportBtn = new QPushButton(tr("Export…"), box);
        auto *importBtn = new QPushButton(tr("Import…"), box);
        row3->addWidget(exportBtn);
        row3->addWidget(importBtn);
        v->addLayout(row3);

        root->addWidget(box);

        connect(saveBtn, &QPushButton::clicked, this, [this, kind] { createPreset(kind); });
        connect(applyBtn, &QPushButton::clicked, this, [this, kind] { applyPreset(kind); });
        connect(renameBtn, &QPushButton::clicked, this, [this, kind] { renamePreset(kind); });
        connect(deleteBtn, &QPushButton::clicked, this, [this, kind] { deletePreset(kind); });
        connect(exportBtn, &QPushButton::clicked, this, [this, kind] { exportPreset(kind); });
        connect(importBtn, &QPushButton::clicked, this, [this, kind] { importPreset(kind); });
    };

    section(tr("Filter presets"), PresetStore::Kind::Filters, m_filterList);
    section(tr("Highlighter presets"), PresetStore::Kind::Highlighters, m_highlighterList);
    root->addStretch(1);
}

void PresetPane::refresh(PresetStore::Kind kind)
{
    QListWidget *list = listFor(kind);
    const QString keep = selectedName(kind);
    list->clear();
    for (const QString &name : m_store->names(kind))
        list->addItem(name);
    if (!keep.isEmpty()) {
        const auto items = list->findItems(keep, Qt::MatchExactly);
        if (!items.isEmpty())
            list->setCurrentItem(items.first());
    }
}

QString PresetPane::selectedName(PresetStore::Kind kind) const
{
    const QListWidgetItem *item = listFor(kind)->currentItem();
    return item ? item->text() : QString();
}

void PresetPane::createPreset(PresetStore::Kind kind)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save preset"),
                                               tr("Preset name:"), QLineEdit::Normal,
                                               QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;

    const QJsonObject content = (kind == PresetStore::Kind::Filters)
                                    ? m_filters->saveState()
                                    : m_highlighters->saveState();
    if (!m_store->save(kind, name, content)) {
        QMessageBox::warning(this, QStringLiteral("loftail"),
                             tr("Could not save the preset."));
        return;
    }
    refresh(kind);
}

void PresetPane::applyPreset(PresetStore::Kind kind)
{
    const QString name = selectedName(kind);
    if (name.isEmpty())
        return;
    const QJsonObject content = m_store->preset(kind, name);
    // Applying replaces the current set on that axis, not merges (SPEC.md §9).
    if (kind == PresetStore::Kind::Filters)
        m_filters->restoreState(content);
    else
        m_highlighters->restoreState(content);
}

void PresetPane::renamePreset(PresetStore::Kind kind)
{
    const QString from = selectedName(kind);
    if (from.isEmpty())
        return;
    bool ok = false;
    const QString to = QInputDialog::getText(this, tr("Rename preset"),
                                             tr("New name:"), QLineEdit::Normal, from, &ok)
                           .trimmed();
    if (!ok || to.isEmpty() || to == from)
        return;
    if (!m_store->rename(kind, from, to)) {
        QMessageBox::warning(this, QStringLiteral("loftail"),
                             tr("Could not rename the preset."));
        return;
    }
    refresh(kind);
}

void PresetPane::deletePreset(PresetStore::Kind kind)
{
    const QString name = selectedName(kind);
    if (name.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Delete preset"),
                              tr("Delete preset \"%1\"?").arg(name))
        != QMessageBox::Yes)
        return;
    m_store->remove(kind, name);
    refresh(kind);
}

void PresetPane::exportPreset(PresetStore::Kind kind)
{
    const QString name = selectedName(kind);
    if (name.isEmpty())
        return;
    const QString file = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export preset"), name + QStringLiteral(".json"),
        tr("JSON files (*.json)"));
    if (file.isEmpty())
        return;
    if (!m_store->exportPreset(kind, name, file))
        QMessageBox::warning(this, QStringLiteral("loftail"),
                             tr("Could not export the preset."));
}

void PresetPane::importPreset(PresetStore::Kind kind)
{
    // Deliberately unread: see the comment below on where an imported preset lands.
    Q_UNUSED(kind);
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Import preset"), QString(),
        tr("JSON files (*.json)"));
    if (file.isEmpty())
        return;
    // The file is self-describing (it declares its own kind); import routes it to
    // the matching collection regardless of which section's button was used.
    PresetStore::Kind actual{};
    if (!m_store->importPreset(file, &actual)) {
        QMessageBox::warning(this, QStringLiteral("loftail"),
                             tr("Not a valid loftail preset file."));
        return;
    }
    refresh(actual);
}

} // namespace loftail
