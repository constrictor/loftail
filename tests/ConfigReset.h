#pragma once

#include "LogFileStore.h"
#include "LogSettingsStore.h"

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

} // namespace loftail
