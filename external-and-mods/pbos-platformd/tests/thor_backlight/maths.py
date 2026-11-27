#!/usr/bin/env python3
"""The Python daemon's curves, and the follow from its formula, (sm8550-thor-backlightd's run(), as they
are there) over maths.cpp's grid; prints the same lines."""
import math
import sys

STEAM_LOWEST, STEPS, PANEL_MIN, TOP_CAP = 0.01, 4095, 10, 3413
STEAM_MID, STEAM_HIGH = 0.29730177875068026, 0.8408964152537145
BOTTOM_CURVE = (0.514020912017522, -0.0624984606065165, 0.00882449823664514, 4.45251742834015e-05)
FOLLOW_TAU, FOLLOW_SNAP_RATIO = 0.05, 1.002
top_max, bottom_max = 3413, 65535


def level_of(value, maximum):
    return max(0.0, min(1.0, value / maximum))


def panel_value(f, maximum):
    step = int(1 + max(0.0, min(1.0, f)) * (STEPS - 1))
    return max(PANEL_MIN, min(maximum, step * maximum // STEPS))


def stock_top(f):
    return panel_value(f, top_max)


def stock_bottom(f):
    a, b, c, e = BOTTOM_CURVE
    d = max(0.0, min(1.0, f)) * 100
    return panel_value(min(100.0, a + b * d + c * d * d + e * d * d * d) / 100, bottom_max)


def steam_position(level):
    l = max(0.0, min(1.0, level))
    if l <= STEAM_LOWEST:
        return 0.0
    if l <= STEAM_MID:
        return 0.5 * (math.log2(l / STEAM_LOWEST) / 4.893856189774724)
    if l <= STEAM_HIGH:
        return 0.5 + 0.4 * (math.log2(l / STEAM_MID) / 1.5)
    return 0.9 + 0.1 * (math.log2(l / STEAM_HIGH) / 0.25)


def follow(g, want, frm, t):
    """The continuous follow (thor_backlight.h), written from its formula:
    v = v * (target / v) ** (1 - exp(-dt / TAU)); g is [v, t] or []."""
    if not g:
        if frm == want:
            return want
        g[:] = [float(max(1, frm)), t]
    target = float(max(1, want))
    dt = t - g[1]
    g[1] = t
    if dt > 0:
        g[0] = g[0] * (target / g[0]) ** (1 - math.exp(-dt / FOLLOW_TAU))
    r = g[0] / target
    if abs(g[0] - target) <= 1 or (r < FOLLOW_SNAP_RATIO and r > 1 / FOLLOW_SNAP_RATIO):
        g.clear()
        return want
    return max(1, round(g[0]))


out = sys.stdout
for i in range(-100, 10101):
    f = i / 10000.0
    out.write(f"top {i} {stock_top(f)}\nbottom {i} {stock_bottom(f)}\n")
for v in range(0, bottom_max + 1):
    level = level_of(v, bottom_max)
    out.write(f"pos {v} {'%.8f' % steam_position(level)} {max(1, round(level * bottom_max))}\n")
for v in range(0, top_max + 1):
    out.write(f"tpos {v} {'%.8f' % steam_position(level_of(v, top_max))}\n")
ends = [1, 2, 10, 11, 100, 655, 1000, 3413, 30000, 65535]
for a in ends:
    for b in ends:
        g, shown = [], a
        for k in range(41):
            shown = follow(g, b, shown, k * 0.016)
            out.write(f"follow {a} {b} {k} {shown} {1 if g else 0}\n")
g, shown, want = [], 65266, 65266
for ms in range(1601):
    if ms % 40 == 0 and ms <= 1000:
        want = 65266 - ms * 45
    if ms % 16 == 0 or ms % 40 == 0:
        shown = follow(g, want, shown, ms / 1000.0)
        out.write(f"drag {ms} {want} {shown}\n")
for i in range(-20, 21):
    out.write(f"round {i} {round(i / 4.0)}\n")
