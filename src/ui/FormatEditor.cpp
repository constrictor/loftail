#include "FormatEditor.h"

#include "UiColors.h"

#include "Decoder.h"
#include "Fonts.h"
#include "FormatDetector.h"
#include "FormatPreview.h"
#include "PatternCompiler.h"

#include <QComboBox>
#include <QCoreApplication>
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
// A form label that owns its mnemonic. QFormLayout's addRow(QString, QWidget *) sets the
// buddy for you; the QLayout overload cannot, and a buddy-less QLabel shows the '&'
// rather than acting on it.
QLabel *buddyLabel(const QString &text, QWidget *buddy)
{
    auto *label = new QLabel(text, buddy->parentWidget());
    label->setBuddy(buddy);
    return label;
}

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

// Not a member, so there is no tr() in scope; the context is named explicitly so these
// land in the catalogue beside the editor's other strings rather than in one of their
// own. The encoding NAMES are proper nouns and a translator will mostly leave them, but
// "Auto-detect" and "System 8-bit" are prose and the context is what tells them apart.
QString encodingName(Encoding e)
{
    switch (e) {
    case Encoding::Auto:    return QCoreApplication::translate("loftail::FormatEditor", "Auto-detect");
    case Encoding::Utf8:    return QCoreApplication::translate("loftail::FormatEditor", "UTF-8");
    case Encoding::Utf16LE: return QCoreApplication::translate("loftail::FormatEditor", "UTF-16 LE");
    case Encoding::Utf16BE: return QCoreApplication::translate("loftail::FormatEditor", "UTF-16 BE");
    case Encoding::System:  return QCoreApplication::translate("loftail::FormatEditor",
                                                               "System 8-bit");
    }
    return QString();
}
} // namespace

FormatEditor::FormatEditor(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    refresh();
}

void FormatEditor::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    // The editor is dropped into a dialog's or a group box's layout, which supplies the
    // margins; adding another set here would indent it twice.
    outer->setContentsMargins(0, 0, 0, 0);
    auto *form = new QFormLayout;
    outer->addLayout(form);

    // --- Pattern -----------------------------------------------------------
    auto *patRow = new QHBoxLayout;
    m_patternEdit = new QLineEdit(this);
    m_patternEdit->setObjectName(QStringLiteral("formatPatternEdit")); // findChild, for tests
    m_patternEdit->setPlaceholderText(QStringLiteral("e.g. %d{%Y-%m-%d %H:%M:%S,%q} [%t] %-5p %c - %m%n"));
    ensureReadablePlaceholder(m_patternEdit);
    patRow->addWidget(m_patternEdit, 1);
    // M8: re-run autodetection over the sample and fill the pattern field. It only
    // pre-fills — the user still confirms via the dialog's OK, so nothing is applied
    // silently. With no sample there is nothing to guess from, so it is disabled.
    m_detectButton = new QPushButton(tr("&Detect"), this);
    m_detectButton->setObjectName(QStringLiteral("formatDetectButton")); // findChild, for tests
    m_detectButton->setToolTip(tr("Guess the pattern from the sample lines"));
    m_detectButton->setEnabled(!m_sample.isEmpty());
    patRow->addWidget(m_detectButton);
    // Built by hand rather than by addRow(QString, QLayout *): that overload makes a
    // label with NO BUDDY, and a QLabel interprets '&' as a mnemonic only when it has
    // one — so every accelerator on a row whose field is a layout rendered as a literal
    // ampersand ("&Encoding:"), and did nothing. Measured: the label's width hint is
    // that of the text WITH the '&' in it. Same construction, and the same reason, as
    // the Host row in OpenRemoteDialog.
    //
    // The letter is C rather than the P of "conversion &pattern", because these
    // accelerators only became live here and P is already &Promote to Parent Pattern in the
    // dialog this editor sits in — two mnemonics on one letter make Alt+P a focus cycle
    // rather than a shortcut.
    form->addRow(buddyLabel(tr("&Conversion pattern:"), m_patternEdit), patRow);
    connect(m_patternEdit, &QLineEdit::textChanged, this, &FormatEditor::refresh);
    connect(m_detectButton, &QPushButton::clicked, this, &FormatEditor::detect);

    // Inline compile error, pointing at the offending offset (CompileError::offset).
    m_errorLabel = new QLabel(this);
    m_errorLabel->setObjectName(QStringLiteral("formatErrorLabel")); // findChild, for tests
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(errorColor(palette()).name()));
    m_errorLabel->setVisible(false);
    form->addRow(QString(), m_errorLabel);

    // Missing %p / %c warning (filtering on that axis degrades).
    m_warnLabel = new QLabel(this);
    m_warnLabel->setObjectName(QStringLiteral("formatWarnLabel")); // findChild, for tests
    m_warnLabel->setWordWrap(true);
    m_warnLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(warningColor(palette()).name()));
    m_warnLabel->setVisible(false);
    form->addRow(QString(), m_warnLabel);

    // --- Encoding ----------------------------------------------------------
    auto *encRow = new QHBoxLayout;
    m_encodingCombo = new QComboBox(this);
    m_encodingCombo->setObjectName(QStringLiteral("formatEncodingCombo")); // findChild, for tests
    for (const Encoding e : kEncodingByIndex)
        m_encodingCombo->addItem(encodingName(e));
    encRow->addWidget(m_encodingCombo);
    m_detectedLabel = new QLabel(this);
    m_detectedLabel->setObjectName(QStringLiteral("formatDetectedLabel")); // findChild, for tests
    m_detectedLabel->setToolTip(
        tr("What auto-detect makes of the sample lines previewed below. Every log is "
           "examined on its own when it is opened, so another one may come out "
           "differently."));
    m_detectedLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(mutedColor(palette()).name()));
    encRow->addWidget(m_detectedLabel);
    encRow->addStretch();
    form->addRow(buddyLabel(tr("&Encoding:"), m_encodingCombo), encRow);
    connect(m_encodingCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &FormatEditor::refresh);

    // --- Source time zone --------------------------------------------------
    auto *srcRow = new QHBoxLayout;
    m_sourceZoneCombo = new QComboBox(this);
    m_sourceZoneCombo->setObjectName(QStringLiteral("formatSourceZoneCombo")); // findChild, for tests
    m_sourceZoneCombo->addItem(tr("Infer from pattern")); // Default
    m_sourceZoneCombo->addItem(tr("Local time"));         // Local
    m_sourceZoneCombo->addItem(tr("UTC"));                // Utc
    m_sourceZoneCombo->addItem(tr("Fixed offset"));       // FixedOffset
    srcRow->addWidget(m_sourceZoneCombo);
    m_offsetSpin = new QSpinBox(this);
    m_offsetSpin->setObjectName(QStringLiteral("formatOffsetSpin")); // findChild, for tests
    m_offsetSpin->setRange(-720, 840); // minutes east of UTC (−12:00 … +14:00)
    m_offsetSpin->setSuffix(tr(" min"));
    m_offsetSpin->setToolTip(tr("Offset east of UTC, in minutes"));
    srcRow->addWidget(m_offsetSpin);
    srcRow->addStretch();
    form->addRow(buddyLabel(tr("&Source time zone:"), m_sourceZoneCombo), srcRow);
    connect(m_sourceZoneCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        m_offsetSpin->setVisible(m_sourceZoneCombo->currentIndex()
                                 == int(ZoneChoice::Kind::FixedOffset));
    });
    connect(m_offsetSpin, qOverload<int>(&QSpinBox::valueChanged), this, &FormatEditor::refresh);

    // There is deliberately no display-zone control here: how timestamps are SHOWN
    // is chosen from the timestamp column's header context menu, which offers the
    // zone modes alongside the two seconds modes (SPEC.md §4).

    // --- Live preview ------------------------------------------------------
    m_previewCaption = new QLabel(tr("Preview (sample lines split into fields):"), this);
    outer->addWidget(m_previewCaption);
    m_previewTable = new QTableWidget(this);
    m_previewTable->setObjectName(QStringLiteral("formatPreviewTable")); // findChild, for tests
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
    m_matchLabel->setObjectName(QStringLiteral("formatMatchLabel")); // findChild, for tests
    outer->addWidget(m_matchLabel);
}

void FormatEditor::setSample(const QByteArray &sample)
{
    m_sample = sample;
    // The sample arrives after construction here, so the two things that depend on it
    // must be brought up to date together: there is nothing to guess a pattern from
    // without one, and the preview is entirely a function of it.
    m_detectButton->setEnabled(!m_sample.isEmpty());
    refresh();
}

void FormatEditor::setPreviewCaption(const QString &text)
{
    m_previewCaption->setText(text);
}

void FormatEditor::setSettings(const FormatSettings &s)
{
    m_patternEdit->setText(s.pattern);
    m_encodingCombo->setCurrentIndex(encodingToIndex(s.encoding));

    m_sourceZoneCombo->setCurrentIndex(int(s.sourceZone.kind));
    if (s.sourceZone.kind == ZoneChoice::Kind::FixedOffset)
        m_offsetSpin->setValue(s.sourceZone.offsetSeconds / 60);
    m_offsetSpin->setVisible(s.sourceZone.kind == ZoneChoice::Kind::FixedOffset);

    // Not edited by this widget (the header menu and the Run pane own them), but they
    // must survive the round trip: settings() builds a fresh FormatSettings, so without
    // this a pattern edit would silently reset the user's timestamp mode and drop the
    // run-start pattern.
    m_timeDisplay = s.timeDisplay;
    m_runStartPattern = s.runStartPattern;
    m_runStartIsRegex = s.runStartIsRegex;
    m_runStartCaseSensitive = s.runStartCaseSensitive;
}

Encoding FormatEditor::currentEncoding() const
{
    const int i = m_encodingCombo->currentIndex();
    return (i >= 0 && i < int(std::size(kEncodingByIndex))) ? kEncodingByIndex[i] : Encoding::Auto;
}

FormatSettings FormatEditor::settings() const
{
    FormatSettings s;
    s.pattern = m_patternEdit->text();
    s.encoding = currentEncoding();

    s.sourceZone.kind = static_cast<ZoneChoice::Kind>(m_sourceZoneCombo->currentIndex());
    if (s.sourceZone.kind == ZoneChoice::Kind::FixedOffset)
        s.sourceZone.offsetSeconds = m_offsetSpin->value() * 60;

    // Carried through untouched; see setSettings().
    s.timeDisplay = m_timeDisplay;
    s.runStartPattern = m_runStartPattern;
    s.runStartIsRegex = m_runStartIsRegex;
    s.runStartCaseSensitive = m_runStartCaseSensitive;
    return s;
}

void FormatEditor::refresh()
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
            warns << tr("no %p — priority filtering will be unavailable");
        if (format.loggerGroup <= 0)
            warns << tr("no %c — subsystem filtering will be unavailable");
        if (warns.isEmpty()) {
            m_warnLabel->setVisible(false);
        } else {
            m_warnLabel->setText(tr("Warning: %1").arg(warns.join(QStringLiteral("; "))));
            m_warnLabel->setVisible(true);
        }
    } else {
        const CompileError &e = compiled.error();
        const QString where = e.offset >= 0
            ? tr("Error at position %1: %2").arg(e.offset).arg(e.message)
            : tr("Error: %1").arg(e.message);
        m_errorLabel->setText(where);
        m_errorLabel->setVisible(true);
        m_warnLabel->setVisible(false);
    }

    // Resolve the encoding for the preview and report what auto-detect settled on — but
    // ONLY when there were bytes to settle it from. Detection over an empty sample is not
    // a detection: sniff() finds no BOM and no UTF-16 pattern in nothing at all and falls
    // through to its UTF-8 default, so the label claimed "(detected: UTF-8)" with no log
    // open, and did it just as confidently for a pattern or the defaults, which are not
    // about any one file. What it says is a fact about the bytes in the preview below it,
    // and with no bytes there is no fact.
    const Encoding enc = currentEncoding();
    const Decoder decoder = Decoder::detect(m_sample, enc);
    //
    // "in the sample" is the other half of the same point. These settings may belong to a
    // pattern or to the defaults, which are about a CLASS of logs, while the sample is
    // whichever log happens to be open — so the label has to name what it looked at, or
    // it reads as a property of the node.
    m_detectedLabel->setText(enc == Encoding::Auto && !m_sample.isEmpty()
        ? tr("(detected in the sample: %1)").arg(encodingName(decoder.resolvedEncoding()))
        : QString());

    // Build the live preview over the sample.
    const PreviewResult pv = FormatPreview::build(format, m_sample, decoder);

    const QStringList headers =
        pv.headers.isEmpty() ? QStringList{tr("Text (unparsed)")} : pv.headers;
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
        m_matchLabel->setText(tr("No sample lines to preview."));
    else
        m_matchLabel->setText(tr("%1 of %2 sample records matched the pattern.")
                                  .arg(pv.matchedCount)
                                  .arg(pv.totalCount));
}

void FormatEditor::detect()
{
    // Autodetect over the sample (M8, ARCHITECTURE.md §9) using the encoding currently
    // selected, and fill the pattern field. Setting the text triggers refresh() (the
    // live preview), so the user sees the guess resolve and confirms it with the
    // dialog's OK — detection never applies itself.
    const Decoder decoder = Decoder::detect(m_sample, currentEncoding());
    const DetectionResult r =
        FormatDetector::detect(QByteArrayView(m_sample.constData(), m_sample.size()), decoder);
    if (r.detected)
        m_patternEdit->setText(r.pattern);
    else
        m_matchLabel->setText(tr("No log format could be detected — enter the pattern manually."));
}

} // namespace loftail
