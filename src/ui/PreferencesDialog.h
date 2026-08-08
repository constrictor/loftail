#pragma once

#include "FormatSettings.h"

#include <QByteArray>
#include <QDialog>

namespace loftail {

class FormatEditor;

// The Preferences dialog (SPEC.md §4 "Default log format", PLAN.md M18). It edits the
// settings that belong to the APPLICATION rather than to a file or a view — today that
// is the default log format, and the shape leaves room for the next one.
//
// The default format is what a file loftail has not seen before is tried with. It is the
// second of two levels: a file already configured keeps its own format (FormatCache) and
// never consults this. That is what the "Forget Remembered Formats" button is for —
// without it, changing the default appears to do nothing for every file already opened,
// with no way out of the UI.
//
// Like the other dialogs, it applies nothing: values in through the constructor, out
// through the getters, and MainWindow persists. The one exception is the forget button,
// which acts on confirmation — see formatCacheCleared().
class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    // `sample` is the active log's leading bytes, or EMPTY when no log is open — the
    // preview is then blank and Detect is disabled, which is the ordinary state of this
    // dialog on a fresh start. `initial` is the currently saved default.
    // Nothing here reaches for "the current file" (invariant #7).
    PreferencesDialog(const QByteArray &sample,
                      const FormatSettings &initial,
                      QWidget *parent = nullptr);

    // The default format as currently entered. Read after exec() returns Accepted.
    FormatSettings defaultFormat() const;

    // Whether the per-file format cache was cleared while the dialog was open. Unlike
    // everything else here this has ALREADY happened by the time exec() returns: it is
    // destructive, it has its own confirmation, and deferring it to OK would mean a
    // confirmed-then-cancelled clear silently doing nothing. The caller reads this only
    // to refresh anything it derived from the cache.
    bool formatCacheCleared() const { return m_cacheCleared; }

private:
    void forgetRememberedFormats();

    FormatEditor *m_editor = nullptr;
    bool          m_cacheCleared = false;
};

} // namespace loftail
