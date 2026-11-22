// bottom-panel-test: BottomPanel (bottompanel.h), where a raw touch lands on
// the bottom screen, and touchrules.h's swipe up from the bottom edge, on
// the AYN Thor (portrait panel turned right, raw units as pixels, as before
// there was a second device) and the Retroid Pocket Duo (landscape, scaled
// from the axes' range). No Qt, no touchscreen.
//
//   bottom-panel-test   (exit 0: all passed)
#include "../../bottompanel.h"
#include "../../touchrules.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace touch = barry::touch;

namespace {

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        failures++;
}

bool near(double a, double b) { return std::abs(a - b) <= 1e-9; }

void thorDefaults()
{
    unsetenv("BARRY_BOTTOM_W");
    unsetenv("BARRY_BOTTOM_H");
    unsetenv("BARRY_TOUCH_TURN");
    const auto p = barry::BottomPanel::fromEnv();
    check(p.turned && near(p.width, 1240) && near(p.height, 1080), "unset: the Thor's 1240 x 1080, turned right");
    // As EdgeSwipe, TouchTrackpad, keyd and shelld had it: x = raw y,
    // y = 1080 - 1 - raw x, unscaled.
    check(near(p.x(0, 0), 0) && near(p.y(0, 0), 1079), "Thor: raw 0,0 is the bottom left");
    check(near(p.x(1079, 1239), 1239) && near(p.y(1079, 1239), 0), "Thor: raw 1079,1239 is the top right");
    check(near(p.x(500, 620), 620) && near(p.y(500, 620), 579), "Thor: raw 500,620 at 620,579");
}

void thorEnv()
{
    setenv("BARRY_BOTTOM_W", "1240", 1);
    setenv("BARRY_BOTTOM_H", "1080", 1);
    setenv("BARRY_TOUCH_TURN", "right", 1);
    auto p = barry::BottomPanel::fromEnv();
    p.maxX = 1079;
    p.maxY = 1239;  // the axes change nothing on the Thor
    check(near(p.x(500, 620), 620) && near(p.y(500, 620), 579), "Thor (barry_device): the same, axes or not");
}

void duo()
{
    setenv("BARRY_BOTTOM_W", "1280", 1);
    setenv("BARRY_BOTTOM_H", "960", 1);
    setenv("BARRY_TOUCH_TURN", "normal", 1);
    auto p = barry::BottomPanel::fromEnv();
    check(!p.turned && near(p.width, 1280) && near(p.height, 960), "Duo: 1280 x 960, normal");
    // edt-ft5x06 with touchscreen-size 1280 x 960: axes 0..1279, 0..959.
    p.maxX = 1279;
    p.maxY = 959;
    check(near(p.x(0, 0), 0) && near(p.y(0, 0), 0), "Duo: raw 0,0 is the top left");
    check(near(p.x(1279, 959), 1279) && near(p.y(1279, 959), 959), "Duo: raw 1279,959 is the bottom right");
    // Another range is scaled to the screen.
    p.maxX = 4095;
    p.maxY = 4095;
    check(near(p.x(2048, 2048), 640) && near(p.y(2048, 2048), 480), "Duo: a 0..4095 range scaled");
    // No range known: raw units as pixels.
    p.maxX = p.maxY = 0;
    check(near(p.x(700, 300), 700) && near(p.y(700, 300), 300), "Duo: no range, raw as pixels");
}

void swipeUp()
{
    // On the Duo's 960 high screen: down at the bottom edge (within 45),
    // up 150 or more, within 0.8 s.
    check(touch::bottomSwipe(950, 700, 0.3, 960), "swipe up from the edge");
    check(touch::bottomSwipe(915, 765, 0.8, 960), "exactly at the limits");
    check(!touch::bottomSwipe(914, 600, 0.3, 960), "started above the edge");
    check(!touch::bottomSwipe(950, 801, 0.3, 960), "not far enough up");
    check(!touch::bottomSwipe(950, 600, 0.81, 960), "too slow");
    check(!touch::bottomSwipe(950, 1000, 0.3, 960), "down, not up");
    // The Thor's 1080: as shelld's HOME_SWIPE_* (45, 150, 0.8).
    check(touch::bottomSwipe(1070, 900, 0.5, 1080) && !touch::bottomSwipe(1030, 800, 0.5, 1080),
          "the Thor's screen: shelld's numbers");
}

}  // namespace

int main()
{
    thorDefaults();
    thorEnv();
    duo();
    swipeUp();
    std::printf("%s (%d failed)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
