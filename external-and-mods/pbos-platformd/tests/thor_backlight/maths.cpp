// thor-backlight's maths (thor_backlight.h) over a grid, one value per line,
// for maths.py to compare with the Python daemon's own formulas.
#include <cstdio>
#include <optional>

#include "../../thor_backlight.h"

using namespace pp::thor_bl;
using std::optional;

int main() {
    const int top_max = 3413, bottom_max = 65535;
    for (int i = -100; i <= 10100; ++i) {
        double f = i / 10000.0;
        std::printf("top %d %d\nbottom %d %d\n", i, stock_top(f, top_max), i, stock_bottom(f, bottom_max));
    }
    for (int v = 0; v <= bottom_max; ++v) {
        double level = level_of(v, bottom_max);
        std::printf("pos %d %.8f %lld\n", v, steam_position(level), std::max<long long>(1, py_round(level * bottom_max)));
    }
    for (int v = 0; v <= top_max; ++v) std::printf("tpos %d %.8f\n", v, steam_position(level_of(v, top_max)));
    // The follow: a still target from each start, in 16 ms steps.
    const int ends[] = {1, 2, 10, 11, 100, 655, 1000, 3413, 30000, 65535};
    for (int a : ends)
        for (int b : ends) {
            std::optional<Follow> g;
            int shown = a;
            for (int k = 0; k <= 40; ++k) {
                shown = follow(g, b, shown, k * 0.016);
                std::printf("follow %d %d %d %d %d\n", a, b, k, shown, g ? 1 : 0);
            }
        }
    // A drag: a new target every 40 ms, steps every 16 ms and at each value.
    {
        std::optional<Follow> g;
        int shown = 65266, want = 65266;
        for (int ms = 0; ms <= 1600; ++ms) {
            if (ms % 40 == 0 && ms <= 1000) want = 65266 - ms * 45;
            if (ms % 16 == 0 || ms % 40 == 0) {
                shown = follow(g, want, shown, ms / 1000.0);
                std::printf("drag %d %d %d\n", ms, want, shown);
            }
        }
    }
    for (int i = -20; i <= 20; ++i) std::printf("round %d %lld\n", i, py_round(i / 4.0));
    return 0;
}
