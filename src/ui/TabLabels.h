#pragma once

#include <QStringList>

namespace loftail {

// The name a log wears on its tab, decided for the WHOLE set of open logs at once
// (SPEC.md §3, ARCHITECTURE.md §12.4).
//
// A tab shows the log's own name — `logSourceDisplayName()`, which already brackets a
// remote host or an archive container onto it. That is enough right up until two open
// logs answer to the same name, which is the ordinary case for anyone tailing one
// service across hosts or environments: `svc-a/app.log` and `svc-b/app.log` both read
// `app.log`, and only the tooltip told them apart. Where two names collide, each grows
// the nearest parent directory that differs — as many as it takes, and no more.
//
// Why a free function over the whole set rather than a name computed per log as it
// opens: a label is a statement about a log's NEIGHBOURS, so it changes when they do.
// Closing one of two `app.log`s has to shorten the survivor back to `app.log`, and that
// only falls out if the set is relabelled on every open and close. It is also the whole
// reason this is a pure function of a list of addresses — that is the shape the rule
// actually has, and the shape a table-driven test can drive directly.
//
// Not on the ingest path. MainWindow caches what this returns per file (see
// DocumentContext::tabLabel); rewriting a QTabBar entry relays the whole bar out, so
// the labels are recomputed when the set of open logs CHANGES and never per tick.

// Longest parent prefix a label carries before its middle is elided. The log's own
// name is never elided here — the tab bar's own ElideMiddle handles a long one, and a
// label that is short when unambiguous must stay byte-identical when it grows a
// prefix. The elision keeps the prefix's head and tail, which is where the
// distinguishing segments are: the outermost one is what made this label unique, and
// the innermost is the directory the log actually sits in.
inline constexpr int kMaxTabPrefixChars = 24;

// One label per address, in the same order. Handles every address the application
// accepts — a local path, an `ssh://` URL and a path inside an archive — by asking
// RemoteLocation/ArchiveLocation what the parts are rather than cutting up the string.
//
// Segments contributed, outermost last:
//   local     the directories above the file.
//   remote    the directories above the file on that host. The HOST is not a segment:
//             it is already in the log's own name, in brackets, so two hosts serving
//             `/var/log/app.log` never collide in the first place.
//   archived  the directories above the member inside the container, then the
//             directories above the CONTAINER. The container's name is bracketed onto
//             the log's name exactly as a host is, and for the same reason.
//
// Addresses that stay ambiguous after every segment is spent — two users on one host
// naming one path, say — keep their duplicate labels rather than growing something
// invented. The tooltip carries the full address in every case.
QStringList tabLabelsFor(const QStringList &addresses,
                         int maxPrefixChars = kMaxTabPrefixChars);

} // namespace loftail
