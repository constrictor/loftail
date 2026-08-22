#pragma once

#include <QString>

namespace loftail {

// Where a log's CONFIG FILE is (SPEC.md §4). An application that writes a log4cplus log
// is configured by a file saying which subsystems log at which priorities; the settings
// tree carries a path to it (LogProfile::configPath) and this turns that path, plus the
// log it was configured for, into an address the editor can open.
//
// PURE STRING WORK for the remote and archive-member cases, so it answers identically
// for a file that does not exist yet — which is what the "open an empty editor and let
// Save create it" ruling needs, and what lets Preferences show the resolved address for
// a path the user is still typing. The one exception is inherited: ArchiveLocation::split()
// stats a local path (its rule 0, which is what keeps a real directory named `bundle.zip`
// working), so a local answer can change when a file appears. The same weak purity
// logSettingsKey() already has.
//
// THE PATH IS RESOLVED AGAINST THE LOG'S OWN DIRECTORY, and that is the whole reason the
// setting is worth having at the middle level of the tree: one `*.log` entry saying
// `../conf/log4cplus.properties` resolves correctly for every log it matches, each
// against its own directory. An absolute path is taken as it stands. Either way the
// answer lands on the SAME DEVICE as the log — a config for a log on another machine is
// on that machine.
struct ConfigAddress
{
    // THREE STATES, NOT A BOOL, and the distinction is the same one openFile() draws
    // when it returns false meaning "refused and reported" and never "not there yet".
    // Nothing configured is not an error: it is what makes the menu item run a file
    // dialog instead. Fold the two together and a picker is replaced by a refusal.
    enum class State {
        Unset,     // no config path configured for this log — ask the user for one
        Resolved,  // `address` is openable
        Refused,   // `reason` says why, in words meant for a person
    };

    State   state = State::Unset;

    // Normalized and openable; empty unless Resolved. NEVER carries a password: every
    // remote answer is built from RemoteLocation::parse(), which discards one, and
    // emitted through RemoteLocation::toString(), which never writes one.
    QString address;

    // The directory a relative path was resolved against, for Save's "that directory
    // does not exist" message and for the resolved-address preview in Preferences.
    // Empty when nothing was resolved.
    QString baseDir;

    // Why not. Empty unless Refused. Already translated, and any address inside it has
    // been through RemoteLocation::withoutPassword().
    QString reason;
};

// Resolve `configuredPath` (LogProfile::configPath) against `logAddress`.
//
// The reduction is: find the filesystem the log is ON and the directory it is IN, then
// place the configured path there.
//
//   archived  -> the CONTAINER, reduced once more (a container may itself be remote).
//                `/srv/bundle.zip/var/log/app.log` anchors at `/srv/bundle.zip`, whose
//                directory is `/srv/` — beside the container, never inside it.
//   remote    -> the host, and the directory part of the remote path.
//   local     -> the directory part of the path.
//
// Refused for: a config path spelled as an ssh:// or sftp:// URL (the transport is
// DERIVED from the log's and never spelled separately — a second URL means a second
// host, a second credential prompt, and, at the pattern level, one host named for many
// logs); a resolved path that lands inside an archive container, which cannot be
// written and which the ruling puts beside the container instead; a remote-shaped log
// address that does not parse; and an empty log address.
ConfigAddress resolveConfigAddress(const QString &logAddress, const QString &configuredPath);

} // namespace loftail
