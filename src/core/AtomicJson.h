#pragma once

#include <QJsonDocument>
#include <QString>

// Atomic JSON file I/O for the preset and export/import files (ARCHITECTURE.md
// §8.1). Because SPEC.md §3 allows several instances to run at once, a preset file
// is a shared mutable resource: a crash or a concurrent write must never leave a
// truncated file. Every write goes to a temporary file in the SAME directory and
// is renamed into place — an atomic replace on POSIX and Windows — so a reader
// always sees either the old complete file or the new complete file, never a
// partial one. (QSettings gives its own store the same guarantee; this is the
// piece SPEC/ARCHITECTURE flags as "ours to get right".)
namespace loftail::AtomicJson {

// Serialize `doc` and replace `path` atomically (temp file + rename, via QSaveFile).
// Creates parent directories as needed. Returns false without leaving a partial or
// truncated file when the directory cannot be created or the write/commit fails.
bool write(const QString &path, const QJsonDocument &doc, QString *error = nullptr);

// As write(), but the resulting file is readable and writable by its owner ONLY.
// For a file that may hold a secret: the host bookmark store, when the user has
// chosen to save a password (M11, SPEC.md §3). Permissions are applied after the
// atomic rename, since the rename replaces the file and with it its mode.
bool writePrivate(const QString &path, const QJsonDocument &doc, QString *error = nullptr);

// Read and parse `path`. `*ok` (when given) is false on a missing file or a parse
// error; the returned document is then null.
QJsonDocument read(const QString &path, bool *ok = nullptr);

} // namespace loftail::AtomicJson
