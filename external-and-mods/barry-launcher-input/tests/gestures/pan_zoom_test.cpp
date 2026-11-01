// pan-zoom-test: PanZoomGesture (panzoom.h), the Launcher's pan, zoom and
// scroll rules, on frames of finger positions: no Qt, no screen.
//
//   pan-zoom-test   (exit 0: all passed)
//
// Covers: the slop (a tap stays a tap), a pan, a pinch (zoom around the
// midpoint), a pinch flowing into a pan without a lift, a finger landing or
// lifting without a jump, a quick pinch's release (no fling from the
// finger left, unless it moves), tap against long press, a fling's glide and
// a touch stopping it, the zoom's clamps, a list's bounds and axes, the
// wheel, double taps.
#include "../../panzoom.h"
#include "../../touchrules.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using barry::PanZoomGesture;
namespace touch = barry::touch;

namespace {

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        failures++;
}

bool near(double a, double b, double eps = 1e-6)
{
    return std::abs(a - b) <= eps;
}

// A gesture on a 1000 x 800 area at scale 1, zoom 0.25 .. 8, the clock its
// own (seconds).
struct Rig {
    std::vector<std::string> log;
    double t = 100;
    PanZoomGesture g{{
        [this](double x, double y) { log.push_back("tap " + std::to_string(int(x)) + "," + std::to_string(int(y))); },
        [this](double, double) { log.push_back("double"); },
        [this](double, double) { log.push_back("long"); },
        nullptr,
        [this] { log.push_back("drag"); },
    }};
    std::map<int, PanZoomGesture::Point> fingers;

    Rig(bool zoom = true)
    {
        g.setSize(1000, 800);
        g.setScale(1);
        g.setZoomEnabled(zoom);
        g.setZoomRange(zoom ? 0.25 : 1, zoom ? 8 : 1);
        g.setView({0, 0, 1});
    }
    // The fingers as they are now, dt seconds after the last frame.
    void frame(double dt = 0.008)
    {
        t += dt;
        g.due(t);
        g.frame(fingers, t, t);
    }
    void down(int id, double x, double y, double dt = 0.008)
    {
        fingers[id] = {x, y};
        frame(dt);
    }
    void move(int id, double x, double y, double dt = 0.008)
    {
        fingers[id] = {x, y};
        frame(dt);
    }
    void up(int id, double dt = 0.008)
    {
        fingers.erase(id);
        frame(dt);
    }
    void wait(double s)
    {
        t += s;
        g.due(t);
    }
    bool has(const std::string &what) const
    {
        for (const auto &l : log)
            if (l.rfind(what, 0) == 0)
                return true;
        return false;
    }
    // The content point at area (x, y).
    double cx(double x) const { return g.view().x + (x - 500) / g.view().zoom; }
    double cy(double y) const { return g.view().y + (y - 400) / g.view().zoom; }
    // Run the glide to its end (or for at most s seconds).
    void glide(double s = 10)
    {
        for (double end = t + s; g.animating() && t < end;) {
            t += 1.0 / 120;
            g.step(t);
        }
    }
};

void slopAndTap()
{
    Rig r;
    r.down(1, 500, 400);
    r.move(1, 510, 405);  // inside the slop
    check(near(r.g.view().x, 0) && near(r.g.view().y, 0), "slop: a finger wandering 11 px moves nothing");
    check(!r.g.dragging(), "slop: not a drag yet");
    r.up(1);
    check(r.has("tap 510,405"), "slop: lifted, it is a tap where it lifted");
    check(!r.g.animating(), "slop: a tap throws nothing");
}

void pan()
{
    Rig r;
    r.down(1, 500, 400);
    r.move(1, 520, 400);  // past the slop: a drag from here
    check(r.has("drag"), "pan: past the slop it is a drag");
    check(near(r.g.view().x, 0), "pan: no jump by the slop");
    r.move(1, 600, 450);
    check(near(r.g.view().x, -80) && near(r.g.view().y, -50), "pan: the content follows the finger");
    r.wait(0.1);  // rests, then lifts
    r.up(1);
    check(!r.g.animating(), "pan: a finger that rested before lifting throws nothing");
    check(!r.has("tap"), "pan: a drag is no tap");
}

void pinch()
{
    Rig r;
    r.down(1, 400, 400);
    r.down(2, 600, 400);
    r.move(1, 380, 400);
    r.move(2, 620, 400);  // spread +20: past the slop
    check(r.g.dragging(), "pinch: spreading past the slop is a drag");
    const double underX = r.cx(500), underY = r.cy(400);
    r.move(1, 260, 400);
    r.move(2, 740, 400);  // spread 120 -> 240 over two frames
    check(near(r.g.view().zoom, 2, 1e-9), "pinch: zoom follows the fingers' spread (x2)");
    check(near(r.cx(500), underX, 1e-9) && near(r.cy(400), underY, 1e-9),
          "pinch: the point under the midpoint stays under it");

    // On into a pan, the same fingers, no lift.
    const double before = r.g.view().x;
    r.move(1, 360, 400);
    r.move(2, 840, 400);  // both 100 to the right (midpoint +50 per frame)
    check(near(r.g.view().zoom, 2, 1e-9), "pinch -> pan: the zoom holds");
    check(near(r.g.view().x, before - 100 / 2.0, 1e-9), "pinch -> pan: the content follows the midpoint");

    // A finger lifts: nothing jumps; the one left pans on.
    const auto v = r.g.view();
    r.up(2);
    check(near(r.g.view().x, v.x) && near(r.g.view().y, v.y) && near(r.g.view().zoom, v.zoom),
          "lift: one finger lifting moves nothing");
    r.move(1, 400, 420);
    check(near(r.g.view().x, v.x - 40 / 2.0, 1e-9) && near(r.g.view().y, v.y - 20 / 2.0, 1e-9),
          "lift: the finger left pans from where it is");
    // A finger lands far away: nothing jumps either.
    const auto w = r.g.view();
    r.down(3, 900, 100);
    check(near(r.g.view().x, w.x) && near(r.g.view().y, w.y) && near(r.g.view().zoom, w.zoom),
          "land: a finger landing moves nothing");
    r.move(3, 900, 100);
    check(near(r.g.view().zoom, w.zoom), "land: still, it zooms nothing");
    r.up(1);
    r.up(3);
    check(!r.has("tap"), "pinch: no tap");
}

void quickPinchRelease()
{
    {
        Rig r;
        r.down(1, 400, 400);
        r.down(2, 600, 400);
        for (int i = 1; i <= 6; i++) {
            r.move(1, 400 - 15 * i, 400 + 3 * i, 0.004);
            r.move(2, 600 + 15 * i, 400 + 3 * i, 0.004);
        }
        r.up(2, 0.004);
        r.move(1, 309, 419, 0.004);  // the finger left jiggles a little
        r.up(1, 0.004);
        check(!r.g.animating(), "quick pinch: the finger left after it throws nothing");
    }
    {
        Rig r;
        r.down(1, 400, 400);
        r.down(2, 600, 400);
        for (int i = 1; i <= 6; i++) {
            r.move(1, 400 - 15 * i, 400, 0.004);  // the midpoint drifts too
            r.move(2, 600 + 25 * i, 400, 0.004);
        }
        r.fingers.clear();
        r.frame(0.004);  // both lift at once, still spreading
        check(!r.g.animating(), "quick pinch: both lifting while zooming throws nothing");
    }
    {
        Rig r;
        r.down(1, 400, 400);
        r.down(2, 600, 400);
        for (int i = 1; i <= 4; i++) {
            r.move(1, 400 - 15 * i, 400, 0.008);
            r.move(2, 600 + 15 * i, 400, 0.008);
        }
        r.up(2);
        for (int i = 1; i <= 8; i++)
            r.move(1, 340, 400 + 12 * i, 0.008);  // then really moves, fast
        r.up(1, 0.008);
        check(r.g.animating(), "quick pinch: the finger left throws once it moved by itself");
    }
}

void tapAndLongPress()
{
    Rig r;
    r.down(1, 300, 300);
    r.wait(touch::LONG_PRESS_S - 0.05);
    check(!r.has("long"), "long press: not before its time");
    r.wait(0.06);
    check(r.has("long"), "long press: held still for the long-press time");
    r.move(1, 400, 300);
    check(!r.g.dragging() && near(r.g.view().x, 0), "long press: then nothing moves");
    r.up(1);
    check(!r.has("tap"), "long press: no tap after it");

    Rig q;
    q.down(1, 300, 300);
    q.down(2, 500, 300);
    q.wait(1);
    q.up(1);
    q.up(2);
    check(!q.has("long") && !q.has("tap"), "two fingers held: no long press, no tap");

    Rig n;
    n.down(1, 300, 300);
    n.up(1, 0.1);
    check(n.has("tap 300,300"), "tap: down and up");
    check(n.g.nextDue() == std::nullopt, "tap: nothing left due (double taps off)");
}

void flingAndCatch()
{
    Rig r;
    r.down(1, 500, 700);
    double y = 700;
    for (int i = 0; i < 12; i++) {
        y -= 12;  // 1500 px/s upward
        r.move(1, 500, y);
    }
    const double atRelease = r.g.view().y;
    r.up(1);
    check(r.g.animating(), "fling: a throw glides on");
    // Speeds along the glide: falling, never reversing.
    double last = r.g.view().y, lastStep = 1e9;
    bool slowing = true, onward = true;
    const double t0 = r.t;
    for (int i = 0; i < 240 && r.g.animating(); i++) {
        r.t += 1.0 / 120;
        r.g.step(r.t);
        const double step = r.g.view().y - last;
        if (step < -1e-9)
            onward = false;
        if (step > lastStep + 1e-6)
            slowing = false;
        lastStep = step;
        last = r.g.view().y;
    }
    check(onward, "fling: the content goes on the way it was thrown");
    check(slowing, "fling: and slows down");
    r.glide();
    const double dist = r.g.view().y - atRelease;
    // Android's scroller for 1500 px/s at 420 ppi: about 193 px over about
    // 0.37 s.
    std::printf("     fling 1500 px/s: %.0f px in %.2f s\n", dist, r.t - t0);
    check(dist > 160 && dist < 230, "fling: Android's distance for its speed");
    check(r.t - t0 > 0.3 && r.t - t0 < 0.45, "fling: Android's duration for its speed");
    check(!r.g.animating(), "fling: stops by itself");

    // A touch stops a glide at once, and is no tap.
    Rig c;
    c.down(1, 500, 700);
    for (int i = 0; i < 10; i++)
        c.move(1, 500, 700 - 20 * (i + 1));
    c.up(1);
    for (int i = 0; i < 6; i++) {
        c.t += 1.0 / 120;
        c.g.step(c.t);
    }
    check(c.g.animating(), "catch: gliding");
    c.log.clear();
    c.down(2, 300, 300);
    const double held = c.g.view().y;
    check(!c.g.animating(), "catch: a touch stops it at once");
    check(c.g.dragging(), "catch: and is a drag from there (it takes the touch)");
    c.t += 0.05;
    c.g.step(c.t);
    check(near(c.g.view().y, held), "catch: nothing glides on");
    c.up(2, 0.05);
    check(!c.has("tap"), "catch: the touch that stopped it taps nothing");
}

void clamps()
{
    Rig r;
    r.down(1, 490, 400);
    r.down(2, 510, 400);
    r.move(1, 0, 400);
    r.move(2, 1000, 400);
    r.move(1, -5000, 400);
    r.move(2, 6000, 400);
    check(near(r.g.view().zoom, 8), "clamp: zoom stops at the max");
    r.up(1);
    r.up(2);
    r.g.wheel(500, 400, 0, -40, r.t);
    check(near(r.g.view().zoom, 0.25), "clamp: wheel out stops at the min");
    const double under = r.cx(200);
    r.g.wheel(200, 400, 0, 1, r.t);
    check(near(r.g.view().zoom, 0.25 * touch::WHEEL_ZOOM, 1e-9), "wheel: a notch zooms a step");
    check(near(r.cx(200), under, 1e-9), "wheel: around the pointer");
}

void list()
{
    // A list: zoom off, down only, content 0 .. 2000 (the center 400 .. 1600).
    Rig r(false);
    r.g.setAxes(false, true);
    r.g.setBounds(500, 400, 500, 1600);
    r.g.setView({500, 400, 1});
    r.down(1, 500, 600);
    r.move(1, 540, 600);  // sideways: nothing to move that way
    r.move(1, 560, 600);
    check(!r.g.dragging(), "list: sideways is no scroll (a page swipe's)");
    r.up(1);
    check(!r.has("tap"), "list: nor a tap");

    r.down(1, 500, 600);
    r.move(1, 500, 580);
    r.move(1, 500, 500);
    check(near(r.g.view().y, 480) && near(r.g.view().x, 500), "list: up scrolls down the content, along y only");
    r.move(1, 500, 900);
    check(near(r.g.view().y, 400), "list: stops at the top (no overscroll)");
    r.up(1, 0.1);

    // Thrown at the end: stops there, early.
    r.g.setView({500, 1500, 1});
    r.down(1, 500, 700);
    for (int i = 0; i < 10; i++)
        r.move(1, 500, 700 - 30 * (i + 1));
    r.up(1);
    check(r.g.animating(), "list: thrown toward the end");
    const double t0 = r.t;
    r.glide();
    check(near(r.g.view().y, 1600), "list: the glide stops at the end");
    check(r.t - t0 < 0.5, "list: at once there, not running on");

    // A wheel notch: WHEEL_PX.
    r.g.setView({500, 1000, 1});
    r.g.wheel(500, 400, 0, -1, r.t);
    check(near(r.g.view().y, 1000 + touch::WHEEL_PX), "list: a wheel notch down scrolls WHEEL_PX");
}

void doubleTap()
{
    Rig r;
    r.g.setDoubleTapZoom(true);
    r.down(1, 300, 300);
    r.up(1, 0.05);
    check(!r.has("tap"), "double tap on: a tap waits for a second one");
    r.down(1, 310, 305, 0.1);
    r.up(1, 0.05);
    check(r.has("double") && !r.has("tap"), "double tap: two taps are one double tap");
    const double under = r.cx(310);
    r.glide();
    check(near(r.g.view().zoom, touch::DOUBLE_TAP_ZOOM), "double tap: zooms in a step");
    check(near(r.cx(310), under, 1e-9), "double tap: around it");

    Rig s;
    s.g.setDoubleTapZoom(true);
    s.down(1, 300, 300);
    s.up(1, 0.05);
    s.wait(touch::TAP_S + 0.01);
    check(s.has("tap 300,300"), "double tap on: one tap alone comes after its wait");

    Rig d;
    d.g.setDoubleTapZoom(true);
    d.down(1, 300, 300);
    d.up(1, 0.05);
    d.down(1, 300, 300, 0.05);
    d.move(1, 360, 300);  // the second touch drags
    check(d.has("tap 300,300") && !d.has("double"), "double tap on: a tap then a drag is a tap and a drag");
    d.up(1);
}

}  // namespace

int main()
{
    slopAndTap();
    pan();
    pinch();
    quickPinchRelease();
    tapAndLongPress();
    flingAndCatch();
    clamps();
    list();
    doubleTap();
    std::printf("%s (%d failed)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
