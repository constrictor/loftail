#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace loftail {

// M5 — Presets (SPEC.md §9, ARCHITECTURE.md §8). Filter presets and highlighter
// presets are two INDEPENDENT, separately-recallable named collections, each stored
// as one schema-versioned JSON file under QStandardPaths::AppConfigLocation and
// written atomically (temp file + rename) for the multi-instance case (§8.1).
//
// A preset's CONTENT is an opaque JSON object supplied by the owning pane
// (name-based filter widget state, or the name/palette-index highlight rule list).
// Because that content carries names and palette indices — never interned ids,
// never RGB — an exported preset is portable across files and across the light/dark
// theme by construction (§8): the importing user's palette supplies the colors, and
// re-indexing re-resolves the names.
//
// The store is constructed against a directory so tests can isolate it; production
// passes the AppConfigLocation. It touches no QApplication.
class PresetStore
{
public:
    enum class Kind {
        Filters,      // a complete set of filters (SPEC.md §9)
        Highlighters, // a complete set of highlight rules
    };

    // The schema version stamped into every preset file and every exported file, so
    // a preset shared today still imports after the format evolves (§8).
    static constexpr int kSchemaVersion = 1;

    explicit PresetStore(const QString &dir) : m_dir(dir) {}

    // The AppConfigLocation-based directory used in production (no hardcoded paths —
    // CLAUDE.md conventions). Empty if the location cannot be resolved.
    static QString defaultDir();

    // The saved preset names for a kind, sorted, or empty when none exist.
    QStringList names(Kind kind) const;

    // The content object saved under `name`, or an empty object when absent.
    QJsonObject preset(Kind kind, const QString &name) const;

    // Create or replace `name` with `content` (create-from-current-state / rename
    // target). Atomic. Returns false on a write failure.
    bool save(Kind kind, const QString &name, const QJsonObject &content);

    // Delete `name`. Atomic. A no-op (returning true) when it does not exist.
    bool remove(Kind kind, const QString &name);

    // Rename `from` to `to`, replacing any existing `to`. Atomic. Returns false when
    // `from` is absent or the write fails.
    bool rename(Kind kind, const QString &from, const QString &to);

    // Export one preset to a user-chosen file, schema-versioned and self-describing
    // (kind + name + content) so it round-trips through importPreset(). Atomic.
    bool exportPreset(Kind kind, const QString &name, const QString &file) const;

    // Import a preset file written by exportPreset(): its declared kind and name are
    // returned via `*kind`/`*name`, and the content is saved into this store under
    // that name. Returns false on a missing/unparseable/wrong-schema file.
    bool importPreset(const QString &file, Kind *kind = nullptr, QString *name = nullptr);

    // Parse a kind string ("filters"/"highlighters") <-> value, for the file format.
    static QString kindToString(Kind kind);
    static bool kindFromString(const QString &s, Kind *kind);

private:
    QString fileFor(Kind kind) const;
    QJsonObject readCollection(Kind kind) const; // { schemaVersion, kind, presets{} }
    bool writeCollection(Kind kind, const QJsonObject &presets);

    QString m_dir;
};

} // namespace loftail
