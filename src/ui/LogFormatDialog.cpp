#include "LogFormatDialog.h"

#include "Decoder.h"
#include "FormatPreview.h"
#include "PatternCompiler.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
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
    m_patternEdit = new QLineEdit(this);
    m_patternEdit->setPlaceholderText(QStringLiteral("e.g. %d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"));
    form->addRow(QStringLiteral("Conversion &pattern:"), m_patternEdit);
    connect(m_patternEdit, &QLineEdit::textChanged, this, &LogFormatDialog::refresh);

    // Inline compile error, pointing at the offending offset (CompileError::offset).
    m_errorLabel = new QLabel(this);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral("color: #c0392b;")); // red
    m_errorLabel->setVisible(false);
    form->addRow(QString(), m_errorLabel);

    // Missing %p / %c warning (filtering on that axis degrades).
    m_warnLabel = new QLabel(this);
    m_warnLabel->setWordWrap(true);
    m_warnLabel->setStyleSheet(QStringLiteral("color: #b9770e;")); // amber
    m_warnLabel->setVisible(false);
    form->addRow(QString(), m_warnLabel);

    // --- Encoding ----------------------------------------------------------
    auto *encRow = new QHBoxLayout;
    m_encodingCombo = new QComboBox(this);
    for (const Encoding e : kEncodingByIndex)
        m_encodingCombo->addItem(encodingName(e));
    encRow->addWidget(m_encodingCombo);
    m_detectedLabel = new QLabel(this);
    m_detectedLabel->setStyleSheet(QStringLiteral("color: gray;"));
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

    // --- Display time zone -------------------------------------------------
    m_displayZoneCombo = new QComboBox(this);
    m_displayZoneCombo->addItem(QStringLiteral("As written in the file")); // Default
    m_displayZoneCombo->addItem(QStringLiteral("Local time"));             // Local
    m_displayZoneCombo->addItem(QStringLiteral("UTC"));                    // Utc
    form->addRow(QStringLiteral("&Display time zone:"), m_displayZoneCombo);

    // --- Live preview ------------------------------------------------------
    outer->addWidget(new QLabel(QStringLiteral("Preview (sample lines split into fields):"), this));
    m_previewTable = new QTableWidget(this);
    m_previewTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_previewTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_previewTable->horizontalHeader()->setStretchLastSection(true);
    m_previewTable->verticalHeader()->setVisible(false);
    outer->addWidget(m_previewTable, 1);

    m_matchLabel = new QLabel(this);
    outer->addWidget(m_matchLabel);

    // --- Buttons -----------------------------------------------------------
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
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

    // Display zone: FixedOffset is not offered here, so Default/Local/Utc map 1:1.
    const int di = (initial.displayZone.kind == ZoneChoice::Kind::Local) ? 1
                 : (initial.displayZone.kind == ZoneChoice::Kind::Utc)   ? 2
                                                                         : 0;
    m_displayZoneCombo->setCurrentIndex(di);
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

    switch (m_displayZoneCombo->currentIndex()) {
    case 1:  s.displayZone.kind = ZoneChoice::Kind::Local; break;
    case 2:  s.displayZone.kind = ZoneChoice::Kind::Utc;   break;
    default: s.displayZone.kind = ZoneChoice::Kind::Default; break;
    }
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
            item->setForeground(QColor(150, 150, 150));
            m_previewTable->setItem(r, 0, item);
            for (int c = 1; c < cols; ++c)
                m_previewTable->setItem(r, c, new QTableWidgetItem(QString()));
        }
    }

    if (pv.totalCount == 0)
        m_matchLabel->setText(QStringLiteral("No sample lines to preview."));
    else
        m_matchLabel->setText(QStringLiteral("%1 of %2 sample records matched the pattern.")
                                  .arg(pv.matchedCount)
                                  .arg(pv.totalCount));
}

} // namespace loftail
