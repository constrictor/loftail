// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RemoteLocation.h"
#include "SshFetcher.h"

#include <QString>
#include <QVector>

#include <utility>

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
    // Ask this host to deflate what it sends. Off by default, because the deflating is
    // done by the machine holding the log (SshFetchOptions::compress). Persisted as an
    // ADDED key with no schema bump: an older binary reads it as absent, which is the
    // default, where a bump would make it refuse the whole file and lose every host.
    bool    compress = false;

    // Whether `password` below is stored ON DISK IN CLEAR TEXT. Off by default, and
    // the UI must state plainly what turning it on means before it can be turned on
    // (SshPromptDialogs). Nothing here is encrypted and nothing pretends to be:
    // an encryption key stored beside the thing it encrypts is theatre, and calling
    // it "saved securely" would be a lie a user might act on.
    //
    // An OS KEYCHAIN is the real answer to that objection, and is preferred wherever
    // one will answer (SecretStore.h, M14) — it holds the key somewhere loftail cannot
    // reach, which is exactly what a key sitting beside its ciphertext does not. This
    // field is what remains when there is none: a fallback the user is told about BY
    // NAME before they consent to it, never a substitute chosen behind their back. A
    // keychain that is present and refuses does NOT land here; it is reported instead.
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
    explicit HostBookmarkStore(QString dir) : m_dir(std::move(dir)) {}

    // The AppConfigLocation-based directory used in production (no hardcoded paths —
    // CLAUDE.md conventions). Empty if the location cannot be resolved.
    static QString defaultDir();

    // The file bookmarks live in, for showing the user exactly where a password they
    // asked to save will be written. A warning that does not name the file is not
    // much of a warning.
    QString filePath() const;

    // Name-unique and in saved order. A file written before names became the identity
    // may hold duplicates, so later ones are dropped here rather than shown as list
    // entries that cannot be told apart or removed individually.
    QVector<HostBookmark> all() const;

    // Create or replace the bookmark with the same NAME, keeping the list stable
    // otherwise. Returns false on a write failure.
    //
    // The name is the identity because it is what the list shows: two entries reading
    // the same are indistinguishable to the person picking one, whatever differs
    // underneath. Saving therefore overwrites silently — there is nothing to confirm,
    // since the user named the thing they are saving.
    bool save(const HostBookmark &bookmark) const;
    bool remove(const QString &name) const;

    // Names are compared trimmed and case-insensitively: "Prod" and "prod " are the
    // same entry, for the same reason as above.
    static bool sameName(const QString &a, const QString &b);
    static int indexOfName(const QVector<HostBookmark> &bookmarks, const QString &name);

    // Replace the whole list — what the Open Remote dialog does on OK.
    bool replaceAll(const QVector<HostBookmark> &bookmarks) const;

    // The FIRST bookmark matching a location, if any, so an open can pick up its auth
    // choice, poll cadence and remembered password. Uniqueness is by name, so one
    // connection may be saved under several names; an ssh:// address carries no name,
    // and all of them describe the same connection, so the first will do.
    static HostBookmark find(const QVector<HostBookmark> &bookmarks,
                             const RemoteLocation &location, bool *found = nullptr);

    // The bookmark keyed by the string the credential cache and the keychain are BOTH
    // keyed on — RemoteLocation::target(). What passwordAccepted() needs, since it is
    // handed a target and nothing else.
    //
    // Compares FORWARDS — locationFor({}).target() == target — rather than parsing
    // "user@host:port" back apart, which target() does not support: it emits "host:port"
    // with no '@' when there is no user, and an IPv6 literal carries colons of its own.
    // Forward comparison cannot get either wrong. Returns -1 when there is none.
    static int indexOfTarget(const QVector<HostBookmark> &bookmarks, const QString &target);

private:
    QString m_dir;
};

} // namespace loftail
