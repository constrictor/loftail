#include "DefaultFormatStore.h"

#include <QSettings>

namespace loftail {

namespace {
constexpr auto kGroup = "defaultFormat";

// The pattern loftail falls back to before anyone has configured one. NOT translated:
// it is a log4cplus conversion pattern, and translating it would stop it matching log
// text (ARCHITECTURE.md §9.1).
constexpr auto kBuiltInPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
} // namespace

FormatSettings DefaultFormatStore::builtIn()
{
    FormatSettings s;
    s.pattern = QString::fromLatin1(kBuiltInPattern);
    // encoding and sourceZone stay at their struct defaults (Auto / infer from pattern),
    // which is what a file nobody has said anything about should get.
    return s;
}

FormatSettings DefaultFormatStore::load(QSettings &settings)
{
    FormatSettings s = builtIn();

    settings.beginGroup(QLatin1String(kGroup));
    // Presence, not emptiness. An EMPTY saved pattern is a real answer — it parses
    // nothing, so every never-seen file reaches the dialog — and that is how a user who
    // wants to be asked about each log says so. Reading it as "nothing saved" would make
    // that setting impossible to express and silently reinstate the built-in.
    if (settings.contains(QStringLiteral("pattern"))) {
        s.pattern = settings.value(QStringLiteral("pattern")).toString();
        s.encoding = static_cast<Encoding>(
            settings.value(QStringLiteral("encoding"), uint(Encoding::Auto)).toUInt());
        s.sourceZone =
            ZoneChoice::fromString(settings.value(QStringLiteral("sourceZone")).toString());
    }
    settings.endGroup();

    return s;
}

void DefaultFormatStore::save(QSettings &settings, const FormatSettings &s)
{
    settings.beginGroup(QLatin1String(kGroup));
    settings.setValue(QStringLiteral("pattern"), s.pattern);
    settings.setValue(QStringLiteral("encoding"), uint(s.encoding));
    settings.setValue(QStringLiteral("sourceZone"), s.sourceZone.toString());
    settings.endGroup();
    settings.sync();
}

} // namespace loftail
