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

// JSON keys. Never translated (ARCHITECTURE.md §9.1). The PROFILE's own keys are not
// here: they are logProfileToJson()/logProfileFromJson()'s, in LogProfile.cpp, because a
// profile lives in two stores at once — this file's defaults and pattern nodes, and the
// per-log records (LogFileStore.h) — and moves between them every time somebody presses
// Promote to Parent Pattern. Two serializers would have to agree key for key and would
// drift the first time a field was added to only one of them.
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

constexpr auto kKindWildcard = "wildcard";
constexpr auto kKindRegex    = "regex";

// --- The patterns loftail ships with, seeded once. --------------------------------
//
// One entry today. `example` is a representative address the seed is FOR, and it is
// there so an existing pattern that already claims such a log wins by being asked
// rather than by string-comparing match texts: `messages*`, `*messages*` and
// `/var/log/messages` are three spellings of an answer the user has already given.
//
// The conversion pattern is NEVER translated (ARCHITECTURE.md §9.1) — it has to match
// log text — and it is deliberately the tag-WITHOUT-pid variant of the three the
// detector offers. `%c[%i]:` splits `sshd[1234]` into two columns and then fails to
// match `kernel: ...`, which carries no pid at all; a line that does not match starts
// no record (invariant #2), so on an ordinary mixed /var/log/messages every kernel line
// would be folded into the record above it. `%c:` takes `sshd[1234]` whole and matches
// both shapes, which is the trade this makes: one column fewer, no swallowed lines.
constexpr auto kSyslogPattern = "%D{%b %e %H:%M:%S} %h %c: %m%n";

// `messages*` and not `messages`, because logrotate's output is what a reader reaches
// for as often as the live file: `messages.1`, `messages-20260827`, and the `.gz` of
// either, whose name a bare compressed stream strips back to the same thing
// (logMatchTarget()).
constexpr auto kSyslogMatch   = "messages*";
constexpr auto kSyslogExample = "/var/log/messages";

constexpr auto kSeedVersionKey = "builtInPatternSeed";

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
        tree.setDefaults(logProfileFromJson(root.value(QLatin1String(kDefaultsKey)).toObject()));

    const QJsonArray patterns = root.value(QLatin1String(kPatternsKey)).toArray();
    for (const auto &v : patterns) {
        const QJsonObject o = v.toObject();
        LogPatternNode n;
        n.id = o.value(QLatin1String(kIdKey)).toString();
        n.kind = o.value(QLatin1String(kKindKey)).toString() == QLatin1String(kKindRegex)
            ? LogPatternNode::Kind::Regex
            : LogPatternNode::Kind::Wildcard;
        n.match = o.value(QLatin1String(kMatchKey)).toString();
        n.caseSensitive = o.value(QLatin1String(kCaseKey)).toBool();
        n.matchFullPath = o.value(QLatin1String(kFullPathKey)).toBool();
        n.profile = logProfileFromJson(o.value(QLatin1String(kProfileKey)).toObject());
        tree.addPattern(n); // regenerates a missing or duplicated id
    }

    // The file level, read one last time. It is NOT put into the tree — the tree has no
    // file level any more (M21) — but handed to the caller through legacyFiles() so
    // LogFileStore::adoptLegacy() can drain it into the per-log pool. Kept rather than
    // discarded because it is somebody's whole per-log format configuration, and the
    // upgrade must not be the thing that loses it.
    const QJsonArray files = root.value(QLatin1String(kFilesKey)).toArray();
    for (const auto &v : files) {
        const QJsonObject o = v.toObject();
        const QString path = o.value(QLatin1String(kPathKey)).toString();
        if (path.isEmpty())
            continue;
        m_legacyFiles.push_back(LegacyFileNode{
            path, logProfileFromJson(o.value(QLatin1String(kProfileKey)).toObject())});
    }

    return tree;
}

bool LogSettingsStore::save(const LogSettingsTree &tree, QString *error) const
{
    if (m_readOnly) {
        if (error)
            *error = QStringLiteral("logsettings.json was written by a newer version");
        return false;
    }

    QJsonObject root;
    root.insert(QLatin1String(kSchemaVersionKey), kSchemaVersion);
    root.insert(QLatin1String(kDefaultsKey), logProfileToJson(tree.defaults()));

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
        o.insert(QLatin1String(kProfileKey), logProfileToJson(n.profile));
        patterns.append(o);
    }
    root.insert(QLatin1String(kPatternsKey), patterns);

    // NO `files[]`. The file level lives one record per log under fileSettings/ (M21,
    // LogFileStore.h). Writing the key here is what closes the drain: once this file has
    // been rewritten without it, legacyFiles() is empty on every later launch and the
    // migration is over with nothing to remember that it happened.
    return AtomicJson::write(filePath(), QJsonDocument(root), error);
}

bool LogSettingsStore::seedBuiltInPatterns(LogSettingsTree &tree, QSettings &settings)
{
    // Asked ONCE per generation. A seed the user has since deleted must stay deleted —
    // see the header; this test, and not the tree's contents, is what makes that true.
    if (settings.value(QLatin1String(kSeedVersionKey), 0).toInt() >= kSeedVersion)
        return false;

    bool added = false;
    // The user got there first: whatever claims /var/log/messages today keeps it, and
    // ours would be unreachable behind it anyway (first match wins).
    if (tree.matchingPattern(QString::fromLatin1(kSyslogExample)) < 0) {
        LogPatternNode n;
        n.kind  = LogPatternNode::Kind::Wildcard;
        n.match = QString::fromLatin1(kSyslogMatch);
        // Name-only and case-insensitive, which is the default a pattern added by hand
        // gets: what this names is what the log is CALLED, not where it lives.
        n.matchFullPath = false;
        n.caseSensitive = false;
        n.profile.format.pattern = QString::fromLatin1(kSyslogPattern);
        // Everything else stays at the struct default — auto-detected encoding, the zone
        // inferred from the pattern (%D is local time, which is what syslog stamps),
        // timestamps as written, no run splitting, no wrapping.
        tree.addPattern(n);
        added = true;
    }

    // The flag goes down only once the tree it describes is actually on disk. A store
    // that refused the write — a newer schema, an unwritable config directory — is asked
    // again next launch rather than remembering a seed that never happened.
    if (added && !save(tree))
        return false;

    settings.setValue(QLatin1String(kSeedVersionKey), kSeedVersion);
    settings.sync();
    return added;
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
        // Into the drain, not into the tree: since M21 the tree has no file level, and
        // these go to the per-log pool through the same adoptLegacy() pass that takes
        // M20's `files[]`. Two upgrade paths, one destination.
        m_legacyFiles.push_back(LegacyFileNode{path, p});
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
