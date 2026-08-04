#pragma once

#include "FormatSettings.h"

#include <QString>

#include <optional>

QT_BEGIN_NAMESPACE
class QSettings;
QT_END_NAMESPACE

namespace loftail {

// The per-file format cache (SPEC.md §4, ARCHITECTURE.md §8): a file already
// configured reopens with its chosen pattern, encoding, and time zones without
// asking again. Keyed by CANONICAL path — per file only, with NO directory-level
// fallback: a newly opened file is never assumed to share a sibling's format.
//
// Persisted as a QSettings array under `formatCache`, mirroring the shape the M5
// `documents` array (ARCHITECTURE.md §12.4) will grow into. The caller passes the
// QSettings so tests can isolate the store; production uses a default QSettings()
// resolved from the app's org/name (no hardcoded paths — CLAUDE.md conventions).
class FormatCache
{
public:
    // The stored key for a path: its canonical form, falling back to the absolute
    // path when the file does not yet exist on disk. A remote `ssh://` URL is keyed
    // by its normal form instead — it has no on-disk canonical path (M11).
    static QString canonicalKey(const QString &path);

    // The saved settings for `path`, or nullopt when the file has not been seen.
    static std::optional<FormatSettings> load(QSettings &settings, const QString &path);

    // Remember `s` for `path`, replacing any previous entry for that file.
    static void save(QSettings &settings, const QString &path, const FormatSettings &s);

private:
    FormatCache() = delete;
};

} // namespace loftail
