#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include "ArchiveLocation.h"
#include "LogFileStore.h"
#include "LogSource.h"
#include "RemoteLocation.h"

using namespace loftail;

// M12 — the nested-path value type for a log inside an archive (SPEC.md §3,
// ARCHITECTURE.md §6.4). Like its remote counterpart, an archived log travels through
// the whole application as a path STRING, so the contract under test is that one log
// has exactly ONE spelling. The two rules that carry that weight are the resolution
// rule (where does the container end?) and the collapse rule (a bare compressed stream
// keeps its plain path).
//
// UNGATED ON PURPOSE. Recognising, normalizing, persisting and displaying an archived
// path must be identical in a build with libarchive and one without, or the two would
// disagree about what a given settings file means — the reason RemoteLocation is
// always compiled, applied again. Core-only, no QApplication, no libarchive.
class TestArchiveLocation : public QObject
{
    Q_OBJECT

private slots:
    void classifiesSuffixes();
    void prefersTheLongerSuffixSoTarGzIsATar();
    void splitsAContainerFromItsMember();
    void aBareCompressedStreamHasAnImpliedMember();
    void aContainerWithNoMemberIsNotOpenable();
    void collapsesASingleStreamBackToItsPlainPath();
    void normalizeIsIdempotentAndWorkingDirectoryIndependent();
    void anExistingRegularFileIsNeverSplit();
    void aRemoteContainerSplitsAndKeepsItsAddress();
    void displayNamesReadLikeALog();
    void availabilityAsksAboutTheContainer();
    void anAddressWithNoMemberPickedIsNotWellFormed();
    void theSettingsKeyKeepsAnArchivedPathUnmangled();
    void aPlainPathIsUntouchedByAllOfIt();
    void openingReportsWhenArchivesAreNotBuiltIn();

private:
    // "/logs/x" is NOT an absolute path on Windows — it lacks a drive, so
    // absoluteFilePath() prepends the current one and toString() yields "D:/logs/x".
    // Expectations are therefore built through the same transform the code uses, so
    // they assert the RELATIONSHIP (container made absolute, member appended) rather
    // than a POSIX spelling that is only correct on one platform.
    static QString abs(const QString &path) { return QFileInfo(path).absoluteFilePath(); }

    // A real directory to resolve relative paths and rule 0 against.
    QTemporaryDir m_dir;
};

void TestArchiveLocation::classifiesSuffixes()
{
    QVERIFY(ArchiveLocation::isSingleStreamName(QStringLiteral("app.log.gz")));
    QVERIFY(ArchiveLocation::isSingleStreamName(QStringLiteral("app.log.xz")));
    QVERIFY(ArchiveLocation::isSingleStreamName(QStringLiteral("app.log.bz2")));
    QVERIFY(ArchiveLocation::isSingleStreamName(QStringLiteral("app.log.zst")));
    QVERIFY(ArchiveLocation::isSingleStreamName(QStringLiteral("APP.LOG.GZ"))); // case

    QVERIFY(ArchiveLocation::isContainerName(QStringLiteral("bundle.zip")));
    QVERIFY(ArchiveLocation::isContainerName(QStringLiteral("bundle.tar")));
    QVERIFY(ArchiveLocation::isContainerName(QStringLiteral("bundle.7z")));
    QVERIFY(ArchiveLocation::isContainerName(QStringLiteral("bundle.TGZ"))); // case

    // A plain log is neither, and neither is a name that merely contains a suffix.
    QVERIFY(!ArchiveLocation::isSingleStreamName(QStringLiteral("app.log")));
    QVERIFY(!ArchiveLocation::isContainerName(QStringLiteral("app.log")));
    QVERIFY(!ArchiveLocation::isSingleStreamName(QStringLiteral("gz")));
    QVERIFY(!ArchiveLocation::isSingleStreamName(QStringLiteral(".gz"))); // no stem

    // Classification is pure string work: it must answer for a path that is not there.
    QVERIFY(ArchiveLocation::isArchivePath(QStringLiteral("/nowhere/at/all/app.log.gz")));
}

void TestArchiveLocation::prefersTheLongerSuffixSoTarGzIsATar()
{
    // The whole reason the container table is consulted first: a .tar.gz holds many
    // members and must be treated as one, not as a bare gzip stream with an implied one.
    QVERIFY(ArchiveLocation::isContainerName(QStringLiteral("logs.tar.gz")));
    QVERIFY(!ArchiveLocation::isSingleStreamName(QStringLiteral("logs.tar.gz")));
    QVERIFY(ArchiveLocation::isContainerName(QStringLiteral("logs.tar.xz")));
    QVERIFY(!ArchiveLocation::isSingleStreamName(QStringLiteral("logs.tar.xz")));
}

void TestArchiveLocation::splitsAContainerFromItsMember()
{
    const auto loc = ArchiveLocation::split(QStringLiteral("/logs/bundle.tar.gz/var/log/app.log"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->container, QStringLiteral("/logs/bundle.tar.gz"));
    QCOMPARE(loc->member, QStringLiteral("var/log/app.log"));
    QVERIFY(!loc->isSingleStream());
    QVERIFY(!loc->needsMember());
    QVERIFY(loc->isOpenable());
    QCOMPARE(loc->toString(),
             abs(QStringLiteral("/logs/bundle.tar.gz")) + QStringLiteral("/var/log/app.log"));
}

void TestArchiveLocation::aBareCompressedStreamHasAnImpliedMember()
{
    const auto loc = ArchiveLocation::split(QStringLiteral("/logs/app.log.gz"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->container, QStringLiteral("/logs/app.log.gz"));
    QVERIFY(loc->member.isEmpty());
    QVERIFY(loc->isSingleStream());
    // Empty member means two very different things; this is the one where it is fine.
    QVERIFY(!loc->needsMember());
    QVERIFY(loc->isOpenable());
    QCOMPARE(loc->displayMember(), QStringLiteral("app.log"));
}

void TestArchiveLocation::aContainerWithNoMemberIsNotOpenable()
{
    const auto loc = ArchiveLocation::split(QStringLiteral("/logs/bundle.zip"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->container, QStringLiteral("/logs/bundle.zip"));
    QVERIFY(loc->member.isEmpty());
    QVERIFY(loc->needsMember());
    QVERIFY(!loc->isOpenable());

    // And opening it says so rather than failing obscurely, because the member is
    // picked once at the entry point and this address never got one.
    QString error;
    QVERIFY(!openLogSource(QStringLiteral("/logs/bundle.zip"), OpenPolicy::Interactive, &error));
    QVERIFY2(error.contains(QStringLiteral("several logs")), qPrintable(error));
}

void TestArchiveLocation::collapsesASingleStreamBackToItsPlainPath()
{
    // THE COLLAPSE RULE. Without it `/logs/app.log.gz` and the nested spelling of the
    // same thing would be two Document paths — two tabs, two format-cache entries and
    // two spools for one log.
    QCOMPARE(ArchiveLocation::normalize(QStringLiteral("/logs/app.log.gz")),
             abs(QStringLiteral("/logs/app.log.gz")));
    QCOMPARE(ArchiveLocation::normalize(QStringLiteral("/logs/app.log.gz/app.log")),
             abs(QStringLiteral("/logs/app.log.gz")));

    // A multi-member container does NOT collapse: the member is what is being read.
    QCOMPARE(ArchiveLocation::normalize(QStringLiteral("/logs/b.tgz/app.log")),
             abs(QStringLiteral("/logs/b.tgz")) + QStringLiteral("/app.log"));
}

void TestArchiveLocation::normalizeIsIdempotentAndWorkingDirectoryIndependent()
{
    const QString absolute = m_dir.path() + QStringLiteral("/b.tgz/app.log");
    const QString once = normalizeLogPath(absolute);
    QCOMPARE(normalizeLogPath(once), once);

    // A relative spelling from inside the directory must reach the same key, or the
    // format cache would lose the file's remembered format when the cwd changed.
    const QString previous = QDir::currentPath();
    QVERIFY(QDir::setCurrent(m_dir.path()));
    const QString relative = normalizeLogPath(QStringLiteral("b.tgz/app.log"));
    QVERIFY(QDir::setCurrent(previous));
    QCOMPARE(relative, once);
}

void TestArchiveLocation::anExistingRegularFileIsNeverSplit()
{
    // RULE 0. A real directory called `bundle.zip` with a log in it is not an archive,
    // and the file that is actually there wins over the reading where it is one.
    QVERIFY(QDir(m_dir.path()).mkpath(QStringLiteral("bundle.zip")));
    const QString inside = m_dir.path() + QStringLiteral("/bundle.zip/app.log");
    QFile f(inside);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("hello\n");
    f.close();

    QVERIFY(!ArchiveLocation::split(inside).has_value());
    QVERIFY(!ArchiveLocation::isArchivePath(inside));
    QCOMPARE(normalizeLogPath(inside), inside);
    QVERIFY(!logPathIsSpooled(inside));

    // The same string with nothing on disk behind it DOES read as an archive member —
    // which is the deliberate asymmetry, and why rule 0 is checked first.
    const QString absent = m_dir.path() + QStringLiteral("/other.zip/app.log");
    QVERIFY(ArchiveLocation::isArchivePath(absent));
}

void TestArchiveLocation::aRemoteContainerSplitsAndKeepsItsAddress()
{
    // The point of the nested spelling: an archive is a file type and SSH is a way of
    // reaching a file, so the two compose with no new scheme to invent.
    const auto loc = ArchiveLocation::split(
        QStringLiteral("ssh://deploy@web1/var/log/bundle.tar.gz/app.log"));
    QVERIFY(loc.has_value());
    QCOMPARE(loc->container, QStringLiteral("ssh://deploy@web1:22/var/log/bundle.tar.gz"));
    QCOMPARE(loc->member, QStringLiteral("app.log"));
    QVERIFY(loc->isOpenable());
    QVERIFY(logPathIsSpooled(loc->toString()));

    // sftp:// and a spelled-out default port are the same log, as they are without an
    // archive in the picture.
    QCOMPARE(normalizeLogPath(QStringLiteral("sftp://deploy@web1:22/var/log/bundle.tar.gz/app.log")),
             normalizeLogPath(QStringLiteral("ssh://deploy@web1/var/log/bundle.tar.gz/app.log")));

    // A remote log that is merely compressed collapses too.
    QCOMPARE(normalizeLogPath(QStringLiteral("ssh://web1/var/log/app.log.1.gz")),
             QStringLiteral("ssh://web1:22/var/log/app.log.1.gz"));
}

void TestArchiveLocation::displayNamesReadLikeALog()
{
    // The member is what the user is reading; the container is context.
    QCOMPARE(logSourceDisplayName(QStringLiteral("/logs/bundle.tar.gz/var/log/app.log")),
             QStringLiteral("app.log (bundle.tar.gz)"));

    // A bare compressed stream names the log the writer meant, once — not twice.
    QCOMPARE(logSourceDisplayName(QStringLiteral("/logs/app.log.gz")),
             QStringLiteral("app.log"));
    QCOMPARE(logSourceDisplayName(QStringLiteral("ssh://web1/var/log/app.log.gz")),
             QStringLiteral("app.log (web1)"));

    QCOMPARE(logSourceDisplayPath(QStringLiteral("/logs/b.tgz/app.log")),
             abs(QStringLiteral("/logs/b.tgz")) + QStringLiteral("/app.log"));
}

void TestArchiveLocation::availabilityAsksAboutTheContainer()
{
    // Session restore calls this on every remembered path, so it must not open the
    // archive: confirming the member is in there costs what expanding it costs.
    const QString container = m_dir.path() + QStringLiteral("/present.tgz");
    QFile f(container);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("not really a tarball");
    f.close();

    QVERIFY(logSourceAvailable(container + QStringLiteral("/app.log")));
    QVERIFY(logSourceAvailable(container + QStringLiteral("/no/such/member.log")));
    QVERIFY(!logSourceAvailable(m_dir.path() + QStringLiteral("/absent.tgz/app.log")));
}

void TestArchiveLocation::anAddressWithNoMemberPickedIsNotWellFormed()
{
    // The line between waiting and failing (§6.5), and this side of it is decidable
    // with no I/O at all: a multi-member container with no member spelled out names no
    // log, whether or not the container is there. Answering "well-formed" for one that
    // was merely absent turned M17's refusal into a wait that could not end — the file
    // arriving changed nothing, because the address still named nothing to open.
    const QString absent = m_dir.path() + QStringLiteral("/nothere.tar.gz");
    QVERIFY(!logPathIsWellFormed(absent));
    QVERIFY(!logPathIsWellFormed(m_dir.path() + QStringLiteral("/nothere.zip")));

    // A member picked, and a single-stream container which implies its own, are both
    // well-formed while they are missing — that is exactly the case that waits.
    QVERIFY(logPathIsWellFormed(absent + QStringLiteral("/var/log/app.log")));
    QVERIFY(logPathIsWellFormed(m_dir.path() + QStringLiteral("/nothere.log.gz")));
}

void TestArchiveLocation::theSettingsKeyKeepsAnArchivedPathUnmangled()
{
    // The latent bug M11 fixed for URLs, in its archived form: canonicalFilePath() is
    // empty for a member that is not a file on this filesystem, and the absolute-path
    // fallback would then produce a working-directory-dependent key.
    const QString path = m_dir.path() + QStringLiteral("/b.tgz/var/log/app.log");
    const QString key = logSettingsKey(path);
    QCOMPARE(key, normalizeLogPath(path));

    const QString previous = QDir::currentPath();
    QVERIFY(QDir::setCurrent(QDir::tempPath()));
    const QString elsewhere = logSettingsKey(path);
    QVERIFY(QDir::setCurrent(previous));
    QCOMPARE(elsewhere, key);

    // And settings saved against it come back — through the per-log pool (M21), which
    // is where the file level lives now. The key function is unchanged, so what this
    // asserts is unchanged; only the store it asks has moved.
    QTemporaryDir configDir;
    QVERIFY(configDir.isValid());
    LogFileStore store(configDir.path());
    store.load();

    LogFileSettings saved;
    saved.address = path;
    saved.profile = LogProfile::builtIn();
    saved.profile->format.pattern = QStringLiteral("%d %p %c - %m%n");
    QVERIFY(store.save(saved, LogProfile::builtIn()));

    const LogFileSettings hit = store.read(path);
    QVERIFY(hit.profile.has_value());
    QCOMPARE(hit.profile->format.pattern, saved.profile->format.pattern);
}

void TestArchiveLocation::aPlainPathIsUntouchedByAllOfIt()
{
    // The fall-through every call site depends on: adding archives must cost an
    // ordinary log nothing at all.
    const QString plain = QStringLiteral("/var/log/app.log");
    QVERIFY(!ArchiveLocation::isArchivePath(plain));
    QVERIFY(!logPathIsSpooled(plain));
    QCOMPARE(normalizeLogPath(plain), plain);
    QCOMPARE(logSourceDisplayName(plain), QStringLiteral("app.log"));
    QCOMPARE(logSourceDisplayPath(plain), plain);
    QCOMPARE(normalizeLogPath(QString()), QString());
}

void TestArchiveLocation::openingReportsWhenArchivesAreNotBuiltIn()
{
    QString error;
    const std::unique_ptr<LogSource> src =
        openLogSource(m_dir.path() + QStringLiteral("/b.tgz/app.log"),
                      OpenPolicy::Interactive, &error);
#if defined(LOFTAIL_HAVE_ARCHIVE)
    // The container is not a real tarball, so this fails — but it must fail because
    // the bytes are wrong, never because support is missing.
    QVERIFY(!error.contains(QStringLiteral("not built into")));
#else
    // Present but unsupported explains itself, exactly as the SSH-less build does.
    QVERIFY(!src);
    QVERIFY2(error.contains(QStringLiteral("not built into this copy")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("libarchive")), qPrintable(error));
#endif
}

QTEST_APPLESS_MAIN(TestArchiveLocation)
#include "tst_archivelocation.moc"
