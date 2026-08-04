#pragma once

#include "RemoteLocation.h"
#include "SshFetcher.h"

#include <QString>
#include <QVector>

namespace loftail {

// A remembered SSH host and the logs worth opening on it (SPEC.md §3, M11).
struct HostBookmark
{
    enum class Auth {
        Agent,    // SSH agent, then the usual ~/.ssh key files. The default.
        KeyFile,  // a specific private key
        Password, // ask, optionally remembering the answer — see `savePassword`
    };

    QString label;   // what the user calls this host; falls back to `host`
    QString user;
    QString host;
    int     port = RemoteLocation::kDefaultPort;
    Auth    auth = Auth::Agent;
    QString keyFile;             // Auth::KeyFile only
    QStringList paths;           // remembered log paths on this host
    int     pollMs = 1000;       // network poll cadence
    qint64  tailStartBytes = 0;  // 0 = fetch the whole file

    // Whether `password` below is stored ON DISK IN CLEAR TEXT. Off by default, and
    // the UI must state plainly what turning it on means before it can be turned on
    // (SshPromptDialogs). Nothing here is encrypted and nothing pretends to be:
    // an encryption key stored beside the thing it encrypts is theatre, and calling
    // it "saved securely" would be a lie a user might act on.
    bool    savePassword = false;
    QString password;

    QString displayName() const { return label.isEmpty() ? host : label; }
    RemoteLocation locationFor(const QString &path) const;
    SshFetchOptions fetchOptions() const;
};

// The saved hosts, as one schema-versioned JSON file written atomically — the same
// shape and the same guarantees as PresetStore (ARCHITECTURE.md §8/§8.1), and for
// the same reason: several loftail instances may be running.
//
// A FILE, not QSettings, specifically because a password may be in it. On Windows
// QSettings means the registry, which is a worse home for a secret and offers no way
// to restrict access; a file can be — and is — made readable by its owner alone.
class HostBookmarkStore
{
public:
    static constexpr int kSchemaVersion = 1;

    // Constructed against a directory so tests can isolate it, exactly as PresetStore
    // is; production passes defaultDir().
    explicit HostBookmarkStore(const QString &dir) : m_dir(dir) {}

    // The AppConfigLocation-based directory used in production (no hardcoded paths —
    // CLAUDE.md conventions). Empty if the location cannot be resolved.
    static QString defaultDir();

    // The file bookmarks live in, for showing the user exactly where a password they
    // asked to save will be written. A warning that does not name the file is not
    // much of a warning.
    QString filePath() const;

    QVector<HostBookmark> all() const;

    // Create or replace the bookmark whose (user, host, port) matches, keeping the
    // list stable otherwise. Returns false on a write failure.
    bool save(const HostBookmark &bookmark);
    bool remove(const QString &user, const QString &host, int port);

    // Replace the whole list — what the Open Remote dialog does on OK.
    bool replaceAll(const QVector<HostBookmark> &bookmarks);

    // The bookmark matching a location, if any, so an open can pick up its auth
    // choice, poll cadence and remembered password.
    static HostBookmark find(const QVector<HostBookmark> &bookmarks,
                             const RemoteLocation &location, bool *found = nullptr);

private:
    QString m_dir;
};

} // namespace loftail
