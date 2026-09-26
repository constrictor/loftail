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

#include "SerialProfile.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <utility>

namespace loftail {

// A remembered serial device and the logs worth opening on it (SPEC.md §3).
//
// THE PRESET NAME IS NOT IN HERE, and that is deliberate. Which preset applies to a log is
// answered by LogProfile::serialProfile resolved through the three-level settings tree — which
// is what makes `serial://*` able to say "every board of ours is a Linux box" — and a second
// place saying it is the drift this tree forbids. This record holds only what the tree cannot:
// which logs on this device are worth listing.
struct SerialDeviceBookmark
{
    QString     id;      // the device name an address carries: a stable id or a port name
    QString     label;   // what the user calls it; falls back to `id`
    QStringList paths;   // remembered log paths on it

    QString displayName() const { return label.isEmpty() ? id : label; }
};

// The presets and the remembered devices, as one schema-versioned JSON file written atomically —
// HostBookmarkStore's shape and its guarantees, for its reason: several loftail instances may be
// running.
//
// A FILE RATHER THAN QSettings, and here for a narrower reason than hosts.json's: no password
// lives in this one — a serial login's password goes to the keychain under target(), like every
// other — but the pattern is worth keeping identical so that a future field which IS a secret
// does not need the store moved underneath it.
class SerialProfileStore
{
public:
    static constexpr int kSchemaVersion = 1;

    explicit SerialProfileStore(QString dir) : m_dir(std::move(dir)) {}

    // The AppConfigLocation-based directory used in production (no hardcoded paths).
    static QString defaultDir();
    QString filePath() const;

    // Name-unique and in saved order. A file written before names became the identity may hold
    // duplicates, so later ones are dropped here rather than shown as list entries that cannot
    // be told apart or removed individually.
    QVector<SerialProfile> presets() const;

    // Create or replace the preset with the same NAME, keeping the list stable otherwise. The
    // name is the identity because it is what the list shows: two entries reading the same are
    // indistinguishable to the person picking one, whatever differs underneath.
    bool savePreset(const SerialProfile &preset) const;
    bool removePreset(const QString &name) const;
    bool replacePresets(const QVector<SerialProfile> &presets) const;

    // The named preset, or builtIn() where there is none.
    //
    // A MISSING PRESET FALLS BACK RATHER THAN REFUSING THE OPEN. The settings tree and this file
    // are separate documents a user may edit or copy independently, so a renamed preset is an
    // ordinary state — and a log that will not open because of it is worse than one that opens
    // at 115200 8N1 and says so in the diagnostic log (SPEC.md §3).
    SerialProfile presetNamed(const QString &name) const;

    QVector<SerialDeviceBookmark> devices() const;
    bool saveDevice(const SerialDeviceBookmark &device) const;
    bool forgetPath(const QString &deviceId, const QString &path) const;

    // Names are compared trimmed and case-insensitively: "Board" and "board " are one entry, for
    // the same reason as above.
    static bool sameName(const QString &a, const QString &b);
    static int indexOfName(const QVector<SerialProfile> &presets, const QString &name);

private:
    QString m_dir;
};

} // namespace loftail
