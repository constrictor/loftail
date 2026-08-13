#include "LogProfile.h"

namespace loftail {

namespace {
// The pattern loftail falls back to before anyone has configured one. NOT translated:
// it is a log4cplus conversion pattern, and translating it would stop it matching log
// text (ARCHITECTURE.md §9.1).
constexpr auto kBuiltInPattern = "%d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n";
} // namespace

LogProfile LogProfile::builtIn()
{
    LogProfile p;
    p.format.pattern = QString::fromLatin1(kBuiltInPattern);
    // Everything else stays at its struct default — auto-detected encoding, zone
    // inferred from the pattern, timestamps as written, no run splitting, no wrapping —
    // which is what a log nobody has said anything about should get.
    return p;
}

} // namespace loftail
