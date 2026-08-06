#pragma once

#include <QList>
#include <QString>

QT_BEGIN_NAMESPACE
class QObject;
class QTranslator;
QT_END_NAMESPACE

namespace loftail {

// What language the user interface is in, decided once at startup.
//
// loftail's own strings all go through tr() (CLAUDE.md, "Localization"), but no
// catalogue ships yet, so today this always settles on the source language — English.
// The reason it is a decision at all, rather than nothing, is that QT'S OWN strings do
// not settle there by themselves.
//
// A `QDialogButtonBox`'s Open and Cancel, a `QMessageBox`'s Yes and No, and the standard
// shortcut names all come from Qt's `qtbase_<lang>` catalogue, which something else
// loads for the system locale before main() gets a say — Qt itself in some builds, the
// platform theme plugin in others (KDE's does). The result on a non-English desktop was
// a dialog whose ten labels were English and whose two buttons were not. Half-translated
// reads worse than either end state, and which layer did the loading is not something
// loftail can portably know or unwind.
//
// So the language is stated rather than inferred: install the catalogues that match the
// language loftail is actually speaking, and where loftail has no catalogue, pin Qt's
// strings to their source text so both halves of every dialog agree. Nothing here
// touches QLocale, so dates, numbers and file sizes stay in the user's own format —
// which is a separate question from what language a button says, and the one place the
// system locale should still win (invariant #10, and OpenArchiveDialog's size column).
//
// The moment a real `loftail_<lang>.qm` is shipped and matches, this stops pinning
// anything and installs that catalogue plus Qt's for the same language instead. No call
// site changes; the pin exists only for the gap.

// Choose the UI language and install the translators for it. Call once, after the
// QApplication exists and BEFORE anything user-visible is built — the command-line
// parser's help text and MainWindow's entire menu bar are both constructed once and
// never re-read, so a translator installed after them arrives too late.
//
// Returns the translators installed, parented to `parent` (or to the application), in
// installation order. Callers can ignore it; a test uses it to undo the install.
QList<QTranslator *> installUiLanguage(QObject *parent = nullptr);

// The language tag settled on — a BCP 47 tag such as "de", or "en" when no catalogue
// matched and the source language is being used as-is.
QString uiLanguage();

} // namespace loftail
