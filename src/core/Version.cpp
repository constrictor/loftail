#include "Version.h"

#ifndef LOFTAIL_VERSION
#define LOFTAIL_VERSION "0.0.0"
#endif

// Empty is the ordinary value, not a fallback: a local build carries no build id and
// prints a bare version. The guard is for a translation unit compiled outside the
// loftail_core target, which would otherwise fail to build rather than say "unknown".
#ifndef LOFTAIL_BUILD_ID
#define LOFTAIL_BUILD_ID ""
#endif

namespace loftail {

QString applicationVersion()
{
    return QStringLiteral(LOFTAIL_VERSION);
}

QString applicationBuildId()
{
    return QStringLiteral(LOFTAIL_BUILD_ID);
}

QString applicationVersionString()
{
    const QString build = applicationBuildId();
    if (build.isEmpty())
        return applicationVersion();
    return applicationVersion() + QLatin1Char('+') + build;
}

} // namespace loftail
