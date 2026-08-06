#include "UiLanguage.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QStringList>
#include <QTranslator>

namespace loftail {

namespace {

// The language settled on. Written once by installUiLanguage(), read by uiLanguage().
QString g_uiLanguage = QStringLiteral("en");

// The source language of every tr() in the codebase. Not a user setting and not
// negotiable: it is what the string literals in src/ are written in.
constexpr auto kSourceLanguage = "en";

// A translator that answers every lookup with the source text.
//
// Installed only when loftail has no catalogue for the user's language, and installed
// LAST — QCoreApplication::installTranslator() prepends, and translate() stops at the
// first non-null answer, so the last one installed is consulted first. That ordering is
// the whole mechanism: it does not remove whatever loaded Qt's own catalogue (there is
// no portable way to reach a translator installed by a plugin), it simply answers ahead
// of it, and answers in the language loftail is speaking.
//
// It answers for loftail's own contexts too, which is correct and free: the source text
// IS the English string, so tr("Open") returns "Open" exactly as it would with no
// translator at all.
class SourceTextTranslator final : public QTranslator
{
public:
    using QTranslator::QTranslator;

    QString translate(const char *context, const char *sourceText, const char *disambiguation,
                      int n) const override
    {
        Q_UNUSED(context);
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        // A null return means "no answer, ask the next translator", so an empty source
        // must fall through rather than being claimed. Everything else is claimed —
        // including plural lookups, where returning the source leaves %n to be
        // substituted by the caller, which is what an untranslated build already does.
        if (!sourceText || !*sourceText)
            return QString();
        return QString::fromUtf8(sourceText);
    }

    // Not empty: it answers everything. isEmpty() only decides whether installing this
    // sends a LanguageChange event, and one that changes every string should.
    bool isEmpty() const override { return false; }
};

// Where a shipped loftail_<lang>.qm would be found. None of these exist today; they are
// the three layouts a catalogue could arrive in, checked cheapest first.
QStringList catalogueDirs()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    return {QStringLiteral(":/i18n"), appDir + QStringLiteral("/translations"),
            appDir + QStringLiteral("/../share/loftail/translations")};
}

// Load `name`_<lang> for the first of `languages` that has one, from `dir`. Returns the
// language tag that matched, or an empty string.
QString loadFirstMatch(QTranslator *translator, const QString &name, const QStringList &languages,
                       const QString &dir)
{
    for (const QString &language : languages) {
        // Qt matches "de_AT" against a "de" catalogue itself, so the bare tag needs no
        // separate pass; what it will not do is try the next language for us.
        if (translator->load(name + QLatin1Char('_') + language, dir))
            return language;
    }
    return QString();
}

} // namespace

QList<QTranslator *> installUiLanguage(QObject *parent)
{
    QObject *owner = parent ? parent : static_cast<QObject *>(QCoreApplication::instance());
    QList<QTranslator *> installed;

    // uiLanguages() is the user's ordered preference, not just one locale, and its first
    // entry is not always the most specific — walk it in order and take the first hit.
    QStringList languages = QLocale::system().uiLanguages();
    for (QString &language : languages)
        language.replace(QLatin1Char('-'), QLatin1Char('_'));

    auto *appCatalogue = new QTranslator(owner);
    QString matched;
    const QStringList dirs = catalogueDirs();
    for (const QString &dir : dirs) {
        matched = loadFirstMatch(appCatalogue, QStringLiteral("loftail"), languages, dir);
        if (!matched.isEmpty())
            break;
    }

    if (matched.isEmpty()) {
        // No catalogue: loftail speaks its source language, so Qt must too.
        delete appCatalogue;
        g_uiLanguage = QString::fromLatin1(kSourceLanguage);
        auto *pin = new SourceTextTranslator(owner);
        QCoreApplication::installTranslator(pin);
        installed.append(pin);
        return installed;
    }

    g_uiLanguage = matched;

    // Qt's own catalogue for the SAME language, so the standard buttons agree with the
    // labels beside them. Installed first, so loftail's own catalogue (installed after,
    // and therefore consulted before) wins wherever the two define the same context.
    auto *qtCatalogue = new QTranslator(owner);
    if (qtCatalogue->load(QStringLiteral("qtbase_") + matched,
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QCoreApplication::installTranslator(qtCatalogue);
        installed.append(qtCatalogue);
    } else {
        delete qtCatalogue;
    }

    QCoreApplication::installTranslator(appCatalogue);
    installed.append(appCatalogue);
    return installed;
}

QString uiLanguage()
{
    return g_uiLanguage;
}

} // namespace loftail
