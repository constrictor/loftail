#pragma once

#include "FormatSettings.h"
#include "Record.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

QT_BEGIN_NAMESPACE
class QSettings;
QT_END_NAMESPACE

namespace loftail {

// M5 — Session persistence (SPEC.md §10, ARCHITECTURE.md §8, §12.4). What loftail
// restores on relaunch: the last file(s), their format, active filters and
// highlighters, column layout, and the global window geometry and pane layout.
//
// Per invariant #7 / §12.4 the schema stores a `documents` ARRAY from day one, even
// though there is exactly one element today — writing it as a single-document schema
// now would force a migration when multi-file lands. Per-file scope (format,
// filters, highlighters, columns) lives in each array element; window/pane geometry
// is global. Filters/highlighters are stored as their portable name/index JSON so
// restoring is the same code path as applying a preset.
//
// Backed by QSettings, which gives its own store the atomic-write guarantee the
// multi-instance case needs (§8.1); the global keys are last-writer-wins (§10).
struct SessionDocument
{
    QString        path;
    FormatSettings format;       // pattern / encoding / source+display zones + run-start (§4, §3a)
    QByteArray     columnState;  // QHeaderView state: order, sizes, hidden (§5)
    QJsonObject    filters;      // FilterPane portable state (names, not ids)
    QJsonObject    highlighters; // { rules: [...] } — names + palette indices (§8)

    // Run selection (SPEC.md §3a). The run-start PATTERN rides in `format`. This
    // records WHICH run was viewed, by a STABLE key (start byte offset, with the
    // start timestamp as a fallback hint) rather than the ordinal, which shifts as
    // the file grows. runAll == the explicit "all runs" view; otherwise a
    // selectedRunStartOffset >= 0 names a specific run; the default (nothing saved)
    // re-resolves to the newest run.
    bool   runAll = false;
    qint64 selectedRunStartOffset = -1;
    qint64 selectedRunStartTimestamp = Record::kNoTimestamp;
};

struct Session
{
    int                     schemaVersion = 1;
    QByteArray              geometry;    // QWidget::saveGeometry()
    QByteArray              windowState; // QMainWindow::saveState() — dock/pane layout
    int                     activeDocument = 0;
    QVector<SessionDocument> documents;

    bool hasDocuments() const { return !documents.isEmpty(); }
    const SessionDocument *active() const
    {
        if (activeDocument < 0 || activeDocument >= documents.size())
            return documents.isEmpty() ? nullptr : &documents.first();
        return &documents.at(activeDocument);
    }
};

class SessionStore
{
public:
    static constexpr int kSchemaVersion = 1;

    // Read the whole session (empty documents when nothing was saved, or when the
    // stored schema version is not understood).
    static Session load(QSettings &settings);

    // Write the whole session, replacing any previous one. The `documents` array is
    // rewritten wholesale so a shrunk list leaves no stale tail.
    static void save(QSettings &settings, const Session &session);

private:
    SessionStore() = delete;
};

} // namespace loftail
