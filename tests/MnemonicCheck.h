#pragma once

#include <QAbstractButton>
#include <QChar>
#include <QHash>
#include <QLabel>
#include <QString>
#include <QWidget>

// Two things that are silently wrong about an '&' in a dialog, checked over a whole
// window at once because both are properties of the window rather than of one widget.
//
// A '&' in a QLabel is a mnemonic ONLY when the label has a buddy; without one Qt draws
// the ampersand and the accelerator does nothing. That is exactly what QFormLayout's
// addRow(QString, QLayout *) overload produces — it has no widget to point the buddy at —
// so a form row whose field is a layout reads "&Encoding:" on screen. A button parses its
// own '&' and needs no buddy, but draws from the same pool of letters.
//
// And two live mnemonics on one letter make Alt+that a focus cycle between them instead
// of a shortcut to either, which is invisible until somebody presses it.
namespace loftail_test {

// The letter an accelerator claims, or a null QChar. "&&" is an escaped literal.
inline QChar mnemonicOf(const QString &text)
{
    const QString bare = QString(text).replace(QStringLiteral("&&"), QString());
    const int i = bare.indexOf(u'&');
    return i >= 0 && i + 1 < bare.size() ? bare.at(i + 1).toUpper() : QChar();
}

// False with `error` filled in when a mnemonic is dead or shared. `expected` is what the
// caller believes is there to find, so a check that has quietly stopped reaching any
// mnemonic at all fails rather than passing on an empty set.
inline bool mnemonicsAreSound(const QWidget *window, QString *error, int expected = 1)
{
    QHash<QChar, QString> claimed;
    const auto claim = [&](const QChar key, const QString &text) {
        if (claimed.contains(key)) {
            *error = QStringLiteral("Alt+%1 is claimed by both \"%2\" and \"%3\"")
                         .arg(key, claimed.value(key), text);
            return false;
        }
        claimed.insert(key, text);
        return true;
    };

    for (const QLabel *label : window->findChildren<const QLabel *>()) {
        const QChar key = mnemonicOf(label->text());
        if (key.isNull())
            continue;
        if (!label->buddy()) {
            *error = QStringLiteral("label \"%1\" draws its '&' instead of acting on it: "
                                    "no buddy").arg(label->text());
            return false;
        }
        if (!claim(key, label->text()))
            return false;
    }
    for (const QAbstractButton *button : window->findChildren<const QAbstractButton *>()) {
        const QChar key = mnemonicOf(button->text());
        if (key.isNull())
            continue;
        if (!claim(key, button->text()))
            return false;
    }

    if (claimed.size() < expected) {
        *error = QStringLiteral("found %1 mnemonics, expected at least %2: the check is "
                                "no longer reaching them").arg(claimed.size()).arg(expected);
        return false;
    }
    return true;
}

} // namespace loftail_test
