#include "SessionStore.h"

#include <QJsonDocument>
#include <QSettings>

namespace loftail {

namespace {
constexpr auto kGroup = "session";
constexpr auto kSchema = "schemaVersion";
constexpr auto kGeometry = "geometry";
constexpr auto kWindowState = "windowState";
constexpr auto kActive = "activeDocument";
constexpr auto kDocuments = "documents";

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

    // An unknown (or absent) schema version yields an empty session rather than a
    // half-read one — a clean first launch (§8).
    const int version = settings.value(QLatin1String(kSchema), 0).toInt();
    if (version != kSchemaVersion) {
        settings.endGroup();
        return session;
    }

    session.schemaVersion = version;
    session.geometry = settings.value(QLatin1String(kGeometry)).toByteArray();
    session.windowState = settings.value(QLatin1String(kWindowState)).toByteArray();
    session.activeDocument = settings.value(QLatin1String(kActive), 0).toInt();

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
        d.format.displayZone =
            ZoneChoice::fromString(settings.value(QStringLiteral("displayZone")).toString());
        d.columnState = settings.value(QStringLiteral("columnState")).toByteArray();
        d.filters = stringToJson(settings.value(QStringLiteral("filters")).toString());
        d.highlighters =
            stringToJson(settings.value(QStringLiteral("highlighters")).toString());
        session.documents.append(d);
    }
    settings.endArray();

    settings.endGroup();
    return session;
}

void SessionStore::save(QSettings &settings, const Session &session)
{
    settings.beginGroup(QLatin1String(kGroup));

    // Clear the array first so a shrunk documents list leaves no stale indices
    // (QSettings::beginWriteArray does not remove entries beyond the new size).
    settings.remove(QLatin1String(kDocuments));

    settings.setValue(QLatin1String(kSchema), kSchemaVersion);
    settings.setValue(QLatin1String(kGeometry), session.geometry);
    settings.setValue(QLatin1String(kWindowState), session.windowState);
    settings.setValue(QLatin1String(kActive), session.activeDocument);

    settings.beginWriteArray(QLatin1String(kDocuments), session.documents.size());
    for (int i = 0; i < session.documents.size(); ++i) {
        settings.setArrayIndex(i);
        const SessionDocument &d = session.documents.at(i);
        settings.setValue(QStringLiteral("path"), d.path);
        settings.setValue(QStringLiteral("pattern"), d.format.pattern);
        settings.setValue(QStringLiteral("encoding"), uint(d.format.encoding));
        settings.setValue(QStringLiteral("sourceZone"), d.format.sourceZone.toString());
        settings.setValue(QStringLiteral("displayZone"), d.format.displayZone.toString());
        settings.setValue(QStringLiteral("columnState"), d.columnState);
        settings.setValue(QStringLiteral("filters"), jsonToString(d.filters));
        settings.setValue(QStringLiteral("highlighters"), jsonToString(d.highlighters));
    }
    settings.endArray();

    settings.endGroup();
    settings.sync();
}

} // namespace loftail
