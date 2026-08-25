#include "RestartTarget.h"

#include "LogAnchor.h"

#include <QCoreApplication>
#include <QLatin1String>

namespace loftail {

namespace {
// Translation context for this file. Nothing in core is a QObject, so there is no
// inherited tr(), and every `reason` below is read by a person — it reaches the refusal
// strip above the document well. See RemoteLocation.cpp for the same shim.
struct Tr
{
    Q_DECLARE_TR_FUNCTIONS(loftail::RestartTarget)
};
} // namespace

RestartTarget resolveRestartTarget(const QString &logAddress, const QString &script)
{
    RestartTarget out;

    // Nothing configured. NOT an error — the caller explains and offers Preferences.
    // Trimmed, so a box holding only whitespace is unconfigured rather than a script that
    // runs nothing and reports success.
    if (script.trimmed().isEmpty())
        return out;

    const auto anchor = logAnchorOf(logAddress);
    if (!anchor) {
        out.state = RestartTarget::State::Refused;
        // withoutPassword() is the ONE filter for an address about to be shown, and it
        // matters most here: this branch is reached precisely when parse() FAILED, which
        // is the case that never went through parse()'s own password-dropping.
        out.reason = logAddress.isEmpty()
            ? Tr::tr("There is no log to restart an application for.")
            : Tr::tr("Not a valid log address: %1")
                  .arg(RemoteLocation::withoutPassword(logAddress));
        return out;
    }

    out.state = RestartTarget::State::Resolved;
    out.script = script;
    out.remote = anchor->remote.has_value();
    out.host = anchor->remote;

    // `anchor->path` is the path on the machine the script will run on — the far-end path
    // for a remote log, the container for an archived one. Never the ssh:// URL and never
    // loftail's nested archive address: neither is something a shell over there could
    // open, and handing one over would be a variable that silently names nothing.
    out.variables.append({QString::fromLatin1(RestartVars::kLogFile), anchor->path});

    // ARCHIVE duplicates LOGFILE rather than replacing it, so a script written for a
    // plain log keeps working when the same log is opened out of a bundle.
    //
    // Keyed on `archived`, NEVER on the member being non-empty: a bare `app.log.gz` is an
    // archive whose one member is implied and therefore unnamed, so testing the member
    // would leave the commonest archived shape of all with no ARCHIVE at all.
    if (anchor->archived) {
        out.variables.append({QString::fromLatin1(RestartVars::kArchive), anchor->path});

        // MEMBER is the one variable with no plain-log counterpart, so it is OMITTED
        // ENTIRELY rather than exported empty — a script can then test for it, and a
        // single-stream container honestly says it has no member to name.
        if (!anchor->member.isEmpty()) {
            out.variables.append(
                {QString::fromLatin1(RestartVars::kMember), anchor->member});
        }
    }

    return out;
}

bool restartTargetIsRunnable(const RestartTarget &target, QString *reason)
{
#if !defined(LOFTAIL_HAVE_SSH)
    if (target.remote) {
        if (reason) {
            *reason = Tr::tr("This build has no SSH support, so a restart command cannot "
                             "be run on another machine.");
        }
        return false;
    }
#else
    Q_UNUSED(target);
#endif
    Q_UNUSED(reason);
    return true;
}

} // namespace loftail
