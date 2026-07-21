#include "PresetStore.h"

#include "AtomicJson.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

namespace loftail {

namespace {
constexpr auto kKeySchema = "schemaVersion";
constexpr auto kKeyKind = "kind";
constexpr auto kKeyPresets = "presets";
constexpr auto kKeyName = "name";
constexpr auto kKeyContent = "content";
} // namespace

QString PresetStore::defaultDir()
{
    // AppConfigLocation resolves from the org/app name set in main() — no hardcoded
    // paths (CLAUDE.md). Presets are global, so a single directory, not per file.
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString PresetStore::kindToString(Kind kind)
{
    return kind == Kind::Filters ? QStringLiteral("filters")
                                 : QStringLiteral("highlighters");
}

bool PresetStore::kindFromString(const QString &s, Kind *kind)
{
    if (s == QLatin1String("filters")) {
        if (kind)
            *kind = Kind::Filters;
        return true;
    }
    if (s == QLatin1String("highlighters")) {
        if (kind)
            *kind = Kind::Highlighters;
        return true;
    }
    return false;
}

QString PresetStore::fileFor(Kind kind) const
{
    const QString base = kind == Kind::Filters ? QStringLiteral("filter-presets.json")
                                               : QStringLiteral("highlighter-presets.json");
    return QDir(m_dir).filePath(base);
}

QJsonObject PresetStore::readCollection(Kind kind) const
{
    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(fileFor(kind), &ok);
    if (!ok || !doc.isObject())
        return QJsonObject();
    const QJsonObject root = doc.object();
    // Ignore a file whose schema we do not understand rather than mangling it (§8).
    if (root.value(QLatin1String(kKeySchema)).toInt(0) != kSchemaVersion)
        return QJsonObject();
    return root.value(QLatin1String(kKeyPresets)).toObject();
}

bool PresetStore::writeCollection(Kind kind, const QJsonObject &presets)
{
    QJsonObject root;
    root.insert(QLatin1String(kKeySchema), kSchemaVersion);
    root.insert(QLatin1String(kKeyKind), kindToString(kind));
    root.insert(QLatin1String(kKeyPresets), presets);
    return AtomicJson::write(fileFor(kind), QJsonDocument(root));
}

QStringList PresetStore::names(Kind kind) const
{
    QStringList out = readCollection(kind).keys();
    out.sort(Qt::CaseInsensitive);
    return out;
}

QJsonObject PresetStore::preset(Kind kind, const QString &name) const
{
    return readCollection(kind).value(name).toObject();
}

bool PresetStore::save(Kind kind, const QString &name, const QJsonObject &content)
{
    if (name.isEmpty())
        return false;
    QJsonObject presets = readCollection(kind);
    presets.insert(name, content); // replaces any existing preset of that name (§9)
    return writeCollection(kind, presets);
}

bool PresetStore::remove(Kind kind, const QString &name)
{
    QJsonObject presets = readCollection(kind);
    if (!presets.contains(name))
        return true; // already gone
    presets.remove(name);
    return writeCollection(kind, presets);
}

bool PresetStore::rename(Kind kind, const QString &from, const QString &to)
{
    if (to.isEmpty())
        return false;
    QJsonObject presets = readCollection(kind);
    if (!presets.contains(from))
        return false;
    const QJsonValue content = presets.value(from);
    presets.remove(from);
    presets.insert(to, content);
    return writeCollection(kind, presets);
}

bool PresetStore::exportPreset(Kind kind, const QString &name, const QString &file) const
{
    const QJsonObject content = preset(kind, name);
    if (content.isEmpty() && !readCollection(kind).contains(name))
        return false;

    QJsonObject root;
    root.insert(QLatin1String(kKeySchema), kSchemaVersion);
    root.insert(QLatin1String(kKeyKind), kindToString(kind));
    root.insert(QLatin1String(kKeyName), name);
    root.insert(QLatin1String(kKeyContent), content);
    return AtomicJson::write(file, QJsonDocument(root));
}

bool PresetStore::importPreset(const QString &file, Kind *kindOut, QString *nameOut)
{
    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(file, &ok);
    if (!ok || !doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    if (root.value(QLatin1String(kKeySchema)).toInt(0) != kSchemaVersion)
        return false;

    Kind kind;
    if (!kindFromString(root.value(QLatin1String(kKeyKind)).toString(), &kind))
        return false;
    const QString name = root.value(QLatin1String(kKeyName)).toString();
    if (name.isEmpty())
        return false;
    const QJsonObject content = root.value(QLatin1String(kKeyContent)).toObject();

    if (kindOut)
        *kindOut = kind;
    if (nameOut)
        *nameOut = name;
    return save(kind, name, content);
}

} // namespace loftail
