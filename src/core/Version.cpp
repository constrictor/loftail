#include "Version.h"

#ifndef LOFTAIL_VERSION
#define LOFTAIL_VERSION "0.0.0"
#endif

namespace loftail {

QString applicationVersion()
{
    return QStringLiteral(LOFTAIL_VERSION);
}

} // namespace loftail
