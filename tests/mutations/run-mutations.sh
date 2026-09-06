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
# ONE CANDIDATE WAS TRIED AND DROPPED, and it is worth not re-attempting blind:
# swapping the order of `replaced` and the shrink in LiveController::checkNow(),
# which CLAUDE.md flags under "A rotation is no longer silent". Nothing goes red
# — tst_tail::rotateTriggersReindex rotates onto a file that happens to be
# LARGER than the one it replaced, so the shrink is not taken either way, and no
# other case anywhere asserts a ReloadCause over a rotation that shrank. That
# rule is genuinely unguarded and is listed as such in tests/GUARDS.md; a patch
# for it belongs here the day a case exists to kill it.
#
#   tests/mutations/run-mutations.sh [--build DIR] [PATCH...]

set -u

BUILD=build
PATCHES=()
while [ $# -gt 0 ]; do
    case "$1" in
        --build) BUILD=$2; shift 2 ;;
        -h|--help) sed -n '20,36p' "$0"; exit 0 ;;
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
[ $status -eq 0 ] && echo "every mutation was killed by its named guard"
exit $status
