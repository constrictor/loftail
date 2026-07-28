#include "FormatCache.h"

#include <QFileInfo>
#include <QSettings>
#include <QVector>

namespace loftail {

namespace {
constexpr auto kArrayPrefix = "formatCache";

struct Entry
{
    QString        path;
    FormatSettings settings;
};

FormatSettings readAt(QSettings &settings)
{
    FormatSettings s;
    s.pattern     = settings.value(QStringLiteral("pattern")).toString();
    s.encoding    = static_cast<Encoding>(settings.value(QStringLiteral("encoding")).toUInt());
    s.sourceZone  = ZoneChoice::fromString(settings.value(QStringLiteral("sourceZone")).toString());
    // The display axis was a ZoneChoice under the key "displayZone" until the header
    // menu subsumed it (SPEC.md §4). The TimeDisplay vocabulary keeps "local"/"utc"
    // spelled the same way, so an entry written before that reads correctly through
    // this fallback, and the legacy "default"/"offset:N" land on AsWritten — which
    // is what they meant for display.
    const QString mode = settings.value(QStringLiteral("timeDisplay")).toString();
    s.timeDisplay = timeDisplayFromString(
        mode.isEmpty() ? settings.value(QStringLiteral("displayZone")).toString() : mode);
    s.runStartPattern       = settings.value(QStringLiteral("runStartPattern")).toString();
    s.runStartIsRegex       = settings.value(QStringLiteral("runStartRegex")).toBool();
    s.runStartCaseSensitive = settings.value(QStringLiteral("runStartCase")).toBool();
    return s;
}

QVector<Entry> readAll(QSettings &settings)
{
    QVector<Entry> entries;
    const int n = settings.beginReadArray(QLatin1String(kArrayPrefix));
    entries.reserve(n);
    for (int i = 0; i < n; ++i) {
        settings.setArrayIndex(i);
        Entry e;
        e.path = settings.value(QStringLiteral("path")).toString();
        e.settings = readAt(settings);
        entries.push_back(e);
    }
    settings.endArray();
    return entries;
}
} // namespace

QString FormatCache::canonicalKey(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

std::optional<FormatSettings> FormatCache::load(QSettings &settings, const QString &path)
{
    const QString key = canonicalKey(path);
    const QVector<Entry> entries = readAll(settings);
    for (const Entry &e : entries) {
        if (e.path == key)
            return e.settings;
    }
    return std::nullopt;
}

void FormatCache::save(QSettings &settings, const QString &path, const FormatSettings &s)
{
    const QString key = canonicalKey(path);
    QVector<Entry> entries = readAll(settings);

    bool replaced = false;
    for (Entry &e : entries) {
        if (e.path == key) {
            e.settings = s;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        entries.push_back(Entry{key, s});

    // Clear the whole array before rewriting so a shrunk list leaves no stale tail
    // (QSettings::beginWriteArray does not remove indices beyond the new size).
    settings.remove(QLatin1String(kArrayPrefix));
    settings.beginWriteArray(QLatin1String(kArrayPrefix), entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        settings.setArrayIndex(i);
        const Entry &e = entries.at(i);
        settings.setValue(QStringLiteral("path"), e.path);
        settings.setValue(QStringLiteral("pattern"), e.settings.pattern);
        settings.setValue(QStringLiteral("encoding"), uint(e.settings.encoding));
        settings.setValue(QStringLiteral("sourceZone"), e.settings.sourceZone.toString());
        settings.setValue(QStringLiteral("timeDisplay"), timeDisplayToString(e.settings.timeDisplay));
        settings.setValue(QStringLiteral("runStartPattern"), e.settings.runStartPattern);
        settings.setValue(QStringLiteral("runStartRegex"), e.settings.runStartIsRegex);
        settings.setValue(QStringLiteral("runStartCase"), e.settings.runStartCaseSensitive);
    }
    settings.endArray();
    settings.sync();
}

} // namespace loftail
