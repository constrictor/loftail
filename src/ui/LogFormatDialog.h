#pragma once

#include "FormatSettings.h"

#include <QByteArray>
#include <QDialog>

QT_BEGIN_NAMESPACE
class QCheckBox;
QT_END_NAMESPACE

namespace loftail {

class FormatEditor;

// The Log Format dialog (SPEC.md §4, PLAN.md M3). It edits ONE FILE's FormatSettings —
// the ConversionPattern, the encoding, and the source time zone — through a FormatEditor,
// which owns the controls, the live preview and the Detect button. The dialog itself is
// the shell: a title naming the file, the editor, the "use for new files" checkbox, and
// OK/Cancel.
//
// It does NOT edit FormatSettings::timeDisplay or the run-start axis; those belong to the
// timestamp column's header menu and the Run pane respectively, and the editor carries
// them through untouched.
//
// Nothing here reaches for "the current file" — the caller hands it the sample bytes to
// preview — and nothing here APPLIES anything. The value goes in through the constructor
// and comes back out through settings() and useAsDefault(); MainWindow does the applying
// and the persisting.
class LogFormatDialog : public QDialog
{
    Q_OBJECT

public:
    // `fileName` is shown in the title only. `sample` is the file's leading bytes
    // (~64 KB) the preview runs over; `initial` seeds the controls.
    LogFormatDialog(const QString &fileName,
                    const QByteArray &sample,
                    const FormatSettings &initial,
                    QWidget *parent = nullptr);

    // The settings as currently entered. Read after exec() returns Accepted.
    FormatSettings settings() const;

    // Ticked: also save this pattern, encoding and source zone as the DEFAULT for files
    // loftail has not seen before (M18, DefaultFormatStore). Reported, never acted on —
    // and only meaningful when exec() returned Accepted, so cancelling cannot change the
    // default even if the box was ticked first.
    //
    // This is the promotion path that matters in practice: a pattern is worth making the
    // default once it has been checked against real lines, which is exactly what this
    // dialog was doing.
    bool useAsDefault() const;

private:
    FormatEditor *m_editor = nullptr;
    QCheckBox    *m_defaultCheck = nullptr;
};

} // namespace loftail
