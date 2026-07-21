#pragma once

#include "FormatSettings.h"

#include <QByteArray>
#include <QDialog>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
QT_END_NAMESPACE

namespace loftail {

// The Log Format dialog (SPEC.md §4, PLAN.md M3). It edits a FormatSettings — the
// ConversionPattern, the encoding, and the source/display time zones — and shows a
// LIVE PREVIEW of the current file's sample lines split into the fields the pattern
// would extract, so a wrong pattern is obvious rather than failing silently.
//
// It compiles the pattern via PatternCompiler for immediate feedback: a compile
// error is shown inline against its offset, and a warning appears when %p or %c is
// missing (filtering on that axis degrades). Nothing here reaches for "the current
// file" — the caller hands it the sample bytes to preview.
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

private slots:
    void refresh(); // recompile the pattern, rebuild the preview, update messages
    void detect();  // autodetect the pattern (M8) and fill the pattern field

private:
    void buildUi(const QString &fileName);
    void seed(const FormatSettings &initial);
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
    QComboBox    *m_displayZoneCombo = nullptr;
    QTableWidget *m_previewTable = nullptr;
    QLabel       *m_matchLabel = nullptr;
};

} // namespace loftail
