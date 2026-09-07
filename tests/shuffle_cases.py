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

"""Run every QtTest binary's cases in a RANDOM ORDER, in one process each.

WHY THIS IS NOT `ctest --schedule-random`. That flag shuffles the order of the
test BINARIES, and every GUI suite here isolates its configuration per PROCESS
(a QTemporaryDir pointed at by XDG_CONFIG_HOME in main()), so shuffling binaries
can never reach the hazard CLAUDE.md records: "a case that opens the same path as
an earlier one inherits whatever that one left behind and passes or fails on the
order QtTest happened to run them in". That is order dependence BETWEEN CASES OF
ONE BINARY, and only a shuffle inside the process exposes it.

QTest::qExec runs the functions named on its command line, in the order named, so
the shuffle is just a permutation passed as arguments — no test code knows.

A binary listed in the EXCLUDED table below chains its cases on purpose and is run
in declaration order instead; the table says why for each.
"""

import argparse
import json
import os
import random
import re
import subprocess
import sys

# Suites whose cases are order-dependent BY DESIGN. Each entry must say why, and
# adding one is a claim that the chaining is the feature rather than a defect.
EXCLUDED = {
    # CLAUDE.md, M21: "its cases chain through the session on purpose — one closes
    # a window and the next relaunches into what it wrote, which is the only way to
    # drive a real restore". A shuffle here asserts the opposite of the contract.
    "tst_sessiongui": "chains through the session file on purpose (CLAUDE.md, M21)",
}


def discover(build_dir):
    out = subprocess.run(
        ["ctest", "--test-dir", build_dir, "--show-only=json-v1"],
        capture_output=True, text=True, check=True).stdout
    tests = []
    for t in json.loads(out).get("tests", []):
        props = {p["name"]: p.get("value") for p in t.get("properties", [])}
        if props.get("DISABLED"):
            continue
        cmd = t.get("command") or []
        if not cmd or not os.path.basename(cmd[0]).startswith(("tst_", "fuzz_")):
            continue
        env = {}
        for entry in props.get("ENVIRONMENT") or []:
            if "=" in entry:
                k, v = entry.split("=", 1)
                env[k] = v
        tests.append((t["name"], cmd[0], env))
    return tests


def functions(exe, env):
    r = subprocess.run([exe, "-functions"], capture_output=True, text=True,
                       env={**os.environ, **env})
    names = []
    for line in r.stdout.splitlines():
        line = line.strip()
        if line.endswith("()"):
            names.append(line[:-2])
    return names


def run_one(name, exe, env, seed, timeout):
    fns = functions(exe, env)
    if len(fns) < 2:
        return None
    order = list(fns)
    if name not in EXCLUDED:
        random.Random(f"{seed}:{name}").shuffle(order)
    r = subprocess.run([exe] + order, capture_output=True, text=True,
                       env={**os.environ, **env}, timeout=timeout)
    return {"name": name, "exe": exe, "seed": seed, "order": order,
            "rc": r.returncode, "out": r.stdout + r.stderr}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rounds", type=int, default=1, help="seeds seed .. seed+rounds-1")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", default=None, help="regex over test names")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    tests = discover(args.build)
    if args.only:
        rx = re.compile(args.only)
        tests = [t for t in tests if rx.search(t[0])]

    failures = []
    for seed in range(args.seed, args.seed + args.rounds):
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(
                lambda t: run_one(t[0], t[1], t[2], seed, args.timeout), tests))
        bad = [r for r in results if r and r["rc"] != 0]
        print(f"seed {seed}: {len(results)} suites, {len(bad)} failed")
        for r in bad:
            print(f"  FAIL {r['name']} (seed {seed})")
            failures.append(r)

    if failures:
        print("\n=== failing output ===")
        for r in failures:
            print(f"\n--- {r['name']} seed {r['seed']} rc={r['rc']} ---")
            print("reproduce: %s %s" % (r["exe"], " ".join(r["order"])))
            for line in r["out"].splitlines():
                if line.startswith(("FAIL!", "QFATAL", "ASSERT", "Totals")):
                    print("  " + line)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
