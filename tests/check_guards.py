#!/usr/bin/env python3
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

"""Check that every guard named in tests/GUARDS.md still exists.

GUARDS.md maps CLAUDE.md's load-bearing decisions to the test cases that pin
them. Nothing else checks that mapping: a case can be renamed, deleted or
weakened and the prose goes quietly stale, which is exactly the failure
CLAUDE.md records for `everyColumnStartsWideEnoughForItsOwnHeading` and
`severalPickedMembersOpenAsSeveralTabs`. This is the mechanical half.

Three outcomes per entry, and the difference between the last two is the whole
point of the script:

  ok       the binary is built and reports that case (`-functions`)
  skipped  the binary is NOT built in THIS configuration, but tests/tst_<x>.cpp
           exists — an optional dependency is off (SSH, archive, keychain,
           presets) or the platform gate excluded it. Never a failure, or the
           no-optional-deps CI leg goes red for a rule it cannot see. Whether a
           binary was built is read off the list CMake wrote at configure time,
           not off the filesystem: one left behind by an earlier configuration
           is still on disk and is not this build's.
  FAIL     the binary is built and does not report the case (renamed, deleted),
           or there is no tst_<x>.cpp at all (a typo in GUARDS.md).

A gate is therefore decided by the SOURCE file's existence and not by a list of
optional binaries kept here, which would go stale in exactly the way this script
exists to prevent.

One exec per binary, cached, so the whole check is a second or so.
"""

import argparse
import os
import re
import subprocess
import sys

# QtTest's framework hooks are not listed by -functions but are legitimate
# things for a rule to name -- tst_densitybar::init is the per-case reset that
# keeps the density-marks preference from leaking between cases.
FRAMEWORK_SLOTS = {
    "initTestCase",
    "initTestCase_data",
    "init",
    "cleanup",
    "cleanupTestCase",
}

GUARD_RE = re.compile(r"\btst_([A-Za-z0-9_]+)(?:::([A-Za-z0-9_]+))?")


def parse_guards(path):
    """Yield (line number, binary, case-or-None) from GUARDS.md's tables.

    Only the LAST cell of a table row is read: a rule's own text routinely
    names a test binary in passing, and reading the whole row would turn prose
    into assertions.
    """
    entries = []
    with open(path, encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line.lstrip().startswith("|"):
                continue
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if not cells:
                continue
            guard_cell = cells[-1]
            if set(guard_cell) <= set("-: "):  # table rule row, or an em-dash-free separator
                continue
            for binary, case in GUARD_RE.findall(guard_cell):
                entries.append((lineno, "tst_" + binary, case or None))
    return entries


def functions_of(binary_path, cache):
    if binary_path in cache:
        return cache[binary_path]
    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    try:
        out = subprocess.run(
            [binary_path, "-functions"],
            capture_output=True,
            text=True,
            timeout=120,
            env=env,
        ).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        cache[binary_path] = None
        print("  ! could not run %s: %s" % (binary_path, exc))
        return None
    names = set()
    for raw in out.splitlines():
        raw = raw.strip()
        if raw.endswith("()"):
            names.add(raw[:-2].split("(")[0])
    cache[binary_path] = names
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--guards", required=True)
    ap.add_argument("--source-dir", required=True, help="the tests/ source directory")
    ap.add_argument("--binary-dir", required=True, help="where the tst_ binaries were built")
    ap.add_argument("--defined-tests", help="file listing the tests this configuration defined")
    args = ap.parse_args()

    defined = None
    if args.defined_tests and os.path.exists(args.defined_tests):
        with open(args.defined_tests, encoding="utf-8") as fh:
            defined = {line.strip() for line in fh if line.strip()}

    entries = parse_guards(args.guards)
    if not entries:
        print("FAIL: %s named no guards at all" % args.guards)
        return 1

    cache = {}
    ok = 0
    skipped = {}
    failures = []

    for lineno, binary, case in entries:
        source = os.path.join(args.source_dir, binary + ".cpp")
        if not os.path.exists(source):
            failures.append(
                "%s:%d: %s names no test binary in this tree (no tests/%s.cpp) -- typo?"
                % (args.guards, lineno, binary, binary)
            )
            continue

        path = os.path.join(args.binary_dir, binary)
        if not os.path.exists(path) and os.path.exists(path + ".exe"):
            path += ".exe"
        gated_out = defined is not None and binary not in defined
        if gated_out or not os.path.exists(path):
            skipped.setdefault(binary, 0)
            skipped[binary] += 1
            continue

        if case is None:
            ok += 1
            continue
        if case in FRAMEWORK_SLOTS:
            ok += 1
            continue

        names = functions_of(path, cache)
        if names is None:
            failures.append(
                "%s:%d: %s would not list its test functions" % (args.guards, lineno, binary)
            )
            continue
        if case in names:
            ok += 1
        else:
            failures.append(
                "%s:%d: %s::%s is named as a guard but %s does not report it"
                % (args.guards, lineno, binary, case, binary)
            )

    print("guards checked: %d ok, %d skipped (not built here), %d failed"
          % (ok, sum(skipped.values()), len(failures)))
    if skipped:
        print("skipped binaries (optional dependency or platform gate off): %s"
              % ", ".join(sorted(skipped)))
    for message in failures:
        print("FAIL: " + message)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
