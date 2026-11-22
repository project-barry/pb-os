#!/usr/bin/env python3
"""Compares tests/thor_backlight's snapshots of the Python daemon and the
module: every field of every snapshot. Also says how the panel writes went
(the glides' steps and refused writes, from the write log), which differ in
timing by nature (the Python looked every 100 ms, the module on events).

  compare.py OUTDIR CASE...
"""
import json
import re
import sys

out, cases = sys.argv[1], sys.argv[2:]
rc = 0


def wlog(case, impl):
    try:
        lines = open(f"{out}/{case}.{impl}.wlog").read().split("\n")
    except OSError:
        return {}
    n = {"writes": 0, "einval": 0}
    for line in filter(None, lines):
        n["writes"] += 1
        n["einval"] += line.endswith("EINVAL")
    return n


for case in cases:
    if case.startswith("duo-"):
        # The Duo's profile: its own checks (scenario.py), no Python.
        try:
            cpp = json.load(open(f"{out}/{case}.cpp.json"))
        except OSError as err:
            print(f"{case}: MISSING ({err})")
            rc = 1
            continue
        failed = [f"  [{s['label']}] {what}" for s in cpp for what, ok in s.get("expect", []) if not ok]
        n = sum(len(s.get("expect", [])) for s in cpp)
        if failed:
            rc = 1
            print(f"{case}: FAIL ({len(failed)} of {n} checks)")
            print("\n".join(failed))
        else:
            print(f"{case}: PASS ({n} checks)")
        continue
    try:
        py = json.load(open(f"{out}/{case}.py.json"))
        cpp = json.load(open(f"{out}/{case}.cpp.json"))
    except OSError as err:
        print(f"{case}: MISSING ({err})")
        rc = 1
        continue
    if case == "idle":
        for p, c in zip(py, cpp):
            if "measure" not in p:
                continue
            print(f"idle {p['label'].split()[-1]}: python {p['measure']}  c++ {c['measure']}")
        continue
    # A refused write is a glide's step, whichever one was due at that
    # moment: its value is timing, not behavior.
    def masked(v):
        if not isinstance(v, list):
            return v
        return [re.sub(r"(cannot write|refuses brightness) \d+", r"\1 N", x) if isinstance(x, str) else x for x in v]

    for p, c in zip(py, cpp):
        if "measure" in p:
            print(f"{case} [{p['label']}]:\n  python {p['measure']}\n  c++    {c.get('measure')}")
    diffs = []
    if len(py) != len(cpp):
        diffs.append(f"  {len(py)} snapshots vs {len(cpp)}")
    for p, c in zip(py, cpp):
        for k in p:
            if k != "measure" and masked(p[k]) != masked(c.get(k)):
                diffs.append(f"  [{p['label']}] {k}:\n    py:  {p[k]}\n    cpp: {c.get(k)}")
    w = f"writes py {wlog(case, 'py')} cpp {wlog(case, 'cpp')}"
    if diffs:
        rc = 1
        print(f"{case}: DIFF ({len(py)} snapshots; {w})")
        print("\n".join(diffs))
    else:
        print(f"{case}: SAME ({len(py)} snapshots; {w})")
sys.exit(rc)
