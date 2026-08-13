#pragma once

#include <QtGlobal>

namespace loftail {

// How a view wraps long records (SPEC.md §5). It lives in core, although the only
// thing that acts on it is LogView, because it is one of the settings a log's node in
// the settings tree carries (LogProfile.h) and nothing in core may see a widget.
//
// LogView aliases this as LogView::WrapMode, so `LogView::WrapMode::AlwaysOn` keeps
// meaning what it always did. The session stores it as an int and always has.
//
// It is a SEED, not a per-file property (invariant #7): the node supplies the mode a
// newly created view of that log starts in, and the view owns it thereafter.
enum class WrapMode : quint8 {
    Off,                // long lines extend horizontally
    SelectedRecordOnly, // only the focused record wraps
    AlwaysOn,           // every record wraps; estimated geometry (§7.1.1)
};

} // namespace loftail
