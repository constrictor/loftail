#pragma once

#include <QtGlobal>

namespace loftail {

// The user-selectable per-file encoding (SPEC.md §4, ARCHITECTURE.md §6.1).
// `Auto` is the DEFAULT and is itself the persisted choice — the stored setting
// is the user's selection, never the encoding auto-detect happened to resolve to
// (§6.1). The Decoder turns `Auto` into one of the concrete values by sniffing.
enum class Encoding : quint8 {
    Auto,     // sniff a BOM, then a NUL-frequency heuristic, then validate UTF-8
    Utf8,     // forced; a BOM is tolerated and skipped
    Utf16LE,  // forced
    Utf16BE,  // forced
    System,   // forced system 8-bit codepage (Latin-1/local on Unix)
};

} // namespace loftail
