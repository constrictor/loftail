#pragma once

#include <QFont>

namespace loftail {

// The fixed-pitch font used by every table that shows log text: the record view
// (all columns, header included) and the format editor's preview.
//
// Fixed pitch is not cosmetic in LogView — the estimated-geometry path computes a
// wrapped record's height as ceil(chars / viewportCols) with no text shaping
// (ARCHITECTURE.md §7.1.1), which is only correct when every glyph has the same
// advance. Asking for a family literally named "monospace" resolves through
// fontconfig on Linux but matches nothing on Windows or macOS, so take the font
// the platform designates as fixed-width instead, at the UI's own text size.
QFont monospaceFont();

} // namespace loftail
