#!/usr/bin/env bash
# loftail — a desktop viewer for log4cplus logs.
# Copyright (C) 2026 Valentyn Pavliuchenko
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The mutation harness: break one decision, watch its guard go red.
#
# CLAUDE.md's convention is "break the code and watch the test go red, at the
# granularity of the DECISION and not of the feature", and it notes that
# mutating a whole feature proves nothing — when a decision is "which of N",
# the case has to fail under the other N-1. Each patch in this directory
# inverts exactly one such decision and names, in its own header, the rule it
# comes from and the ONE guard case that must fail.
#
# For each patch: apply, build, run only that case, require FAILURE, revert.
#
# BY HAND, never in CI: it rebuilds per patch (minutes each) and it edits the
# working tree, so it gates nothing — the TSan recipe's status exactly.
#
# ONE CANDIDATE WAS TRIED, DROPPED, AND IS NOW PATCH 08 — how it got there is the
# lesson worth keeping. Swapping the
# order of `replaced` and the shrink in LiveController::checkNow() was tried first
# against tst_tail and dropped as unkillable — and the reason was not that the rule
# is unimportant but that the question was being asked of the wrong source. A
# MappedLogSource holds the inode it opened, so after a rename+recreate its
# refreshSize() still measures the file it holds and the shrink is never seen
# locally AT ALL. A spool is the opposite: refreshSize() adopts the new generation
# and reports what has been fetched into it, so a remote rotation onto a smaller
# log is the one shape in which the two orders disagree. When a mutation survives,
# ask whether the case is reaching the code before concluding the rule is inert.
#
# ONE CANDIDATE IS STILL DROPPED and is worth not re-attempting blind:
# ConfigFileIO's restore of the file's permissions after the QSaveFile rename.
# Defeating the restore reddens nothing, and not because the case is weak —
# QSaveFile::commit() already carries an existing target's mode across the rename
# on this platform, so the restore is a no-op here and there is no observable
# difference to assert on. It stays in src/ as the defence it is, and it stays in
# the unguarded table.
#
# A SECOND ONE JOINED IT, and it is the mirror image: LogFileStore::remove()'s
# order — map first, slot file second. Both halves of save()'s order are killable
# because the first write's failure ABORTS the second (patch 19), and remove()
# deliberately does not work that way: it ignores what flush() answered and
# unlinks the slot file regardless, so provoking either step into failing leaves
# the same state whichever order they are attempted in. There is nothing to
# assert from outside, and the enumeration in
# tst_writefailure::everyInterruptionOfAWriteRecoversWithoutServingOneLogAnothersRecord
# covers what a reader can actually be hurt by — that every one of those states
# recovers without one log being served another's record.
#
# THE commit() FAILURE BRANCH of AtomicJson::write() and writeConfigFile() is
# likewise not reachable without root, and knowing why saves the next attempt:
# a target that is a directory fails at open(), and a write past RLIMIT_FSIZE
# fails at write() — measured, and the reason patch 20 is about the short write
# rather than about the commit. Making the rename alone fail wants the directory
# to become unwritable between the open and the commit, which is not something a
# single-threaded test can arrange.
#
# ONE PATCH IS MARKED `# HARNESS:` AND IS SKIPPED HERE. Its guard is a case in
# tst_keychainlive, which needs a real Secret Service: every case there is gated
# on a backend answering, and a QSKIP exits 0 — so running it in this driver's
# environment would report SURVIVED about a mutation that was never executed,
# which is worse than reporting nothing. Such a patch names in its own header
# what the guard needs and that it was verified red there by hand.
#
#   tests/mutations/run-mutations.sh [--build DIR] [PATCH...]

set -u

BUILD=build
PATCHES=()
while [ $# -gt 0 ]; do
    case "$1" in
        --build) BUILD=$2; shift 2 ;;
        -h|--help) sed -n '20,32p' "$0"; exit 0 ;;
        *) PATCHES+=("$1"); shift ;;
    esac
done

cd "$(dirname "$0")/../.." || exit 1
ROOT=$PWD
[ ${#PATCHES[@]} -gt 0 ] || PATCHES=(tests/mutations/*.patch)

# A mutation run rewrites tracked files, so the only safe way to put them back
# is `git checkout` — which would take uncommitted work with it. Refuse first.
if [ -n "$(git status --porcelain)" ]; then
    echo "refusing to run on a dirty working tree: commit or stash first" >&2
    git status --short >&2
    exit 2
fi

APPLIED=
revert() {
    if [ -n "$APPLIED" ]; then
        git apply -R "$APPLIED" 2>/dev/null || git checkout -- src
        APPLIED=
    fi
}
# Revert on a failed patch, on ^C and on a kill alike: a harness that leaves a
# deliberately broken tree behind is worse than no harness.
trap 'revert; echo; echo "reverted"; exit 130' INT TERM
trap 'revert' EXIT

RESULTS=()
status=0

for patch in "${PATCHES[@]}"; do
    name=$(basename "$patch" .patch)
    guard=$(sed -n 's/^# GUARD: *//p' "$patch" | head -1)
    if [ -z "$guard" ]; then
        echo "$name: no '# GUARD:' line in the patch" >&2
        RESULTS+=("MALFORMED  $name  (no guard named)")
        status=1
        continue
    fi
    binary=${guard%%::*}
    case=${guard#*::}

    echo "=== $name"
    echo "    guard: $guard"

    # A guard this driver cannot execute. Not an escape hatch: a patch carrying
    # the line says in its own header what the guard needs and that it was
    # verified red there by hand. Skipping it beats running it, because every
    # case in a gated binary QSKIPs and a QSKIP exits 0 — so the run would
    # report SURVIVED about a mutation nothing had looked at, which is the one
    # answer worse than no answer.
    harness=$(sed -n 's/^# HARNESS: *//p' "$patch" | head -1)
    if [ -n "$harness" ]; then
        echo "    not runnable here: $harness"
        RESULTS+=("BY HAND    $name  ($guard)")
        continue
    fi

    if ! git apply --check "$patch" 2>/dev/null; then
        echo "    patch does not apply to this tree"
        RESULTS+=("STALE      $name  ($guard)")
        status=1
        continue
    fi
    git apply "$patch" || { RESULTS+=("STALE      $name"); status=1; continue; }
    APPLIED=$patch

    if ! cmake --build "$BUILD" --target "$binary" >/dev/null 2>&1; then
        echo "    the mutated tree does not build $binary"
        RESULTS+=("NOBUILD    $name  ($guard)")
        status=1
        revert
        continue
    fi

    # The guard alone, not the binary: a case that fails because some OTHER
    # case in the same binary broke says nothing about this decision.
    if QT_QPA_PLATFORM=offscreen "$BUILD/tests/$binary" "$case" >/dev/null 2>&1; then
        echo "    STILL GREEN — the guard does not see this mutation"
        RESULTS+=("SURVIVED   $name  ($guard)")
        status=1
    else
        echo "    went red, as it must"
        RESULTS+=("KILLED     $name  ($guard)")
    fi

    revert
done

# Put the build back on the unmutated tree, or the next ordinary ctest run is
# testing whatever the last patch left compiled.
echo
echo "rebuilding the reverted tree"
cmake --build "$BUILD" >/dev/null 2>&1

echo
echo "--- mutation results ---"
for r in "${RESULTS[@]}"; do echo "$r"; done
[ $status -eq 0 ] && echo "every mutation the harness can run was killed by its named guard"
exit $status
