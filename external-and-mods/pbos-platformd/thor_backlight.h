// thor-backlight's brightness maths (see thor_backlight.cpp): AYN's Android
// curves for the AYN Thor's two panels, Steam's slider and the follow. Here
// on their own so tests/thor_backlight can check them against the Python's.
//
// Each line of the curves is the Python's expression in the same order (no reassociation,
// no fused multiply-add: the build is ISO C++, so GCC keeps fp-contract off),
// and Python's round() is ties-to-even, as nearbyint is.
#pragma once

#include <cmath>
#include <optional>

namespace pp::thor_bl {

constexpr double STEAM_GAMMA = 2.2;
// Steam's slider all the way down writes this level (655 of 65535 on a
// Thor), which is Android's slider at 0.
constexpr double STEAM_LOWEST = 0.01;
// As AYN's Android (Thor 1.0.0.377) sets the panels. A slider position f
// (0-1) becomes one of STEPS levels, 1 + f * (STEPS - 1), and the panel gets
// level * its highest / STEPS, never under PANEL_MIN (the device tree's
// bl-min-level). The top panel's highest is TOP_CAP (bl-max-level 0x0D55;
// the kernel caps it too), the bottom one's its full 65535. The bottom panel
// first goes through AYN's curve: percent = a + b*d + c*d^2 + e*d^3 with
// d = f * 100, up to 100 (it passes 100 above f = 0.9045 and dips a little
// below f = 0.0345, both as on Android).
constexpr int STEPS = 4095;
constexpr int PANEL_MIN = 10;
constexpr int TOP_CAP = 3413;
constexpr double CURVE_A = 0.514020912017522, CURVE_B = -0.0624984606065165, CURVE_C = 0.00882449823664514,
                 CURVE_E = 4.45251742834015e-05;
// A new brightness is followed, not jumped to: every GLIDE_MS the panel
// closes 1 - exp(-dt / FOLLOW_TAU) of the way, in ratios (equal steps to
// the eye, more so on the AMOLED); about 95 % in 150 ms, as the Python's
// glide. Unlike that glide, a new target does not start over: Steam's
// slider sends a value every ~40 ms while dragged, and each restart moved
// the panel a sliver, then a jump once the drag slowed.
constexpr int GLIDE_MS = 16;
constexpr double FOLLOW_TAU = 0.05;
// Close enough: the target is written as it is.
constexpr double FOLLOW_SNAP_RATIO = 1.002;

// Python's max(0.0, min(1.0, x)), NaN included (it comes out as 1.0).
inline double clamp01(double x) {
    double m = x < 1.0 ? x : 1.0;
    return m > 0.0 ? m : 0.0;
}

inline double level_of(long long value, long long maximum) {
    return clamp01(static_cast<double>(value) / static_cast<double>(maximum));
}

// Android's display stack: a position (0-1) in STEPS levels, onto the
// panel's range.
inline int panel_value(double f, int maximum) {
    long long step = static_cast<long long>(1 + clamp01(f) * (STEPS - 1));
    long long v = step * maximum / STEPS;
    if (v > maximum) v = maximum;
    return static_cast<int>(v < PANEL_MIN ? PANEL_MIN : v);
}

inline int stock_top(double f, int top_max) { return panel_value(f, top_max); }

// AYN's curve for the bottom screen, in percent of the panel.
inline int stock_bottom(double f, int bottom_max) {
    double d = clamp01(f) * 100;
    double pct = CURVE_A + CURVE_B * d + CURVE_C * d * d + CURVE_E * d * d * d;
    return panel_value((pct < 100.0 ? pct : 100.0) / 100, bottom_max);
}

// The Retroid Pocket Duo's bottom panel (thor-backlight's DUO profile): its
// slider position on a straight line over the panel's range, in Android's
// STEPS and never under PANEL_MIN (Retroid's own curve not dug yet).
inline int linear_bottom(double f, int bottom_max) { return panel_value(f, bottom_max); }

// Where Steam's slider sits (0-1, from its lowest to full), from the level
// it put out.
inline double steam_position(double level) {
    double lowest = std::pow(STEAM_LOWEST, 1 / STEAM_GAMMA);
    double position = std::pow(clamp01(level), 1 / STEAM_GAMMA);
    double p = (position - lowest) / (1 - lowest);
    return p > 0.0 ? p : 0.0;
}

// Python's round() of a float to an int (ties to even).
inline long long py_round(double x) { return static_cast<long long>(std::nearbyint(x)); }

struct Follow {
    double v;  // where the panel is on the way (not rounded)
    double t;  // when it was last moved
};

// The value to write at time t on the way to want: g holds the way (empty
// when the panel is at its target; from is where the panel is then).
// Steps are in ratios, never under 1; within one unit or
// FOLLOW_SNAP_RATIO the target itself, and the way ends.
inline int follow(std::optional<Follow>& g, int want, int from, double t) {
    if (!g) {
        if (from == want) return want;
        g = Follow{from > 1 ? static_cast<double>(from) : 1.0, t};
    }
    double target = want > 1 ? want : 1;
    double dt = t - g->t;
    g->t = t;
    if (dt > 0) g->v = g->v * std::pow(target / g->v, 1 - std::exp(-dt / FOLLOW_TAU));
    double r = g->v / target;
    if (std::fabs(g->v - target) <= 1 || (r < FOLLOW_SNAP_RATIO && r > 1 / FOLLOW_SNAP_RATIO)) {
        g.reset();
        return want;
    }
    long long v = py_round(g->v);
    return static_cast<int>(v > 1 ? v : 1);
}

}  // namespace pp::thor_bl
