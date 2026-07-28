#include "SessionStore.h"

#include <QJsonDocument>
#include <QSettings>

namespace loftail {

namespace {
constexpr auto kGroup = "session";
constexpr auto kSchema = "schemaVersion";
constexpr auto kGeometry = "geometry";
constexpr auto kWindowState = "windowState";
constexpr auto kActiveView = "activeView";
constexpr auto kActiveDocumentV1 = "activeDocument";
constexpr auto kDocuments = "documents";
constexpr auto kViews = "views";

QString jsonToString(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject stringToJson(const QString &s)
{
    return QJsonDocument::fromJson(s.toUtf8()).object();
}
} // namespace

Session SessionStore::load(QSettings &settings)
{
    Session session;
    settings.beginGroup(QLatin1String(kGroup));

    // A schema version from the future (or an absent one) yields an empty session
    // rather than a half-read one — a clean first launch (§8). Versions 1 and 2 are
    // read and migrated; anything else is discarded.
    const int version = settings.value(QLatin1String(kSchema), 0).toInt();
    if (version != kSchemaVersion && version != 1 && version != 2) {
        settings.endGroup();
        return session;
    }
    const bool v1 = version == 1;

    session.schemaVersion = kSchemaVersion;
    session.geometry = settings.value(QLatin1String(kGeometry)).toByteArray();
    // Only a current windowState is usable: a v1 blob describes a different window
    // entirely, and a v2 one records the collapsed central widget of the all-docks
    // shell, which would squeeze the document well to zero. Geometry (position and
    // size) is still good, so only the pane layout is dropped.
    if (version == kSchemaVersion)
        session.windowState = settings.value(QLatin1String(kWindowState)).toByteArray();

    const int n = settings.beginReadArray(QLatin1String(kDocuments));
    session.documents.reserve(n);
    for (int i = 0; i < n; ++i) {
        settings.setArrayIndex(i);
        SessionDocument d;
        d.path = settings.value(QStringLiteral("path")).toString();
        d.format.pattern = settings.value(QStringLiteral("pattern")).toString();
        d.format.encoding =
            static_cast<Encoding>(settings.value(QStringLiteral("encoding")).toUInt());
        d.format.sourceZone =
            ZoneChoice::fromString(settings.value(QStringLiteral("sourceZone")).toString());
        // Legacy-key fallback: the display axis was a ZoneChoice under "displayZone"
        // until the header menu subsumed it. See FormatCache::readAt for the full
        // reasoning; the vocabularies overlap on "local"/"utc" by design.
        const QString mode = settings.value(QStringLiteral("timeDisplay")).toString();
        d.format.timeDisplay = timeDisplayFromString(
            mode.isEmpty() ? settings.value(QStringLiteral("displayZone")).toString() : mode);
        d.format.runStartPattern =
            settings.value(QStringLiteral("runStartPattern")).toString();
        d.format.runStartIsRegex = settings.value(QStringLiteral("runStartRegex")).toBool();
        d.format.runStartCaseSensitive = settings.value(QStringLiteral("runStartCase")).toBool();
        d.runAll = settings.value(QStringLiteral("runAll"), false).toBool();
        d.selectedRunStartOffset =
            settings.value(QStringLiteral("selectedRunOffset"), qint64(-1)).toLongLong();
        d.selectedRunStartTimestamp = settings
                                          .value(QStringLiteral("selectedRunTs"),
                                                 qint64(Record::kNoTimestamp))
                                          .toLongLong();
        d.filters = stringToJson(settings.value(QStringLiteral("filters")).toString());
        d.highlighters =
            stringToJson(settings.value(QStringLiteral("highlighters")).toString());
        session.documents.append(d);

        // v1 had exactly one view per document, with the column state on the
        // document. Synthesize that view.
        if (v1) {
            SessionView v;
            v.documentIndex = i;
            v.columnState = settings.value(QStringLiteral("columnState")).toByteArray();
            session.views.append(v);
        }
    }
    settings.endArray();

    if (v1) {
        session.activeView = settings.value(QLatin1String(kActiveDocumentV1), 0).toInt();
    } else {
        session.activeView = settings.value(QLatin1String(kActiveView), 0).toInt();
        const int viewCount = settings.beginReadArray(QLatin1String(kViews));
        session.views.reserve(viewCount);
        for (int i = 0; i < viewCount; ++i) {
            settings.setArrayIndex(i);
            // A v2 view also carried a `dockName`; the tab order it used to
            // disambiguate is now just this array's order, so it is read past.
            SessionView v;
            v.documentIndex = settings.value(QStringLiteral("document"), 0).toInt();
            v.columnState = settings.value(QStringLiteral("columnState")).toByteArray();
            v.wrapMode = settings.value(QStringLiteral("wrapMode"), 0).toInt();
            session.views.append(v);
        }
        settings.endArray();
    }

    settings.endGroup();
    return session;
}

void SessionStore::save(QSettings &settings, const Session &session)
{
    settings.beginGroup(QLatin1String(kGroup));

    // Clear the arrays first so a shrunk list leaves no stale indices
    // (QSettings::beginWriteArray does not remove entries beyond the new size).
    settings.remove(QLatin1String(kDocuments));
    settings.remove(QLatin1String(kViews));
    settings.remove(QLatin1String(kActiveDocumentV1)); // superseded by activeView

    settings.setValue(QLatin1String(kSchema), kSchemaVersion);
    settings.setValue(QLatin1String(kGeometry), session.geometry);
    settings.setValue(QLatin1String(kWindowState), session.windowState);
    settings.setValue(QLatin1String(kActiveView), session.activeView);

    settings.beginWriteArray(QLatin1String(kDocuments), session.documents.size());
    for (int i = 0; i < session.documents.size(); ++i) {
        settings.setArrayIndex(i);
        const SessionDocument &d = session.documents.at(i);
        settings.setValue(QStringLiteral("path"), d.path);
        settings.setValue(QStringLiteral("pattern"), d.format.pattern);
        settings.setValue(QStringLiteral("encoding"), uint(d.format.encoding));
        settings.setValue(QStringLiteral("sourceZone"), d.format.sourceZone.toString());
        settings.setValue(QStringLiteral("timeDisplay"), timeDisplayToString(d.format.timeDisplay));
        settings.setValue(QStringLiteral("runStartPattern"), d.format.runStartPattern);
        settings.setValue(QStringLiteral("runStartRegex"), d.format.runStartIsRegex);
        settings.setValue(QStringLiteral("runStartCase"), d.format.runStartCaseSensitive);
        settings.setValue(QStringLiteral("runAll"), d.runAll);
        settings.setValue(QStringLiteral("selectedRunOffset"), d.selectedRunStartOffset);
        settings.setValue(QStringLiteral("selectedRunTs"), d.selectedRunStartTimestamp);
        settings.setValue(QStringLiteral("filters"), jsonToString(d.filters));
        settings.setValue(QStringLiteral("highlighters"), jsonToString(d.highlighters));
    }
    settings.endArray();

    settings.beginWriteArray(QLatin1String(kViews), session.views.size());
    for (int i = 0; i < session.views.size(); ++i) {
        settings.setArrayIndex(i);
        const SessionView &v = session.views.at(i);
        settings.setValue(QStringLiteral("document"), v.documentIndex);
        settings.setValue(QStringLiteral("columnState"), v.columnState);
        settings.setValue(QStringLiteral("wrapMode"), v.wrapMode);
    }
    settings.endArray();

    settings.endGroup();
    settings.sync();
}

} // namespace loftail
