#include "AtomicJson.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr() — and these strings are user-facing all the same: they travel up to
// the status bar through Document::lastError() and LiveController::sourceStatusChanged.
// Q_DECLARE_TR_FUNCTIONS is what lets lupdate file them under a name that means
// something rather than under the file they happen to sit in.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::AtomicJson)
};
} // namespace


bool AtomicJson::write(const QString &path, const QJsonDocument &doc, QString *error)
{
    const QFileInfo info(path);
    const QDir dir = info.absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error)
            *error = Tr::tr("Cannot create directory: %1").arg(dir.absolutePath());
        return false;
    }

    // QSaveFile writes to a temporary sibling and atomically renames it over `path`
    // on commit() — the temp-file+rename the multi-instance case needs (§8.1). On
    // any failure it discards the temp file, so `path` keeps its previous contents.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        if (error)
            *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool AtomicJson::writePrivate(const QString &path, const QJsonDocument &doc, QString *error)
{
    if (!write(path, doc, error))
        return false;
    // After the rename, not before: commit() replaces the file at `path`, so a mode
    // set on the temporary file would not survive. A failure here is worth reporting
    // — the caller wrote a secret expecting it to be unreadable by others.
    if (!QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner)) {
        if (error)
            *error = Tr::tr("Cannot restrict permissions on %1").arg(path);
        return false;
    }
    return true;
}

QJsonDocument AtomicJson::read(const QString &path, bool *ok)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (ok)
            *ok = false;
        return {};
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (ok)
        *ok = (err.error == QJsonParseError::NoError);
    return doc;
}

} // namespace loftail
