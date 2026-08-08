#pragma once

#include "FormatSettings.h"

#include <QByteArray>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
QT_END_NAMESPACE

namespace loftail {

// The format-editing controls (SPEC.md §4): the ConversionPattern with its Detect
// button, the encoding, the source time zone, and a LIVE PREVIEW of sample lines split
// into the fields the pattern would extract — so a wrong pattern is obvious rather than
// failing silently.
//
// This is a widget rather than part of a dialog because two dialogs edit exactly this
// set: LogFormatDialog for one file, and PreferencesDialog for the default a never-seen
// file is tried with (M18). Two copies would drift, and the one that drifted would be
// the one the user reached for second.
//
// It does NOT edit FormatSettings::timeDisplay — the timestamp column's header context
// menu is the sole control for that — nor the run-start axis, which belongs to the Run
// pane. Both are carried through settings() untouched, so a trip through this editor
// cannot reset a choice made elsewhere.
//
// It compiles the pattern via PatternCompiler for immediate feedback: a compile error is
// shown inline against its offset, and a warning appears when %p or %c is missing
// (filtering on that axis degrades). Nothing here reaches for "the current file" — the
// caller hands it the sample bytes to preview, and an EMPTY sample is legitimate: the
// preview is then blank and Detect is disabled, which is what editing a default with no
// log open looks like.
class FormatEditor : public QWidget
{
    Q_OBJECT

public:
    explicit FormatEditor(QWidget *parent = nullptr);

    // The file's leading bytes (~64 KB) the preview and Detect run over. May be empty.
    // Rebuilds the preview, so it may be called at any time.
    void setSample(const QByteArray &sample);

    // Seed the controls. Fields this editor does not own are stashed and handed back by
    // settings().
    void setSettings(const FormatSettings &s);

    // The settings as currently entered.
    FormatSettings settings() const;

    // The caption above the preview table. The default names the sample generically;
    // a caller with a more specific claim to make about where the sample came from
    // (the active log, say) overrides it.
    void setPreviewCaption(const QString &text);

private slots:
    void refresh(); // recompile the pattern, rebuild the preview, update messages
    void detect();  // autodetect the pattern (M8) and fill the pattern field

private:
    void buildUi();
    Encoding currentEncoding() const;

    QByteArray m_sample;

    QLineEdit    *m_patternEdit = nullptr;
    QPushButton  *m_detectButton = nullptr;
    QLabel       *m_errorLabel = nullptr;
    QLabel       *m_warnLabel = nullptr;
    QComboBox    *m_encodingCombo = nullptr;
    QLabel       *m_detectedLabel = nullptr;
    QComboBox    *m_sourceZoneCombo = nullptr;
    QSpinBox     *m_offsetSpin = nullptr;
    QLabel       *m_previewCaption = nullptr;
    QTableWidget *m_previewTable = nullptr;
    QLabel       *m_matchLabel = nullptr;

    // Not edited here; seeded in and handed back out by settings() so a trip through
    // this editor leaves the header menu's and the Run pane's choices alone.
    TimeDisplay   m_timeDisplay = TimeDisplay::AsWritten;
    QString       m_runStartPattern;
    bool          m_runStartIsRegex = false;
    bool          m_runStartCaseSensitive = false;
};

} // namespace loftail
