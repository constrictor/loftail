#include "LogFormatDialog.h"

#include "UiColors.h"

#include "Decoder.h"
#include "Fonts.h"
#include "FormatDetector.h"
#include "FormatPreview.h"
#include "PatternCompiler.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace loftail {

namespace {
// Encoding <-> combo index. Order matches the SPEC.md §4 list.
const Encoding kEncodingByIndex[] = {
    Encoding::Auto, Encoding::Utf8, Encoding::Utf16LE, Encoding::Utf16BE, Encoding::System,
};

int encodingToIndex(Encoding e)
{
    for (int i = 0; i < int(std::size(kEncodingByIndex)); ++i)
        if (kEncodingByIndex[i] == e)
            return i;
    return 0;
}

QString encodingName(Encoding e)
{
    switch (e) {
    case Encoding::Auto:    return QStringLiteral("Auto-detect");
    case Encoding::Utf8:    return QStringLiteral("UTF-8");
    case Encoding::Utf16LE: return QStringLiteral("UTF-16 LE");
    case Encoding::Utf16BE: return QStringLiteral("UTF-16 BE");
    case Encoding::System:  return QStringLiteral("System 8-bit");
    }
    return QString();
}
} // namespace

LogFormatDialog::LogFormatDialog(const QString &fileName,
                                 const QByteArray &sample,
                                 const FormatSettings &initial,
                                 QWidget *parent)
    : QDialog(parent), m_sample(sample)
{
    buildUi(fileName);
    seed(initial);
    refresh();
}

void LogFormatDialog::buildUi(const QString &fileName)
{
    setWindowTitle(fileName.isEmpty() ? QStringLiteral("Log Format")
                                      : QStringLiteral("Log Format — %1").arg(fileName));
    resize(760, 560);

    auto *outer = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    outer->addLayout(form);

    // --- Pattern -----------------------------------------------------------
    auto *patRow = new QHBoxLayout;
    m_patternEdit = new QLineEdit(this);
    m_patternEdit->setPlaceholderText(QStringLiteral("e.g. %d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"));
    ensureReadablePlaceholder(m_patternEdit);
    patRow->addWidget(m_patternEdit, 1);
    // M8: re-run autodetection over the sample and fill the pattern field. It only
    // pre-fills — the user still confirms via OK, so nothing is applied silently.
    m_detectButton = new QPushButton(QStringLiteral("&Detect"), this);
    m_detectButton->setToolTip(QStringLiteral("Guess the pattern from the sample lines"));
    m_detectButton->setEnabled(!m_sample.isEmpty());
    patRow->addWidget(m_detectButton);
    form->addRow(QStringLiteral("Conversion &pattern:"), patRow);
    connect(m_patternEdit, &QLineEdit::textChanged, this, &LogFormatDialog::refresh);
    connect(m_detectButton, &QPushButton::clicked, this, &LogFormatDialog::detect);

    // Inline compile error, pointing at the offending offset (CompileError::offset).
    m_errorLabel = new QLabel(this);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(errorColor(palette()).name()));
    m_errorLabel->setVisible(false);
    form->addRow(QString(), m_errorLabel);

    // Missing %p / %c warning (filtering on that axis degrades).
    m_warnLabel = new QLabel(this);
    m_warnLabel->setWordWrap(true);
    m_warnLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(warningColor(palette()).name()));
    m_warnLabel->setVisible(false);
    form->addRow(QString(), m_warnLabel);

    // --- Encoding ----------------------------------------------------------
    auto *encRow = new QHBoxLayout;
    m_encodingCombo = new QComboBox(this);
    for (const Encoding e : kEncodingByIndex)
        m_encodingCombo->addItem(encodingName(e));
    encRow->addWidget(m_encodingCombo);
    m_detectedLabel = new QLabel(this);
    m_detectedLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(mutedColor(palette()).name()));
    encRow->addWidget(m_detectedLabel);
    encRow->addStretch();
    form->addRow(QStringLiteral("&Encoding:"), encRow);
    connect(m_encodingCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &LogFormatDialog::refresh);

    // --- Source time zone --------------------------------------------------
    auto *srcRow = new QHBoxLayout;
    m_sourceZoneCombo = new QComboBox(this);
    m_sourceZoneCombo->addItem(QStringLiteral("Infer from pattern")); // Default
    m_sourceZoneCombo->addItem(QStringLiteral("Local time"));         // Local
    m_sourceZoneCombo->addItem(QStringLiteral("UTC"));                // Utc
    m_sourceZoneCombo->addItem(QStringLiteral("Fixed offset"));       // FixedOffset
    srcRow->addWidget(m_sourceZoneCombo);
    m_offsetSpin = new QSpinBox(this);
    m_offsetSpin->setRange(-720, 840); // minutes east of UTC (−12:00 … +14:00)
    m_offsetSpin->setSuffix(QStringLiteral(" min"));
    m_offsetSpin->setToolTip(QStringLiteral("Offset east of UTC, in minutes"));
    srcRow->addWidget(m_offsetSpin);
    srcRow->addStretch();
    form->addRow(QStringLiteral("&Source time zone:"), srcRow);
    connect(m_sourceZoneCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        m_offsetSpin->setVisible(m_sourceZoneCombo->currentIndex()
                                 == int(ZoneChoice::Kind::FixedOffset));
    });
    connect(m_offsetSpin, qOverload<int>(&QSpinBox::valueChanged), this, &LogFormatDialog::refresh);

    // There is deliberately no display-zone control here: how timestamps are SHOWN
    // is chosen from the timestamp column's header context menu, which offers the
    // zone modes alongside the two seconds modes (SPEC.md §4).

    // --- Live preview ------------------------------------------------------
    outer->addWidget(new QLabel(QStringLiteral("Preview (sample lines split into fields):"), this));
    m_previewTable = new QTableWidget(this);
    // Same fixed-pitch font as the record view, so the preview shows the sample
    // lines the way the table will render them (and column contents line up).
    m_previewTable->setFont(monospaceFont());
    // One line per sample record, as in the record table: a fixed-width font is
    // wider than the UI default, and wrapped cells would turn the preview into a
    // ragged block instead of aligned columns.
    m_previewTable->setWordWrap(false);
    m_previewTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_previewTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_previewTable->horizontalHeader()->setStretchLastSection(true);
    m_previewTable->verticalHeader()->setVisible(false);
    outer->addWidget(m_previewTable, 1);

    m_matchLabel = new QLabel(this);
    outer->addWidget(m_matchLabel);

    // --- Buttons -----------------------------------------------------------
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    // The platform theme labels standard buttons in the DESKTOP's language, which
    // on a non-English desktop leaves a dialog that is English everywhere else
    // reading half-translated. loftail ships no translations, so state the text
    // explicitly and keep one language on screen.
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancel"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);
}

void LogFormatDialog::seed(const FormatSettings &initial)
{
    m_patternEdit->setText(initial.pattern);
    m_encodingCombo->setCurrentIndex(encodingToIndex(initial.encoding));

    m_sourceZoneCombo->setCurrentIndex(int(initial.sourceZone.kind));
    if (initial.sourceZone.kind == ZoneChoice::Kind::FixedOffset)
        m_offsetSpin->setValue(initial.sourceZone.offsetSeconds / 60);
    m_offsetSpin->setVisible(initial.sourceZone.kind == ZoneChoice::Kind::FixedOffset);

    // Not edited by this dialog (the header menu owns it), but it must survive the
    // round trip: settings() builds a fresh FormatSettings, so without this a pattern
    // edit would silently reset the user's timestamp mode to the default.
    m_timeDisplay = initial.timeDisplay;
}

Encoding LogFormatDialog::currentEncoding() const
{
    const int i = m_encodingCombo->currentIndex();
    return (i >= 0 && i < int(std::size(kEncodingByIndex))) ? kEncodingByIndex[i] : Encoding::Auto;
}

FormatSettings LogFormatDialog::settings() const
{
    FormatSettings s;
    s.pattern = m_patternEdit->text();
    s.encoding = currentEncoding();

    s.sourceZone.kind = static_cast<ZoneChoice::Kind>(m_sourceZoneCombo->currentIndex());
    if (s.sourceZone.kind == ZoneChoice::Kind::FixedOffset)
        s.sourceZone.offsetSeconds = m_offsetSpin->value() * 60;

    s.timeDisplay = m_timeDisplay; // carried through untouched; see seed()
    return s;
}

void LogFormatDialog::refresh()
{
    const QString pattern = m_patternEdit->text();

    // Compile for immediate feedback. A failure is not fatal — the file opens as
    // plain text (SPEC.md §4) — so the preview simply falls back to an empty format.
    LogFormat format;
    auto compiled = PatternCompiler::compile(pattern);
    if (compiled) {
        format = compiled.value();
        m_errorLabel->setVisible(false);

        QStringList warns;
        if (format.prioGroup <= 0)
            warns << QStringLiteral("no %p — priority filtering will be unavailable");
        if (format.loggerGroup <= 0)
            warns << QStringLiteral("no %c — subsystem filtering will be unavailable");
        if (warns.isEmpty()) {
            m_warnLabel->setVisible(false);
        } else {
            m_warnLabel->setText(QStringLiteral("Warning: ") + warns.join(QStringLiteral("; ")));
            m_warnLabel->setVisible(true);
        }
    } else {
        const CompileError &e = compiled.error();
        const QString where = e.offset >= 0
            ? QStringLiteral("Error at position %1: %2").arg(e.offset).arg(e.message)
            : QStringLiteral("Error: %1").arg(e.message);
        m_errorLabel->setText(where);
        m_errorLabel->setVisible(true);
        m_warnLabel->setVisible(false);
    }

    // Resolve the encoding for the preview and report what auto-detect settled on.
    const Encoding enc = currentEncoding();
    const Decoder decoder = Decoder::detect(m_sample, enc);
    m_detectedLabel->setText(enc == Encoding::Auto
        ? QStringLiteral("(detected: %1)").arg(encodingName(decoder.resolvedEncoding()))
        : QString());

    // Build the live preview over the sample.
    const PreviewResult pv = FormatPreview::build(format, m_sample, decoder);

    const QStringList headers =
        pv.headers.isEmpty() ? QStringList{QStringLiteral("Text (unparsed)")} : pv.headers;
    const int cols = headers.size();
    m_previewTable->clear();
    m_previewTable->setColumnCount(cols);
    m_previewTable->setHorizontalHeaderLabels(headers);
    m_previewTable->setRowCount(pv.rows.size());

    for (int r = 0; r < pv.rows.size(); ++r) {
        const PreviewRow &row = pv.rows.at(r);
        if (row.matched && !pv.headers.isEmpty()) {
            for (int c = 0; c < cols; ++c) {
                const QString text = c < row.fields.size() ? row.fields.at(c) : QString();
                m_previewTable->setItem(r, c, new QTableWidgetItem(text));
            }
        } else {
            // Unparsed: show the raw line across the first column, dimmed, so a bad
            // pattern is visible rather than silently dropped (SPEC.md §4).
            auto *item = new QTableWidgetItem(row.rawFirstLine);
            item->setForeground(mutedColor(palette()));
            m_previewTable->setItem(r, 0, item);
            for (int c = 1; c < cols; ++c)
                m_previewTable->setItem(r, c, new QTableWidgetItem(QString()));
        }
    }

    // Fit the fixed-width columns to their contents (the sample is ~20 rows, so this
    // is cheap); Message keeps the remaining width via stretchLastSection.
    m_previewTable->resizeColumnsToContents();
    m_previewTable->resizeRowsToContents();

    if (pv.totalCount == 0)
        m_matchLabel->setText(QStringLiteral("No sample lines to preview."));
    else
        m_matchLabel->setText(QStringLiteral("%1 of %2 sample records matched the pattern.")
                                  .arg(pv.matchedCount)
                                  .arg(pv.totalCount));
}

void LogFormatDialog::detect()
{
    // Autodetect over the sample (M8, ARCHITECTURE.md §9) using the encoding the
    // dialog currently has selected, and fill the pattern field. Setting the text
    // triggers refresh() (the live preview), so the user sees the guess resolve and
    // confirms it with OK — detection never applies itself.
    const Decoder decoder = Decoder::detect(m_sample, currentEncoding());
    const DetectionResult r =
        FormatDetector::detect(QByteArrayView(m_sample.constData(), m_sample.size()), decoder);
    if (r.detected)
        m_patternEdit->setText(r.pattern);
    else
        m_matchLabel->setText(QStringLiteral("No log format could be detected — enter the pattern manually."));
}

} // namespace loftail
