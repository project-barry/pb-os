#!/usr/bin/env python3
"""Off-device equivalence test: the Python sm8550-thor-desktop-brightness and
pbos-platformd's thor-desktop-brightness module, each on a private session
bus (dbus-daemon) with a fake PowerDevil (fake_powerdevil.py) and a fake
bottom panel file. Compares the SetBrightness calls (in order), the files
written to /run/sm8550-thor after setup and after each slider move, and the
key log lines.

  equivalence.py run IMPL OUT.json [SCENARIO...]   one implementation (in the sandbox)
  equivalence.py compare PY.json CPP.json          diff the results
"""
from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import time

RUN = "/run/sm8550-thor"
VAR = "/var/lib/steamos-sm8550"
BL = "/sys/class/backlight/ae94000.dsi.0"
CALLS = "/tmp/calls.log"
BUS = "unix:path=/tmp/bus"
TWO = [["display0", 10000], ["display1", 10000]]


def w(path: str, value) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(str(value))


# Each scenario: config of the fake, files, options:
#   late: the fake starts this long after the daemon (None: before it)
#   emits: [(display, value)] slider moves after setup
#   no_run: no /run/sm8550-thor
S = {
    "normal": dict(fake={"displays": TWO, "bottom": "display1"},
                   files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.4, "bottom": 0.6, "steam": 0.5}'},
                   emits=[("display0", 2500), ("display1", 7000), ("display0", 10000), ("display0", 1)]),
    "late_service": dict(fake={"displays": TWO, "bottom": "display1"}, late=0.7,
                         files={f"{VAR}/thor-screen-brightness.json": '{"top": 1, "bottom": 0}'},
                         emits=[("display0", 3333)]),
    "swapped": dict(fake={"displays": TWO, "bottom": "display0"},
                    files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.25, "bottom": 0.75}'},
                    emits=[("display0", 5000), ("display1", 4000)]),
    "neither": dict(fake={"displays": TWO, "bottom": None},
                    files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.5, "bottom": 0.5}'},
                    emits=[("display1", 1234)]),
    "animated": dict(fake={"displays": TWO, "bottom": "display1", "delay": 0.4, "steps": 4},
                     files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.9, "bottom": 0.3}'}),
    "appended": dict(fake={"displays": TWO, "bottom": "display1", "append": True},
                     files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.2, "bottom": 0.8}',
                            f"{BL}/brightness": "100\n200\n\0"}),
    "scaled": dict(fake={"displays": [["display0", 255], ["display1", 1000]], "bottom": "display1"},
                   bmax="3413\n",
                   files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.123456, "bottom": 0.654321}'},
                   emits=[("display0", 128)]),
    "start_file": dict(fake={"displays": TWO, "bottom": "display1"},
                       files={f"{RUN}/desktop-start.json": '{"top": "0.001", "bottom": true}',
                              f"{VAR}/thor-screen-brightness.json": '{"top": 0.5, "bottom": 0.5}'}),
    "start_bad": dict(fake={"displays": TWO, "bottom": "display1"},
                      files={f"{RUN}/desktop-start.json": '{"top": null, "bottom": 1}',
                             f"{VAR}/thor-screen-brightness.json": '{"top": -3, "bottom": 7}'}),
    "nan": dict(fake={"displays": TWO, "bottom": "display1"},
                files={f"{VAR}/thor-screen-brightness.json": '{"top": "nan", "bottom": "0.5"}'}),
    "no_screens": dict(fake={"displays": TWO, "bottom": "display1"},
                       files={f"{VAR}/thor-screen-brightness.json": "[0.5, 0.5]"}, emits=[("display0", 100)]),
    "three_displays": dict(fake={"displays": TWO + [["display2", 100]], "bottom": "display1"},
                           files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.5, "bottom": 0.5}'}),
    "no_bottom_max": dict(fake={"displays": TWO, "bottom": "display1"}, bmax=None,
                          files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.5, "bottom": 0.5}'}),
    "set_fails": dict(fake={"displays": TWO, "bottom": "display1", "fail": True},
                      files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.5, "bottom": 0.5}'}),
    "no_run_dir": dict(fake={"displays": TWO, "bottom": "display1"}, no_run=True,
                       files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.5, "bottom": 0.5}'}),
    # The Retroid Pocket Duo (the module only, against what is expected): the
    # fake's "bottom" display is the hardware slider, its top panel's.
    "duo_normal": dict(duo=True, bmax="4095\n", fake={"displays": TWO, "bottom": "display0",
                                                      "levels": {"display0": 9000, "display1": 8000}},
                       files={f"{RUN}/desktop-start.json": '{"top": 0.4, "bottom": 0.6, "steam": 1638}',
                              f"{BL}/brightness": "1638\n"},
                       emits=[("display0", 2500), ("display1", 7000)],
                       calls=["display0 4500", "display0 4000", "display1 6000"],
                       ends={"desktop-ready": "1\n", "desktop-top": "0.25000\n", "desktop-bottom": "0.70000\n"}),
    "duo_swapped": dict(duo=True, bmax="4095\n", fake={"displays": TWO, "bottom": "display1",
                                                       "levels": {"display0": 9000, "display1": 8000}},
                        files={f"{RUN}/desktop-start.json": '{"top": 0.8, "bottom": 0.0}',
                               f"{BL}/brightness": "3276\n"},
                        emits=[("display1", 5000), ("display0", 4000)],
                        calls=["display0 7500", "display0 9000", "display1 7500", "display1 8000", "display0 29"],
                        ends={"desktop-ready": "1\n", "desktop-top": "0.50000\n", "desktop-bottom": "0.40000\n"}),
    "duo_neither": dict(duo=True, bmax="4095\n", fake={"displays": TWO, "bottom": None,
                                                       "levels": {"display0": 9000, "display1": 8000}},
                        files={f"{VAR}/thor-screen-brightness.json": '{"top": 0.5, "bottom": 0.5}',
                               f"{BL}/brightness": "2048\n"},
                        emits=[("display1", 1234)],
                        calls=["display0 4500", "display0 9000", "display1 4500", "display1 8000"],
                        ends={"desktop-ready": "1\n"}),
}


def reset(sc: dict) -> None:
    for d in (RUN, VAR, "/sys/class", "/tmp/fake"):
        shutil.rmtree(d, ignore_errors=True)
    for d in (VAR, BL, "/tmp/fake"):
        os.makedirs(d, exist_ok=True)
    if not sc.get("no_run"):
        os.makedirs(RUN)
    w("/sys/firmware/devicetree/base/compatible",
      "retroidpocket,rpduo\0qcom,eliza\0" if sc.get("duo") else "ayn,thor\0qcom,sm8550\0")
    bmax = sc.get("bmax", "10000\n")
    if bmax is not None:
        w(f"{BL}/max_brightness", bmax)
    w(f"{BL}/brightness", "0\n")
    for p, v in sc.get("files", {}).items():
        w(p, v)
    for f in (CALLS,):
        if os.path.exists(f):
            os.unlink(f)
    with open("/tmp/fake/cfg.json", "w") as fh:
        json.dump(sc["fake"], fh)


def files() -> dict:
    out = {}
    if os.path.isdir(RUN):
        for f in sorted(os.listdir(RUN)):
            if f.startswith("desktop-") and not f.endswith(".json"):
                out[f] = open(os.path.join(RUN, f)).read()
    return out


def calls() -> list[str]:
    try:
        return [l for l in open(CALLS).read().splitlines() if l != "owner"]
    except FileNotFoundError:
        return []


def wait_for(cond, timeout: float) -> float | None:
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if cond():
            return time.monotonic() - t0
        time.sleep(0.01)
    return None


def run_one(impl: str, sc: dict) -> dict:
    env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=BUS)
    bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--nopidfile", f"--address={BUS}"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_for(lambda: os.path.exists("/tmp/bus"), 5)
    fake_cmd = ["python3", "/tmp/t/fake_powerdevil.py", "/tmp/fake/cfg.json", CALLS]
    fake = None
    if sc.get("late") is None:
        fake = subprocess.Popen(fake_cmd, env=env)
        wait_for(lambda: os.path.exists(CALLS), 5)
    cmd = (["python3", "/tmp/t/sm8550-thor-desktop-brightness"] if impl == "py"
           else ["/tmp/t/pbos-platformd", "--user", "--only", "thor-desktop-brightness"])
    t0 = time.monotonic()
    d = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if sc.get("late") is not None:
        time.sleep(sc["late"])
        t0 = time.monotonic()
        fake = subprocess.Popen(fake_cmd, env=env)
    ready = wait_for(lambda: os.path.exists(f"{RUN}/desktop-ready"), 6)
    if ready is None:
        time.sleep(0.5)  # nothing to wait for (no /run): let it do whatever it does
    took = None if ready is None else round(time.monotonic() - t0, 2)
    time.sleep(0.3)
    steps = [("setup", calls(), files())]
    if sc.get("emits"):
        import dbus  # the driver's own connection
        b = dbus.bus.BusConnection(BUS)
        obj = b.get_object("org.kde.ScreenBrightness", "/org/kde/ScreenBrightness")
        for name, value in sc["emits"]:
            obj.Emit(name, value, dbus_interface="org.test.Fake")
            time.sleep(0.3)
            steps.append((f"emit {name} {value}", calls(), files()))
    alive = d.poll() is None
    for p in (d, fake, bus):
        if p and p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    log = d.stdout.read().decode(errors="replace")
    return {"steps": steps, "ready_s": took, "alive": alive, "log": log}


def run(impl: str, out_path: str, only: list[str]) -> int:
    res = {}
    for name, sc in S.items():
        if only and name not in only:
            continue
        if sc.get("duo") and impl == "py":
            continue
        reset(sc)
        res[name] = run_one(impl, sc)
        print(f"{impl} {name}: ready after {res[name]['ready_s']} s", flush=True)
    with open(out_path, "w") as fh:
        json.dump(res, fh, indent=1)
    return 0


# Log lines both must print (the rest is each one's own wording/prefix).
KEY_LINES = ["no brightness to share yet", "expected two displays", "turned out to be the bottom screen's",
             "cannot tell the screens apart", "PowerDevil:", "missing: sm8550-thor-backlightd", "top (display"]


def key_lines(log: str) -> list[str]:
    out = []
    for line in log.splitlines():
        for k in KEY_LINES:
            if k in line:
                # From the key on: the module's prefix and D-Bus error texts aside.
                out.append(line[line.index(k):].split(":")[0] if k == "PowerDevil:" else line[line.index(k):])
    return out


def compare(a_path: str, b_path: str) -> int:
    a, b = (json.load(open(p)) for p in (a_path, b_path))
    passed = failed = 0
    for name in a:
        x, y = a[name], b.get(name)
        bad = []
        if y is None:
            bad.append("missing")
        else:
            for i, (sx, sy) in enumerate(zip(x["steps"], y["steps"])):
                if sx[1] != sy[1]:
                    bad.append(f"  {sx[0]} calls\n    py:  {sx[1]}\n    cpp: {sy[1]}")
                if sx[2] != sy[2]:
                    bad.append(f"  {sx[0]} files\n    py:  {sx[2]}\n    cpp: {sy[2]}")
            if len(x["steps"]) != len(y["steps"]):
                bad.append("  step counts differ")
            if key_lines(x["log"]) != key_lines(y["log"]):
                bad.append(f"  log\n    py:  {key_lines(x['log'])}\n    cpp: {key_lines(y['log'])}")
        tail = f"ready py {x['ready_s']} s, cpp {y and y['ready_s']} s"
        if bad:
            failed += 1
            print(f"FAIL {name} ({tail})\n" + "\n".join(bad))
        else:
            passed += 1
            print(f"PASS {name} ({len(x['steps'])} steps, {len(x['steps'][-1][1])} calls; {tail})")
    for name in b:
        if not S[name].get("duo"):
            continue
        last = b[name]["steps"][-1]
        calls = [" ".join(c.split()[1:3]) for c in last[1]]
        ok = calls == S[name]["calls"] and last[2] == S[name]["ends"]
        passed, failed = passed + ok, failed + (not ok)
        print(f"{'PASS' if ok else 'FAIL'} {name} (Duo, the module alone)")
        if not ok:
            print(f"  calls {calls}\n  want  {S[name]['calls']}\n  files {last[2]}\n  want  {S[name]['ends']}\n{b[name]['log']}")
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    if sys.argv[1] == "run":
        sys.exit(run(sys.argv[2], sys.argv[3], sys.argv[4:]))
    sys.exit(compare(sys.argv[2], sys.argv[3]))
