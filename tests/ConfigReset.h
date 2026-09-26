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

#include "LogFileStore.h"
#include "LogSettingsStore.h"
#include "SerialFetchOptions.h"
#include "SerialProfileStore.h"

#include <QDir>
#include <QFile>

namespace loftail {

// EVERY STORE A LOG'S SETTINGS CAN BE IN, cleared between cases.
//
// The GUI suites isolate their configuration by pointing XDG_CONFIG_HOME (and friends) at
// a QTemporaryDir in main(), which is per-PROCESS and not per-case — so a case that opens
// a log and gives it a format leaves that behind for every case after it, and the one
// that then asserts "this log has no settings" passes or fails on which order QtTest ran
// them in. Four suites had grown their own copy of the two lines that answer that, and
// M21 added a THIRD place to clear: the per-log pool is a directory, so a QFile::remove()
// of one path no longer covers it.
//
// One helper rather than four copies, because the cost of missing the new one is a test
// that is green for a reason nobody chose.
inline void clearLogSettings()
{
    QFile::remove(LogSettingsStore(LogSettingsStore::defaultDir()).filePath());
    QDir(LogFileStore(LogFileStore::defaultDir()).directory()).removeRecursively();
}

// The serial presets and the remembered devices (M27).
//
// SEPARATE FROM clearLogSettings() ON PURPOSE, because it is not part of "what this log's
// settings are": a preset is a free-standing named thing, and the header above already records
// that this function is not the whole reset and never was. A case that writes a preset clears it
// itself, and a case that only reads logsettings.json need not pay for a second file removal.
inline void clearSerialSettings()
{
    QFile::remove(SerialProfileStore(SerialProfileStore::defaultDir()).filePath());
    clearSerialFetchOptions();
}

} // namespace loftail
