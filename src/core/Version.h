#pragma once

#include <QString>

namespace loftail {

// Application identity. Kept in core (UI-free) so both the UI layer and tests
// can reach it without a QApplication. Values are also used to configure
// QSettings via QApplication's organization/application names.
inline constexpr auto organizationName = "loftail";
inline constexpr auto applicationName = "loftail";

// The application version string, e.g. "0.1.0". Sourced from the CMake project
// version via a compile definition.
QString applicationVersion();

} // namespace loftail
