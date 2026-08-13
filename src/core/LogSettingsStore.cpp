#include "LogSettingsStore.h"

#include "AtomicJson.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

namespace loftail {

namespace {

constexpr auto kFileName = "logsettings.json";

// JSON keys. Never translated (ARCHITECTURE.md §9.1). The profile keys are the ones
// the QSettings stores this replaced already used for the same fields, so a value can
// still move between here and the session with no mapping table.
constexpr auto kSchemaVersionKey = "schemaVersion";
constexpr auto kDefaultsKey      = "defaults";
constexpr auto kPatternsKey      = "patterns";
constexpr auto kFilesKey         = "files";

constexpr auto kIdKey            = "id";
constexpr auto kKindKey          = "kind";
constexpr auto kMatchKey         = "match";
constexpr auto kCaseKey          = "caseSensitive";
constexpr auto kFullPathKey      = "fullPath";
constexpr auto kProfileKey       = "profile";
constexpr auto kPathKey          = "path";

constexpr auto kPatternKey       = "pattern";
constexpr auto kEncodingKey      = "encoding";
constexpr auto kSourceZoneKey    = "sourceZone";
constexpr auto kTimeDisplayKey   = "timeDisplay";
constexpr auto kRunStartKey      = "runStartPattern";
constexpr auto kRunStartRegexKey = "runStartRegex";
constexpr auto kRunStartCaseKey  = "runStartCase";
constexpr auto kWrapModeKey      = "wrapMode";

constexpr auto kKindWildcard = "wildcard";
constexpr auto kKindRegex    = "regex";

QJsonObject profileToJson(const LogProfile &p)
{
    QJsonObject o;
    o.insert(QLatin1String(kPatternKey), p.format.pattern);
    o.insert(QLatin1String(kEncodingKey), int(p.format.encoding));
    o.insert(QLatin1String(kSourceZoneKey), p.format.sourceZone.toString());
    o.insert(QLatin1String(kTimeDisplayKey), timeDisplayToString(p.format.timeDisplay));
    o.insert(QLatin1String(kRunStartKey), p.format.runStartPattern);
    o.insert(QLatin1String(kRunStartRegexKey), p.format.runStartIsRegex);
    o.insert(QLatin1String(kRunStartCaseKey), p.format.runStartCaseSensitive);
    o.insert(QLatin1String(kWrapModeKey), int(p.wrapMode));
    return o;
}

LogProfile profileFromJson(const QJsonObject &o)
{
    LogProfile p;
    // PRESENCE, NOT EMPTINESS. An empty saved pattern is a real answer — it parses
    // nothing, so every log it applies to reaches the format dialog, which is how a
    // user asks to be consulted about each one. Reading empty as "nothing saved" would
    // silently reinstate the built-in and make that setting unreachable.
    p.format.pattern = o.contains(QLatin1String(kPatternKey))
        ? o.value(QLatin1String(kPatternKey)).toString()
        : LogProfile::builtIn().format.pattern;
    p.format.encoding =
        static_cast<Encoding>(o.value(QLatin1String(kEncodingKey)).toInt(int(Encoding::Auto)));
    p.format.sourceZone =
        ZoneChoice::fromString(o.value(QLatin1String(kSourceZoneKey)).toString());
    p.format.timeDisplay =
        timeDisplayFromString(o.value(QLatin1String(kTimeDisplayKey)).toString());
    p.format.runStartPattern = o.value(QLatin1String(kRunStartKey)).toString();
    p.format.runStartIsRegex = o.value(QLatin1String(kRunStartRegexKey)).toBool();
    p.format.runStartCaseSensitive = o.value(QLatin1String(kRunStartCaseKey)).toBool();
    p.wrapMode = static_cast<WrapMode>(o.value(QLatin1String(kWrapModeKey)).toInt(0));
    return p;
}

// --- The two QSettings stores this replaced, read one last time. -----------------
//
// Kept here rather than in classes of their own: nothing else reads them ever again,
// and a store that exists only to be drained is better read where it is drained.

constexpr auto kLegacyDefaultGroup = "defaultFormat";
constexpr auto kLegacyCacheArray   = "formatCache";

FormatSettings legacyFormatAt(QSettings &s)
{
    FormatSettings f;
    f.pattern    = s.value(QStringLiteral("pattern")).toString();
    f.encoding   = static_cast<Encoding>(s.value(QStringLiteral("encoding")).toUInt());
    f.sourceZone = ZoneChoice::fromString(s.value(QStringLiteral("sourceZone")).toString());
    // The display axis was a ZoneChoice under "displayZone" until the timestamp header
    // menu subsumed it. The TimeDisplay vocabulary spells "local"/"utc" the same way,
    // so an entry written before that reads correctly through this fallback.
    const QString mode = s.value(QStringLiteral("timeDisplay")).toString();
    f.timeDisplay = timeDisplayFromString(
        mode.isEmpty() ? s.value(QStringLiteral("displayZone")).toString() : mode);
    f.runStartPattern       = s.value(QStringLiteral("runStartPattern")).toString();
    f.runStartIsRegex       = s.value(QStringLiteral("runStartRegex")).toBool();
    f.runStartCaseSensitive = s.value(QStringLiteral("runStartCase")).toBool();
    return f;
}

} // namespace

QString LogSettingsStore::defaultDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString LogSettingsStore::filePath() const
{
    return QDir(m_dir).filePath(QLatin1String(kFileName));
}

LogSettingsTree LogSettingsStore::load()
{
    LogSettingsTree tree;
    m_readOnly = false;

    bool ok = false;
    const QJsonDocument doc = AtomicJson::read(filePath(), &ok);
    if (!ok || !doc.isObject())
        return tree;

    const QJsonObject root = doc.object();
    const int version = root.value(QLatin1String(kSchemaVersionKey)).toInt();
    // A file from a LATER version is not read and, more importantly, not written back
    // over: running an older build for one session must not discard the newer build's
    // configuration. There is no migration path downwards, so this is the only safe
    // answer. Earlier versions are migrated upward as they appear (§8).
    if (version > kSchemaVersion) {
        m_readOnly = true;
        return tree;
    }

    if (root.contains(QLatin1String(kDefaultsKey)))
        tree.setDefaults(profileFromJson(root.value(QLatin1String(kDefaultsKey)).toObject()));

    const QJsonArray patterns = root.value(QLatin1String(kPatternsKey)).toArray();
    for (const QJsonValue &v : patterns) {
        const QJsonObject o = v.toObject();
        LogPatternNode n;
        n.id = o.value(QLatin1String(kIdKey)).toString();
        n.kind = o.value(QLatin1String(kKindKey)).toString() == QLatin1String(kKindRegex)
            ? LogPatternNode::Kind::Regex
            : LogPatternNode::Kind::Wildcard;
        n.match = o.value(QLatin1String(kMatchKey)).toString();
        n.caseSensitive = o.value(QLatin1String(kCaseKey)).toBool();
        n.matchFullPath = o.value(QLatin1String(kFullPathKey)).toBool();
        n.profile = profileFromJson(o.value(QLatin1String(kProfileKey)).toObject());
        tree.addPattern(n); // regenerates a missing or duplicated id
    }

    const QJsonArray files = root.value(QLatin1String(kFilesKey)).toArray();
    for (const QJsonValue &v : files) {
        const QJsonObject o = v.toObject();
        const QString path = o.value(QLatin1String(kPathKey)).toString();
        if (path.isEmpty())
            continue;
        // insertFileProfile, NOT setFileProfile: the latter drops an entry equal to
        // what it inherits, and a file node written before a pattern was added is
        // exactly that. Dropping it on load would be a change the user never made.
        tree.insertFileProfile(
            path, profileFromJson(o.value(QLatin1String(kProfileKey)).toObject()));
    }

    return tree;
}

bool LogSettingsStore::save(const LogSettingsTree &tree, QString *error)
{
    if (m_readOnly) {
        if (error)
            *error = QStringLiteral("logsettings.json was written by a newer version");
        return false;
    }

    QJsonObject root;
    root.insert(QLatin1String(kSchemaVersionKey), kSchemaVersion);
    root.insert(QLatin1String(kDefaultsKey), profileToJson(tree.defaults()));

    QJsonArray patterns;
    for (const LogPatternNode &n : tree.patterns()) {
        QJsonObject o;
        o.insert(QLatin1String(kIdKey), n.id);
        o.insert(QLatin1String(kKindKey),
                 QLatin1String(n.kind == LogPatternNode::Kind::Regex ? kKindRegex
                                                                     : kKindWildcard));
        o.insert(QLatin1String(kMatchKey), n.match);
        o.insert(QLatin1String(kCaseKey), n.caseSensitive);
        o.insert(QLatin1String(kFullPathKey), n.matchFullPath);
        o.insert(QLatin1String(kProfileKey), profileToJson(n.profile));
        patterns.append(o);
    }
    root.insert(QLatin1String(kPatternsKey), patterns);

    QJsonArray files;
    for (const LogFileNode &n : tree.files()) {
        QJsonObject o;
        o.insert(QLatin1String(kPathKey), n.path);
        o.insert(QLatin1String(kProfileKey), profileToJson(n.profile));
        files.append(o);
    }
    root.insert(QLatin1String(kFilesKey), files);

    return AtomicJson::write(filePath(), QJsonDocument(root), error);
}

bool LogSettingsStore::migrateLegacy(QSettings &settings)
{
    if (QFile::exists(filePath()))
        return false;

    settings.beginGroup(QLatin1String(kLegacyDefaultGroup));
    const bool hasPattern = settings.contains(QStringLiteral("pattern"));
    LogProfile defaults = LogProfile::builtIn();
    if (hasPattern) {
        // The old store held three of the seven format fields on purpose; the rest
        // stay at the built-in's values, which is what they were.
        defaults.format.pattern = settings.value(QStringLiteral("pattern")).toString();
        defaults.format.encoding = static_cast<Encoding>(
            settings.value(QStringLiteral("encoding"), uint(Encoding::Auto)).toUInt());
        defaults.format.sourceZone =
            ZoneChoice::fromString(settings.value(QStringLiteral("sourceZone")).toString());
    }
    settings.endGroup();

    LogSettingsTree tree;
    tree.setDefaults(defaults);

    const int n = settings.beginReadArray(QLatin1String(kLegacyCacheArray));
    for (int i = 0; i < n; ++i) {
        settings.setArrayIndex(i);
        const QString path = settings.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;
        LogProfile p;
        p.format = legacyFormatAt(settings);
        tree.insertFileProfile(path, p);
    }
    settings.endArray();

    // Nothing to move: a first launch, not an upgrade. Leave the file uncreated so the
    // next call still looks, rather than writing an empty tree that says "migrated".
    if (!hasPattern && n == 0)
        return false;

    if (!save(tree))
        return false;

    // One home for a setting, not two that can disagree. The old groups go now that
    // their contents are somewhere better.
    settings.remove(QLatin1String(kLegacyDefaultGroup));
    settings.remove(QLatin1String(kLegacyCacheArray));
    settings.sync();
    return true;
}

} // namespace loftail
